/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos-p4-cockpit — application entry.
 *
 * espOS provides WiFi, config store + web UI, SignalK discovery/token/
 * stream (in and out), logs, core dump and signed OTA. This file wires the
 * panel on top: display + LVGL (cockpit_hal), the JSON Layout Player (jlp/)
 * with its layout push API on :8081, and the status/alert overlays.
 *
 * Phase 1 of the port: display, layouts, SK values/meta/notifications,
 * anchor PUTs. Audio/voice, the N2K gateway and the BLE gateway follow in
 * later phases (their code compiles against inert stand-ins for now).
 */
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "cockpit_hal/waveshare_audio.h"
#include "espos_n2k/candump_tcp_server.h"
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/twai_transmitter.h"
#include "cockpit_hal/ui.h"
#include "cockpit_hal/waveshare_7b.h"
#include "espos_voice/wyoming_satellite.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd.h"
#include "espos_httpd_sse.h"
#include "espos_log.h"
#include "espos_ota.h"
#include "espos_sk.h"
#include "espos_wifi.h"

#include "jlp/alert_overlay.h"
#include "jlp/audio/chime.h"
#include "jlp/audio/voice_control.h"
#include "jlp/default_layout.h"
#include "jlp/idle_dimmer.h"
#include "jlp/layout/layout_manager.h"
#include "jlp/layout/store.h"
#include "jlp/net/http_api.h"
#include "jlp/net/layout_fetch.h"
#include "jlp/net/mdns_announce.h"
#include "jlp/net/sk_put.h"
#include "jlp/net/wake_discover.h"
#include "jlp/net/sk_server.h"
#include "jlp/net/zone_fetch.h"
#include "jlp/notifications_registry.h"
#include "jlp/status_overlay.h"
#include "jlp/subject_registry.h"
#include "jlp/sun_state.h"
#include "jlp/mic_overlay.h"
#include "jlp/wake_overlay.h"
#include "jlp/zone_registry.h"

static const char* TAG = "cockpit";

namespace {

// ---- SignalK stream state → overlay ------------------------------------
// The overlay reads the stream state at 1 Hz (below); the boot-time
// layout fetch and zone seed fire once, on the first connect.
bool s_boot_fetch_done = false;
bool s_last_connected = false;
espos_n2k::TwaiReceiver* s_n2k_rx = nullptr;
espos_n2k::CandumpTcpServer* s_n2k_server = nullptr;
// Set once the satellite exists, so poll_sk_state() can hand it to wake
// discovery on the first SignalK connect.
espos_voice::WyomingSatellite* s_wyoming_sat = nullptr;
// True when cockpit.wake_host names a wake server explicitly; discovery then
// stays out of the way.
bool s_wake_configured = false;

void poll_sk_state() {
  espos_sk_ws_status_t ws;
  bool connected = espos_sk_ws_get_status(&ws) == ESP_OK && ws.connected;
  if (connected && !s_last_connected) {
    jlp::overlay().set_sk("ok");
    jlp::overlay().hide_sk_lost();
    jlp::SkServer s = jlp::sk_server();
    jlp::overlay().set_sk_server(s.host.c_str(), s.port);
    if (!s_boot_fetch_done) {
      s_boot_fetch_done = true;
      // Seed quiet paths (SK only sends deltas on change) and pull the
      // boot layout from applicationData, both against the server espOS
      // selected.
      const auto& boot_paths = jlp::layout_manager().known_paths();
      if (!boot_paths.empty()) {
        std::vector<std::string> v(boot_paths.begin(), boot_paths.end());
        jlp::zone_fetch_for_paths(s.host, s.port, v);
      }
      jlp::layout_fetch_async_apply(s.host, s.port);
    }
    // Waits for the SignalK connection because boot has no route yet. An
    // explicit cockpit.wake_host wins over whatever the server advertises.
    if (!s_wake_configured) jlp::wake_discover_start(s_wyoming_sat);
  } else if (!connected && s_last_connected) {
    jlp::overlay().set_sk("down");
    jlp::overlay().show_sk_lost();
  } else if (!connected) {
    espos_sk_server_t srv;
    bool have = espos_sk_get_server(&srv) == ESP_OK;
    jlp::overlay().set_sk(!have ? "no server" : ws.enabled ? "connecting" : "off");
  }
  s_last_connected = connected;
}

void poll_status_line() {
  espos_wifi_status_t w;
  if (espos_wifi_get_status(&w) == ESP_OK && w.sm.state == ESPOS_WIFI_ST_CONNECTED) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %ddBm", w.sm.link.ssid, (int)w.rssi);
    jlp::overlay().set_wifi(buf);
  } else {
    jlp::overlay().set_wifi("down");
  }
  int64_t rx_idle = (s_n2k_rx && s_n2k_rx->ever_received()) ? s_n2k_rx->seconds_since_last_rx() : -1;
  jlp::overlay().set_n2k(rx_idle, s_n2k_server ? s_n2k_server->connected_clients() : 0);
  jlp::overlay().set_uptime_heap((uint32_t)(esp_timer_get_time() / 1000000), esp_get_free_heap_size());
}

