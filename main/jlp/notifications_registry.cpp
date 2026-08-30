#include "notifications_registry.h"

#include <algorithm>
#include <string_view>

#include "esp_log.h"
#include "esp_timer.h"
#include "cockpit_hal/ui.h"
#include "espos_sk.h"

static const char* TAG = "jlp.notifs";

namespace jlp {

namespace {
// How long after our own ack-clear echo a re-raise still counts as "the
// source never stopped asserting". Bus alarms come back in ~1 s; a whole
// minute of margin still can't confuse a condition that genuinely cleared
// and later re-occurred with one that never went away.
constexpr int64_t kReassertWindowUs = 60LL * 1000 * 1000;
}  // namespace

void NotificationsRegistry::apply(const std::string& path_after_prefix,
                                  const JsonVariantConst& value) {
  // Default: not an escalation. Each branch that fire_observers()
  // sets this true only when a new notification appeared at >= Alert
  // or an existing one rose to a more severe state. Reset at the top
  // so a previous escalation doesn't bleed into a later non-event
  // (the flag is read by main.cpp's wake hook only).
  last_change_was_escalation_ = false;
  last_escalation_wants_sound_ = false;

  // Treat a null value (path cleared) as removal.
  if (value.isNull()) {
    // A cleared path re-arms any local ack: if it fires again it
    // should pop the overlay anew — unless this clear is the echo of
    // our own ACK, which would erase the ack the moment we made it.
    if (self_cleared_.erase(path_after_prefix) == 0) {
      acked_.erase(path_after_prefix);
    }
    if (map_.erase(path_after_prefix) > 0) {
      ESP_LOGI(TAG, "cleared %s", path_after_prefix.c_str());
      last_changed_path_ = path_after_prefix;
      fire_observers();
    }
    return;
  }
  Notification n;
  n.path = path_after_prefix;
  n.message = value["message"] | "";
  n.state = parse_not_state(value["state"] | "normal");
  // Chime only when the SK `method` array explicitly contains "sound".
  // Absent method, visual-only, or a server-silenced alert (sound
  // dropped) all leave wants_sound false → no beep.
  for (JsonVariantConst m : value["method"].as<JsonArrayConst>()) {
    const char* s = m.as<const char*>();
    if (s && strcmp(s, "sound") == 0) { n.wants_sound = true; break; }
  }

  // Cleared (nominal/normal): re-arm any local ack so a future
  // alert state pops the overlay anew. Keep the entry in the map
  // so a list widget with include_cleared=true can still show it;
  // most_severe() and the default snapshot() skip these by their
  // own filters (severity threshold + the include_cleared param).
  if (n.state == NotState::Nominal || n.state == NotState::Normal) {
    // cleared -> re-arm, unless we caused this clear ourselves.
    if (self_cleared_.erase(path_after_prefix) == 0) {
      acked_.erase(path_after_prefix);
    } else {
      // Our own ack-clear echo. Start the re-assert clock: if the same
      // path alarms again inside the window, the source never stopped
      // asserting and future acks for it should stay local-only.
      ack_clear_echo_at_[path_after_prefix] = esp_timer_get_time();
    }
    auto it = map_.find(path_after_prefix);
    if (it != map_.end() && it->second.state == n.state &&
        it->second.message == n.message) {
      return;  // no change
    }
    map_[path_after_prefix] = n;
    ESP_LOGI(TAG, "cleared %s (state=%s)", path_after_prefix.c_str(),
             not_state_name(n.state));
    last_changed_path_ = path_after_prefix;
    fire_observers();
    return;
  }

  // The path is alerting again, so the clear our ACK caused is behind
  // us: drop the guard. Without this the entry would outlive the echo
  // it was meant to absorb and swallow the NEXT clear too — the one
  // that means the condition really went away — leaving the ack stuck
  // on forever.
  self_cleared_.erase(path_after_prefix);

  // Re-raised shortly after our ack-clear echo: the source is bus-backed
  // and never stopped asserting. Remember that so acknowledge() stops
  // feeding the server clear -> re-raise flap for this path.
  auto echo_it = ack_clear_echo_at_.find(path_after_prefix);
  if (echo_it != ack_clear_echo_at_.end()) {
    if (esp_timer_get_time() - echo_it->second < kReassertWindowUs &&
        n.state >= NotState::Alert) {
      if (reasserting_.insert(path_after_prefix).second) {
        ESP_LOGI(TAG, "%s re-asserts after ack — future acks stay local",
                 path_after_prefix.c_str());
      }
    }
    ack_clear_echo_at_.erase(echo_it);
  }

  // If a previously-acked notification escalates above the level it
  // was acknowledged at, re-arm it so the overlay pops again.
  auto ack_it = acked_.find(path_after_prefix);
  if (ack_it != acked_.end() && n.state > ack_it->second) {
    ESP_LOGI(TAG, "%s escalated %s -> %s, re-arming",
             path_after_prefix.c_str(), not_state_name(ack_it->second),
             not_state_name(n.state));
    acked_.erase(ack_it);
  }

  auto it = map_.find(path_after_prefix);
  if (it != map_.end() && it->second.state == n.state &&
      it->second.message == n.message) {
    // No change.
    return;
  }
  // Escalation = either a brand-new path firing at >= Alert, or an
  // existing path going from a less-severe state to a more-severe
  // one. Same-or-lower-severity updates (or pure message edits at
  // the same level) don't qualify — the operator already saw the
  // higher state on the previous fire.
  const bool was_new = it == map_.end();
  const NotState prev_state = was_new ? NotState::Normal : it->second.state;
  last_change_was_escalation_ = n.state > prev_state;
  // Whether THIS escalating notification asked for sound — so the chime
  // keys off the alert that actually fired, not most_severe() (a
  // different, quieter alert could be more severe and suppress the beep).
  last_escalation_wants_sound_ = last_change_was_escalation_ && n.wants_sound;
  map_[path_after_prefix] = n;
  ESP_LOGI(TAG, "%s = %s \"%s\"%s", path_after_prefix.c_str(),
           not_state_name(n.state), n.message.c_str(),
           last_change_was_escalation_ ? " (escalation)" : "");
  last_changed_path_ = path_after_prefix;
  fire_observers();
}

const Notification* NotificationsRegistry::most_severe() const {
  const Notification* best = nullptr;
  for (const auto& kv : map_) {
    if (acked_.count(kv.first)) continue;  // locally acknowledged
    if (kv.second.state == NotState::Nominal ||
        kv.second.state == NotState::Normal) {
      continue;  // cleared — kept in the map for list views, not "pending"
    }
    if (!best || kv.second.state > best->state) best = &kv.second;
  }
  return best;
}

std::vector<Notification> NotificationsRegistry::snapshot(
    bool include_cleared) const {
  std::vector<Notification> out;
  out.reserve(map_.size());
  for (const auto& kv : map_) {
    // Ack suppresses the alert-overlay popup, NOT the list. The
    // condition is still live on the bus, and the operator needs to
    // see it (red row + alarm message) to remain aware of it. Only
    // most_severe() (the overlay's driver) excludes acked paths.
    if (!include_cleared &&
        (kv.second.state == NotState::Nominal ||
         kv.second.state == NotState::Normal)) {
      continue;
    }
    out.push_back(kv.second);
  }
  std::sort(out.begin(), out.end(),
            [](const Notification& a, const Notification& b) {
              return a.state > b.state;  // descending by severity
            });
  return out;
}

bool NotificationsRegistry::acknowledge(const std::string& path_after_prefix) {
  auto it = map_.find(path_after_prefix);
  if (it == map_.end()) {
    // Nothing tracked under this path; nothing to ack.
    return false;
  }
  acked_[path_after_prefix] = it->second.state;
  const bool send_server_ack = reasserting_.count(path_after_prefix) == 0;
  if (send_server_ack) {
    // The ACK delta the caller is about to send comes back as a clear;
    // don't let it re-arm the ack we just made (see self_cleared_).
    self_cleared_.insert(path_after_prefix);
  }
  ESP_LOGI(TAG, "acked %s (state=%s%s)", path_after_prefix.c_str(),
           not_state_name(it->second.state),
           send_server_ack ? "" : ", local-only");
  last_changed_path_ = path_after_prefix;
  fire_observers();
  return send_server_ack;
}

bool NotificationsRegistry::is_acknowledged(
    const std::string& path_after_prefix) const {
  return acked_.count(path_after_prefix) > 0;
}

void NotificationsRegistry::fire_observers() {
  // Snapshot first because callbacks might (legitimately) call
  // off_change() during the fire loop.
  std::vector<Slot> snapshot = observers_;
  for (const auto& s : snapshot) s.cb();
}


namespace {
// notifications.* is a family the server extends at will (alarms appear on
// paths nobody can enumerate up front), so one espOS family subscription
// covers it. The callback runs on the stream task; apply() drives the
// alert overlay (LVGL), so hop to the UI thread with a parsed copy.
constexpr std::string_view kPrefix = "notifications.";

void on_notification(const espos_sk_update_t* u, void*) {
  if (!u->value_json) return;                 // meta for notifications: ignore
  std::string path(u->path);
  if (path.size() < kPrefix.size() || path.compare(0, kPrefix.size(), kPrefix) != 0) return;
  std::string value(u->value_json);
  cockpit_hal::ui::post([path, value = std::move(value)]() {
    JsonDocument doc;
    if (deserializeJson(doc, value) != DeserializationError::Ok) return;
    notifications().apply(path.substr(kPrefix.size()), doc.as<JsonVariantConst>());
  });
}
}  // namespace

void NotificationsRegistry::hook_sk_ws() {
  // Small period so an alarm reaches the overlay with minimal latency:
  // SK's `period` is the minimum interval, and notification deltas are
  // change-driven and infrequent. Re-sent on every reconnect by espOS.
  constexpr int kNotifPeriodMs = 100;
  int h = espos_sk_subscribe("notifications.*", kNotifPeriodMs, on_notification, nullptr);
  ESP_LOGI(TAG, "subscribed to notifications.* (%d)", h);
}

NotificationsRegistry& notifications() {
  static NotificationsRegistry r;
  return r;
}

}  // namespace jlp
