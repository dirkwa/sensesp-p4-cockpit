#include "http_api.h"

#include <ArduinoJson.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cockpit_hal/ui.h"

#include "espos_voice/wyoming_satellite.h"

#include "../layout/layout_manager.h"
#include "../layout/store.h"
#include "../audio/chime.h"
#include "../audio/voice_control.h"

static const char* TAG = "jlp.http";

namespace jlp {

namespace {
espos_voice::WyomingSatellite* g_wyoming = nullptr;
}  // namespace

void http_api_set_wyoming(espos_voice::WyomingSatellite* sat) {
  g_wyoming = sat;
}

namespace {

constexpr size_t kMaxBodyBytes = 64 * 1024;

esp_err_t layout_post(httpd_req_t* req) {
  if (req->content_len == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"empty body\"}");
    return ESP_OK;
  }
  if ((size_t)req->content_len > kMaxBodyBytes) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"body too large\"}");
    return ESP_OK;
  }

  std::string body;
  body.resize(req->content_len);
  int total = 0;
  while (total < req->content_len) {
    int n = httpd_req_recv(req, &body[total], req->content_len - total);
    if (n <= 0) {
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"recv failed\"}");
      return ESP_FAIL;
    }
    total += n;
  }

  // Hop onto the event_loop task to do the swap, then wait for the
  // result. The handler is design-time only — brief blocking is fine.
  auto result = std::make_shared<ApplyResult>();
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"semaphore\"}");
    return ESP_FAIL;
  }

  cockpit_hal::ui::post([body, result, done]() mutable {
    *result = layout_manager().apply(body, ApplySource::PostLayout);
    xSemaphoreGive(done);
  });

  // 25s budget for the apply: build is fast (~hundreds of ms for a
  // 20-widget layout), but the event_loop task may be backed up
  // draining incoming SK WS deltas after a server reconnect. 25s
  // covers that drain without giving up on a healthy layout. The
  // designer-side proxy timeout is 30s, which leaves room for the
  // 504 to actually reach the browser instead of being aborted.
  if (xSemaphoreTake(done, pdMS_TO_TICKS(25000)) != pdTRUE) {
    vSemaphoreDelete(done);
    httpd_resp_set_status(req, "504 Gateway Timeout");
    httpd_resp_sendstr(req,
                       "{\"ok\":false,\"err\":\"apply timed out\"}");
    return ESP_OK;
  }
  vSemaphoreDelete(done);

  httpd_resp_set_type(req, "application/json");
  // Build responses via ArduinoJson so user-controlled strings
  // (layout name, error / warning messages) get escaped instead of
  // being snprintf'd directly into the JSON — otherwise a layout
  // named `my"layout` breaks the response shape.
  JsonDocument resp;
  if (!result->ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    resp["ok"] = false;
    resp["err"] = result->err;
  } else {
    resp["ok"] = true;
    resp["name"] = result->name;
    resp["screens"] = result->screens;
    resp["widgets"] = result->widgets;
    if (!result->warning.empty()) resp["warning"] = result->warning;
  }
  std::string out;
  serializeJson(resp, out);
  httpd_resp_sendstr(req, out.c_str());
  return ESP_OK;
}

esp_err_t healthz_get(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  return ESP_OK;
}

// GET /beep — fire a test chime so audibility can be checked over HTTP
// without waiting for a real alarm. Reports whether the audio sink is
// actually up so a silent result is diagnosable (init failure vs.
// speaker/volume). play_pcm is non-blocking, so this returns at once.
esp_err_t beep_get(httpd_req_t* req) {
  bool ready = chime().audio_ready();
  if (ready) chime().test();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, ready ? "{\"ok\":true,\"audio\":\"ready\"}"
                                : "{\"ok\":false,\"audio\":\"not-ready\"}");
  return ESP_OK;
}

