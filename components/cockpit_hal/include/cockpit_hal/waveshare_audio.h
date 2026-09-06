/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#pragma once

#include "espos_audio/audio_driver.h"

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace cockpit_hal {

/// Audio-out driver for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
/// (ES8311 codec + NS4150B amp). Pin map lives in the .cpp.
///
/// The codec shares the GT911 touch's I2C bus (it sits on the same
/// SDA/SCL pins) rather than opening a second bus — a second bus on
/// those pins wedges the touch controller.
///
/// NOTE: pins are verified on the 7B only. The 4B is a different
/// "86-box" board whose ES8311 wiring is UNVERIFIED; there the codec
/// still inits but may stay silent until the 4B pins are confirmed.
class WaveshareAudio : public espos_audio::AudioDriver {
 public:
  static constexpr uint32_t kSampleRate = 16000;

  void init() override;
  bool ready() const override { return ready_; }
  uint32_t sample_rate() const override { return kSampleRate; }
  void play_pcm(const int16_t* samples, size_t frames) override;
  void play_cue(const int16_t* samples, size_t frames) override;
  void set_volume(uint8_t pct) override;
  void set_enabled(bool on) override;

  bool begin_stream(uint32_t rate, uint8_t bits, uint8_t channels) override;
  size_t write_stream(const int16_t* samples, size_t frames) override;
  void end_stream() override;

  bool can_capture() const override { return capture_ready_; }
  uint32_t capture_rate() const override { return kSampleRate; }
  size_t record_pcm(int16_t* out, size_t max_frames) override;
  void start_capture() override;
  void stop_capture() override;
  bool probe_mic_channels(espos_audio::AudioDriver::MicLevels& out) override;

  // Two live mics (MIC1|MIC2) — expose the 2-channel wake feed. True only once
  // the 2ch handle actually built at init, so the wake engine's "MM" guard is
  // exact (a failed dual-mic device build leaves the engine on the mono path).
  bool supports_dual_mic() const override { return codec_in2_ != nullptr; }
  void start_capture2() override;
  void stop_capture2() override;
  size_t record_pcm2(int16_t* out, size_t max_frames) override;
  // Takes effect on the next capture open (stop_capture/start_capture cycle),
  // so a caller sweeping values must reopen between them.
  void set_mic_gain_db(float db) override { mic_gain_db_ = db; }
  float mic_gain_db() const override { return mic_gain_db_; }

 private:
  static void audio_task(void* arg);
  void run();  // audio task body

  // Re-enable the shared I2S RX after a capture close. Both capture handles
  // sit on one rx_chan_ and esp_codec_dev disables it on close, so without
  // this the next open on the OTHER handle reads a disabled channel.
  void restore_rx_channel();

  // Wait for an in-flight read to leave before its handle is closed; the
  // caller must have cleared the capturing flag first. False if a read is
  // still active at the deadline (caller must then NOT close).
  bool drain_reader(std::atomic<bool>& reading);
  // Close/reopen an open capture handle around a TX reclock so the shared
  // full-duplex port has no enabled RX peer at a conflicting rate (see the
  // .cpp for esp_codec_dev's check_fs_compatible). suspend_* return whether
  // they actually suspended (false if the handle was already closed OR a read
  // was still active, so the caller must not treat it as suspended);
  // resume_* reopens those at the mic rate. Refcounts are left untouched —
  // the consuming task keeps ownership.
  bool suspend_capture_for_reclock();
  bool suspend_capture2_for_reclock();
  void resume_capture_after_reclock(bool had_mono, bool had_wake);
  // Restore suspended capture only when the applied TX rate is native
  // (kSampleRate) — reopening the mic while TX is still at a non-native
  // playback rate would re-trigger the full-duplex conflict. Captures stay
  // suspended across the whole 22050 stream; end_stream's native reclock is
  // what releases them.
  void maybe_resume_capture(uint32_t applied_rate);
  bool capture_suspended_mono_ = false;
  bool capture_suspended_wake_ = false;

  // Reclock playback to `rate` Hz if it isn't already. Serialised by
  // codec_mutex_. Returns false on error. Used by both the chime path
  // (kSampleRate) and streaming (audio-start's rate).
  bool ensure_rate(uint32_t rate);
  // Reclock the I2S TX channel AND (re)open the codec to `rate`. The I2S
  // channel clock is the master playback clock — reopening only the codec-dev
  // left it at the init rate, causing the intermittent "pipe" tone at 22050.
  bool apply_rate(uint32_t rate);

  i2c_master_bus_handle_t i2c_bus_ = nullptr;
  i2s_chan_handle_t tx_chan_ = nullptr;
  i2s_chan_handle_t rx_chan_ = nullptr;  // codec ADC -> P4 (mic)
  esp_codec_dev_handle_t codec_ = nullptr;    // OUT (DAC / speaker)
  esp_codec_dev_handle_t codec_in_ = nullptr;  // IN (ADC / mic)
  // Shared I2S data interface, kept so the diagnostic probe can build extra
  // ES7210 device handles with a different mic_selected without re-creating I2S.
  const audio_codec_data_if_t* data_if_ = nullptr;
  esp_codec_dev_sample_info_t fs_ = {};
  esp_codec_dev_sample_info_t fs_in_ = {};
  bool capture_ready_ = false;    // ADC brought up at init
  // codec_in_ currently open. Atomic (not volatile): read on the mic
  // consumer tasks, written on the pipeline/reclock tasks — record_pcm()
  // publishes reading_ then re-checks this to admit/bail, and the close
  // paths clear it before draining reading_, so the pairing must be a real
  // cross-core atomic, not a compiler-barrier-only volatile.
  std::atomic<bool> capturing_{false};
  // Reference count of active capture consumers. The mic has two independent
  // users — the wake engine (always-on) and the Wyoming push-to-talk/wake
  // pipeline (run_mic) — that start/stop it on different tasks. A single
  // open/close flag let them desync (one's stop_capture() closed the ADC that
  // the other still needed → record_pcm() returned 0 → AFE ringbuffer empty →
  // wake stopped detecting). Refcounting keeps the ADC open while ANY consumer
  // wants it. Guarded by capture_mutex_ so the count and the open/close stay
  // consistent across tasks.
  int capture_users_ = 0;
  SemaphoreHandle_t capture_mutex_ = nullptr;
  // True while record_pcm() is blocked inside esp_codec_dev_read(). record_pcm
  // can't hold capture_mutex_ across its blocking read (that would deadlock the
  // refcount + stall the other consumer), and esp_codec_dev is NOT safe for a
  // concurrent read + close on one handle. So stop_capture() waits for this to
  // clear before it closes the ADC. Atomic: set/read across tasks.
  std::atomic<bool> reading_{false};

  // --- 2-channel wake-feed handle (MIC1|MIC2) -----------------------------
  // A SECOND ES7210 device on the same shared I2S RX + I2C control, selecting
  // MIC1|MIC2 and opened at channel=2. Used ONLY by the wake engine's feed
  // loop so the mono record_pcm() path (STT/PTT) is untouched. Built lazily on
  // the first start_capture2() (its ES7210 device handle needs the I2C ctrl,
  // which init() builds). Guarded by its own mutex; the mono and 2ch handles
  // are never open at once (wake pauses before the STT pipeline runs), so they
  // don't contend for the RX beyond that ordering.
  esp_codec_dev_handle_t codec_in2_ = nullptr;  // IN (ADC / MIC1|MIC2)
  esp_codec_dev_sample_info_t fs_in2_ = {};
  int capture2_users_ = 0;
  std::atomic<bool> capturing2_{false};  // codec_in2_ open; atomic, see capturing_
  std::atomic<bool> reading2_{false};
  SemaphoreHandle_t capture2_mutex_ = nullptr;

  // Analog PGA gain applied at capture open. Measured on hardware: the same
  // spoken wake word scored 0.0004 at the driver's 37.5 dB ceiling and 0.3088
  // at 18 dB — a ~770x improvement — because the maxed preamp compresses and
  // colours the audio ("like a long metal tube, barely any dynamic") until the
  // melspectrogram front-end sees it as noise. Loud is not clean. Runtime
  // override via GET /mic_gain?db=N.
  float mic_gain_db_ = 18.0f;
  double vol_pct_ = 50.0;
  uint32_t open_rate_ = 0;  // rate the codec is currently opened at

  QueueHandle_t queue_ = nullptr;  // of Clip (owned buffer + len)
  TaskHandle_t task_ = nullptr;
  volatile bool enabled_ = true;
  bool ready_ = false;

  // Shared body of play_pcm()/play_cue(); `cue` marks a clip the audio task
  // must not drop when the codec is briefly busy.
  void enqueue(const int16_t* samples, size_t frames, bool cue);

  // Serialises codec open/close/write between the chime audio_task and a
  // streaming caller (the Wyoming socket task). A stream and a chime never
  // play at once: streaming_ makes play_pcm drop while a stream is active.
  SemaphoreHandle_t codec_mutex_ = nullptr;
  volatile bool streaming_ = false;
  // Reusable interleave buffer for streaming L/R duplication (avoids a
  // malloc per chunk). Grown on demand; freed at end_stream / never huge.
  int16_t* stream_stereo_ = nullptr;
  size_t stream_stereo_frames_ = 0;
};

}  // namespace cockpit_hal
