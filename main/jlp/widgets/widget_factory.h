#pragma once

#include "lvgl.h"
#include <ArduinoJson.h>
#include <set>
#include <string>

namespace jlp {

class SubjectRegistry;

struct BuildCtx {
  lv_obj_t* parent;
  SubjectRegistry& reg;
  std::set<std::string>& live_paths;
};

// Construct a single widget from `spec` under `ctx.parent`. Returns the
// new widget root, or nullptr on validation failure (with the cause in
// `*err`). Caller owns failure recovery (tear down the build, keep the
// previous screen).
lv_obj_t* build_widget(BuildCtx& ctx, JsonObjectConst spec, std::string* err);

// Tell any "stream" widgets that are direct children of `container` whether
// their screen is visible. Streaming runs ONLY while visible; the layout
// manager calls this from the screen switcher and after a layout swap.
// UI thread only.
void stream_widgets_notify_visibility(lv_obj_t* container, bool visible);

}  // namespace jlp