// GET /mic_probe — dump the last ~2 s of captured mic PCM (exactly what the
// wake loop streams to the detector) as a 16 kHz mono WAV. A diagnostic for
// "wake never fires": lets the audio the detector sees be inspected off-panel.
esp_err_t mic_probe_get(httpd_req_t* req) {
  if (!g_wyoming || !g_wyoming->wake_enabled()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "wake not enabled");
    return ESP_OK;
  }
  // Privacy: never hand out mic audio while the mic is muted. The mute also
  // purges the retained ring (voice().set_mic_muted -> wake_pcm_clear), so an
  // unmute doesn't leak pre-mute audio either.
  if (voice().mic_muted()) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "mic muted");
    return ESP_OK;
  }
  const size_t max_samples = 32000;  // 2 s @ 16 kHz
  auto* pcm = static_cast<int16_t*>(malloc(max_samples * sizeof(int16_t)));
  if (!pcm) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "oom");
    return ESP_OK;
  }
  size_t n = g_wyoming->wake_pcm_snapshot(pcm, max_samples);
  const uint32_t rate = 16000;
  const uint32_t data_bytes = (uint32_t)(n * sizeof(int16_t));
  uint8_t hdr[44];
  const uint32_t riff = 36 + data_bytes;
  memcpy(hdr, "RIFF", 4);
  hdr[4] = riff & 0xff; hdr[5] = (riff >> 8) & 0xff;
  hdr[6] = (riff >> 16) & 0xff; hdr[7] = (riff >> 24) & 0xff;
  memcpy(hdr + 8, "WAVEfmt ", 8);
  hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;   // fmt size
  hdr[20] = 1; hdr[21] = 0;                              // PCM
  hdr[22] = 1; hdr[23] = 0;                              // mono
  hdr[24] = rate & 0xff; hdr[25] = (rate >> 8) & 0xff;
  hdr[26] = (rate >> 16) & 0xff; hdr[27] = (rate >> 24) & 0xff;
  const uint32_t byte_rate = rate * 2;
  hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
  hdr[30] = (byte_rate >> 16) & 0xff; hdr[31] = (byte_rate >> 24) & 0xff;
  hdr[32] = 2; hdr[33] = 0;                              // block align
  hdr[34] = 16; hdr[35] = 0;                             // bits
  memcpy(hdr + 36, "data", 4);
  hdr[40] = data_bytes & 0xff; hdr[41] = (data_bytes >> 8) & 0xff;
  hdr[42] = (data_bytes >> 16) & 0xff; hdr[43] = (data_bytes >> 24) & 0xff;
  httpd_resp_set_type(req, "audio/wav");
  // Don't let mic audio linger in caches/proxies.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  // Age of the newest sample in the ring. MUST be checked by anything doing
  // offline analysis: the ring is only fed while the wake loop streams, so it
  // FREEZES when capture stops and keeps serving the same bytes. Repeated
  // requests then return byte-identical WAVs that look like real recordings.
  // static: httpd_resp_set_hdr stores the POINTER, it does not copy, so the
  // buffer must outlive the response. Single-threaded httpd, so this is safe.
  static char age_buf[16];
  const uint32_t age = g_wyoming->wake_pcm_age_ms();
  snprintf(age_buf, sizeof(age_buf), "%lu", (unsigned long)age);
  httpd_resp_set_hdr(req, "X-Mic-Age-Ms", age_buf);
  httpd_resp_send_chunk(req, (const char*)hdr, sizeof(hdr));
  httpd_resp_send_chunk(req, (const char*)pcm, data_bytes);
  httpd_resp_send_chunk(req, nullptr, 0);
  free(pcm);
  return ESP_OK;
}

// GET /mic_probe4 — measure RMS + peak of all four ES7210 mic inputs to find
// which one(s) carry a live mic. Diagnostic for the undocumented board wiring:
// speak while calling it; whichever channels track speech are the real mics.
// GET /mic_gain            — report the analog mic preamp gain
// GET /mic_gain?db=30      — set it, then reopen the capture so it takes effect
//
// Diagnostic for wake-word audio quality. The PGA was pinned at the driver's
// 37.5 dB ceiling to fight a quiet mic, but that badly distorts speech
// ("like a long metal tube, barely any dynamic") and a wake model then scores
// the audio the same as silence. The right value is empirical, so sweep it.
// get_db() quantises: 3 dB steps to 33, then 34.5 / 36 / 37.5.
esp_err_t mic_gain_get(httpd_req_t* req) {
  if (!g_wyoming || !g_wyoming->audio()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "no audio");
    return ESP_OK;
  }
  auto* audio = g_wyoming->audio();
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(q, "db", val, sizeof(val)) == ESP_OK) {
      const float db = strtof(val, nullptr);
      if (db < 0.0f || db > 37.5f) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "db must be 0..37.5");
        return ESP_OK;
      }
      audio->set_mic_gain_db(db);
      // The gain is applied at capture open, so cycle the capture to adopt it.
      // The wake loop holds a reference, hence stop-then-start rather than a
      // bare open: the refcount must go to zero for the ADC to actually close.
      audio->stop_capture();
      audio->start_capture();
    }
  }
  char body[96];
  const int n = snprintf(body, sizeof(body),
                         "{\"mic_gain_db\":%.1f,\"note\":\"applied on capture "
                         "reopen; get_db quantises\"}",
                         audio->mic_gain_db());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body, n);
  return ESP_OK;
}

