#pragma once

#include <stdint.h>
#include <string>

#include "lvgl.h"
#include "notifications_registry.h"

namespace jlp {

/** Full-screen modal that pops above the active screen (and above
 *  the status overlay strip) whenever the notifications registry
 *  reports a pending notification with state >= min_state. Tapping
 *  ACK PUTs `state: "normal"` to the same path and the overlay
 *  re-evaluates; if more notifications remain it refills with the
 *  next-most-severe, else hides.
 *
 *  Created once at boot, configured by each LayoutManager::apply()
 *  from the layout's optional top-level `notifications` block. */
class AlertOverlay {
 public:
  void init();

  /** Re-configure from a layout block. Safe to call on every
   *  layout swap. */
  void configure(bool enabled, NotState min_state);

 private:
  void rebuild();

  lv_obj_t* root_ = nullptr;      // hidden tile that covers the screen
  lv_obj_t* path_label_ = nullptr;
  lv_obj_t* state_label_ = nullptr;
  lv_obj_t* msg_label_ = nullptr;
  lv_obj_t* ack_button_ = nullptr;
  std::string current_path_;       // path-after-prefix shown right now
  // lv_tick when the overlay appeared or switched alarms; taps inside
  // the guard window after that are ignored (pop-under-finger).
  uint32_t shown_at_ = 0;

  bool enabled_ = true;
  NotState min_state_ = NotState::Alarm;
};

AlertOverlay& alert_overlay();

}  // namespace jlp