// ---- health watchdog (every 30 s, on the UI thread) ---------------------
// The esp_hosted SDIO link to the C6 can wedge: the transport spams
// "H_SDIO_DRV: task still writing Rx data to queue!", every RPC to the
// radio times out, and nothing reaches the network — yet the WiFi state
// machine still reports CONNECTED, because the disconnect event never
// arrives over the jammed link. The panel then sits unreachable
// indefinitely with a healthy heap and a responsive UI, so none of the
// other checks fire.
//
// The SignalK stream is the honest signal: it is real traffic over that
// same transport, so it drops within seconds of a wedge. Treat "WiFi
// claims connected, but the stream has been down a long time" as a dead
// link. The threshold is generous — a server restart or a genuine
// network outage must not reboot the panel — and espOS reconnects the
// stream by itself in every case where a reboot would not have helped.
bool sk_link_stalled() {
  static uint32_t down_since_s = 0;
  constexpr uint32_t kStallSeconds = 180;

  espos_wifi_status_t w;
  const bool wifi_claims_up =
      espos_wifi_get_status(&w) == ESP_OK && w.sm.state == ESPOS_WIFI_ST_CONNECTED;
  espos_sk_ws_status_t ws;
  const bool have_ws = espos_sk_ws_get_status(&ws) == ESP_OK;
  const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

  // Only meaningful once the stream has been enabled and a server chosen;
  // an unprovisioned panel is not "stalled", it is idle.
  if (!wifi_claims_up || !have_ws || !ws.enabled || ws.reconnects == 0) {
    down_since_s = 0;
    return false;
  }
  if (ws.connected) {
    down_since_s = 0;
    return false;
  }
  if (down_since_s == 0) {
    down_since_s = now_s;
    return false;
  }
  return (now_s - down_since_s) >= kStallSeconds;
}

void health_check() {
  static int consecutive_fail = 0;
  bool ok = true;
  const char* reason = "";
  // Deliberately NOT "wifi disconnected". A router reboot, an AP roam or a
  // trip out of range are normal and recoverable: espos_wifi reconnects by
  // itself, and rebooting throws away the UI, every socket and any unsaved
  // state to fix nothing. Worse, while the network is marginal the panel
  // reboots every ~90 s, which is what a user hit -- barely able to interact
  // between restarts.
  //
  // A wedged transport still gets caught, by sk_link_stalled() below: it
  // watches real traffic over the link rather than what the state machine
  // believes, and only fires when WiFi claims to be up while the stream has
  // been down for minutes. That is the case a reboot actually fixes.
  if (esp_get_free_heap_size() < 64 * 1024) {
    ok = false;
    reason = "heap exhausted";
    // Internal RAM is the scarce resource on the P4 (esp_hosted, LVGL and
    // the esp-sr AFE all want DMA-capable RAM). The wake pipeline dips to
    // ~24 KB free / ~23 KB largest block while it allocates, then settles
    // near 95 KB — thresholds sit below that trough, not below the steady
    // state, or the panel reboots itself every boot.
  } else if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 16 * 1024 ||
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8 * 1024) {
    ok = false;
    reason = "internal RAM exhausted";
  } else if (s_n2k_rx && s_n2k_rx->ever_received() && s_n2k_rx->seconds_since_last_rx() > 30) {
    ok = false;
    reason = "n2k rx stalled";
  } else if (sk_link_stalled()) {
    ok = false;
    reason = "wifi transport wedged (sk stream down while 'connected')";
  }
  if (!ok) {
    consecutive_fail++;
    ESP_LOGW("watchdog", "health check FAIL %d/3: %s", consecutive_fail, reason);
    if (consecutive_fail >= 3) {
      ESP_LOGE("watchdog", "rebooting due to: %s", reason);
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
  } else {
    consecutive_fail = 0;
  }
}