esp_err_t mic_probe4_get(httpd_req_t* req) {
  if (!g_wyoming) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "no satellite");
    return ESP_OK;
  }
  if (voice().mic_muted()) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "mic muted");
    return ESP_OK;
  }
  espos_audio::AudioDriver::MicLevels lv;
  if (!g_wyoming->probe_mic_levels(lv)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "no mic probe on this board");
    return ESP_OK;
  }
  char body[256];
  int n = snprintf(
      body, sizeof(body),
      "{\"rms\":[%u,%u,%u,%u],\"peak\":[%u,%u,%u,%u],"
      "\"note\":\"index 0..3 = ES7210 MIC1..MIC4; live mic tracks speech\"}",
      lv.rms[0], lv.rms[1], lv.rms[2], lv.rms[3], lv.peak[0], lv.peak[1],
      lv.peak[2], lv.peak[3]);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body, n);
  return ESP_OK;
}

// --- screenshot endpoint -------------------------------------------------
//
// GET /screenshot                — JPEG (default; cheap enough for the
//                                  designer's live-mirror poll).
// GET /screenshot?fmt=bmp        — 16-bit BMP, raw RGB565. Larger and
//                                  slower (~1.2 MB at 1024x600, ~6 s
//                                  over esp_hosted SDIO) but
//                                  bit-perfect and decodes natively in
//                                  any browser; kept for debug diffs.
// GET /screenshot?fmt=jpeg       — explicit JPEG; same as no arg.
//
// JPEG uses the espressif/esp_new_jpeg software encoder (no HW JPEG on
// P4 today). RGB565 → JPEG at quality 70 is ~30-50 KB for our 1024×600
// frame and encodes in well under a second, which is fast enough for
// 1-2 fps polling from the designer.

// Builds a 70-byte BMP header for a width*height image stored as
// 16-bit RGB565 with bit masks. BMP rows are bottom-up.
static void bmp_header_rgb565(uint8_t out[70], uint32_t w, uint32_t h) {
  const uint32_t row_bytes = w * 2;
  const uint32_t pixel_bytes = row_bytes * h;
  const uint32_t file_size = 70 + pixel_bytes;
  const uint32_t pixel_offset = 70;

  auto put32 = [&](size_t at, uint32_t v) {
    out[at] = v & 0xff; out[at+1] = (v>>8)&0xff;
    out[at+2] = (v>>16)&0xff; out[at+3] = (v>>24)&0xff;
  };
  auto put16 = [&](size_t at, uint16_t v) {
    out[at] = v & 0xff; out[at+1] = (v>>8)&0xff;
  };
  std::memset(out, 0, 70);
  out[0]='B'; out[1]='M';
  put32(2, file_size);
  put32(10, pixel_offset);
  put32(14, 56);          // BITMAPV3INFOHEADER
  put32(18, w);
  put32(22, h);           // positive: bottom-up — we'll write rows reversed
  put16(26, 1);           // planes
  put16(28, 16);          // bpp
  put32(30, 3);           // BI_BITFIELDS
  put32(34, pixel_bytes);
  put32(38, 2835);        // x ppm (72dpi)
  put32(42, 2835);        // y ppm
  put32(54, 0xF800);      // R mask (top 5 bits of high byte)
  put32(58, 0x07E0);      // G mask
  put32(62, 0x001F);      // B mask
  put32(66, 0x00000000);  // A mask (unused)
}

