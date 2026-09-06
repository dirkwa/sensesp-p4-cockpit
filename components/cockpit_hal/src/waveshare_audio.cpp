/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "cockpit_hal/waveshare_audio.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"  // dual-mic capture codec (U17); mics are on this, not ES8311
#include "esp_log.h"
#include "driver/gpio.h"
#include "cockpit_hal/i2c_bus.h"  // the touch HAL's i2c_master bus (port 0)

static const char* TAG = "ws-audio";

// The ES8311 sits on the SAME I2C pins (7/8) as the GT911 touch, which
// the touch HAL brings up (i2c_master, port 0). We must NOT open a
// second bus on those pins — doing so fails with ESP_ERR_INVALID_
// STATE and kills touch. Instead we reuse the underlying IDF i2c_master
// handle the touch HAL created for port 0 (cockpit_hal::i2c_bus), so codec + touch are
// two devices on one shared bus, exactly the i2c_master sharing model.
static constexpr int kI2cPort = 0;

// Shared audio pin map for the Waveshare P4 7B / 4B panels. These match
// the production (FIB) board mapping in Waveshare's own BSP
// (esp32_p4_function_ev_board.h): SCLK=12, MCLK=13, LCLK/WS=10, DOUT=9,
// DSIN=11. An earlier guess swapped WS(=11) and DIN(=10); a wrong WS
// line means the codec never gets valid L/R frame sync → amp hisses but
// emits no tone. Do NOT "simplify" these back to a guessed order.
static constexpr int kI2sMclk = 13;
static constexpr int kI2sBclk = 12;  // SCLK
static constexpr int kI2sWs = 10;    // LCLK / word-select
static constexpr int kI2sDout = 9;   // P4 -> codec (playback)
static constexpr int kI2sDin = 11;   // ES7210 SDOUT -> P4 (mic capture)
// I2C (SDA=7/SCL=8) is owned by the touch HAL; we borrow
// its port-0 bus handle rather than re-declaring the pins here.
static constexpr int kPaCtrl = 53;   // NS4150B enable, active high
// ES8311_CODEC_DEFAULT_ADDR (0x30) is the 8-bit form; the i2c ctrl
// driver right-shifts it to the 7-bit 0x18 the datasheet lists.
static constexpr uint8_t kEs8311Addr = ES8311_CODEC_DEFAULT_ADDR;

static constexpr int kMclkMultiple = 384;

// Mic capture: the two onboard mics are wired to the ES7210 ADC (U17), NOT
// the ES8311 — confirmed on the 7B schematic (ES8311 MIC input is unpopulated;
// the ES7210's SDOUT1/TDMOUT = GPIO11/ASDOUT is our I2S DIN via R144). ES7210
// I2C addr is 8-bit 0x80 (7-bit 0x40, A0/A1 to GND on the board); the P4 is
// I2S master so the ES7210 is a slave.
// Capture MIC1 ONLY: MIC2 is dead on this board (flatlines at ~2 counts), and
// selecting MIC1|MIC2 into a mono stream buries the live MIC1 voice — the
// capture level collapses. Verified on hardware: MIC1 tracks speech, MIC2
// does not.
static constexpr uint8_t kEs7210Addr = ES7210_CODEC_DEFAULT_ADDR;  // 0x80
static constexpr uint8_t kEs7210MicSel = ES7210_SEL_MIC1;

// One playback clip in flight: heap buffer of int16 mono samples that
// the audio task owns and frees after writing. Kept small — a chime is
// a fraction of a second at 16 kHz.
namespace {
// Upper bound on a single clip: 5 s of mono frames at the codec rate.
// Far above any alert tone, well below the size where frames*channels*
// bytes could overflow size_t.
constexpr size_t kMaxClipFrames =
    5 * cockpit_hal::WaveshareAudio::kSampleRate;
struct Clip {
  int16_t* samples;
  size_t frames;
  bool cue;  // must not be dropped (wake feedback), see play_cue()
};
constexpr int kQueueDepth = 4;
}  // namespace

namespace cockpit_hal {

void WaveshareAudio::init() {
  // --- Reuse the touch bus (port 0). Must run AFTER the
  //     touch HAL's init(); app_main inits audio after
  //     lvgl_init (which inits touch), so the bus already exists. ---
  i2c_bus_ = cockpit_hal::i2c_bus();
  if (!i2c_bus_) {
    ESP_LOGE(TAG, "I2C bus not initialised by the touch HAL yet — no audio");
    return;
  }

  // --- I2S standard mode, master, provides MCLK. Full-duplex on one port:
  //     TX drives the ES8311 DAC (speaker); RX carries the ES7210 ADC's
  //     SDOUT (the onboard mics). Both share MCLK/BCLK/WS. ---
  esp_err_t err;
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;  // clear stale DMA data (matches Waveshare ref)
  err = i2s_new_channel(&chan_cfg, &tx_chan_, &rx_chan_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s channel init failed: %s", esp_err_to_name(err));
    return;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)kMclkMultiple;
  // Stereo slot even though the ES8311 is mono: the codec's I2S
  // interface expects a full L/R frame and takes the left channel;
  // a MONO slot left the DAC starved (hiss, no tone) on real hardware.
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = (gpio_num_t)kI2sMclk;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)kI2sBclk;
  std_cfg.gpio_cfg.ws = (gpio_num_t)kI2sWs;
  std_cfg.gpio_cfg.dout = (gpio_num_t)kI2sDout;
  std_cfg.gpio_cfg.din = (gpio_num_t)kI2sDin;
  err = i2s_channel_init_std_mode(tx_chan_, &std_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s std init failed: %s", esp_err_to_name(err));
    return;
  }
  err = i2s_channel_enable(tx_chan_);
  if (err != ESP_OK) {
    // ESP_ERROR_CHECK would be a no-op in non-assert builds and let init
    // fall through to advertise a ready path that never enabled TX.
    ESP_LOGE(TAG, "i2s channel enable failed: %s", esp_err_to_name(err));
    i2s_del_channel(tx_chan_);
    tx_chan_ = nullptr;
    return;
  }

