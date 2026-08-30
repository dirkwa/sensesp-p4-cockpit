#pragma once

#include <stdint.h>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace jlp {

/** SK notification state, ordered by severity. Used by the alert
 *  overlay's `min_state` filter and by the list widget's optional
 *  per-row state colouring. */
enum class NotState : uint8_t {
  Nominal,    // 0 — least severe, "all good"
  Normal,
  Alert,
  Warn,
  Alarm,
  Emergency   // 5 — most severe
};

inline NotState parse_not_state(const char* s) {
  if (!s) return NotState::Normal;
  if (!strcmp(s, "nominal"))   return NotState::Nominal;
  if (!strcmp(s, "normal"))    return NotState::Normal;
  if (!strcmp(s, "alert"))     return NotState::Alert;
  // SK sources in the wild emit both "warn" (spec) and "warning"
  // (older convention); accept either. Designer normalises the
  // same way in fetchNotifications().
  if (!strcmp(s, "warn"))      return NotState::Warn;
  if (!strcmp(s, "warning"))   return NotState::Warn;
  if (!strcmp(s, "alarm"))     return NotState::Alarm;
  if (!strcmp(s, "emergency")) return NotState::Emergency;
  return NotState::Normal;
}

inline const char* not_state_name(NotState s) {
  switch (s) {
    case NotState::Nominal:   return "nominal";
    case NotState::Normal:    return "normal";
    case NotState::Alert:     return "alert";
    case NotState::Warn:      return "warn";
    case NotState::Alarm:     return "alarm";
    case NotState::Emergency: return "emergency";
  }
  return "normal";
}

/** A single notification snapshot. `path` is the part after
 *  "notifications." for compact display; the full SK path is
 *  reconstructed when needed for PUTs. */
struct Notification {
  std::string path;     // e.g. "mob.<uuid>" (after "notifications.")
  std::string message;
  NotState state;
  // SK `method` array carries "visual"/"sound". The chime fires ONLY when
  // "sound" is explicitly present — a producer must opt in. A visual-only
  // method, a server-silenced alert (which drops "sound"), or a delta with
  // no method at all all stay quiet. (The overlay still pops on state.)
  bool wants_sound = false;
};

/** Tracks the live set of `notifications.*` paths broadcast by SK.
 *  Fires on_change(...) whenever the set or any entry's state
 *  changes; consumers (alert overlay, list widget) re-query the
 *  registry on each change. */
class NotificationsRegistry {
 public:
  /** Wire into the SK WS client's value callback at boot. Idempotent. */
  void hook_sk_ws();

  /** Manually feed a notification delta. Normally not called by user
   *  code — the WS callback does this. */
  void apply(const std::string& path_after_prefix,
             const JsonVariantConst& value);

  /** Most severe currently-pending notification, or nullptr if all
   *  are normal/nominal/cleared. Acknowledged notifications are
   *  skipped (see acknowledge()). */
  const Notification* most_severe() const;

  /** All currently-tracked notifications, sorted by severity
   *  descending. Includes locally-acknowledged paths (ack only
   *  dismisses the alert overlay; the row stays visible because the
   *  bus condition is still live and the operator needs awareness).
   *  By default skips cleared (normal/nominal) entries; set
   *  `include_cleared` to true for an audit-style view. */
  std::vector<Notification> snapshot(bool include_cleared = false) const;

  /** Locally acknowledge a notification by path. The notification
   *  stays in the registry (the bus condition is still live) but is
   *  suppressed from most_severe()/snapshot() — so the alert overlay
   *  dismisses and the device stays usable, regardless of whether the
   *  alarm clears on the N2K bus. Fires on_change().
   *
   *  The ack auto-clears (the notification re-arms) when the path
   *  goes to normal/nominal, OR when its state escalates above the
   *  level it was acked at (e.g. alarm -> emergency re-pops).
   *
   *  Returns whether the caller should ALSO send the SK ack delta.
   *  False for a path known to re-assert (a bus-backed alarm): our
   *  state=normal delta only clears the server copy for the ~1 s it
   *  takes the source to re-raise it, and that clear->re-raise flap —
   *  multiplied by every consumer on the network — is pure churn. A
   *  local-only ack leaves the server truthfully alarming and just
   *  silences this panel. */
  [[nodiscard]] bool acknowledge(const std::string& path_after_prefix);

