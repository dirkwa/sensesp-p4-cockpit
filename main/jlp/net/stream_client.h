// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace jlp {

// One decoded MJPEG frame. `px` is RGB565; the hardware decoder pads the
// height to a multiple of 16, so only the top `h` rows are image — read
// them with a stride of `w * 2` and ignore the rest. The buffer stays
// untouched until the NEXT frame callback: a renderer may hand it to LVGL
// and block in the callback until the frame is on glass. The 1-byte ACK
// to the server goes out only after the callback returns, which is what
// paces the whole stream to the panel's real throughput.
struct StreamFrame {
  const uint8_t* px;
  uint32_t w;
  uint32_t h;
  uint32_t h_padded;
  size_t bytes;
};

using StreamFrameCb = std::function<void(const StreamFrame&)>;

struct StreamStats {
  bool running;
  bool connected;
  uint32_t frames_received;
  uint32_t frames_decoded;
  uint32_t decode_errors;
  uint32_t protocol_errors;
  uint32_t reconnects;
  uint64_t bytes_received;
  int64_t last_frame_us;
  uint32_t interval_samples;
  uint32_t interval_p50_ms;
  uint32_t interval_p99_ms;
  uint32_t interval_max_ms;
};

// MJPEG-over-TCP puller: [u32 BE length][baseline JPEG] framing from the
// signalk-esp32-stream server, decoded by the P4's hardware JPEG engine.
// The client ACKs each frame with a single byte and the server sends only
// the latest frame per ACK, so at most one frame is ever in flight — the
// esp-hosted SDIO link wedges under sustained unpaced inbound TCP
// (esp-hosted-mcu#184), and this bounded request/response shape is the
// profile it demonstrably survives.
//
// Singleton by design: the P4 has one JPEG decode engine and JLP shows one
// screen at a time. `cb` runs on the stream task and must never touch
// LVGL directly; pass nullptr to decode-and-discard (link soak).
bool stream_client_start(const char* host, uint16_t port, StreamFrameCb cb);

// Non-blocking: requests the stream task to exit; the task frees its own
// buffers on the way out (safe to call from the UI task). A start() during
// the brief wind-down returns false — retry on a timer.
void stream_client_stop();

StreamStats stream_client_stats();

}  // namespace jlp