  // RX (mic) shares the full clock/slot/pin config with TX — the ES7210's
  // SDOUT drives DIN (GPIO 11). Enabled at init (not deferred to first read)
  // so the RX DMA is clocked from the start. A capture failure is non-fatal:
  // playback still works.
  err = i2s_channel_init_std_mode(rx_chan_, &std_cfg);
  if (err == ESP_OK) err = i2s_channel_enable(rx_chan_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "i2s rx init/enable failed (no mic): %s",
             esp_err_to_name(err));
    i2s_del_channel(rx_chan_);
    rx_chan_ = nullptr;
  }

  // --- ES8311 codec via esp_codec_dev. ---
  audio_codec_i2c_cfg_t i2c_ctrl_cfg = {};
  i2c_ctrl_cfg.port = kI2cPort;  // shared with touch; bus_handle is what's used
  i2c_ctrl_cfg.addr = kEs8311Addr;
  i2c_ctrl_cfg.bus_handle = i2c_bus_;
  const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);

  audio_codec_i2s_cfg_t i2s_data_cfg = {};
  i2s_data_cfg.port = I2S_NUM_0;
  i2s_data_cfg.tx_handle = tx_chan_;
  i2s_data_cfg.rx_handle = rx_chan_;  // null if RX init failed above
  const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
  data_if_ = data_if;  // kept for the mic-channel diagnostic probe

  const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

  es8311_codec_cfg_t es_cfg = {};
  es_cfg.ctrl_if = ctrl_if;
  es_cfg.gpio_if = gpio_if;
  // DAC only — the ES8311 handles speaker output; capture is the ES7210's
  // job (the ES8311 mic input is unpopulated on this board).
  es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es_cfg.pa_pin = kPaCtrl;
  es_cfg.use_mclk = true;
  es_cfg.hw_gain.pa_voltage = 5.0;
  es_cfg.hw_gain.codec_dac_voltage = 3.3;
  const audio_codec_if_t* codec_if = es8311_codec_new(&es_cfg);
  if (!codec_if) {
    ESP_LOGE(TAG, "es8311_codec_new failed");
    return;
  }

  esp_codec_dev_cfg_t dev_cfg = {};
  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  dev_cfg.codec_if = codec_if;
  dev_cfg.data_if = data_if;
  codec_ = esp_codec_dev_new(&dev_cfg);
  if (!codec_) {
    ESP_LOGE(TAG, "esp_codec_dev_new failed");
    return;
  }

  // Open the codec once and keep it open; per-clip open/close proved
  // unreliable on hardware. The amp is enabled via PA_CTRL (below) and
  // each clip carries a silence tail so the I2S DMA ring settles to
  // zero between beeps — idle is genuinely silent (verified, no hiss).
  fs_.sample_rate = kSampleRate;
  fs_.channel = 2;  // stereo frame; mono clips are duplicated L/R on write
  fs_.bits_per_sample = 16;
  err = esp_codec_dev_open(codec_, &fs_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(err));
    return;
  }
  open_rate_ = kSampleRate;
  esp_codec_dev_set_out_vol(codec_, vol_pct_);

  // --- Mic capture device: ES7210 ADC (U17, I2C 0x80). Its SDOUT feeds our
  //     I2S RX (rx_handle on DIN=11). Its own I2C control interface + the
  //     shared I2S data interface. Opened lazily in start_capture(). ---
  if (rx_chan_) {
    audio_codec_i2c_cfg_t adc_i2c = {};
    adc_i2c.port = kI2cPort;  // shared touch/codec bus
    adc_i2c.addr = kEs7210Addr;
    adc_i2c.bus_handle = i2c_bus_;
    const audio_codec_ctrl_if_t* adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);

    es7210_codec_cfg_t adc_cfg = {};
    adc_cfg.ctrl_if = adc_ctrl;
    adc_cfg.mic_selected = kEs7210MicSel;
    adc_cfg.master_mode = false;  // P4 I2S is master
    const audio_codec_if_t* adc_if = es7210_codec_new(&adc_cfg);

    if (adc_if) {
      esp_codec_dev_cfg_t in_cfg = {};
      in_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
      in_cfg.codec_if = adc_if;
      in_cfg.data_if = data_if;
      codec_in_ = esp_codec_dev_new(&in_cfg);
    }
    if (codec_in_) {
      capture_ready_ = true;
      ESP_LOGI(TAG, "ES7210 mic capture available (%lu Hz)",
               (unsigned long)kSampleRate);
    } else {
      ESP_LOGW(TAG, "ES7210 capture init failed — no mic");
    }

    // Second ES7210 device selecting MIC1|MIC2 for the on-device wake feed's
    // 2-channel path. Same shared I2C ctrl + I2S data interface; opened lazily
    // by start_capture2() at channel=2. If this fails, supports_dual_mic()
    // still reports true (it keys off capture_ready_) but start_capture2()
    // will no-op — the wake engine falls back to the mono path only if the
    // handle is missing, so guard on codec_in2_ there.
    if (codec_in_) {
      es7210_codec_cfg_t adc2_cfg = {};
      adc2_cfg.ctrl_if = adc_ctrl;
      adc2_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
      adc2_cfg.master_mode = false;
      const audio_codec_if_t* adc2_if = es7210_codec_new(&adc2_cfg);
      if (adc2_if) {
        esp_codec_dev_cfg_t in2_cfg = {};
        in2_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
        in2_cfg.codec_if = adc2_if;
        in2_cfg.data_if = data_if;
        codec_in2_ = esp_codec_dev_new(&in2_cfg);
      }
      if (codec_in2_) {
        ESP_LOGI(TAG, "ES7210 dual-mic (MIC1|MIC2) wake path available");
      } else {
        ESP_LOGW(TAG, "ES7210 dual-mic device init failed — wake stays mono");
      }
    }
  }

  // Serialises codec open/close/write between the chime task and a
  // streaming caller. Created before the audio task starts.
  codec_mutex_ = xSemaphoreCreateMutex();
  if (!codec_mutex_) {
    ESP_LOGE(TAG, "codec mutex alloc failed");
    return;
  }
  // Serialises the capture refcount + ADC open/close between the wake engine
  // and the run_mic pipeline (different tasks). Separate from codec_mutex_,
  // which guards the playback codec.
  capture_mutex_ = xSemaphoreCreateMutex();
  if (!capture_mutex_) {
    ESP_LOGE(TAG, "capture mutex alloc failed");
    // capture_ready_ was set true when codec_in_ opened; clear it so
    // can_capture() reports false and start_capture()/stop_capture() bail out
    // rather than running the refcount + open/close UNGUARDED (the very race
    // this mutex exists to prevent).
    capture_ready_ = false;
    return;
  }
  // Guards the 2ch wake-feed handle's refcount + open/close. Separate from
  // capture_mutex_ (mono handle); the two handles are never open at once but
  // each needs its own consistent refcount across the wake feed vs the run_mic
  // task lifetimes. If alloc fails, drop the dual-mic handle so start_capture2
  // bails and the wake engine stays mono rather than running unguarded.
  capture2_mutex_ = xSemaphoreCreateMutex();
  if (!capture2_mutex_) {
    ESP_LOGW(TAG, "dual-mic mutex alloc failed — wake stays mono");
    if (codec_in2_) {
      esp_codec_dev_delete(codec_in2_);
      codec_in2_ = nullptr;
    }
  }

  // Force the NS4150B amp enable HIGH directly, independent of the
  // codec driver's own pa_pin handling.
  gpio_config_t pa_cfg = {};
  pa_cfg.pin_bit_mask = 1ULL << kPaCtrl;
  pa_cfg.mode = GPIO_MODE_OUTPUT;
  gpio_config(&pa_cfg);
  gpio_set_level((gpio_num_t)kPaCtrl, 1);

  // --- Playback plumbing. ---
  queue_ = xQueueCreate(kQueueDepth, sizeof(Clip));
  if (!queue_) {
    ESP_LOGE(TAG, "queue alloc failed");
    return;
  }
  // Stack from internal RAM (task stacks can't live in PSRAM). 4 KB is
  // ample for the write loop; no recursion, no big locals.
  if (xTaskCreate(&WaveshareAudio::audio_task, "audio", 4096, this, 5,
                  &task_) != pdPASS) {
    ESP_LOGE(TAG, "audio task create failed");
    return;
  }

  ready_ = true;
  ESP_LOGI(TAG, "ES8311 audio ready (%lu Hz mono)", (unsigned long)kSampleRate);
}