struct ScreenshotResult {
  bool ok = false;
  std::string err;
  uint32_t w = 0;
  uint32_t h = 0;
  // Owned pixel buffer (RGB565, top-down as returned by lv_snapshot_take).
  // The endpoint copies rows from this into the HTTP response bottom-up
  // to honour BMP row order.
  std::vector<uint8_t> pixels;
};

// Runs on the event_loop task. Takes a snapshot of the active screen.
// LVGL's internal allocator can't give us a 1.2MB RGB565 buffer
// (it's not PSRAM-aware in this configuration), so we allocate the
// pixel store ourselves in PSRAM and wrap it via lv_draw_buf_init.
static void take_screenshot(ScreenshotResult* out) {
  lv_obj_t* scr = lv_screen_active();
  if (!scr) { out->err = "no active screen"; return; }
  const int32_t w = LV_HOR_RES;
  const int32_t h = LV_VER_RES;
  const uint32_t stride = w * 2;
  const uint32_t size = stride * h;
  uint8_t* pixels = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!pixels) {
    out->err = "PSRAM alloc failed";
    return;
  }
  lv_draw_buf_t buf;
  if (lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, stride, pixels,
                       size) != LV_RESULT_OK) {
    heap_caps_free(pixels);
    out->err = "lv_draw_buf_init failed";
    return;
  }
  if (lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &buf) !=
      LV_RESULT_OK) {
    heap_caps_free(pixels);
    out->err = "lv_snapshot_take_to_draw_buf failed";
    return;
  }
  out->w = w;
  out->h = h;
  // Move out of the lock-held LVGL world before the slow HTTP send.
  out->pixels.assign(pixels, pixels + size);
  heap_caps_free(pixels);
  out->ok = true;
}