  /** True if `path` is currently acknowledged. */
  bool is_acknowledged(const std::string& path_after_prefix) const;

  /** True if the most-recent change that fired observers was an
   *  escalation — i.e. a notification appeared or was raised to a
   *  more severe state than what was previously stored for that
   *  path. Clears (-> normal/nominal), severity reductions, and
   *  message-only edits all return false.
   *
   *  Used by the wake hook in main.cpp to keep the panel from
   *  lighting up when an existing notification simply clears
   *  (e.g. a brief inverter-imbalance trip that auto-resolves
   *  doesn't drag the helm out of idle).
   *
   *  Only meaningful when read inside an on_change() callback
   *  on the event_loop task. */
  bool last_change_was_escalation() const {
    return last_change_was_escalation_;
  }

  /** True if the last change was an escalation AND that notification's
   *  SK `method` asked for "sound". The chime gate uses this so the beep
   *  reflects the alert that actually fired — not most_severe(), which
   *  could be a different, quieter (visual-only) alert. Only meaningful
   *  inside an on_change() callback. */
  bool last_escalation_wants_sound() const {
    return last_escalation_wants_sound_;
  }

  /** Path (after "notifications.") of the change that fired the current
   *  on_change() callbacks. The chime gate pairs it with
   *  is_acknowledged() so an already-acked alarm flapping back to its
   *  acked severity never re-beeps. Only meaningful inside an
   *  on_change() callback. */
  const std::string& last_changed_path() const { return last_changed_path_; }

  /** Register a change observer. Returns an opaque token; the caller
   *  must call `off_change(token)` before any captured pointer is
   *  destroyed. Fires on the event_loop task. */
  using Observer = std::function<void()>;
  using ObserverToken = uint32_t;
  ObserverToken on_change(Observer cb) {
    ObserverToken tok = next_token_++;
    observers_.push_back({tok, std::move(cb)});
    return tok;
  }
  void off_change(ObserverToken token) {
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
      if (it->token == token) {
        observers_.erase(it);
        return;
      }
    }
  }

 private:
  void fire_observers();

  struct Slot {
    ObserverToken token;
    Observer cb;
  };
  std::unordered_map<std::string, Notification> map_;
  // Paths whose most recent clear was caused by our own ACK delta.
  //
  // Acking sends state=normal to the same path the alarm source drives,
  // so the server echoes a clear straight back at us. Treating that as a
  // genuine "condition went away" would erase the ack we just made, and
  // the source's next alarm — often under a second later — would pop the
  // overlay again. The alarm is not re-firing because it escalated; it
  // never stopped. So a clear on a path we just acked leaves the ack in
  // place, and only a clear we did NOT cause re-arms it.
  std::unordered_set<std::string> self_cleared_;

  // Locally-acknowledged paths -> the severity they were acked at.
  // Used to suppress the overlay while the bus keeps re-asserting,
  // and to re-arm if the condition later escalates.
  std::unordered_map<std::string, NotState> acked_;

  // Paths whose ack-clear echo was followed by a re-raise (the source
  // keeps asserting the alarm). Once a path proves that, acknowledge()
  // stops sending the server ack delta for it — see acknowledge().
  // Process-lifetime: a reboot forgets, and the first ack after boot
  // re-learns from one clear->re-raise round trip.
  std::unordered_set<std::string> reasserting_;
  // Path -> esp_timer time (us) when our own ack-clear echo arrived.
  // A re-raise within the window promotes the path into reasserting_.
  std::unordered_map<std::string, int64_t> ack_clear_echo_at_;
  std::vector<Slot> observers_;
  ObserverToken next_token_ = 1;
  // Set by apply() right before each fire_observers() call so that
  // observers running on the same call stack can read it via
  // last_change_was_escalation(). Reset to false at the top of every
  // apply() invocation.
  bool last_change_was_escalation_ = false;
  bool last_escalation_wants_sound_ = false;
  std::string last_changed_path_;
};

NotificationsRegistry& notifications();

}  // namespace jlp