void WaveshareAudio::play_cue(const int16_t* samples, size_t frames) {
  // The wake cue is the user's only signal that the panel heard them, and it
  // fires exactly as the pipeline takes the mic -- the moment the disposable
  // policy below is most likely to drop it. Enqueue it as a cue so the audio
  // task waits for the codec instead of giving up after 50 ms.
  enqueue(samples, frames, /*cue=*/true);
}

void WaveshareAudio::play_pcm(const int16_t* samples, size_t frames) {
  enqueue(samples, frames, /*cue=*/false);
}

void WaveshareAudio::enqueue(const int16_t* samples, size_t frames, bool cue) {
  if (!ready_ || !enabled_ || !samples || frames == 0) return;
  // A chime during an active voice stream is disposable — drop it rather
  // than fight the stream for the codec (and gap the speech with a tail).
  // A cue is not disposable, but it still must not talk over TTS.
  if (streaming_) return;
  // Cap the clip length. Alerts are well under a second; this both
  // rejects absurd inputs and keeps every downstream byte-size
  // computation (here and the stereo+tail buffer in run()) far from
  // size_t overflow.
  if (frames > kMaxClipFrames) return;

  // Copy into a heap buffer the audio task will own and free.
  Clip clip;
  clip.frames = frames;
  clip.cue = cue;
  clip.samples = (int16_t*)malloc(frames * sizeof(int16_t));
  if (!clip.samples) return;
  memcpy(clip.samples, samples, frames * sizeof(int16_t));

  if (xQueueSend(queue_, &clip, 0) != pdPASS) {
    // Queue full. A chime is disposable, so drop it -- the caller is often the
    // LVGL event_loop task and must never block on audio.
    if (!cue) {
      free(clip.samples);
      return;
    }
    // A cue is not disposable: a burst of chimes must not be what silences the
    // one sound the user needs. Evict the oldest queued clip to make room. If
    // that clip is itself a cue, keep it and drop ours instead -- the earlier
    // cue is the one the user is already waiting on.
    Clip old;
    if (xQueueReceive(queue_, &old, 0) == pdPASS) {
      if (old.cue) {
        xQueueSend(queue_, &old, 0);
        free(clip.samples);
        return;
      }
      free(old.samples);
    }
    if (xQueueSend(queue_, &clip, 0) != pdPASS) free(clip.samples);
  }
}