// Encode the captured RGB565 frame to JPEG. Returns the encoded bytes in
// `out` on success. The encoder is opened/closed per call: encode is
// >100 ms vs ~1 ms open/close, and statelessness keeps things safe
// across concurrent /screenshot requests.
//
// The precompiled esp_new_jpeg blob shipped for P4 only accepts
// JPEG_PIXEL_FORMAT_RGB888 input (RGB565_LE errors with "src type
// error" at jpeg_enc_open). Convert RGB565 → RGB888 in PSRAM before
// encoding; the conversion is ~1.8 MB for a 1024x600 frame and runs in
// under 10 ms.
static bool encode_rgb565_to_jpeg(const ScreenshotResult& src,
                                  std::vector<uint8_t>* out,
                                  std::string* err) {
  const size_t px_count = (size_t)src.w * src.h;
  const size_t rgb888_size = px_count * 3;
  uint8_t* rgb888 =
      (uint8_t*)heap_caps_malloc(rgb888_size, MALLOC_CAP_SPIRAM);
  if (!rgb888) {
    *err = "rgb888 PSRAM alloc failed";
    return false;
  }
  // RGB565 little-endian → RGB888. LVGL writes pixels low-byte first,
  // and the bits are R[15..11] G[10..5] B[4..0]. We expand 5/6/5 to
  // 8/8/8 with bit replication (the standard way: copy the top bits
  // into the low bits so 0x1F maps to 0xFF rather than 0xF8).
  const uint8_t* p = src.pixels.data();
  uint8_t* q = rgb888;
  for (size_t i = 0; i < px_count; ++i) {
    const uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    const uint8_t r5 = (v >> 11) & 0x1F;
    const uint8_t g6 = (v >>  5) & 0x3F;
    const uint8_t b5 =  v        & 0x1F;
    q[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    q[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    q[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    p += 2;
    q += 3;
  }

  jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
  cfg.width       = (int)src.w;
  cfg.height      = (int)src.h;
  cfg.src_type    = JPEG_PIXEL_FORMAT_RGB888;
  cfg.subsampling = JPEG_SUBSAMPLE_420;
  cfg.quality     = 70;
  jpeg_enc_handle_t enc = nullptr;
  if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK || !enc) {
    heap_caps_free(rgb888);
    *err = "jpeg_enc_open failed";
    return false;
  }
  // JPEG output cannot exceed the raw RGB888 size in practice; size the
  // sink to that as a safe upper bound.
  uint8_t* sink =
      (uint8_t*)heap_caps_malloc(rgb888_size, MALLOC_CAP_SPIRAM);
  if (!sink) {
    jpeg_enc_close(enc);
    heap_caps_free(rgb888);
    *err = "jpeg sink PSRAM alloc failed";
    return false;
  }
  int written = 0;
  jpeg_error_t rc =
      jpeg_enc_process(enc, rgb888, (int)rgb888_size,
                       sink, (int)rgb888_size, &written);
  jpeg_enc_close(enc);
  heap_caps_free(rgb888);
  if (rc != JPEG_ERR_OK || written <= 0) {
    heap_caps_free(sink);
    *err = std::string("jpeg_enc_process rc=") + std::to_string((int)rc);
    return false;
  }
  out->assign(sink, sink + written);
  heap_caps_free(sink);
  return true;
}

// Single-flight guard. Each screenshot takes an lv_snapshot (1.2 MB
// PSRAM alloc) + JPEG encode on the event_loop task — hundreds of ms
// each. The designer's live-mirror mode polls /screenshot in a tight
// loop; if several requests arrive faster than event_loop can serve
// them they stack up as deferred tasks and starve everything else on
// event_loop (layout apply, the n2k heartbeat, the notifications
// drain). A push during that window times out -> "device proxy 504".
//
// Serve at most one screenshot at a time. A second concurrent request
// gets 429 immediately instead of queuing more event_loop work; the
// mirror loop just retries on its next iteration.
static std::atomic<bool> g_screenshot_busy{false};

esp_err_t screenshot_get(httpd_req_t* req) {
  bool expected = false;
  if (!g_screenshot_busy.compare_exchange_strong(expected, true)) {
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"screenshot in flight\"}");
    return ESP_OK;
  }
  // RAII-style release: any return path below clears the flag.
  struct BusyGuard {
    ~BusyGuard() { g_screenshot_busy.store(false); }
  } busy_guard;

  // Pick format from `?fmt=`. Default jpeg — that's what the designer's
  // live mirror polls. Anything not "bmp" falls through to jpeg.
  bool want_bmp = false;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < 64) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char fmt[16] = {};
      if (httpd_query_key_value(query, "fmt", fmt, sizeof(fmt)) == ESP_OK) {
        if (strcmp(fmt, "bmp") == 0) want_bmp = true;
      }
    }
  }

  auto result = std::make_shared<ScreenshotResult>();
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "semaphore alloc failed");
    return ESP_FAIL;
  }
  cockpit_hal::ui::post([result, done]() {
    take_screenshot(result.get());
    xSemaphoreGive(done);
  });
  if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) {
    vSemaphoreDelete(done);
    httpd_resp_set_status(req, "504 Gateway Timeout");
    httpd_resp_sendstr(req, "snapshot timeout");
    return ESP_OK;
  }
  vSemaphoreDelete(done);

  if (!result->ok) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, result->err.c_str());
    return ESP_OK;
  }

  const uint32_t w = result->w, h = result->h;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  if (want_bmp) {
    const uint32_t row_bytes = w * 2;
    uint8_t header[70];
    bmp_header_rgb565(header, w, h);
    httpd_resp_set_type(req, "image/bmp");
    if (httpd_resp_send_chunk(req, (const char*)header, sizeof(header)) !=
        ESP_OK) {
      return ESP_FAIL;
    }
    // BMP rows are bottom-up; LVGL gives top-down. Send last row first.
    for (int32_t y = (int32_t)h - 1; y >= 0; --y) {
      const char* row =
          reinterpret_cast<const char*>(&result->pixels[y * row_bytes]);
      if (httpd_resp_send_chunk(req, row, row_bytes) != ESP_OK) {
        return ESP_FAIL;
      }
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    ESP_LOGI(TAG, "screenshot[bmp] %ux%u (%u bytes)", (unsigned)w,
             (unsigned)h, (unsigned)(70 + row_bytes * h));
    return ESP_OK;
  }

  // JPEG path.
  std::vector<uint8_t> jpeg_out;
  std::string err;
  const int64_t t0 = esp_timer_get_time();
  if (!encode_rgb565_to_jpeg(*result, &jpeg_out, &err)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, err.c_str());
    return ESP_OK;
  }
  const int64_t t1 = esp_timer_get_time();
  httpd_resp_set_type(req, "image/jpeg");
  if (httpd_resp_send(req, (const char*)jpeg_out.data(),
                      jpeg_out.size()) != ESP_OK) {
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "screenshot[jpeg] %ux%u (%u bytes, encode %lld ms)",
           (unsigned)w, (unsigned)h, (unsigned)jpeg_out.size(),
           (long long)((t1 - t0) / 1000));
  return ESP_OK;
}