// UI-thread stall watchdog: reboot when the LVGL task stops ticking.
void ui_wdt_task(void*) {
  uint32_t last = 0;
  int stalled_s = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now = cockpit_hal::ui::heartbeat();
    if (now == last) {
      if (++stalled_s >= 15) {
        ESP_LOGE("ui_wdt", "ui thread stalled %ds (hb=%lu); rebooting", stalled_s, (unsigned long)now);
        esp_restart();
      }
    } else {
      stalled_s = 0;
      last = now;
    }
  }
}

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(espos_log_init());
  ESP_ERROR_CHECK(espos_config_init(nullptr, nullptr));

  // ---- display + LVGL first: the panel shows something within a second
  static cockpit_hal::Waveshare7BDisplay display;
  static cockpit_hal::Waveshare7BTouch touch;
  cockpit_hal::ui::start(&display, &touch);
  {
    int32_t pct = 95;
    espos_config_get_i32(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_BRIGHTNESS, &pct);
    display.set_brightness((uint8_t)pct);
  }

  // Panel speaker + mics (ES8311 + NS4150B, ES7210). Same hardware on 7B
  // and 4B. Drives the alert chime and the voice satellite. Must come
  // after the touch HAL: the codecs share its I2C bus.
  static cockpit_hal::WaveshareAudio audio;
  audio.init();

  // Wyoming voice satellite (:10700): the boat's signalk-wyoming
  // orchestrator dials in, plays TTS through the speaker and captures the
  // mic (push-to-talk via the voice widget or the wake word). Wake runs
  // on-device (esp-sr WakeNet "Hi ESP" from the model partition) unless
  // cockpit.wake_host names a signalk-openwakeword server, in which case
  // the mic streams there and any custom-trained word works.
  espos_voice::WyomingSatelliteConfig wy_cfg;
  // Set explicitly: espos_voice defaults to a generic "espos", and this is
  // the name the orchestrator and Home Assistant show for the device.
  wy_cfg.name = "cockpit";
  {
    char wake_host[64] = "";
    espos_config_get_str(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_WAKE_HOST, wake_host, sizeof(wake_host), nullptr);
    char wake_word[32] = "";
    espos_config_get_str(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_WAKE_WORD, wake_word, sizeof(wake_word), nullptr);
    if (wake_host[0]) {
      wy_cfg.on_device_wake = false;
      wy_cfg.wake_host = wake_host;
      wy_cfg.wake_port = 10400;
      if (wake_word[0]) wy_cfg.wake_words = {wake_word};
      // An explicit host is a deliberate choice -- a second wake server, or a
      // specific word. Discovery would overwrite it with the SK server and the
      // setting would silently do nothing.
      s_wake_configured = true;
    } else {
      wy_cfg.on_device_wake = true;
    }
  }
  wy_cfg.wake_input_gain = 6;   // mic quiet (~665 raw); x6 into WakeNet range
  wy_cfg.wake_threshold = 0.45f;
  // Per-chunk RMS normalisation is an AGC: it scales quiet ambient toward
  // the same target as speech (cap permitting), compressing exactly the
  // speech-vs-silence contrast the orchestrator's energy gate endpoints
  // on. Measured at the gate on this boat: ambient ~900, speech ~1400 —
  // 1.5:1, ungateable, every utterance ran to the cap. A fixed gain keeps
  // the mic's real ~4:1 ratio intact and lets the gate's adaptive floor do
  // its job.
  wy_cfg.mic_stream_target_rms = 0;
  wy_cfg.mic_stream_gain = 8;
  // The STT stream is normalised toward a target RMS (see wyoming_satellite.h):
  // the orchestrator's speech floor is hardcoded at 700 for a far louder mic,
  // and this panel measures ~19 RMS raw, so no fixed multiplier works across
  // rooms and speakers. mic_stream_gain is only the fallback when
  // mic_stream_target_rms is 0.
  // No awake blip: mic and speaker share one I2S bus inches apart, and the
  // orchestrator's endpointer would seed its noise floor from the tone.
  // Cue ON: without it the talker has no idea when the mic opens, waits a
  // polite beat, and the endpointer closes the utterance on 3 s of silence
  // before the question begins. The old floor-pollution reason is gone —
  // the orchestrator mutes the mic for the cue + 0.5 s before endpointing.
  wy_cfg.awake_cue = true;
  static espos_voice::WyomingSatellite wyoming_sat(&audio, wy_cfg);
  s_wyoming_sat = &wyoming_sat;
  wyoming_sat.set_mic_muted_fn([] { return jlp::voice().mic_muted(); });
  jlp::http_api_set_wyoming(&wyoming_sat);

  // ---- JLP on the UI thread: overlay, layout manager, stored/default layout
  cockpit_hal::ui::post([] {
    char host[33] = "";
    espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_HOSTNAME, host, sizeof(host), nullptr);
    jlp::overlay().init();
    jlp::overlay().set_hostname(host[0] ? host : "cockpit");
    jlp::layout_manager().init(jlp::overlay().content_root());
    jlp::store_init();
    jlp::chime().init(&audio);
    jlp::voice().init(&wyoming_sat, &audio);
    std::string stored;
    jlp::ApplyResult r{};
    if (jlp::store_read(&stored)) {
      r = jlp::layout_manager().apply(stored, jlp::ApplySource::BootStore);
      if (!r.ok) {
        ESP_LOGW(TAG, "stored layout rejected (%s); clearing + falling back", r.err.c_str());
        jlp::store_clear();
      }
    }
    if (!r.ok) {
      r = jlp::layout_manager().apply(jlp::kDefaultLayoutJson, jlp::ApplySource::BootDefault);
      if (!r.ok) ESP_LOGE(TAG, "default layout rejected: %s", r.err.c_str());
    }
    // After a layout swap that introduces new paths, seed their values via
    // REST (SK only streams on change); espOS subscribes them itself.
    jlp::layout_manager().set_post_swap_hook(
        [](bool new_paths_introduced, const std::set<std::string>& new_paths, jlp::ApplySource src,
           const std::set<std::string>& all_paths) {
          const bool boot = src == jlp::ApplySource::BootStore || src == jlp::ApplySource::BootFetched;
          const std::set<std::string>& to_fetch = boot ? all_paths : new_paths;
          if (to_fetch.empty()) return;
          std::vector<std::string> v(to_fetch.begin(), to_fetch.end());
          jlp::SkServer s = jlp::sk_server();
          if (!s.host.empty()) jlp::zone_fetch_for_paths(s.host, s.port, v);
          if (new_paths_introduced) jlp::subscribe_new_paths(new_paths);
        });
    jlp::zones().hook_sk_ws();
    jlp::notifications().hook_sk_ws();
    jlp::sun_state().hook_sk_ws();
    jlp::alert_overlay().init();
    jlp::mic_overlay().init();    // after alert_overlay: an alarm wins
    jlp::wake_overlay().init();   // after alert_overlay: z-order
    jlp::idle_dimmer().init();
    (void)jlp::notifications().on_change([]() {
      if (jlp::notifications().last_change_was_escalation()) jlp::idle_dimmer().wake();
    });
    cockpit_hal::ui::every(1000, [] { poll_sk_state(); poll_status_line(); });
    cockpit_hal::ui::every(30000, health_check);
    cockpit_hal::ui::every(5000, [] {
      // esp_hosted's SDIO buffers are 64-byte-aligned DMA allocations, and
      // hosted_malloc_align() tries TWO pools: SPIRAM|DMA first (with
      // MEMPOOL_PREFER_SPIRAM) then INTERNAL|DMA as fallback. Report both.
      // Reporting only one is how the C6 link died unseen: iram_big read a
      // healthy 26 KB while a 9 KB INTERNAL|DMA request was failing.
      ESP_LOGI(TAG, "n2k: rx_idle=%llds cl=%u | heap=%lu iram=%u iram_min=%u iram_big=%u "
                    "dma_int=%u dma_ps=%u psram=%lu",
               (long long)((s_n2k_rx && s_n2k_rx->ever_received()) ? s_n2k_rx->seconds_since_last_rx() : -1),
               s_n2k_server ? (unsigned)s_n2k_server->connected_clients() : 0u,
               (unsigned long)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    });
  });

  // ---- network: espOS
  ESP_ERROR_CHECK(espos_httpd_start());
  ESP_ERROR_CHECK(espos_wifi_start());
  ESP_ERROR_CHECK(espos_sk_start());
  ESP_ERROR_CHECK(espos_ota_start());

  // ---- NMEA 2000 gateway: TWAI rx/tx + candump TCP server (:2599)
  // Pins live here, not in espos_n2k: the 7B's on-board TJA1051T CAN
  // transceiver is wired to GPIO22/21, which is a fact about this board.
  static espos_n2k::TwaiReceiver n2k_rx(
      {.tx_pin = GPIO_NUM_22, .rx_pin = GPIO_NUM_21});
  static espos_n2k::TwaiTransmitter n2k_tx;
  static espos_n2k::CandumpTcpServer n2k_server(&n2k_rx, &n2k_tx);
  s_n2k_rx = &n2k_rx;
  s_n2k_server = &n2k_server;
  n2k_rx.start();
  n2k_tx.start();
  n2k_server.start();

  // ---- the layout push API (designer) on its own port + mDNS
  int32_t api_port = 8081;
  espos_config_get_i32(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_API_PORT, &api_port);
  jlp::http_api_start((uint16_t)api_port);
  jlp::mdns_announce_start((uint16_t)api_port);
  // Network is up (lwIP initialised by espOS): safe to open the satellite's
  // TCP listener and the wake pipeline.
  wyoming_sat.start();

  xTaskCreate(ui_wdt_task, "ui_wdt", 2560, nullptr, configMAX_PRIORITIES - 1, nullptr);
  ESP_LOGI(TAG, "boot: iram=%u iram_big=%u psram=%lu", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
