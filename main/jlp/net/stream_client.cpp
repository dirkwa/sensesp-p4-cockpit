// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#include "stream_client.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "jlp.stream";

namespace jlp {
namespace {

constexpr size_t kMaxFrameBytes = 128 * 1024;
constexpr int kMaxBackoffMs = 10000;
constexpr int kRecvTimeoutS = 10;
constexpr size_t kIntervalRing = 256;

struct State {
  std::string host;
  uint16_t port = 0;
  // Frame geometry = the panel resolution (passed in by the caller so this
  // module stays display-agnostic; the 4B panel is 720x720, not 1024x600).
  // The HW decoder emits heights padded to a multiple of 16.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t height_padded = 0;
  size_t decoded_bytes = 0;
  StreamFrameCb cb;

  std::atomic<bool> running{true};
  // When set, the ACK for the current frame is withheld: the server sends
  // only the newest frame per ACK, so no ACK means no further inbound data
  // and the shared radio is free for the voice mic uplink (a heavy stream
  // downlink otherwise starves it — captures come back as empty
  // transcripts). Set from the UI watch tick while the satellite is
  // listening/speaking; the frame already in hand is still decoded/shown.
  std::atomic<bool> paused{false};
  std::atomic<int> sock{-1};
  std::atomic<uint32_t> peer_be{0};  // resolved server IPv4, network order

  jpeg_decoder_handle_t decoder = nullptr;
  uint8_t* rx_buf = nullptr;
  uint8_t* out_buf[2] = {nullptr, nullptr};
  size_t out_buf_size = 0;
  int out_idx = 0;