// The widget catalog is constant per-firmware-build — kept as a raw
// JSON literal so we don't allocate dozens of JsonObjects per /hello
// call. ArduinoJson's `serialized()` splices it in verbatim.
constexpr const char* kWidgetCatalogJson =
    "{"
      "\"label\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"display\",\"show_description\",\"bg_color\",\"fg_color\"]},"
      "\"value\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"display\",\"bg_color\",\"fg_color\"]},"
      "\"toggle\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"bg_color\",\"fg_color\"]},"
      "\"arc\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"display\",\"min\",\"max\",\"start_angle\",\"end_angle\",\"ticks\",\"tick_labels\",\"bands\",\"bg_color\",\"fg_color\"]},"
      "\"bar\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"display\",\"min\",\"max\",\"vertical\",\"bg_color\",\"fg_color\"]},"
      "\"bargroup\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bars\",\"bg_color\",\"fg_color\"]},"
      "\"button\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bind\",\"press_value\",\"release_value\",\"hold_ms\",\"bg_color\",\"fg_color\"]},"
      "\"notifications\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"max_rows\",\"row_height\",\"columns\",\"row_color_field\",\"include_cleared\",\"bg_color\",\"fg_color\"]},"
      "\"anchor\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"display\",\"fg_color\"]},"
      "\"anchor_track\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"display\",\"fg_color\"]},"
      "\"voice\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bg_color\",\"fg_color\"]},"
      // The catalogue is what a designer should OFFER, not everything the
      // device accepts: it feeds the "Add widget" palette directly. Listing
      // the mute_speaker/mute_mic aliases here would put four buttons on it
      // for two controls and invite new layouts to use the deprecated name.
      // Acceptance lives in widget_factory's dispatch; JLP-PROTOCOL documents
      // the aliases for anyone reading an old layout.
      "\"speaker\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bg_color\",\"fg_color\"]},"
      "\"mic\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bg_color\",\"fg_color\"]},"
      "\"volume\":{\"fields\":[\"x\",\"y\",\"w\",\"h\",\"label\",\"bg_color\",\"fg_color\"]}"
    "}";

static esp_err_t screen_post(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  if (req->content_len <= 0 || req->content_len > 256) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"body must be 1..256 bytes\"}");
    return ESP_OK;
  }

  std::string body(req->content_len, '\0');
  int total = 0;
  while (total < req->content_len) {
    int n = httpd_req_recv(req, &body[total], req->content_len - total);
    if (n <= 0) {
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"recv failed\"}");
      return ESP_FAIL;
    }
    total += n;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"invalid json\"}");
    return ESP_OK;
  }
  const char* id = doc["id"];
  if (!id || !*id) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"missing id\"}");
    return ESP_OK;
  }

  // select_screen touches LVGL, so it runs on the UI task. Report the
  // resulting active screen from the same hop: reading it afterwards
  // from here could observe a tap that landed in between.
  std::string want(id);
  auto found = std::make_shared<bool>(false);
  auto active = std::make_shared<std::string>();
  auto known = std::make_shared<std::vector<std::string>>();
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"semaphore\"}");
    return ESP_FAIL;
  }
  cockpit_hal::ui::post([want, found, active, known, done]() {
    *found = layout_manager().select_screen(want);
    *active = layout_manager().active_screen();
    *known = layout_manager().screen_ids();
    xSemaphoreGive(done);
  });
  if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) {
    vSemaphoreDelete(done);
    httpd_resp_set_status(req, "504 Gateway Timeout");
    httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"select timed out\"}");
    return ESP_OK;
  }
  vSemaphoreDelete(done);

  if (!*found) {
    JsonDocument out;
    out["ok"] = false;
    // A single-screen layout has no tab strip, so nothing is selectable;
    // say that rather than reporting an unknown id.
    out["err"] = known->empty() ? "layout is single-screen"
                                : "no screen with that id";
    JsonArray arr = out["screens"].to<JsonArray>();
    for (const auto& s : *known) arr.add(s);
    std::string s;
    serializeJson(out, s);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, s.c_str());
    return ESP_OK;
  }

  JsonDocument out;
  out["ok"] = true;
  out["active"] = *active;
  std::string s;
  serializeJson(out, s);
  httpd_resp_sendstr(req, s.c_str());
  return ESP_OK;
}

