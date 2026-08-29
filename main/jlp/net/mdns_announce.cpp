#include "mdns_announce.h"

#include "cockpit_hal/ui.h"
#include "esp_log.h"
#include "mdns.h"

static const char* TAG = "jlp.mdns";

namespace jlp {

void mdns_announce_start(uint16_t api_port) {
  // espOS starts mDNS with the SignalK discovery (needs WiFi up); retry
  // until the service add sticks, then stop.
  static uint16_t port = api_port;
  static uint32_t timer = 0;
  timer = cockpit_hal::ui::every(2000, []() {
    {
      mdns_txt_item_t txt[] = {
          // Keep widgets + firmware in lockstep with /hello in
          // http_api.cpp. mDNS browsers use these for capability
          // discovery before falling back to a real /hello fetch.
          // Canonical kinds only, for the same reason /hello lists them:
          // this is what a client should offer, not every spelling the
          // device will accept.
          {"schema",   "1"},
          {"widgets",  "label,value,toggle,arc,bar,bargroup,button,notifications,"
                       "anchor,anchor_track,voice,speaker,mic,volume,slider,stream"},
          {"firmware", "p4-cockpit-jlp-2.0.0"},
          {"api",      "/layout,/hello,/healthz,/screenshot"},
      };
      esp_err_t err = mdns_service_add(
          NULL, "_signalk-player", "_tcp", port,
          txt, sizeof(txt) / sizeof(txt[0]));
      if (err == ESP_OK) {
        ESP_LOGI(TAG, "announced _signalk-player._tcp on port %u", port);
        cockpit_hal::ui::cancel(timer);
      } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        cockpit_hal::ui::cancel(timer);
      }
    }
  });
}

}  // namespace jlp
