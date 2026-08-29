#include "layout_manager.h"

#include <ArduinoJson.h>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "esp_log.h"

#include "../alert_overlay.h"
#include "../idle_dimmer.h"
#include "../notifications_registry.h"
#include "../status_overlay.h"
#include "../subject_registry.h"
#include "../widgets/widget_factory.h"
#include "store.h"

static const char* TAG = "jlp.layout";

namespace jlp {

namespace {

constexpr int kSchemaVersion = 1;
constexpr size_t kMaxJsonBytes = 64 * 1024;

// Per-screen container + tab button colors.
constexpr uint32_t kStripBgHex = 0x161b22;
constexpr uint32_t kTabBgHex = 0x21262d;
constexpr uint32_t kTabActiveBgHex = 0x58a6ff;
constexpr uint32_t kTabFgHex = 0xe6edf3;
constexpr uint32_t kTabActiveFgHex = 0x0d1117;

struct ScreenSwitcherCtx {
  std::vector<lv_obj_t*> screens;
  std::vector<lv_obj_t*> tabs;
  // Screen ids, parallel to `screens`. Index is not a stable handle
  // across layout edits, so remote selection addresses screens by id.
  std::vector<std::string> ids;
  int active = 0;
};

// One per layout. Lives in heap, freed when the layout's root is
// destroyed (event_cb on root).
static void switcher_show(ScreenSwitcherCtx* ctx, int idx) {
  if (idx < 0 || idx >= (int)ctx->screens.size()) return;
  for (size_t i = 0; i < ctx->screens.size(); i++) {
    if ((int)i == idx) lv_obj_clear_flag(ctx->screens[i], LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(ctx->screens[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (size_t i = 0; i < ctx->tabs.size(); i++) {
    bool a = (int)i == idx;
    lv_obj_set_style_bg_color(
        ctx->tabs[i], lv_color_hex(a ? kTabActiveBgHex : kTabBgHex),
        LV_PART_MAIN);
    lv_obj_t* lbl = lv_obj_get_child(ctx->tabs[i], 0);
    if (lbl) {
      lv_obj_set_style_text_color(
          lbl, lv_color_hex(a ? kTabActiveFgHex : kTabFgHex), LV_PART_MAIN);
    }
  }
  ctx->active = idx;
  // Stream widgets open/close their network stream with screen visibility —
  // a hidden screen must not keep pulling frames through the hosted link.
  for (size_t i = 0; i < ctx->screens.size(); i++) {
    stream_widgets_notify_visibility(ctx->screens[i], (int)i == idx);
  }
}

// Builds the screen(s) under `root`. Single screen: widgets parented
// directly to root. Multi-screen: each screen gets its own container
// stacked top-to-top, only the active one is visible, and a bottom
// strip of tab buttons switches between them.
bool build_screens(lv_obj_t* root, JsonObjectConst doc, JsonArrayConst screens,
                   SubjectRegistry& reg, std::string* err,
                   std::set<std::string>* live_paths, unsigned* widget_count) {
  if (screens.size() == 0) { *err = "no screens"; return false; }

  if (screens.size() == 1) {
    JsonObjectConst s = screens[0];
    JsonArrayConst widgets = s["widgets"];
    BuildCtx ctx{root, reg, *live_paths};
    for (JsonObjectConst w : widgets) {
      if (!build_widget(ctx, w, err)) return false;
      ++*widget_count;
    }
    return true;
  }

  // Multi-screen: stack of hidden containers + bottom tab strip.
  const int strip_h = doc["tab_strip_height"] | 56;
  auto* sctx = new ScreenSwitcherCtx();

  // Free the switcher context when the layout root dies.
  lv_obj_set_user_data(root, sctx);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        delete static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);

  // Tab strip pinned to the bottom of `root`.
  lv_obj_t* strip = lv_obj_create(root);
  lv_obj_set_size(strip, lv_pct(100), strip_h);
  lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(strip, lv_color_hex(kStripBgHex), LV_PART_MAIN);
  lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(strip, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(strip, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  // Per-screen container above the strip, all sharing the same area.
  for (JsonObjectConst s : screens) {
    lv_obj_t* page = lv_obj_create(root);
    lv_obj_set_size(page, lv_pct(100), LV_VER_RES - strip_h);
    // Note: this is RELATIVE to `root` which is itself sized by the
    // caller (and offset by the status overlay if enabled). LV_VER_RES
    // minus strip_h gives a useful default that works when root fills
    // content_root (the common case).
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    sctx->screens.push_back(page);
    sctx->ids.push_back(s["id"] | "");

    JsonArrayConst widgets = s["widgets"];
    BuildCtx ctx{page, reg, *live_paths};
    for (JsonObjectConst w : widgets) {
      if (!build_widget(ctx, w, err)) return false;
      ++*widget_count;
    }
  }

  // Tab buttons (one per screen, flex-row equal widths).
  for (size_t i = 0; i < screens.size(); i++) {
    JsonObjectConst s = screens[i];
    const char* title = s["title"] | (s["id"] | "?");

    lv_obj_t* btn = lv_obj_create(strip);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(kTabBgHex), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTabFgHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);

    // Stash the screen index as a small int in user_data.
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t* e) {
          auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
          auto* parent_root = lv_obj_get_parent(lv_obj_get_parent(w));
          auto* ctx =
              static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(parent_root));
          int idx = (int)(intptr_t)lv_obj_get_user_data(w);
          if (ctx) switcher_show(ctx, idx);
        },
        LV_EVENT_CLICKED, nullptr);

    sctx->tabs.push_back(btn);
  }

  switcher_show(sctx, 0);
  return true;
}

// Screen selection by id, for the tab strip's remote equivalent.
// Both must run on the UI task: they touch LVGL and read the ctx that
// the layout root owns. A single-screen layout has no switcher at all
// (build_screens() never allocates one), so user_data is NULL there —
// hence the guard rather than a bare cast.
static ScreenSwitcherCtx* switcher_of(lv_obj_t* root) {
  if (!root) return nullptr;
  return static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(root));
}

// Subject kind a given widget type expects on its `bind` path. Must
// stay in lockstep with the get_or_create() calls in widget_factory.cpp.
// Returns nullopt for widgets that don't bind a path at all (button,
// notifications) — those skip the kind-conflict check.
std::optional<SubjectKind> expected_subject_kind(const char* type) {
  if (!type) return std::nullopt;
  if (!strcmp(type, "label"))    return SubjectKind::Float;
  if (!strcmp(type, "value"))    return SubjectKind::Float;
  if (!strcmp(type, "arc"))      return SubjectKind::Float;
  if (!strcmp(type, "bar"))      return SubjectKind::Float;
  if (!strcmp(type, "toggle"))   return SubjectKind::Int;
  // bargroup: handled separately because each sub-bar is its own
  // (path, kind) pair. button + notifications: no bind. Anything
  // unknown returns nullopt and is left to validate's existing
  // dispatch error path.
  return std::nullopt;
}

const char* subject_kind_name(SubjectKind k) {
  switch (k) {
    case SubjectKind::Float:  return "Float";
    case SubjectKind::Int:    return "Int";
    case SubjectKind::Bool:   return "Bool";
    case SubjectKind::String: return "String";
  }
  return "?";
}

// Record a (path, kind) requirement into `seen`. Returns false +
// fills *err on conflict (path already requested with a different
// kind).
bool record_path_kind(const std::string& path, SubjectKind kind,
                      std::unordered_map<std::string, SubjectKind>* seen,
                      const std::string& widget_label, std::string* err) {
  auto it = seen->find(path);
  if (it == seen->end()) {
    (*seen)[path] = kind;
    return true;
  }
  if (it->second != kind) {
    *err = std::string("kind conflict on bind path '") + path +
           "': " + widget_label + " wants " + subject_kind_name(kind) +
           " but a previous widget already bound it as " +
           subject_kind_name(it->second);
    return false;
  }
  return true;
}

// Validate global structure before building. Catches duplicates,
// missing required fields, AND kind conflicts on shared bind paths
// (e.g. one widget wants a path as Float, another as Int) so the
// layout is rejected up-front rather than half-built in LVGL.
bool validate(JsonObjectConst doc, std::string* err) {
  int schema = doc["schema"] | 0;
  if (schema != kSchemaVersion) {
    char buf[64];
    snprintf(buf, sizeof(buf), "unsupported schema %d (want %d)", schema,
             kSchemaVersion);
    *err = buf;
    return false;
  }
  JsonArrayConst screens = doc["screens"];
  if (screens.isNull() || screens.size() == 0) {
    *err = "no screens";
    return false;
  }

  std::unordered_set<std::string> ids;
  // path -> kind that's already been requested across the whole
  // layout. Survives screens, since widgets on different screens can
  // still bind the same path.
  std::unordered_map<std::string, SubjectKind> seen_paths;
  for (JsonObjectConst s : screens) {
    const char* sid = s["id"] | (const char*)nullptr;
    if (!sid) { *err = "screen missing id"; return false; }
    if (!ids.insert(sid).second) {
      *err = std::string("duplicate screen id: ") + sid;
      return false;
    }
    JsonArrayConst widgets = s["widgets"];
    std::unordered_set<std::string> wids;
    for (JsonObjectConst w : widgets) {
      const char* wid = w["id"] | (const char*)nullptr;
      if (!wid) { *err = "widget missing id"; return false; }
      if (!wids.insert(wid).second) {
        *err = std::string("duplicate widget id in screen ") + sid + ": " + wid;
        return false;
      }
      const char* type = w["type"] | (const char*)nullptr;
      if (!type) {
        *err = std::string("widget missing type in screen ") + sid;
        return false;
      }

      // Kind-conflict check. bargroup is a bag of independent sub-
      // bars; everything else has at most one bind path.
      const std::string label = std::string(type) + " '" + wid + "'";
      if (!strcmp(type, "bargroup")) {
        JsonArrayConst bars = w["bars"];
        for (JsonObjectConst b : bars) {
          const char* bp = b["bind"] | (const char*)nullptr;
          if (!bp) continue;  // sub-bar with no bind is a render-only stub
          if (!record_path_kind(bp, SubjectKind::Float, &seen_paths,
                                label, err)) {
            return false;
          }
        }
        continue;
      }
      const char* bind = w["bind"] | (const char*)nullptr;
      if (!bind) continue;  // toggle requires bind, others don't —
                            // missing-required-bind is build_*'s job
      auto kind = expected_subject_kind(type);
      if (!kind) continue;  // unknown / no-bind kinds: leave to dispatch
      if (!record_path_kind(bind, *kind, &seen_paths, label, err)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

void LayoutManager::init(lv_obj_t* parent) { parent_ = parent; }

ApplyResult LayoutManager::apply(const std::string& json, ApplySource src) {
  ApplyResult r{false, "", "", 0, 0, ""};
  if (!parent_) { r.err = "layout_manager not initialised"; return r; }

  if (json.size() > kMaxJsonBytes) {
    r.err = "layout too large";
    return r;
  }

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json);
  if (de) { r.err = std::string("parse: ") + de.c_str(); return r; }

  if (!validate(doc.as<JsonObjectConst>(), &r.err)) return r;

  // Atomic swap: build the new tree as a child of `parent_` but hidden,
  // then unhide on success and delete the old. If the build fails, the
  // current layout stays up untouched.
  //
  // (Earlier this code used lv_obj_create(NULL) which creates a SCREEN,
  // not a regular object — re-parenting a screen under another object
  // is undefined in LVGL 9 and resulted in widgets that never rendered.)
  lv_obj_t* staging = lv_obj_create(parent_);
  lv_obj_set_size(staging, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(staging, 0, 0);
  lv_obj_set_style_bg_opa(staging, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(staging, 0, LV_PART_MAIN);
  lv_obj_clear_flag(staging, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(staging, LV_OBJ_FLAG_HIDDEN);

  std::set<std::string> live_paths;
  JsonArrayConst screens = doc["screens"];
  if (!build_screens(staging, doc.as<JsonObjectConst>(), screens, registry(),
                     &r.err, &live_paths, &r.widgets)) {
    lv_obj_delete(staging);
    return r;
  }

  // Apply status_overlay flag before the swap. Default true (matches
  // the original always-on behavior — existing layouts unchanged).
  // Toggling resizes content_root, so the staging tree needs to be
  // re-parented after the resize to inherit the new geometry.
  bool overlay_visible = true;
  if (doc["status_overlay"].is<bool>()) {
    overlay_visible = doc["status_overlay"];
  }
  overlay().set_visible(overlay_visible);

  // Re-configure the alert overlay from the layout's top-level
  // `notifications` block. Defaults: enabled + min_state alarm.
  bool notif_enabled = true;
  NotState notif_min = NotState::Alarm;
  JsonObjectConst notif = doc["notifications"];
  if (!notif.isNull()) {
    if (notif["enabled"].is<bool>()) notif_enabled = notif["enabled"];
    notif_min = parse_not_state(notif["min_state"] | "alarm");
  }
  alert_overlay().configure(notif_enabled, notif_min);

  // Apply the backlight idle timeout (0 = disabled) and the dim-while-
  // idle brightness. Default dim_pct=0 (fully off); the wake overlay
  // takes the first tap so nothing underneath fires by accident.
  uint32_t idle_timeout_sec = 0;
  uint8_t idle_dim_pct = 0;
  JsonObjectConst display = doc["display"];
  if (!display.isNull()) {
    if (display["idle_timeout_sec"].is<uint32_t>()) {
      idle_timeout_sec = display["idle_timeout_sec"];
    }
    if (display["idle_dim_pct"].is<uint8_t>()) {
      idle_dim_pct = display["idle_dim_pct"];
    }
  }
  idle_dimmer().configure(idle_timeout_sec, idle_dim_pct);

  lv_obj_t* old_root = current_root_;
  lv_obj_clear_flag(staging, LV_OBJ_FLAG_HIDDEN);
  current_root_ = staging;
  if (old_root) lv_obj_delete(old_root);

  // Single-screen layouts have no switcher, so nothing else ever marks
  // their widgets visible. Multi-screen roots got switcher_show(0) during
  // the staging build — a stream widget's start then races the old
  // layout's teardown and retries on its watch timer, so no extra call is
  // needed there.
  if (screens.size() == 1) {
    stream_widgets_notify_visibility(current_root_, true);
  }

  // Persist only after a successful swap, and only for pushed layouts
  // (boot paths re-read the same persisted blob).
  if (src == ApplySource::PostLayout) {
    if (!store_write_atomic(json)) {
      r.warning = "layout rendered but not persisted";
    }
  }

  active_name_ = doc["name"] | "(unnamed)";
  active_source_ = src;

  // Compare against the previously-known path set so the post-swap
  // hook can decide whether to restart the WS (needed only if the
  // new layout introduced paths SK isn't currently subscribed to).
  // Capture the diff itself too — the hook uses it to fetch SK meta
  // for just the new paths instead of re-fetching everything.
  std::set<std::string> introduced;
  for (const auto& p : live_paths) {
    if (known_paths_.find(p) == known_paths_.end()) {
      introduced.insert(p);
    }
  }
  bool new_paths = !introduced.empty();
  known_paths_ = live_paths;
  if (post_swap_) post_swap_(new_paths, introduced, src, live_paths);

  r.ok = true;
  r.name = active_name_;
  r.screens = screens.size();
  ESP_LOGI(TAG,
           "applied layout '%s' (src=%d screens=%u widgets=%u paths=%u "
           "new=%d)",
           r.name.c_str(), (int)src, r.screens, r.widgets,
           (unsigned)live_paths.size(), new_paths ? 1 : 0);
  return r;
}

bool LayoutManager::select_screen(const std::string& id) {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return false;
  for (size_t i = 0; i < ctx->ids.size(); i++) {
    if (ctx->ids[i] == id) {
      switcher_show(ctx, (int)i);
      return true;
    }
  }
  return false;
}

std::string LayoutManager::active_screen() const {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return "";
  if (ctx->active < 0 || ctx->active >= (int)ctx->ids.size()) return "";
  return ctx->ids[ctx->active];
}

std::vector<std::string> LayoutManager::screen_ids() const {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return {};
  return ctx->ids;
}

LayoutManager& layout_manager() {
  static LayoutManager m;
  return m;
}

}  // namespace jlp