void WaveshareAudio::set_volume(uint8_t pct) {
  if (pct > 100) pct = 100;
  vol_pct_ = (double)pct;
  if (codec_) esp_codec_dev_set_out_vol(codec_, vol_pct_);
}

void WaveshareAudio::set_enabled(bool on) {
  enabled_ = on;
  // Also drop the amp so anything already queued or mid-write goes
  // quiet; without this, disabling would only stop future enqueues.
  if (ready_) gpio_set_level((gpio_num_t)kPaCtrl, on ? 1 : 0);
}

void WaveshareAudio::audio_task(void* arg) {
  static_cast<WaveshareAudio*>(arg)->run();
}

void WaveshareAudio::run() {
  Clip clip;
  for (;;) {
    if (xQueueReceive(queue_, &clip, portMAX_DELAY) != pdPASS) continue;
    if (!clip.samples) continue;

    // Codec is opened as 2-channel; duplicate the mono clip into an
    // interleaved L/R buffer so both slots carry the tone. A trailing
    // block of SILENCE is appended: the I2S DMA ring auto-repeats its
    // last buffer when no new data is written, so without a zero tail
    // the end of the clip loops forever as a continuous tone. The
    // silence leaves the ring at zero. Codec is kept open (see init());
    // blocking here is fine (dedicated task).
    size_t n = clip.frames;
    size_t tail = kSampleRate / 10;  // 100 ms of zeros
    size_t total = n + tail;
    int16_t* stereo = (int16_t*)malloc(total * 2 * sizeof(int16_t));
    if (stereo) {
      for (size_t i = 0; i < n; ++i) {
        stereo[2 * i] = clip.samples[i];
        stereo[2 * i + 1] = clip.samples[i];
      }
      memset(stereo + n * 2, 0, tail * 2 * sizeof(int16_t));
      // Share the codec with the streaming path. If a voice stream grabbed
      // the codec meanwhile, skip this chime (it's disposable). A cue waits
      // longer: it fires as the pipeline takes the mic, so the codec is busy
      // exactly then, and 50 ms silently loses the one sound the user needs.
      const TickType_t wait =
          clip.cue ? pdMS_TO_TICKS(400) : pdMS_TO_TICKS(50);
      if (xSemaphoreTake(codec_mutex_, wait) == pdTRUE) {
        if (!streaming_) {
          ensure_rate(kSampleRate);
          esp_codec_dev_write(codec_, stereo, total * 2 * sizeof(int16_t));
        }
        xSemaphoreGive(codec_mutex_);
      }
      free(stereo);
    }
    free(clip.samples);
  }
}

// Reclock the I2S TX channel to `rate` and (re)open the codec at that rate.
// Caller holds codec_mutex_. Returns false on failure with open_rate_ left
// consistent (or 0 if even the fallback failed).
bool WaveshareAudio::apply_rate(uint32_t rate) {
  // The MASTER I2S clock is what actually sets playback speed. Reopening only
  // the codec-dev (as before) left the I2S channel clock at its init rate, so
  // a 22050 Hz voice sometimes played at the 16000 Hz clock — the "talking
  // through a pipe" tone. Reconfigure the channel clock explicitly: the API
  // requires the channel be disabled (READY) first, then re-enabled.
  esp_codec_dev_close(codec_);
  // Close any OPEN capture codec-dev handle before touching the shared port
  // clock. TX and RX are one full-duplex I2S port, and esp_codec_dev refuses
  // a TX rate that differs from an *enabled* RX peer
  // (audio_codec_data_i2s.c check_fs_compatible: the conflict fires only
  // while paired->in_enable is true, which a bare i2s_channel_disable does
  // NOT clear — only closing the codec-dev handle does). Left open, the
  // 22050 TTS reclock is rejected while the mic runs at 16000, the reply
  // plays through the wrong clock, and the user hears a beep then nothing.
  //
  // Critically, capture stays suspended for the WHOLE non-native stream:
  // reopening the mic at 16000 while TX is still 22050 hits the very same
  // conflict in reverse. The suspend is remembered in capture_suspended_*
  // and only undone once TX returns to kSampleRate (end_stream's reclock).
  if (suspend_capture_for_reclock()) capture_suspended_mono_ = true;
  if (suspend_capture2_for_reclock()) capture_suspended_wake_ = true;
  i2s_channel_disable(tx_chan_);
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  clk.mclk_multiple = (i2s_mclk_multiple_t)kMclkMultiple;
  esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan_, &clk);
  if (err == ESP_OK) err = i2s_channel_enable(tx_chan_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s reclock to %lu Hz failed: %s", (unsigned long)rate,
             esp_err_to_name(err));
    maybe_resume_capture(rate);  // native-rate failure still restores the mic
    return false;
  }
  fs_.sample_rate = rate;
  fs_.channel = 2;
  fs_.bits_per_sample = 16;
  err = esp_codec_dev_open(codec_, &fs_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "codec open at %lu Hz failed: %s", (unsigned long)rate,
             esp_err_to_name(err));
    maybe_resume_capture(rate);  // don't strand the mic on a playback-open fail
    return false;
  }
  esp_codec_dev_set_out_vol(codec_, vol_pct_);
  open_rate_ = rate;
  // Restore capture ONLY when TX is back at the mic's native rate — i.e. the
  // end_stream() reclock, never the 22050 playback reclock (that would
  // conflict again). Until then the mic stays down, which is fine: the wake
  // gate holds it off during Speaking + the echo tail anyway.
  maybe_resume_capture(rate);
  return true;
}