  // Counters are read racily from the httpd/UI tasks; the interval ring is
  // copied under the spinlock for the percentile computation.
  std::atomic<bool> connected{false};
  std::atomic<uint32_t> frames_received{0};
  std::atomic<uint32_t> frames_decoded{0};
  std::atomic<uint32_t> decode_errors{0};
  std::atomic<uint32_t> protocol_errors{0};
  std::atomic<uint32_t> reconnects{0};
  std::atomic<uint64_t> bytes_received{0};
  std::atomic<int64_t> last_frame_us{0};
  portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
  uint32_t intervals_ms[kIntervalRing] = {};
  uint32_t interval_count = 0;
};

// The task owns its State and frees it on exit; stop() only requests.
// g_mux serialises the g_state handover between start/stop/stats (other
// tasks) and the task's own exit path.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
State* g_state = nullptr;

void free_state(State* st) {
  if (st->decoder) jpeg_del_decoder_engine(st->decoder);
  free(st->rx_buf);
  for (auto& buf : st->out_buf) free(buf);
  delete st;
}

int connect_to_server(State& st) {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", st.port);

  struct addrinfo* result = nullptr;
  int err = getaddrinfo(st.host.c_str(), port_str, &hints, &result);
  if (err != 0 || !result) {
    ESP_LOGW(TAG, "DNS failed for %s: %d", st.host.c_str(), err);
    return -1;
  }

  int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (sock < 0) {
    freeaddrinfo(result);
    return -1;
  }

  struct timeval tv = {.tv_sec = kRecvTimeoutS, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  // The ACK send must be bounded too: stop() is flag-only, so a peer that
  // stops reading (hung server, half-open link) would otherwise pin the
  // task in send() forever and the singleton could never restart.
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  err = connect(sock, result->ai_addr, result->ai_addrlen);
  if (err == 0 && result->ai_family == AF_INET) {
    st.peer_be.store(((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr);
  }
  freeaddrinfo(result);
  if (err != 0) {
    close(sock);
    return -1;
  }
  return sock;
}

bool read_exact(State& st, int sock, uint8_t* buf, size_t len) {
  size_t got = 0;
  while (got < len && st.running.load()) {
    ssize_t n = recv(sock, buf + got, len - got, 0);
    if (n <= 0) return false;
    got += n;
  }
  return got == len;
}

void record_interval(State& st, uint32_t ms) {
  taskENTER_CRITICAL(&st.ring_mux);
  st.intervals_ms[st.interval_count % kIntervalRing] = ms;
  st.interval_count++;
  taskEXIT_CRITICAL(&st.ring_mux);
}

// Send the single-byte ACK for the current frame, first waiting out any
// voice pause. Every ACK path goes through here — including the rejected
// wrong-size frame — so a misconfigured capture can't keep releasing
// frames and starving the voice uplink. Returns false if the connection
// should end (stopped, or send failed).
bool ack_frame(State& st, int sock) {
  while (st.paused.load() && st.running.load()) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!st.running.load()) return false;
  uint8_t ack = 1;
  return send(sock, &ack, 1, 0) == 1;
}

void run_connection(State& st, int sock) {
  int64_t prev_frame_us = 0;
  bool wrong_size_warned = false;

  while (st.running.load()) {
    uint8_t len_buf[4];
    int64_t t0 = esp_timer_get_time();
    if (!read_exact(st, sock, len_buf, 4)) return;

    uint32_t frame_len = ((uint32_t)len_buf[0] << 24) | ((uint32_t)len_buf[1] << 16) |
                         ((uint32_t)len_buf[2] << 8) | (uint32_t)len_buf[3];
    if (frame_len == 0 || frame_len > kMaxFrameBytes) {
      // A length outside the contract means framing is lost; there are no
      // resync markers in this protocol, so reconnect for a clean start.
      st.protocol_errors.fetch_add(1);
      ESP_LOGW(TAG, "bad frame length %u, reconnecting", (unsigned)frame_len);
      return;
    }

    int64_t t1 = esp_timer_get_time();
    if (!read_exact(st, sock, st.rx_buf, frame_len)) return;
    int64_t t2 = esp_timer_get_time();

    st.frames_received.fetch_add(1);
    st.bytes_received.fetch_add(frame_len + 4);

    // A frame that isn't panel-sized would "decode fine" into garbage rows
    // (and break the 1:1 touch mapping); surface a misconfigured capture as
    // an error instead. Header parse only — no hardware involved.
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(st.rx_buf, frame_len, &info) != ESP_OK ||
        info.width != st.width || info.height != st.height) {
      st.decode_errors.fetch_add(1);
      if (!wrong_size_warned) {
        wrong_size_warned = true;
        ESP_LOGW(TAG, "frame is %ux%u, need %ux%u — fix the capture resolution",
                 (unsigned)info.width, (unsigned)info.height, (unsigned)st.width,
                 (unsigned)st.height);
      }
      // Still an ACK: the server cannot tell a rejected frame from a shown
      // one, and withholding it would stall the pacing loop on a
      // misconfigured capture instead of surfacing the error counter. Via
      // ack_frame so a wrong-size flood also yields to voice.
      if (!ack_frame(st, sock)) return;
      continue;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint8_t* out = st.out_buf[st.out_idx];
    uint32_t out_size = 0;
    esp_err_t ret = jpeg_decoder_process(st.decoder, &decode_cfg, st.rx_buf, frame_len,
                                         out, st.out_buf_size, &out_size);
    int64_t t3 = esp_timer_get_time();

    if (ret == ESP_OK) {
      st.frames_decoded.fetch_add(1);
      if (st.cb) {
        StreamFrame frame = {
            .px = out,
            .w = st.width,
            .h = st.height,
            .h_padded = st.height_padded,
            .bytes = out_size,
        };
        st.cb(frame);
      }
      // Alternate output buffers so the frame just handed out stays valid
      // until the next callback (a renderer may still be showing it).
      st.out_idx ^= 1;

      int64_t now = esp_timer_get_time();
      if (prev_frame_us != 0) {
        record_interval(st, (uint32_t)((now - prev_frame_us) / 1000));
      }
      prev_frame_us = now;
      st.last_frame_us.store(now);
    } else {
      // The server can't know a frame failed to decode; ACK anyway so one
      // bad frame never stalls the pacing loop.
      st.decode_errors.fetch_add(1);
      ESP_LOGW(TAG, "decode failed: %s (%u bytes)", esp_err_to_name(ret),
               (unsigned)frame_len);
    }

    // Hold the ACK while voice has the radio (ack_frame waits out the
    // pause): no ACK means the server sends no further frame, so the mic
    // uplink runs uncontended. The frame just decoded stays on screen; the
    // picture freezes for the ~few seconds of an utterance, then resumes.
    if (!ack_frame(st, sock)) return;

    uint32_t n = st.frames_decoded.load();
    if (n != 0 && (n & 0x3F) == 0) {
      ESP_LOGI(TAG, "frame %u: %u bytes, hdr=%lldms recv=%lldms dec=%lldms",
               (unsigned)n, (unsigned)frame_len, (t1 - t0) / 1000, (t2 - t1) / 1000,
               (t3 - t2) / 1000);
    }
  }
}

void stream_task(void* arg) {
  auto* st = static_cast<State*>(arg);
  int backoff_ms = 1000;
  bool had_session = false;

  while (st->running.load()) {
    int sock = connect_to_server(*st);
    if (sock < 0) {
      for (int waited = 0; waited < backoff_ms && st->running.load(); waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }

    if (had_session) st->reconnects.fetch_add(1);
    had_session = true;
    st->sock.store(sock);
    st->connected.store(true);
    ESP_LOGI(TAG, "connected to %s:%u", st->host.c_str(), st->port);

    const uint32_t frames_before = st->frames_decoded.load();
    run_connection(*st, sock);

    st->connected.store(false);
    st->sock.store(-1);
    close(sock);
    ESP_LOGI(TAG, "disconnected (%u frames total)", (unsigned)st->frames_decoded.load());

    // Pace session drops too: a peer that accepts and then immediately
    // fails (close, bad framing every time) must not turn into a tight
    // connect/close loop through the hosted link. Only a session that
    // actually delivered frames resets the backoff.
    if (st->frames_decoded.load() != frames_before) backoff_ms = 1000;
    else backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
    for (int waited = 0; waited < backoff_ms && st->running.load(); waited += 100) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  taskENTER_CRITICAL(&g_mux);
  if (g_state == st) g_state = nullptr;
  taskEXIT_CRITICAL(&g_mux);
  ESP_LOGI(TAG, "stopped");
  free_state(st);
  vTaskDelete(nullptr);
}

}  // namespace

bool stream_client_start(const char* host, uint16_t port, uint32_t width,
                         uint32_t height, StreamFrameCb cb) {
  if (width == 0 || height == 0) return false;
  taskENTER_CRITICAL(&g_mux);
  bool busy = g_state != nullptr;
  taskEXIT_CRITICAL(&g_mux);
  if (busy) return false;  // running, or a stopped task still winding down

  auto* st = new State();
  st->host = host;
  st->port = port;
  st->width = width;
  st->height = height;
  st->height_padded = (height + 15) & ~15u;
  st->decoded_bytes = (size_t)width * st->height_padded * 2;
  st->cb = std::move(cb);

  jpeg_decode_engine_cfg_t engine_cfg = {
      .intr_priority = 0,
      .timeout_ms = 100,
      .flags = {},
  };
  esp_err_t err = jpeg_new_decoder_engine(&engine_cfg, &st->decoder);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "decoder engine init failed: %s", esp_err_to_name(err));
    delete st;
    return false;
  }

  // The driver requires cache-line-aligned buffers from its own allocator
  // (both land in PSRAM); a plain heap_caps_malloc trips its alignment
  // assert. Do not add MALLOC_CAP anything here.
  jpeg_decode_memory_alloc_cfg_t rx_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
  jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t got = 0;
  st->rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(kMaxFrameBytes, &rx_cfg, &got);
  st->out_buf[0] =
      (uint8_t*)jpeg_alloc_decoder_mem(st->decoded_bytes, &out_cfg, &st->out_buf_size);
  size_t out1_size = 0;
  st->out_buf[1] =
      (uint8_t*)jpeg_alloc_decoder_mem(st->decoded_bytes, &out_cfg, &out1_size);
  if (!st->rx_buf || !st->out_buf[0] || !st->out_buf[1]) {
    ESP_LOGE(TAG, "buffer allocation failed");
    free_state(st);
    return false;
  }
  // Both buffers get the same value passed to jpeg_decoder_process, so the
  // decode limit must be the SMALLER of the two allocations.
  st->out_buf_size = std::min(st->out_buf_size, out1_size);

  taskENTER_CRITICAL(&g_mux);
  busy = g_state != nullptr;
  if (!busy) g_state = st;
  taskEXIT_CRITICAL(&g_mux);
  if (busy) {  // lost a start race
    free_state(st);
    return false;
  }

  // Core 0: the ui task owns core 1, and socket I/O + the blocking decode
  // call must never compete with LVGL's render slice. Priority 2 stays
  // strictly below the espos_voice tasks (3) and wake tasks (5), so
  // ACK-paced stream work always yields the core to voice work; equal
  // priority would round-robin whole ticks away from the mic's small
  // deadline-bound chunks.
  if (xTaskCreatePinnedToCore(stream_task, "jlp_stream", 8192, st, 2, nullptr, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    taskENTER_CRITICAL(&g_mux);
    if (g_state == st) g_state = nullptr;
    taskEXIT_CRITICAL(&g_mux);
    free_state(st);
    return false;
  }

  ESP_LOGI(TAG, "started, target %s:%u", host, port);
  return true;
}

void stream_client_set_paused(bool paused) {
  taskENTER_CRITICAL(&g_mux);
  State* st = g_state;
  if (st) st->paused.store(paused);
  taskEXIT_CRITICAL(&g_mux);
}

void stream_client_stop() {
  taskENTER_CRITICAL(&g_mux);
  State* st = g_state;
  if (st) st->running.store(false);
  taskEXIT_CRITICAL(&g_mux);
  if (!st) return;
  // Flag only — NEVER touch the socket from the caller's task. lwip
  // sockets are not full-duplex safe here, and a shutdown() from the UI
  // task while the stream task sits in recv()/close() on the same fd
  // deadlocks both inside the sock lock (observed live: first tab-away
  // froze the UI until the ui_wdt rebooted the panel). The task checks
  // `running` around every socket op; while frames are flowing it exits
  // within one frame, and a dead server is bounded by the 10 s recv
  // timeout. The 1.x client used the same flag-only stop for this reason.
  ESP_LOGI(TAG, "stop requested");
}

bool stream_client_stop_diagnostic() {
  taskENTER_CRITICAL(&g_mux);
  State* st = g_state;
  const bool diag = st && !st->cb;
  if (diag) st->running.store(false);
  taskEXIT_CRITICAL(&g_mux);
  if (!st) return true;  // nothing running counts as stopped
  if (diag) ESP_LOGI(TAG, "stop requested (diagnostic)");
  return diag;
}

bool stream_client_peer(uint32_t* addr_be) {
  taskENTER_CRITICAL(&g_mux);
  State* st = g_state;
  const uint32_t a = (st && st->connected.load()) ? st->peer_be.load() : 0;
  taskEXIT_CRITICAL(&g_mux);
  if (a == 0) return false;
  *addr_be = a;
  return true;
}

StreamStats stream_client_stats() {
  StreamStats out = {};
  taskENTER_CRITICAL(&g_mux);
  State* st = g_state;
  if (!st) {
    taskEXIT_CRITICAL(&g_mux);
    return out;
  }

  out.running = st->running.load();
  out.connected = st->connected.load();
  out.frames_received = st->frames_received.load();
  out.frames_decoded = st->frames_decoded.load();
  out.decode_errors = st->decode_errors.load();
  out.protocol_errors = st->protocol_errors.load();
  out.reconnects = st->reconnects.load();
  out.bytes_received = st->bytes_received.load();
  out.last_frame_us = st->last_frame_us.load();
  taskEXIT_CRITICAL(&g_mux);

  // The ring lives inside State, which the task frees only after clearing
  // g_state under g_mux — but we already dropped g_mux. Re-take and
  // re-check so the copy can't race the free.
  uint32_t ring[kIntervalRing];
  uint32_t count = 0;
  taskENTER_CRITICAL(&g_mux);
  if (g_state == st) {
    taskENTER_CRITICAL(&st->ring_mux);
    count = std::min<uint32_t>(st->interval_count, kIntervalRing);
    memcpy(ring, st->intervals_ms, count * sizeof(uint32_t));
    taskEXIT_CRITICAL(&st->ring_mux);
  }
  taskEXIT_CRITICAL(&g_mux);

  if (count > 0) {
    std::sort(ring, ring + count);
    out.interval_samples = count;
    out.interval_p50_ms = ring[count / 2];
    out.interval_p99_ms = ring[(count * 99) / 100];
    out.interval_max_ms = ring[count - 1];
  }
  return out;
}

}  // namespace jlp