esp_err_t hello_get(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  const std::string& name = layout_manager().active_name();
  ApplySource src = layout_manager().active_source();
  const char* src_str = "boot";
  switch (src) {
    case ApplySource::BootStore:    src_str = "littlefs";    break;
    case ApplySource::BootDefault:  src_str = "default";     break;
    case ApplySource::BootFetched:  src_str = "applicationData"; break;
    case ApplySource::PostLayout:   src_str = "post";        break;
    case ApplySource::Boot: default: src_str = "boot";       break;
  }
  // ArduinoJson handles escaping for user-controlled `name` (used as
  // both the `name` field and `active_layout_name`); the static
  // widget catalog goes in via serialized() so we don't pay the
  // parse + reserialise cost on every call.
  JsonDocument resp;
  resp["schema"] = 1;
  resp["name"] = name;
  {
    // the espOS hostname (wifi.hostname, default espos-<mac>) and the
    // running app version (version.txt) — no compiled-in identity
    char host[33] = "";
    espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_HOSTNAME, host, sizeof(host), nullptr);
    if (!host[0]) {
      espos_wifi_status_t w;
      if (espos_wifi_get_status(&w) == ESP_OK) snprintf(host, sizeof(host), "%s", w.hostname);
    }
    resp["hostname"] = host[0] ? host : "cockpit";
    // The designer gates pushes on this string and parses a trailing
    // M.m.p out of it (schema.ts firmwareMeets), so anything after the
    // numbers — a "-dev" suffix, a git-describe hash — reads as "version
    // unknown" and it refuses to push. Keep the numeric version last.
    static char fw[64];
    snprintf(fw, sizeof(fw), "p4-cockpit-jlp-%s", esp_app_get_description()->version);
    resp["firmware"] = fw;
  }
  resp["store"] = jlp::store_boot_report();  // persistence backend status

  // Which screen the helm is on, and what else it could be switched to.
  // Read on the UI task: the switcher's vectors are owned there and a
  // tab tap or a layout swap can reallocate them under us. Best-effort —
  // /hello must answer even if the UI task is briefly backed up.
  {
    auto active = std::make_shared<std::string>();
    auto ids = std::make_shared<std::vector<std::string>>();
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done) {
      cockpit_hal::ui::post([active, ids, done]() {
        *active = layout_manager().active_screen();
        *ids = layout_manager().screen_ids();
        xSemaphoreGive(done);
      });
      if (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) == pdTRUE) {
        resp["active_screen"] = *active;
        JsonArray arr = resp["screens"].to<JsonArray>();
        for (const auto& s : *ids) arr.add(s);
      }
      vSemaphoreDelete(done);
    }
  }
  // Persisted panel-local audio state, so it is checkable without a serial
  // console. NOT "audio" — that key already carries the codec-ready string
  // below, and reusing it silently replaced this object with that string.
  JsonObject audio_state = resp["audio_state"].to<JsonObject>();
  audio_state["speaker_muted"] = jlp::voice().speaker_muted();
  audio_state["mic_muted"] = jlp::voice().mic_muted();
  audio_state["volume"] = jlp::voice().volume();
  JsonObject display = resp["display"].to<JsonObject>();
  // Report the live panel geometry from the active board HAL so the
  // designer maps its canvas to the real resolution (720x720 on the
  // 4B, 1024x600 on the 7B). Fall back to the 7B size only if the
  // driver isn't wired yet, which can't happen once /hello serves.
  if (auto* d = cockpit_hal::ui::display()) {
    display["w"] = d->width();
    display["h"] = d->height();
  } else {
    // Shouldn't be reachable once /hello serves; log it so a future
    // boot-order regression is visible instead of silently handing the
    // designer the wrong board's resolution.
    ESP_LOGW(TAG, "get_display() null in /hello; falling back to 1024x600");
    display["w"] = 1024;
    display["h"] = 600;
  }
  display["idle_timeout"] = true;
  display["idle_dim_pct"] = true;
  resp["widgets"] = serialized(kWidgetCatalogJson);
  resp["screenshot"]["formats"] = serialized("[\"jpeg\",\"bmp\"]");
  resp["active_layout_name"] = name;
  resp["layout_source"] = src_str;
  resp["audio"] = chime().audio_ready() ? "ready" : "unavailable";
  if (g_wyoming && g_wyoming->running()) {
    resp["voice"] = g_wyoming->client_connected() ? "connected" : "listening";
    if (g_wyoming->wake_enabled()) {
      // Wake diagnostics. On-device: `word` names the loaded WakeNet model,
      // `detections` climbs on each wake; `capturing` = the engine owns the
      // mic. `mic_muted` gates it. (Network path adds `chunks`/`peak`.)
      JsonObject w = resp["wake"].to<JsonObject>();
      w["on_device"] = g_wyoming->wake_on_device();
      w["word"] = g_wyoming->wake_word();
      w["connected"] = g_wyoming->wake_connected();
      w["capturing"] = g_wyoming->wake_capturing();
      w["mic_muted"] = voice().mic_muted();
      w["chunks"] = g_wyoming->wake_chunks();
      w["detections"] = g_wyoming->wake_detections();
      w["peak"] = g_wyoming->wake_peak();
    }
  } else {
    resp["voice"] = "unavailable";
  }
  std::string out;
  serializeJson(resp, out);
  httpd_resp_sendstr(req, out.c_str());
  return ESP_OK;
}

}  // namespace