// Reopen capture handles suspended for a reclock, but ONLY once TX is back at
// the native mic rate — reopening at 16000 while TX is still 22050 would hit
// the same full-duplex conflict. Called from every apply_rate() exit.
void WaveshareAudio::maybe_resume_capture(uint32_t applied_rate) {
  if (applied_rate != kSampleRate) return;  // still non-native: keep suspended
  const bool had_mono = capture_suspended_mono_;
  const bool had_wake = capture_suspended_wake_;
  capture_suspended_mono_ = false;
  capture_suspended_wake_ = false;
  resume_capture_after_reclock(had_mono, had_wake);
}

bool WaveshareAudio::ensure_rate(uint32_t rate) {
  // Caller holds codec_mutex_. Only touch the hardware on an actual rate
  // switch — the codec + I2S clock stay put otherwise.
  if (rate == open_rate_) return true;
  if (apply_rate(rate)) return true;
  // Reclock/open failed — fall back to the native rate so the chime path and
  // idle stay functional rather than leaving the codec closed.
  if (!apply_rate(kSampleRate)) open_rate_ = 0;
  return false;
}

bool WaveshareAudio::begin_stream(uint32_t rate, uint8_t bits,
                                  uint8_t channels) {
  // Mono 16-bit only (the codec is opened as a stereo frame and we
  // duplicate L/R). A nonconforming format is refused so the caller can
  // fall back rather than play garbage. A muted board stays silent — like
  // play_pcm(), honour enabled_ so "a quiet helm stays quiet".
  if (!ready_ || !enabled_ || bits != 16 || channels != 1 || rate == 0) {
    return false;
  }
  // Non-reentrant: the mutex is held for the whole stream, so a second
  // begin_stream() before end_stream() would block forever. Refuse instead.
  if (streaming_) return false;
  if (xSemaphoreTake(codec_mutex_, portMAX_DELAY) != pdTRUE) return false;
  bool ok = ensure_rate(rate);
  if (ok) {
    streaming_ = true;
    // Make sure the amp is live for the stream.
    gpio_set_level((gpio_num_t)kPaCtrl, 1);
  } else {
    xSemaphoreGive(codec_mutex_);
  }
  // On success we HOLD codec_mutex_ across the whole stream so no chime can
  // reopen the codec mid-utterance; end_stream releases it.
  return ok;
}

size_t WaveshareAudio::write_stream(const int16_t* samples, size_t frames) {
  if (!streaming_ || !samples || frames == 0) return 0;
  // Duplicate mono -> interleaved L/R into a reusable buffer, then do the
  // BLOCKING codec write. Blocking here is the backpressure that paces the
  // sender; we never drop stream audio.
  if (frames > stream_stereo_frames_) {
    int16_t* nb = (int16_t*)realloc(stream_stereo_, frames * 2 * sizeof(int16_t));
    if (!nb) return 0;  // keep the old buffer; caller may retry smaller
    stream_stereo_ = nb;
    stream_stereo_frames_ = frames;
  }
  for (size_t i = 0; i < frames; ++i) {
    stream_stereo_[2 * i] = samples[i];
    stream_stereo_[2 * i + 1] = samples[i];
  }
  esp_err_t err =
      esp_codec_dev_write(codec_, stream_stereo_, frames * 2 * sizeof(int16_t));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "stream write failed: %s", esp_err_to_name(err));
    return 0;  // report the codec fault rather than a phantom success
  }
  return frames;
}

void WaveshareAudio::end_stream() {
  if (!streaming_) return;
  // Flush a short block of silence: the I2S DMA ring auto-repeats its last
  // buffer, so without a zero tail the final chunk drones. ~100 ms at the
  // stream rate.
  size_t tail = open_rate_ ? open_rate_ / 10 : kSampleRate / 10;
  size_t bytes = tail * 2 * sizeof(int16_t);
  void* zeros = calloc(1, bytes);
  if (zeros) {
    esp_err_t err = esp_codec_dev_write(codec_, zeros, bytes);
    if (err != ESP_OK) {
      // A failed drain leaves the DMA ring on the last audio buffer, so the
      // stream's tail can loop. Log it — the codec is in a bad state.
      ESP_LOGW(TAG, "silence drain failed: %s", esp_err_to_name(err));
    }
    free(zeros);
  }
  streaming_ = false;
  if (stream_stereo_) {
    free(stream_stereo_);
    stream_stereo_ = nullptr;
    stream_stereo_frames_ = 0;
  }
  // Restore the native capture clock. TX and RX share one I2S port here, so
  // playing a 22050 Hz TTS reply also reclocks the MIC to 22050 Hz — while
  // capture_rate() keeps reporting kSampleRate. Every later consumer then gets
  // 22050 Hz samples labelled 16 kHz (pitch-shifted, time-compressed); WakeNet
  // never matches the wake word again until a reboot.
  if (open_rate_ != kSampleRate) {
    apply_rate(kSampleRate);
    restore_rx_channel();  // apply_rate() cycles tx_chan_, taking rx down too
  }
  // Release the codec so chimes can play again. (begin_stream took it.)
  xSemaphoreGive(codec_mutex_);
}

void WaveshareAudio::start_capture() {
  if (!capture_ready_) return;
  if (capture_mutex_) xSemaphoreTake(capture_mutex_, portMAX_DELAY);
  // Refcount: the ADC opens on the first consumer and stays open while any
  // consumer wants it. A second start_capture() (e.g. run_mic while the wake
  // engine is already listening — or a resume() racing run_mic's stop) just
  // bumps the count; it must NOT re-open (already open) nor let the other
  // consumer's stop close it underneath us.
  if (capture_users_++ == 0) {
    fs_in_.sample_rate = kSampleRate;
    fs_in_.channel = 1;  // mono capture — one ADC channel
    fs_in_.bits_per_sample = 16;
    esp_err_t err = esp_codec_dev_open(codec_in_, &fs_in_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "mic open failed: %s", esp_err_to_name(err));
      capture_users_ = 0;
      if (capture_mutex_) xSemaphoreGive(capture_mutex_);
      return;
    }
    // ES7210 analog PGA. Was pinned at the driver ceiling (37.5 dB) to fight a
    // quiet mic, but a listening test showed the result is badly distorted —
    // "like speaking through a long metal tube, barely any dynamic" — and that
    // distortion is why openWakeWord scores this audio the same as silence
    // (loud is not clean; the melspectrogram front-end needs fine structure).
    // Runtime-settable so the gain can be swept and compared by ear/score
    // without a reflash per value. get_db() quantises: 3 dB steps to 33, then
    // 34.5 / 36 / 37.5.
    esp_codec_dev_set_in_gain(codec_in_, mic_gain_db_);
    // The open reconfigures the shared RX slot for THIS handle's channel count
    // (see restore_rx_channel) and leaves the channel disabled. Put it back.
    restore_rx_channel();
    capturing_ = true;
  }
  if (capture_mutex_) xSemaphoreGive(capture_mutex_);
}

size_t WaveshareAudio::record_pcm(int16_t* out, size_t max_frames) {
  if (!out || max_frames == 0) return 0;
  // BLOCKING read of mono 16-bit frames from the ES7210 (opened mono, so one
  // sample per frame — no L/R de-interleave needed). The block paces the
  // caller. Uses its OWN device handle, independent of the playback stream.
  // esp_codec_dev_read takes an int byte count — clamp so a huge request
  // can't overflow it and then be reported as a full success.
  size_t frames = max_frames;
  const size_t max_by_int = (size_t)INT_MAX / sizeof(int16_t);
  if (frames > max_by_int) frames = max_by_int;
  // Claim the read BEFORE the capturing_ check, then re-check under the flag:
  // the reclock/stop paths set their intent and then wait out reading_, so a
  // reader that publishes reading_ first and only then confirms capturing_ is
  // still true can never call esp_codec_dev_read() on a handle those paths
  // are about to close (esp_codec_dev is not safe for concurrent read+close).
  reading_.store(true);
  if (!capturing_) {
    reading_.store(false);
    return 0;
  }
  esp_err_t err =
      esp_codec_dev_read(codec_in_, out, (int)(frames * sizeof(int16_t)));
  reading_.store(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mic read failed: %s", esp_err_to_name(err));
    return 0;
  }
  return frames;
}

void WaveshareAudio::stop_capture() {
  if (!capture_ready_) return;
  if (capture_mutex_) xSemaphoreTake(capture_mutex_, portMAX_DELAY);
  // Only the LAST consumer to leave closes the ADC. This is what stops one
  // consumer's stop_capture() from cutting the mic out from under the other.
  if (capture_users_ > 0 && --capture_users_ == 0 && capturing_) {
    // Wait out any in-flight record_pcm() before closing — esp_codec_dev is
    // not safe for a concurrent read + close on one handle. A read is a single
    // ~32 ms chunk, so this settles almost immediately; bounded so a wedged
    // read can't hang teardown forever.
    for (int i = 0; i < 100 && reading_.load(); i++) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    esp_codec_dev_close(codec_in_);
    capturing_ = false;
    restore_rx_channel();
  }
  if (capture_mutex_) xSemaphoreGive(capture_mutex_);
}