void http_api_start(uint16_t port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.stack_size = 8192;
  config.ctrl_port = 32768 + port;  // unique per httpd instance
  config.recv_wait_timeout = 120;
  config.send_wait_timeout = 30;
  config.lru_purge_enable = true;
  // Keep ahead of the registrations below (9 today) — httpd aborts with
  // ESP_ERR_HTTPD_HANDLERS_FULL at boot the moment this is exceeded.
  config.max_uri_handlers = 12;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "failed to start jlp api on port %u", port);
    return;
  }

  httpd_uri_t layout_uri = {
      .uri = "/layout",
      .method = HTTP_POST,
      .handler = layout_post,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &layout_uri);

  httpd_uri_t screen_uri = {
      .uri = "/screen",
      .method = HTTP_POST,
      .handler = screen_post,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &screen_uri);

  httpd_uri_t hz_uri = {
      .uri = "/healthz",
      .method = HTTP_GET,
      .handler = healthz_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &hz_uri);

  httpd_uri_t hello_uri = {
      .uri = "/hello",
      .method = HTTP_GET,
      .handler = hello_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &hello_uri);

  httpd_uri_t screenshot_uri = {
      .uri = "/screenshot",
      .method = HTTP_GET,
      .handler = screenshot_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &screenshot_uri);

  httpd_uri_t beep_uri = {
      .uri = "/beep",
      .method = HTTP_GET,
      .handler = beep_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &beep_uri);

  httpd_uri_t mic_probe_uri = {
      .uri = "/mic_probe",
      .method = HTTP_GET,
      .handler = mic_probe_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &mic_probe_uri);

  httpd_uri_t mic_probe4_uri = {
      .uri = "/mic_probe4",
      .method = HTTP_GET,
      .handler = mic_probe4_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &mic_probe4_uri);

  httpd_uri_t mic_gain_uri = {
      .uri = "/mic_gain",
      .method = HTTP_GET,
      .handler = mic_gain_get,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &mic_gain_uri);

  ESP_LOGI(
      TAG,
      "jlp api on :%u (POST /layout, GET /hello, /healthz, /screenshot)",
      port);
}

}  // namespace jlp