// Close an open capture codec-dev handle before a TX reclock, WITHOUT
// touching its refcount — the consuming task still "owns" it; we cycle the
// hardware under it. esp_codec_dev's full-duplex arbitration
// (check_fs_compatible) rejects a differing TX rate only while the RX peer's
// in_enable is true, and only a codec-dev close clears that flag. Returns
// whether it was open (so resume knows to reopen). Caller holds codec_mutex_
// (playback path); we take the capture mutex to serialise against
// record_pcm()/start/stop.
bool WaveshareAudio::suspend_capture_for_reclock() {
  if (!capture_mutex_) return false;
  xSemaphoreTake(capture_mutex_, portMAX_DELAY);
  bool was_open = capturing_;
  if (was_open) {
    // Order matters: clear capturing_ FIRST so a reader re-checking under its
    // reading_ flag bails, THEN drain the one read possibly already in flight,
    // THEN close. Doing it the other way lets a reader that passed its check
    // call esp_codec_dev_read() on the handle we just closed.
    capturing_ = false;
    for (int i = 0; i < 100 && reading_.load(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    esp_codec_dev_close(codec_in_);  // refcount untouched — consumer keeps it
  }
  xSemaphoreGive(capture_mutex_);
  return was_open;
}

bool WaveshareAudio::suspend_capture2_for_reclock() {
  if (!capture2_mutex_) return false;
  xSemaphoreTake(capture2_mutex_, portMAX_DELAY);
  bool was_open = capturing2_;
  if (was_open) {
    capturing2_ = false;  // clear before draining (see suspend_capture_for_reclock)
    for (int i = 0; i < 100 && reading2_.load(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    esp_codec_dev_close(codec_in2_);
  }
  xSemaphoreGive(capture2_mutex_);
  return was_open;
}

// Reopen capture handles suspended for a reclock, at the mic's native rate.
void WaveshareAudio::resume_capture_after_reclock(bool had_mono, bool had_wake) {
  if (had_mono && capture_mutex_) {
    xSemaphoreTake(capture_mutex_, portMAX_DELAY);
    if (!capturing_ && capture_users_ > 0) {
      if (esp_codec_dev_open(codec_in_, &fs_in_) == ESP_OK) {
        esp_codec_dev_set_in_gain(codec_in_, mic_gain_db_);
        capturing_ = true;
      } else {
        ESP_LOGW(TAG, "mic reopen after reclock failed");
      }
      restore_rx_channel();
    }
    xSemaphoreGive(capture_mutex_);
  }
  if (had_wake && capture2_mutex_) {
    xSemaphoreTake(capture2_mutex_, portMAX_DELAY);
    if (!capturing2_ && capture2_users_ > 0) {
      if (esp_codec_dev_open(codec_in2_, &fs_in2_) == ESP_OK) {
        esp_codec_dev_set_in_gain(codec_in2_, mic_gain_db_);
        capturing2_ = true;
      } else {
        ESP_LOGW(TAG, "wake mic reopen after reclock failed");
      }
      restore_rx_channel();
    }
    xSemaphoreGive(capture2_mutex_);
  }
}

// Re-enable the SHARED I2S RX around capture open/close.
//
// The mono handle (codec_in_, 1ch) and the 2-channel wake handle (codec_in2_,
// 2ch) are two esp_codec_dev instances over ONE rx_chan_. Two things take that
// channel down, and BOTH have to be undone:
//
//  - close: esp_codec_dev calls i2s_channel_disable()
//    (audio_codec_data_i2s.c `_i2s_drv_enable`).
//  - open: the two handles ask for different channel counts, so the open runs
//    i2s_channel_reconfig_std_slot() (via set_drv_fs) to reshape the RX slot.
//    Reconfiguring requires a disabled channel and leaves it disabled.
//
// The open case is the one that actually bites: rx_chan_ is enabled once at
// init and never again, so the first dual->mono->dual handoff leaves it down
// and every read fails with "i2s_channel_read: The channel is not enabled"
// while the AFE starves ("Ringbuffer of AFE is empty"). Observed on hardware as
// on-device wake firing exactly ONCE and never re-arming after the first
// pipeline. Re-enabling only after close is NOT enough — the reconfig happens
// later, inside the next open.
//
// ESP_ERR_INVALID_STATE just means it is already enabled (the other handle
// still holds it open) — not an error worth logging.
void WaveshareAudio::restore_rx_channel() {
  if (!rx_chan_) return;
  esp_err_t err = i2s_channel_enable(rx_chan_);
  if (err == ESP_OK) {
    // It was DOWN and we just brought it back — worth knowing which call site
    // dropped it. ESP_ERR_INVALID_STATE (already enabled) is the quiet path.
    ESP_LOGI(TAG, "rx re-enabled (was down)");
  } else if (err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "rx re-enable failed: %s", esp_err_to_name(err));
  }
}

void WaveshareAudio::start_capture2() {
  if (!codec_in2_ || !capture2_mutex_) return;
  xSemaphoreTake(capture2_mutex_, portMAX_DELAY);
  // Same refcount discipline as start_capture(), on the independent 2ch handle.
  // The wake engine is the only consumer (its feed loop + pause/resume), so the
  // count rarely exceeds 1, but resume() racing a stray stop still wants it.
  if (capture2_users_++ == 0) {
    fs_in2_.sample_rate = kSampleRate;
    fs_in2_.channel = 2;  // MIC1|MIC2 interleaved — two ADC channels
    fs_in2_.bits_per_sample = 16;
    esp_err_t err = esp_codec_dev_open(codec_in2_, &fs_in2_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "dual-mic open failed: %s", esp_err_to_name(err));
      capture2_users_ = 0;
      xSemaphoreGive(capture2_mutex_);
      return;
    }
    // Same PGA as the mono path. Hardcoding the ceiling here made /mic_gain a
    // half-truth: the endpoint set mic_gain_db_ and reported it back, but the
    // on-device wake engine feeds off THIS handle, so the detector kept the
    // 37.5 dB the mono path was moved off — the gain measured 770x worse.
    esp_codec_dev_set_in_gain(codec_in2_, mic_gain_db_);
    restore_rx_channel();  // reconfig on open leaves the shared RX disabled
    capturing2_ = true;
  }
  xSemaphoreGive(capture2_mutex_);
}

size_t WaveshareAudio::record_pcm2(int16_t* out, size_t max_frames) {
  if (!out || max_frames == 0) return 0;
  // BLOCKING read of 2-channel interleaved [MIC1,MIC2] 16-bit frames. One frame
  // = two int16 samples, so a byte count of frames * 2 * sizeof(int16_t).
  // Clamp so frames*2 can't overflow the int esp_codec_dev_read takes.
  size_t frames = max_frames;
  const size_t max_by_int = (size_t)INT_MAX / (2 * sizeof(int16_t));
  if (frames > max_by_int) frames = max_by_int;
  // Claim-then-recheck (see record_pcm) so a close can't land under the read.
  reading2_.store(true);
  if (!capturing2_) {
    reading2_.store(false);
    return 0;
  }
  esp_err_t err = esp_codec_dev_read(codec_in2_, out,
                                     (int)(frames * 2 * sizeof(int16_t)));
  reading2_.store(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "dual-mic read failed: %s", esp_err_to_name(err));
    return 0;
  }
  return frames;
}

void WaveshareAudio::stop_capture2() {
  if (!codec_in2_ || !capture2_mutex_) return;
  xSemaphoreTake(capture2_mutex_, portMAX_DELAY);
  if (capture2_users_ > 0 && --capture2_users_ == 0 && capturing2_) {
    // Wait out any in-flight record_pcm2() before closing (esp_codec_dev is not
    // safe for concurrent read + close on one handle). Bounded like the mono
    // path so a wedged read can't hang teardown.
    for (int i = 0; i < 100 && reading2_.load(); i++) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    esp_codec_dev_close(codec_in2_);
    capturing2_ = false;
    restore_rx_channel();  // see the mono path — one shared rx_chan_
  }
  xSemaphoreGive(capture2_mutex_);
}

namespace {
// Capture one ES7210 mic pair (2 channels, standard I2S — no TDM) and fill the
// RMS + peak for each of the two selected inputs. `mic_a`/`mic_b` are the
// MicLevels indices (0..3) the two selected channels map to. Returns false on
// any codec error. Opens/closes its own ES7210 device handle so it doesn't
// touch the normal mono capture path.
bool probe_pair(const audio_codec_ctrl_if_t* ctrl,
                const audio_codec_data_if_t* data_if, uint8_t sel, int idx_a,
                int idx_b, float gain_db, espos_audio::AudioDriver::MicLevels& out) {
  es7210_codec_cfg_t cfg = {};
  cfg.ctrl_if = ctrl;
  cfg.mic_selected = sel;
  cfg.master_mode = false;
  const audio_codec_if_t* dev_if = es7210_codec_new(&cfg);
  if (!dev_if) return false;
  esp_codec_dev_cfg_t dev_cfg = {};
  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  dev_cfg.codec_if = dev_if;
  dev_cfg.data_if = data_if;
  esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
  if (!dev) return false;

  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = WaveshareAudio::kSampleRate;
  fs.channel = 2;  // both selected inputs, interleaved L/R
  fs.bits_per_sample = 16;
  bool ok = false;
  if (esp_codec_dev_open(dev, &fs) == ESP_OK) {
    // Probe at the live PGA so /mic_probe reports what the capture paths
    // actually hear, not a fixed-gain reading of its own.
    esp_codec_dev_set_in_gain(dev, gain_db);
    // ~0.5 s of 2-channel frames, read in ~32 ms chunks.
    constexpr int kFrames = 256;  // 512 int16 (L/R) per read
    int16_t buf[kFrames * 2];
    double sum_a = 0, sum_b = 0;
    uint16_t peak_a = 0, peak_b = 0;
    long n = 0;
    for (int chunk = 0; chunk < 32; chunk++) {
      if (esp_codec_dev_read(dev, buf, sizeof(buf)) != ESP_OK) break;
      for (int i = 0; i < kFrames; i++) {
        int a = buf[i * 2], b = buf[i * 2 + 1];
        sum_a += (double)a * a;
        sum_b += (double)b * b;
        uint16_t ma = (uint16_t)(a < 0 ? -a : a);
        uint16_t mb = (uint16_t)(b < 0 ? -b : b);
        if (ma > peak_a) peak_a = ma;
        if (mb > peak_b) peak_b = mb;
      }
      n += kFrames;
    }
    if (n > 0) {
      out.rms[idx_a] = (uint16_t)sqrt(sum_a / n);
      out.rms[idx_b] = (uint16_t)sqrt(sum_b / n);
      out.peak[idx_a] = peak_a;
      out.peak[idx_b] = peak_b;
      ok = true;
    }
    esp_codec_dev_close(dev);
  }
  esp_codec_dev_delete(dev);
  return ok;
}
}  // namespace

bool WaveshareAudio::probe_mic_channels(espos_audio::AudioDriver::MicLevels& out) {
  if (!capture_ready_ || !data_if_ || !i2c_bus_) return false;
  // Serialise against playback/capture opens on the shared codec + I2S.
  if (codec_mutex_) xSemaphoreTake(codec_mutex_, portMAX_DELAY);
  audio_codec_i2c_cfg_t i2c = {};
  i2c.port = kI2cPort;
  i2c.addr = kEs7210Addr;
  i2c.bus_handle = i2c_bus_;
  const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&i2c);
  bool ok = false;
  if (ctrl) {
    // MIC1|MIC2 first, then MIC3|MIC4 — each pair stays in standard I2S (2
    // slots); 3+ inputs would force TDM and a different RX slot layout.
    bool p12 = probe_pair(ctrl, data_if_, ES7210_SEL_MIC1 | ES7210_SEL_MIC2, 0,
                          1, mic_gain_db_, out);
    bool p34 = probe_pair(ctrl, data_if_, ES7210_SEL_MIC3 | ES7210_SEL_MIC4, 2,
                          3, mic_gain_db_, out);
    ok = p12 || p34;
  }
  // probe_pair() opened and closed its own devices on the shared RX, which
  // both reconfigured the slot layout and disabled the channel. The wake
  // engine's handle is reopened by its resume(), but that reopen happens
  // BEFORE this function returns on some paths and cannot fix a channel we
  // take down afterwards — so put the RX back here too. Without this the
  // engine resumes onto a dead channel and its feed reads pure silence
  // (rms ~6) while the hardware is fine, which looks like the mic died.
  restore_rx_channel();
  if (codec_mutex_) xSemaphoreGive(codec_mutex_);
  return ok;
}

}  // namespace cockpit_hal
