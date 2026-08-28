#include "widget_factory.h"

#include <math.h>

#include "../audio/chime.h"
#include "../audio/voice_control.h"
#include "../net/drop_here.h"
#include "../net/sk_put.h"
#include "../notifications_registry.h"
#include "../subject_registry.h"
#include "../zone_registry.h"

namespace jlp {

namespace {

uint32_t kFgHex = 0xe6edf3;
uint32_t kMutedHex = 0x8b949e;
uint32_t kAccentHex = 0x58a6ff;
constexpr uint32_t kTileBgHex = 0x161b22;

constexpr int32_t kBarSteps = 1000;  // LVGL bar/arc integer range

// Per-widget color overrides. Hex strings in spec ("#rrggbb" or "#rgb").
// bg_color tints the tile background; fg_color tints the value text.
// SK zones still take precedence — these are only fallbacks when no
// zone matches (or when no bind/no zones are configured).
struct Colors {
  uint32_t bg;
  uint32_t fg;
};

// Parse "#rrggbb" or "#rgb" into a 24-bit hex color. Returns true on
// success; on failure (missing field, malformed) leaves *out untouched.
bool parse_hex_color(const char* s, uint32_t* out) {
  if (!s || *s != '#') return false;
  const char* h = s + 1;
  size_t n = strlen(h);
  if (n != 3 && n != 6) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    uint32_t d;
    if      (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
    else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
    else return false;
    v = (v << 4) | d;
  }
  if (n == 3) {
    // expand 0xRGB to 0xRRGGBB
    uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
    v = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
  }
  *out = v;
  return true;
}

Colors parse_colors(JsonObjectConst spec) {
  Colors c{kTileBgHex, kFgHex};
  parse_hex_color(spec["bg_color"] | (const char*)nullptr, &c.bg);
  parse_hex_color(spec["fg_color"] | (const char*)nullptr, &c.fg);
  return c;
}

struct Disp {
  float scale;
  float offset;
  int decimals;
  char unit[12];
  char path[80];  // bind path, for zone lookups in observers
};

Disp parse_display(JsonObjectConst spec) {
  Disp d{1.f, 0.f, 1, "", ""};
  JsonObjectConst display = spec["display"];
  if (!display.isNull()) {
    d.scale = display["scale"] | 1.f;
    d.offset = display["offset"] | 0.f;
    d.decimals = display["decimals"] | 1;
    snprintf(d.unit, sizeof(d.unit), "%s", display["unit"] | "");
  }
  snprintf(d.path, sizeof(d.path), "%s", spec["bind"] | "");
  return d;
}

void apply_geometry(lv_obj_t* obj, JsonObjectConst spec) {
  lv_obj_set_pos(obj, spec["x"] | 0, spec["y"] | 0);
  lv_obj_set_size(obj, spec["w"] | 120, spec["h"] | 60);
}

// Resolve a desired pixel font size to a compiled Montserrat font.
// LVGL can't synthesize fonts at runtime — we have to pick from
// what's compiled in (lv_conf.h: LV_FONT_MONTSERRAT_{14,16,20,28,36}
// on the cockpit build). Snap to the closest compiled size below
// `desired` so tiles don't overflow when the operator picks a
// larger-than-fits value; `fallback` is the size used when the
// spec doesn't specify (preserves existing per-widget defaults).
const lv_font_t* resolve_font(int desired_px, const lv_font_t* fallback) {
  if (desired_px <= 0) return fallback;
  if (desired_px >= 36) return &lv_font_montserrat_36;
  if (desired_px >= 28) return &lv_font_montserrat_28;
  if (desired_px >= 20) return &lv_font_montserrat_20;
  if (desired_px >= 16) return &lv_font_montserrat_16;
  return &lv_font_montserrat_14;
}

// Read `display.font_size` if present; otherwise return fallback.
const lv_font_t* font_from_spec(JsonObjectConst spec,
                                const lv_font_t* fallback) {
  JsonObjectConst display = spec["display"];
  if (display.isNull()) return fallback;
  int sz = display["font_size"] | 0;
  return resolve_font(sz, fallback);
}

// Returns the zone-coded color for `display_value` on `path`, or
// `fallback` if the path has no zones or value is outside all zones.
uint32_t zone_color(const char* path, float display_value, uint32_t fallback) {
  if (!path || !*path) return fallback;
  const Zone* z = zones().match(path, display_value);
  return z ? color_for_state(z->state) : fallback;
}

// Map a display-space value into [0, kBarSteps] for LVGL.
int32_t scale_to_steps(float display_value, float min, float max) {
  if (max <= min) return 0;
  float n = (display_value - min) / (max - min);
  if (n < 0) n = 0;
  if (n > 1) n = 1;
  return (int32_t)(n * kBarSteps + 0.5f);
}

// ---- label ----
//
// Render modes:
//   - caption only (no `bind`)     -> one large-font line of static text
//   - bind only   (no `label`)     -> body text large-font, centered
//   - both                         -> small-font caption on top, body
//                                     below (typical HMI tile layout)
//
// Body text is the SK meta `description` when one is published, else
// the formatted numeric value. This makes a label bound to e.g.
// `electrical.switches.bank.213.1.state` show "BMS DnC" instead of
// "1.0", which matches what operators read on the physical relay.
// The tile background is zone-tinted from the current value, same as
// toggle / arc / bar.
lv_obj_t* build_label(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const char* path = spec["bind"] | (const char*)nullptr;
  const char* caption = spec["label"] | (const char*)nullptr;
  const Colors colors = parse_colors(spec);
  const bool show_description = spec["show_description"] | false;

  // No bind: single static text label, return that directly.
  if (!path) {
    lv_obj_t* lbl = lv_label_create(ctx.parent);
    apply_geometry(lbl, spec);
    lv_obj_set_style_text_color(lbl, lv_color_hex(colors.fg), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        lbl, font_from_spec(spec, &lv_font_montserrat_28), LV_PART_MAIN);
    lv_label_set_text(lbl, caption ? caption : "");
    return lbl;
  }

  lv_subject_t* sub = ctx.reg.get_or_create(path, SubjectKind::Float);
  if (!sub) { *err = std::string("kind conflict on ") + path; return nullptr; }
  ctx.live_paths.insert(path);

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 4, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  if (caption && *caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  lv_obj_t* val = lv_label_create(root);
  lv_obj_set_style_text_color(val, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(
      val, font_from_spec(spec, &lv_font_montserrat_28), LV_PART_MAIN);
  // If a description is already in the registry (loaded via REST
  // fetch on layout apply), show it immediately so the widget is
  // legible before — or even without — the first value delta. Slow-
  // updating SK paths (switch states, SOC) might not see a delta
  // for a long time; without this the label sits at "—" until the
  // value changes, which can be never.
  if (show_description) {
    const std::string& desc = zones().description(std::string(path));
    lv_label_set_text(val, desc.empty() ? "—" : desc.c_str());
  } else {
    lv_label_set_text(val, "—");
  }
  if (caption && *caption) {
    lv_obj_align(val, LV_ALIGN_TOP_LEFT, 0, 20);
  } else {
    lv_obj_center(val);
  }

  // The observer needs both the value-label and the tile root so it
  // can update the text and the bg color on each delta. Stash both on
  // the value label.
  struct LabelCtx {
    Disp d;
    lv_obj_t* tile;
    Colors colors;
    bool show_description;
  };
  auto* lctx = new LabelCtx{parse_display(spec), root, colors, show_description};
  lv_obj_set_user_data(val, lctx);
  lv_obj_add_event_cb(
      val,
      [](lv_event_t* e) {
        delete static_cast<LabelCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);

  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* lc = static_cast<LabelCtx*>(lv_obj_get_user_data(w));
        float raw = lv_subject_get_float(s);
        float v = raw * lc->d.scale + lc->d.offset;
        bool wrote_desc = false;
        if (lc->show_description) {
          const std::string& desc = zones().description(lc->d.path);
          if (!desc.empty()) {
            lv_label_set_text(w, desc.c_str());
            wrote_desc = true;
          }
        }
        if (!wrote_desc) {
          lv_label_set_text_fmt(w, "%.*f %s", lc->d.decimals, v, lc->d.unit);
        }
        // Zones are in raw SK units (e.g. ratio 0..1 for SOC); match
        // against raw. Fall back to the spec'd bg_color when no zone
        // matches — zone always wins to keep alarms visible.
        //
        // tile is a SIBLING object (the root), not the observer's own
        // target. LVGL deletes a parent's children in list order, so
        // during a layout swap the tile can be freed before this
        // observer's target label — and lv_subject_add_observer_obj
        // delivers a synchronous notify on registration. A value
        // delta in flight during the swap then fires this callback
        // against a freed tile → Load access fault in get_local_style.
        // Guard the sibling deref; the observer auto-unsubscribes on
        // its own target's delete, so `w` above is always valid here.
        if (!lv_obj_is_valid(lc->tile)) return;
        uint32_t bg = zone_color(lc->d.path, raw, lc->colors.bg);
        lv_obj_set_style_bg_color(lc->tile, lv_color_hex(bg), LV_PART_MAIN);
      },
      val, nullptr);

  return root;
}

// ---- value ----
//
// Big-number readout tile. Differs from `label` in two ways:
//
//   - `value` centers the formatted number as the dominant glyph in
//     the tile; caption is the small top-left label, unit is small
//     bottom-right. `label` puts the caption on top and the value
//     below it, sized like a caption+value pair rather than one big
//     readout. `label` also has a `show_description` opt-in the
//     `value` widget doesn't need — `value` is always number-first.
//   - Zone state tints the WHOLE tile background by default (matches
//     the helm convention "if it's red, look at it now").
//
// Schema fields: x, y, w, h, label?, bind (required), display? (with
// scale/offset/decimals/unit auto-prefilled by the designer from SK
// displayUnits + unitsPreferences), bg_color?, fg_color?.
lv_obj_t* build_value(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const char* path = spec["bind"] | (const char*)nullptr;
  if (!path) { *err = "value: bind required"; return nullptr; }
  const char* caption = spec["label"] | (const char*)nullptr;
  const Colors colors = parse_colors(spec);

  lv_subject_t* sub = ctx.reg.get_or_create(path, SubjectKind::Float);
  if (!sub) { *err = std::string("kind conflict on ") + path; return nullptr; }
  ctx.live_paths.insert(path);

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(root, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 6, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  if (caption && *caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Big centered value. Font size: explicit `display.font_size`
  // wins; otherwise auto-scale with tile height so a tall tile
  // reads from across the cockpit.
  int box_h = spec["h"] | 60;
  const lv_font_t* default_big = &lv_font_montserrat_28;
  if (box_h < 60) default_big = &lv_font_montserrat_20;
  if (box_h < 40) default_big = &lv_font_montserrat_16;
  const lv_font_t* big_font = font_from_spec(spec, default_big);
  lv_obj_t* val = lv_label_create(root);
  lv_obj_set_style_text_color(val, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(val, big_font, LV_PART_MAIN);
  lv_label_set_text(val, "—");
  lv_obj_center(val);

  // Unit label flush bottom-right. The big number stays the focal
  // point; unit is informational only.
  lv_obj_t* unit_lbl = lv_label_create(root);
  lv_obj_set_style_text_color(unit_lbl, lv_color_hex(kMutedHex),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(unit_lbl, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(unit_lbl, "");
  lv_obj_align(unit_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

  struct ValueCtx {
    Disp d;
    lv_obj_t* tile;
    lv_obj_t* unit_lbl;
    Colors colors;
  };
  auto* vctx = new ValueCtx{parse_display(spec), root, unit_lbl, colors};
  lv_obj_set_user_data(val, vctx);
  lv_obj_add_event_cb(
      val,
      [](lv_event_t* e) {
        delete static_cast<ValueCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);
  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* vc = static_cast<ValueCtx*>(lv_obj_get_user_data(w));
        float raw = lv_subject_get_float(s);
        float v = raw * vc->d.scale + vc->d.offset;
        // Big number: just the formatted value, no unit (unit sits
        // in its own bottom-right label so it doesn't grow / shrink
        // the centered text when the value width changes).
        lv_label_set_text_fmt(w, "%.*f", vc->d.decimals, v);
        // unit_lbl and tile are SIBLING objects, not the observer's
        // own target. During a layout swap LVGL can free them before
        // this observer's target label, and the synchronous notify
        // from lv_subject_add_observer_obj (or a value delta arriving
        // mid-swap) then derefs freed memory → Load access fault in
        // get_local_style. Guard each sibling; `w` is always valid
        // (the observer auto-unsubscribes on its target's delete).
        if (lv_obj_is_valid(vc->unit_lbl)) {
          // Unit label: kept separate; set once is enough but cheap.
          lv_label_set_text(vc->unit_lbl, vc->d.unit);
        }
        if (!lv_obj_is_valid(vc->tile)) return;
        // Zone tint paints the whole tile bg. Raw value matched
        // against raw-unit zones (firmware convention). Falls back
        // to spec'd bg_color when no zone matches.
        uint32_t bg = zone_color(vc->d.path, raw, vc->colors.bg);
        lv_obj_set_style_bg_color(vc->tile, lv_color_hex(bg), LV_PART_MAIN);
      },
      val, nullptr);

  return root;
}

// ---- toggle ----
//
// Requires `bind` (Int subject). No optimistic latch — visual state
// follows the subscription only. PUT path (tap → server PUT → echo
// back) lands in step 7.
namespace {
// Switch-tile chrome, shared by @audio_mute and the mute_speaker/mute_mic
// tiles so their captions get the same treatment.
constexpr int kSwitchW = 60;
constexpr int kSwitchH = 30;
constexpr int kTilePad = 8;

// Space left for the caption once the switch and padding are taken. Layouts
// may omit w (JLP default 120), which leaves only ~44 px — less than
// "SPEAKER" needs — so the caller clips instead of drawing under the switch.
int label_width_for(JsonObjectConst spec) {
  const int w = spec["w"] | 120;
  const int avail = w - (kTilePad * 2) - kSwitchW - 4;  // 4 = breathing room
  return avail > 16 ? avail : 16;
}
}  // namespace

// Panel-local audio-mute toggle (bind "@audio_mute"). Same look as a
// normal toggle, but ON = muted (chime suppressed on this panel, current
// and future) with no SK path behind it — it reads and writes
// chime().muted() directly. Authoritative local state, so no
// subscription and no reconcile timer.
//
// Unlike the mute_speaker/mute_mic tiles, this one stays ON = muted: its
// caption says what the switch DOES ("MUTE CHIME"), not what the hardware
// is, so ON reading as "muting" is the consistent reading. Those two are
// captioned SPEAKER/MIC and had to invert to match.
lv_obj_t* build_audio_mute_toggle(BuildCtx& ctx, JsonObjectConst spec,
                                  std::string* err) {
  // Only one mute control per layout: each switch snapshots
  // chime().muted() at build time, so two would drift out of sync (tap
  // one, the other's visual is stale and inverts on its next tap).
  // live_paths is layout-scoped, so this resets on each apply.
  if (!ctx.live_paths.insert("@audio_mute").second) {
    *err = "toggle: only one @audio_mute per layout";
    return nullptr;
  }
  const Colors colors = parse_colors(spec);
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  const char* caption = spec["label"] | "MUTE CHIME";
  lv_obj_t* l = lv_label_create(root);
  lv_obj_set_style_text_color(l, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(l, font_from_spec(spec, &lv_font_montserrat_20),
                             LV_PART_MAIN);
  lv_label_set_text(l, caption);
  // "MUTE CHIME" is wider than the default tile leaves room for; clip rather
  // than run under the switch.
  lv_obj_set_width(l, label_width_for(spec));
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* sw = lv_switch_create(root);
  lv_obj_set_size(sw, kSwitchW, kSwitchH);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (chime().muted()) lv_obj_add_state(sw, LV_STATE_CHECKED);

  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
        bool on = lv_obj_has_state(w, LV_STATE_CHECKED);  // post-flip state
        chime().set_muted(on);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  return root;
}

namespace {
// Shared chrome for a panel-local toggle tile (caption left, switch right,
// no SK path). Returns the switch so the caller wires its VALUE_CHANGED.
lv_obj_t* make_local_toggle(BuildCtx& ctx, JsonObjectConst spec,
                            const char* default_caption, bool initial_on) {
  const Colors colors = parse_colors(spec);
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  const char* caption = spec["label"] | default_caption;
  lv_obj_t* l = lv_label_create(root);
  lv_obj_set_style_text_color(l, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(l, font_from_spec(spec, &lv_font_montserrat_20),
                             LV_PART_MAIN);
  lv_label_set_text(l, caption);
  // Bound the caption to what is left after the switch and padding, and dot
  // it rather than let it run under the switch: at the documented default
  // width (120) only ~44 px remain, which "SPEAKER" already exceeds.
  lv_obj_set_width(l, label_width_for(spec));
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* sw = lv_switch_create(root);
  lv_obj_set_size(sw, kSwitchW, kSwitchH);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (initial_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_set_user_data(root, sw);  // let callers return root but reach sw
  return root;
}
}  // namespace

// `speaker` — panel-local speaker switch. ON = speaker works, OFF = silent.
// Accepts the older `mute_speaker` kind as an alias; the tile behaved this way
// under that name too, which is exactly why it was renamed.
lv_obj_t* build_speaker(BuildCtx& ctx, JsonObjectConst spec,
                        std::string* err) {
  // One per layout: each switch snapshots voice().speaker_muted() at build
  // time, so a second would drift out of sync (like @audio_mute). The
  // sentinel keeps its old name deliberately -- it never appears on the wire,
  // and sharing it with the mute_speaker alias is what stops a layout using
  // both spellings from getting two switches.
  if (!ctx.live_paths.insert("@mute_speaker").second) {
    *err = "speaker: only one per layout";
    return nullptr;
  }
  lv_obj_t* root =
      make_local_toggle(ctx, spec, "SPEAKER", !voice().speaker_muted());
  lv_obj_t* sw = static_cast<lv_obj_t*>(lv_obj_get_user_data(root));
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
        voice().set_speaker_muted(!lv_obj_has_state(w, LV_STATE_CHECKED));
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  return root;
}

// `mic` — panel-local mic switch / privacy control. ON = mic live, OFF = the
// mic never streams (push-to-talk and wake-word both suppressed). Accepts the
// older `mute_mic` kind as an alias.
lv_obj_t* build_mic(BuildCtx& ctx, JsonObjectConst spec,
                    std::string* err) {
  // One per layout; shared with the mute_mic alias. See build_speaker.
  if (!ctx.live_paths.insert("@mute_mic").second) {
    *err = "mic: only one per layout";
    return nullptr;
  }
  lv_obj_t* root = make_local_toggle(ctx, spec, "MIC", !voice().mic_muted());
  lv_obj_t* sw = static_cast<lv_obj_t*>(lv_obj_get_user_data(root));
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
        voice().set_mic_muted(!lv_obj_has_state(w, LV_STATE_CHECKED));
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  return root;
}

// `volume` — draggable speaker-volume slider (0-100), applied at the codec via
// voice().set_volume. Panel-local; caption above the bar.
lv_obj_t* build_volume(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  (void)err;
  const Colors colors = parse_colors(spec);
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  const char* caption = spec["label"] | "VOLUME";
  lv_obj_t* l = lv_label_create(root);
  lv_obj_set_style_text_color(l, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(l, font_from_spec(spec, &lv_font_montserrat_20),
                             LV_PART_MAIN);
  lv_label_set_text(l, caption);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* sld = lv_slider_create(root);
  lv_slider_set_range(sld, 0, 100);
  lv_slider_set_value(sld, voice().volume(), LV_ANIM_OFF);
  lv_obj_set_width(sld, LV_PCT(100));
  lv_obj_align(sld, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(sld, lv_color_hex(colors.fg), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sld, lv_color_hex(colors.fg), LV_PART_KNOB);

  // Track the drag live so the level follows the knob, but only write it to
  // NVS on release: VALUE_CHANGED fires per pixel, and persisting each step
  // would put a whole sweep's worth of writes through the flash.
  lv_obj_add_event_cb(
      sld,
      [](lv_event_t* e) {
        auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
        voice().set_volume((uint8_t)lv_slider_get_value(w), /*persist=*/false);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  auto commit_cb = [](lv_event_t* e) {
    auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
    voice().set_volume((uint8_t)lv_slider_get_value(w), /*persist=*/true);
  };
  lv_obj_add_event_cb(sld, commit_cb, LV_EVENT_RELEASED, nullptr);
  // A drag that leaves the widget never emits RELEASED; without this the
  // last level would apply but never persist.
  lv_obj_add_event_cb(sld, commit_cb, LV_EVENT_PRESS_LOST, nullptr);
  return root;
}

lv_obj_t* build_toggle(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const char* path = spec["bind"] | (const char*)nullptr;
  if (!path) { *err = "toggle: bind required"; return nullptr; }
  const Colors colors = parse_colors(spec);

  // Local action sentinel: bind "@audio_mute" makes the toggle a
  // panel-local audio mute — it reflects/flips chime().muted() instead
  // of a SK path, with no PUT and no subscription. Handled entirely in
  // build_audio_mute_toggle to keep the SK-backed path below clean.
  if (std::string(path) == "@audio_mute") {
    return build_audio_mute_toggle(ctx, spec, err);
  }

  lv_subject_t* sub = ctx.reg.get_or_create(path, SubjectKind::Int);
  if (!sub) { *err = std::string("kind conflict on ") + path; return nullptr; }
  ctx.live_paths.insert(path);

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  // LVGL default theme adds a 1-2 px outline + a soft shadow around
  // every lv_obj. Both extend past the geometric bounding box and
  // make tightly-spaced tiles look loose on the device (designer
  // doesn't replicate them). Zero both so the visible tile matches
  // the JSON (x,y,w,h) 1:1.
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // Inline layout: caption flushed left and vertically centered,
  // switch flushed right and vertically centered. Switch takes a
  // fixed comfortable touch size; caption fills the rest.
  const char* caption = spec["label"] | (const char*)nullptr;
  if (caption) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_style_text_color(l, lv_color_hex(colors.fg), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        l, font_from_spec(spec, &lv_font_montserrat_20), LV_PART_MAIN);
    lv_label_set_text(l, caption);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }

  lv_obj_t* sw = lv_switch_create(root);
  lv_obj_set_size(sw, 60, 30);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        if (lv_subject_get_int(s)) lv_obj_add_state(w, LV_STATE_CHECKED);
        else                       lv_obj_remove_state(w, LV_STATE_CHECKED);
      },
      sw, nullptr);

  // Tile background takes the zone color of the current int value, if
  // the bound path has zones. No-op for typical bool switches.
  struct ToggleObsCtx {
    Disp d;
    Colors colors;
  };
  auto* d_root = new ToggleObsCtx{parse_display(spec), colors};
  lv_obj_set_user_data(root, d_root);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        delete static_cast<ToggleObsCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);
  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* tc = static_cast<ToggleObsCtx*>(lv_obj_get_user_data(w));
        int32_t raw = lv_subject_get_int(s);
        // Zones live in raw SK units; match against raw, not display.
        // Fall back to the spec'd bg_color when no zone matches.
        uint32_t c = zone_color(tc->d.path, (float)raw, tc->colors.bg);
        lv_obj_set_style_bg_color(w, lv_color_hex(c), LV_PART_MAIN);
      },
      root, nullptr);

  // Click → send PUT for the opposite of what the SUBSCRIPTION says
  // (not what LVGL just latched, since lv_switch flips itself on click
  // before our handler fires). Let LVGL's optimistic visual flip stand
  // briefly so the tap feels responsive, then 500ms later reconcile
  // against the subject — if no echo arrived, snap back to truth.
  // 500ms is comfortably above the ~300ms Maretron 126208 ACK round
  // trip so a successful command doesn't trigger a visible re-flip.
  struct TogglePressCtx {
    std::string path;
    lv_subject_t* sub;
    lv_obj_t* sw;
    lv_timer_t* reconcile;
  };
  auto* ctx_owned = new TogglePressCtx{path, sub, sw, nullptr};
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        auto* c = static_cast<TogglePressCtx*>(lv_event_get_user_data(e));
        bool was_on = lv_subject_get_int(c->sub) != 0;
        put_bool(c->path, !was_on);
        // Cancel any prior pending reconcile — user can tap again
        // before the previous one fires.
        if (c->reconcile) {
          lv_timer_delete(c->reconcile);
          c->reconcile = nullptr;
        }
        c->reconcile = lv_timer_create(
            [](lv_timer_t* t) {
              auto* c = static_cast<TogglePressCtx*>(lv_timer_get_user_data(t));
              bool sub_on = lv_subject_get_int(c->sub) != 0;
              if (sub_on) lv_obj_add_state(c->sw, LV_STATE_CHECKED);
              else        lv_obj_remove_state(c->sw, LV_STATE_CHECKED);
              lv_timer_delete(t);
              c->reconcile = nullptr;
            },
            500, c);
        lv_timer_set_repeat_count(c->reconcile, 1);
      },
      LV_EVENT_CLICKED, ctx_owned);
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        auto* c = static_cast<TogglePressCtx*>(lv_event_get_user_data(e));
        if (c->reconcile) lv_timer_delete(c->reconcile);
        delete c;
      },
      LV_EVENT_DELETE, ctx_owned);

  return root;
}

// ---- shared user_data struct for arc and bar ----
struct RangeBinding {
  Disp display;
  float min;  // display-space
  float max;
  Colors colors;  // bg/fg overrides; zone match still wins
};

// ---- arc ----
//
// Layout: a transparent container of the user's geometry holds the
// arc widget (sized to fill, ignoring clicks) plus two child labels
// for caption and value. The labels are siblings of the arc, not
// children — `lv_arc` clips children to its arc shape which hides
// any centered text.
lv_obj_t* build_arc(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const char* path = spec["bind"] | (const char*)nullptr;
  if (!path) { *err = "arc: bind required"; return nullptr; }
  const Colors colors = parse_colors(spec);
  lv_subject_t* sub = ctx.reg.get_or_create(path, SubjectKind::Float);
  if (!sub) { *err = std::string("kind conflict on ") + path; return nullptr; }
  ctx.live_paths.insert(path);

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // Arcs are circular: take min(w,h) so they stay round regardless of
  // the user's bounding box. Extra width/height becomes empty space
  // around the arc (caption + value still center on root, which lines
  // them up inside the squared arc since the arc is centered too).
  int box_w = spec["w"] | 120;
  int box_h = spec["h"] | 60;
  int side = box_w < box_h ? box_w : box_h;
  int sa = spec["start_angle"] | 135;
  int ea = spec["end_angle"] | 45;
  float v_min = spec["min"] | 0.f;
  float v_max = spec["max"] | 100.f;
  Disp tmp_disp = parse_display(spec);

  // Total sweep, normalised to 0..360. Bands map their (from, to)
  // values into the same sweep.
  int total_sweep = ea - sa;
  if (total_sweep <= 0) total_sweep += 360;

  // ---- Bands (advisory colored ring painted UNDER the indicator).
  // Each band is its own lv_arc with no indicator and a thin track
  // styled in the band's color. Created before the indicator so the
  // indicator paints on top.
  JsonArrayConst bands = spec["bands"];
  if (!bands.isNull()) {
    for (JsonObjectConst b : bands) {
      float from = b["from"] | 0.f;
      float to = b["to"] | 0.f;
      const char* hex = b["color"] | "#3fb950";
      if (to < from) { float t = to; to = from; from = t; }
      // Map [from, to] in display-space back to arc angle range.
      float span = v_max - v_min;
      if (span <= 0) continue;
      float t0 = (from * tmp_disp.scale + tmp_disp.offset - v_min) / span;
      float t1 = (to   * tmp_disp.scale + tmp_disp.offset - v_min) / span;
      if (t0 < 0) t0 = 0;
      if (t1 > 1) t1 = 1;
      if (t1 <= t0) continue;
      int ang0 = sa + (int)(total_sweep * t0);
      int ang1 = sa + (int)(total_sweep * t1);
      uint32_t color = 0x3fb950;
      parse_hex_color(hex, &color);

      lv_obj_t* band = lv_arc_create(root);
      lv_obj_set_size(band, side, side);
      lv_obj_align(band, LV_ALIGN_CENTER, 0, 0);
      lv_arc_set_bg_angles(band, ang0 % 360, ang1 % 360);
      // Zero the indicator — we only want the bg ring visible.
      lv_arc_set_angles(band, ang0 % 360, ang0 % 360);
      lv_obj_remove_style(band, NULL, LV_PART_KNOB);
      lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_arc_color(band, lv_color_hex(color), LV_PART_MAIN);
      lv_obj_set_style_arc_width(band, 4, LV_PART_MAIN);
    }
  }

  // ---- Tick marks (drawn via small line segments).
  // Major ticks at evenly-spaced angles around the arc. No labels in
  // v1 to keep the firmware light; designer can show tick numerals
  // since SVG text is cheap on the browser.
  int tick_count = spec["ticks"] | 0;
  if (tick_count > 1) {
    float r_outer = side / 2.0f;
    float r_inner = r_outer - 6.0f;
    if (r_inner < 0) r_inner = 0;
    float cx = side / 2.0f;
    float cy = side / 2.0f;
    for (int i = 0; i < tick_count; i++) {
      float t = (float)i / (float)(tick_count - 1);
      float a = sa + total_sweep * t;
      float rad = a * 3.14159265f / 180.0f;
      // lv_line stores the points pointer rather than copying — each
      // tick needs its own backing buffer that lives as long as the
      // line object. Heap-allocate and free in LV_EVENT_DELETE.
      lv_obj_t* tick = lv_line_create(root);
      auto* tick_pts = new lv_point_precise_t[2];
      tick_pts[0].x = (lv_value_precise_t)(cx + r_inner * cosf(rad)
                                           + (box_w - side) / 2);
      tick_pts[0].y = (lv_value_precise_t)(cy + r_inner * sinf(rad)
                                           + (box_h - side) / 2);
      tick_pts[1].x = (lv_value_precise_t)(cx + r_outer * cosf(rad)
                                           + (box_w - side) / 2);
      tick_pts[1].y = (lv_value_precise_t)(cy + r_outer * sinf(rad)
                                           + (box_h - side) / 2);
      lv_line_set_points(tick, tick_pts, 2);
      lv_obj_add_event_cb(
          tick,
          [](lv_event_t* e) {
            delete[] static_cast<lv_point_precise_t*>(
                lv_event_get_user_data(e));
          },
          LV_EVENT_DELETE, tick_pts);
      lv_obj_set_style_line_color(tick, lv_color_hex(kMutedHex), LV_PART_MAIN);
      lv_obj_set_style_line_width(tick, 1, LV_PART_MAIN);
    }
  }

  lv_obj_t* arc = lv_arc_create(root);
  lv_obj_set_size(arc, side, side);
  lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_range(arc, 0, kBarSteps);
  lv_arc_set_bg_angles(arc, sa, ea);
  lv_arc_set_angles(arc, sa, sa);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(kAccentHex), LV_PART_INDICATOR);
  // Pin the arc track + indicator width to ~8% of the arc side so the
  // device matches the designer's SVG (stroke=8 in a 100-unit
  // viewBox). LVGL 9's default theme uses a fixed ~25 px which
  // overshadows small arcs and makes adjacent arcs visually collide
  // even when the layout coords don't overlap.
  const int arc_stroke = side / 12;  // ~8%
  lv_obj_set_style_arc_width(arc, arc_stroke, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, arc_stroke, LV_PART_INDICATOR);
  // Also lift the inactive bg arc above the bands so the bands sit
  // visibly OUTSIDE rather than fighting the track. Re-pin the arc
  // on top by setting it as the parent's last child via z-order:
  lv_obj_move_foreground(arc);
  (void)v_min; (void)v_max;  // captured by RangeBinding below

  auto* rb_arc = new RangeBinding{parse_display(spec),
                                  spec["min"] | 0.f, spec["max"] | 100.f,
                                  colors};
  lv_obj_set_user_data(arc, rb_arc);
  auto free_rb = [](lv_event_t* e) {
    delete static_cast<RangeBinding*>(lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_target(e))));
  };
  lv_obj_add_event_cb(arc, free_rb, LV_EVENT_DELETE, nullptr);
  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* rb = static_cast<RangeBinding*>(lv_obj_get_user_data(w));
        float raw = lv_subject_get_float(s);
        float v = raw * rb->display.scale + rb->display.offset;
        lv_arc_set_value(w, scale_to_steps(v, rb->min, rb->max));
        // Zones live in raw SK units; match against raw, not display.
        // Fall back to fg_color (which doubles as the indicator color)
        // when no zone matches, else default accent.
        uint32_t fallback = rb->colors.fg != kFgHex ? rb->colors.fg : kAccentHex;
        uint32_t c = zone_color(rb->display.path, raw, fallback);
        lv_obj_set_style_arc_color(w, lv_color_hex(c), LV_PART_INDICATOR);
      },
      arc, nullptr);

  // Caption + value as siblings of the arc, both centered on root.
  // Caption sits above the value when present.
  const char* caption = spec["label"] | (const char*)nullptr;
  lv_obj_t* val = lv_label_create(root);
  lv_obj_set_style_text_color(val, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_label_set_text(val, "—");
  lv_obj_center(val);
  if (caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align_to(cap, val, LV_ALIGN_OUT_TOP_MID, 0, -2);
  }

  auto* rb_val = new RangeBinding(*rb_arc);
  lv_obj_set_user_data(val, rb_val);
  lv_obj_add_event_cb(val, free_rb, LV_EVENT_DELETE, nullptr);
  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* rb = static_cast<RangeBinding*>(lv_obj_get_user_data(w));
        float v = lv_subject_get_float(s) * rb->display.scale + rb->display.offset;
        lv_label_set_text_fmt(w, "%.*f %s", rb->display.decimals, v,
                              rb->display.unit);
      },
      val, nullptr);

  return root;
}

// ---- bar ----
lv_obj_t* build_bar(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const char* path = spec["bind"] | (const char*)nullptr;
  if (!path) { *err = "bar: bind required"; return nullptr; }
  const Colors colors = parse_colors(spec);
  lv_subject_t* sub = ctx.reg.get_or_create(path, SubjectKind::Float);
  if (!sub) { *err = std::string("kind conflict on ") + path; return nullptr; }
  ctx.live_paths.insert(path);

  // Tile-style frame so the bar is identifiable as a widget even with
  // no live value (when value is 0 the indicator doesn't draw; only
  // the track shows). Caption + value text always visible.
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(root, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  bool vertical = spec["vertical"] | false;

  const char* caption = spec["label"] | (const char*)nullptr;
  if (caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  lv_obj_t* val = lv_label_create(root);
  lv_obj_set_style_text_color(val, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_text(val, "—");
  lv_obj_align(val, LV_ALIGN_TOP_RIGHT, 0, 0);

  // Bar takes the rest of the box.
  lv_obj_t* bar = lv_bar_create(root);
  lv_bar_set_range(bar, 0, kBarSteps);
  // Track: medium grey, clearly visible against tile bg.
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(kAccentHex), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  if (vertical) {
    lv_obj_set_size(bar, 24, lv_pct(75));
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  } else {
    lv_obj_set_size(bar, lv_pct(100), 24);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  }

  // Two heap copies: bar's observer reads bar's user_data, val's
  // observer reads val's user_data. Each widget owns its copy.
  auto* rb_bar = new RangeBinding{parse_display(spec),
                                  spec["min"] | 0.f, spec["max"] | 100.f,
                                  colors};
  auto* rb_val = new RangeBinding(*rb_bar);
  lv_obj_set_user_data(bar, rb_bar);
  lv_obj_set_user_data(val, rb_val);
  auto free_rb = [](lv_event_t* e) {
    delete static_cast<RangeBinding*>(lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_target(e))));
  };
  lv_obj_add_event_cb(bar, free_rb, LV_EVENT_DELETE, nullptr);
  lv_obj_add_event_cb(val, free_rb, LV_EVENT_DELETE, nullptr);

  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* rb = static_cast<RangeBinding*>(lv_obj_get_user_data(w));
        float raw = lv_subject_get_float(s);
        float v = raw * rb->display.scale + rb->display.offset;
        lv_bar_set_value(w, scale_to_steps(v, rb->min, rb->max), LV_ANIM_OFF);
        // Zones live in raw SK units; match against raw, not display.
        // Fall back to fg_color override (indicator color) when no
        // zone matches, else default accent.
        uint32_t fallback = rb->colors.fg != kFgHex ? rb->colors.fg : kAccentHex;
        uint32_t c = zone_color(rb->display.path, raw, fallback);
        lv_obj_set_style_bg_color(w, lv_color_hex(c), LV_PART_INDICATOR);
      },
      bar, nullptr);
  lv_subject_add_observer_obj(
      sub,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* rb = static_cast<RangeBinding*>(lv_obj_get_user_data(w));
        float v = lv_subject_get_float(s) * rb->display.scale + rb->display.offset;
        lv_label_set_text_fmt(w, "%.*f %s", rb->display.decimals, v,
                              rb->display.unit);
      },
      val, nullptr);

  return root;
}

// ---- bargroup ----
//
// N vertical bars under a single caption. Each sub-bar binds
// independently (its own SK path, min/max, display) and gets its
// own SK-zone tinting from the live registry. The container handles
// caption + horizontal layout; each sub-bar reuses the RangeBinding
// observer pattern from build_bar.
//
// Per-cell layout (top→bottom):
//   - max-value tick (small, top-right of the bar track)
//   - bar track (rectangle, fills bottom-up, zone-tinted)
//     * live value text centered on the bar (white-on-tint)
//   - min-value tick (small, bottom-right of the bar track)
//   - per-bar caption (centered under the cell)
struct BarCellBinding {
  RangeBinding range;       // existing range/zone wiring reused
  lv_obj_t* value_label;    // live formatted value drawn on the bar
};

lv_obj_t* build_bargroup(BuildCtx& ctx, JsonObjectConst spec,
                         std::string* err) {
  const Colors colors = parse_colors(spec);
  JsonArrayConst bars = spec["bars"];
  if (bars.isNull() || bars.size() == 0) {
    *err = "bargroup: bars[] required";
    return nullptr;
  }

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(root, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // Group caption flushed top-left.
  const char* caption = spec["label"] | (const char*)nullptr;
  int caption_h = 0;
  if (caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
    caption_h = 20;
  }

  // Per-bar geometry: equal slots across the inner width, beneath
  // the caption. Each cell has a vertical bar that fills its track
  // from the bottom plus a label below. Side ticks for min/max go
  // alongside the bar on the right (when there's room).
  int inner_w = (spec["w"] | 120) - 16;        // pad_all=8
  int inner_h = (spec["h"] | 60) - 16 - caption_h;
  int n = (int)bars.size();
  if (n <= 0) n = 1;
  int slot_w = inner_w / n;
  int bar_h = inner_h - 24;  // label below
  if (bar_h < 20) bar_h = 20;

  int idx = 0;
  for (JsonObjectConst bspec : bars) {
    const char* b_path = bspec["bind"] | (const char*)nullptr;
    if (!b_path) { idx++; continue; }
    lv_subject_t* sub = ctx.reg.get_or_create(b_path, SubjectKind::Float);
    if (!sub) {
      *err = std::string("kind conflict on ") + b_path;
      return nullptr;
    }
    ctx.live_paths.insert(b_path);

    // Bar sits on the LEFT of the cell; ticks fill the right gap.
    int cell_x = slot_w * idx + 2;
    int cell_y = caption_h;

    // Pull the per-bar display + range now so we can compute the
    // tick width before laying out the bar itself.
    Disp d{1.f, 0.f, 0, "", ""};
    JsonObjectConst display = bspec["display"];
    if (!display.isNull()) {
      d.scale = display["scale"] | 1.f;
      d.offset = display["offset"] | 0.f;
      d.decimals = display["decimals"] | 0;
      snprintf(d.unit, sizeof(d.unit), "%s", display["unit"] | "");
    }
    snprintf(d.path, sizeof(d.path), "%s", b_path);
    float v_min = bspec["min"] | 0.f;
    float v_max = bspec["max"] | 100.f;
    auto fmt_tick = [&](float v) {
      char buf[24];
      // The range fields are in display-space already (matches the
      // standalone bar widget convention) — don't re-apply scale.
      snprintf(buf, sizeof(buf), "%.*f", d.decimals, v);
      return std::string(buf);
    };
    std::string min_text = fmt_tick(v_min);
    std::string max_text = fmt_tick(v_max);
    // Resize bar dynamically per-cell: longer tick text (negatives,
    // 4+ digit values) needs more reserve. ~7 px per char in
    // Montserrat 14.
    // Tick column has to fit the widest of: min text, max text, and
    // the unit text (rendered on a separate row below the max tick).
    // ~7 px per char in Montserrat 14 is a safe upper bound.
    size_t unit_len = d.unit[0] ? std::strlen(d.unit) : 0;
    size_t widest = std::max({min_text.size(), max_text.size(), unit_len});
    int tick_reserve = std::max(30, (int)widest * 7 + 4);
    int bar_w = slot_w - tick_reserve - 6;
    if (bar_w < 14) bar_w = 14;
    if (bar_w > slot_w - 12) bar_w = slot_w - 12;

    lv_obj_t* bar = lv_bar_create(root);
    lv_obj_set_size(bar, bar_w, bar_h);
    lv_obj_set_pos(bar, cell_x, cell_y);
    lv_bar_set_range(bar, 0, kBarSteps);
    // When the range straddles zero we render the fill as a signed
    // delta from the zero baseline (LV_BAR_MODE_RANGE), so a value
    // of -5 on a [-10..10] bar shows a fill from the midline going
    // down rather than a fill from the bottom going up to 40%.
    bool signed_range = v_min < 0.f && v_max > 0.f;
    if (signed_range) {
      lv_bar_set_mode(bar, LV_BAR_MODE_RANGE);
    }
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    // LVGL's default theme uses LV_RADIUS_CIRCLE on bars, which turns
    // small near-square bars into pills/circles. Force a small fixed
    // radius so the bar reads as a rectangle at every aspect.
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kAccentHex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    // Vertical: lv_bar's default mode is horizontal; size+orientation
    // is determined by the widget aspect, so a tall narrow bar fills
    // from the top by default — we want bottom-up.
    lv_bar_set_orientation(bar, LV_BAR_ORIENTATION_VERTICAL);

    // Zero baseline marker for signed ranges. Just a "0" text label
    // at the y-coord that corresponds to v=0 on the right side of
    // the bar — same column as the min/max ticks. The bar's RANGE
    // fill (below) naturally produces the visual "above / below
    // zero" cue; an extra horizontal line on the track would be
    // covered by the indicator anyway.
    if (signed_range) {
      // y = top_of_bar + bar_h * (1 - (0 - v_min) / (v_max - v_min))
      float frac = (0.f - v_min) / (v_max - v_min);
      int baseline_y = cell_y + bar_h - (int)(bar_h * frac);
      lv_obj_t* zero_lbl = lv_label_create(root);
      lv_obj_set_style_text_color(zero_lbl, lv_color_hex(0x8b949e),
                                  LV_PART_MAIN);
      lv_obj_set_style_text_font(zero_lbl, &lv_font_montserrat_14,
                                 LV_PART_MAIN);
      lv_label_set_text(zero_lbl, "0");
      lv_obj_set_pos(zero_lbl, cell_x + bar_w + 4, baseline_y - 8);
    }

    // Live value text drawn ON the bar (centered).
    lv_obj_t* val_label = lv_label_create(root);
    lv_obj_set_style_text_color(val_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(val_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(val_label, "—");
    lv_obj_set_width(val_label, bar_w);
    lv_obj_set_style_text_align(val_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(val_label, cell_x, cell_y + bar_h / 2 - 8);

    // Min/max tick labels on the right of the bar. Auto-size them so
    // negative / many-digit values don't wrap; we reserved room
    // above. Place them flush-right of the reserved area so the
    // numbers visually align with the top / bottom of the bar.
    int tick_x = cell_x + bar_w + 4;
    int tick_w = slot_w - bar_w - 6;
    lv_obj_t* max_tick = lv_label_create(root);
    lv_obj_set_style_text_color(max_tick, lv_color_hex(kMutedHex),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(max_tick, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_label_set_text(max_tick, max_text.c_str());
    lv_obj_set_pos(max_tick, tick_x, cell_y - 2);
    // Don't constrain width — let LVGL size to text content so long
    // negative values don't wrap. Cap height to one line.
    lv_obj_set_height(max_tick, 16);
    lv_obj_t* min_tick = lv_label_create(root);
    lv_obj_set_style_text_color(min_tick, lv_color_hex(kMutedHex),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(min_tick, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_label_set_text(min_tick, min_text.c_str());
    lv_obj_set_pos(min_tick, tick_x, cell_y + bar_h - 16);
    lv_obj_set_height(min_tick, 16);
    // Unit on its own line below the max tick so it stays inside
    // the cell even for wide units ("hour", "kWh") that would
    // otherwise overflow into the next slot or get clipped at the
    // tile edge. Same muted color as the ticks.
    if (d.unit[0]) {
      lv_obj_t* unit_lbl = lv_label_create(root);
      lv_obj_set_style_text_color(unit_lbl, lv_color_hex(kMutedHex),
                                  LV_PART_MAIN);
      lv_obj_set_style_text_font(unit_lbl, &lv_font_montserrat_14,
                                 LV_PART_MAIN);
      lv_label_set_text(unit_lbl, d.unit);
      lv_obj_set_pos(unit_lbl, tick_x, cell_y + 14);
      lv_obj_set_height(unit_lbl, 16);
    }
    (void)tick_w;

    auto* bcb = new BarCellBinding{
        RangeBinding{d, v_min, v_max, colors}, val_label};
    lv_obj_set_user_data(bar, bcb);
    if (signed_range) {
      // Pre-init the start to the zero-steps position; the observer
      // sets only the end on each update. RANGE mode draws the fill
      // BETWEEN start and end, so anchoring start at zero produces
      // the "band from zero" behavior.
      int32_t zero_steps = scale_to_steps(0.f, v_min, v_max);
      lv_bar_set_start_value(bar, zero_steps, LV_ANIM_OFF);
      lv_bar_set_value(bar, zero_steps, LV_ANIM_OFF);
    }
    lv_obj_add_event_cb(
        bar,
        [](lv_event_t* e) {
          delete static_cast<BarCellBinding*>(lv_obj_get_user_data(
              static_cast<lv_obj_t*>(lv_event_get_target(e))));
        },
        LV_EVENT_DELETE, nullptr);
    lv_subject_add_observer_obj(
        sub,
        [](lv_observer_t* obs, lv_subject_t* s) {
          auto* w = lv_observer_get_target_obj(obs);
          auto* bcb = static_cast<BarCellBinding*>(lv_obj_get_user_data(w));
          auto& rb = bcb->range;
          float raw = lv_subject_get_float(s);
          float v = raw * rb.display.scale + rb.display.offset;
          int32_t v_steps = scale_to_steps(v, rb.min, rb.max);
          if (rb.min < 0.f && rb.max > 0.f) {
            // Signed range: draw the fill from the zero baseline to
            // v, regardless of sign. start = min(zero, v); end =
            // max(zero, v).
            int32_t zero_steps = scale_to_steps(0.f, rb.min, rb.max);
            int32_t start = v_steps < zero_steps ? v_steps : zero_steps;
            int32_t end   = v_steps > zero_steps ? v_steps : zero_steps;
            lv_bar_set_start_value(w, start, LV_ANIM_OFF);
            lv_bar_set_value(w, end, LV_ANIM_OFF);
          } else {
            lv_bar_set_value(w, v_steps, LV_ANIM_OFF);
          }
          uint32_t fallback = rb.colors.fg != kFgHex ? rb.colors.fg
                                                    : kAccentHex;
          uint32_t c = zone_color(rb.display.path, raw, fallback);
          lv_obj_set_style_bg_color(w, lv_color_hex(c), LV_PART_INDICATOR);
          // Live value text. We deliberately omit the unit here — the
          // unit appears once next to the ticks; repeating it on every
          // bar would crowd the small cell.
          char buf[24];
          snprintf(buf, sizeof(buf), "%.*f", rb.display.decimals, v);
          lv_label_set_text(bcb->value_label, buf);
        },
        bar, nullptr);

    // Per-bar caption below the bar. Width = bar_w so the text
    // centers under the bar itself, NOT the whole slot (which
    // includes the tick column on the right). Otherwise the
    // caption visually shifts right of where the operator expects
    // it.
    const char* b_label = bspec["label"] | "";
    if (*b_label) {
      lv_obj_t* lbl = lv_label_create(root);
      lv_obj_set_style_text_color(lbl, lv_color_hex(kMutedHex), LV_PART_MAIN);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_label_set_text(lbl, b_label);
      lv_obj_set_pos(lbl, cell_x, cell_y + bar_h + 4);
      lv_obj_set_width(lbl, bar_w);
      lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    idx++;
  }

  return root;
}

// ---- button ----
//
// Momentary + hold-to-act semantics:
//   - PRESSED fires put(press_value) if hold_ms == 0; else starts a
//     timer that fires the PUT after hold_ms. A visual fill animates
//     during the hold so the operator sees the latch loading.
//   - RELEASED fires put(release_value) if release_value is set AND
//     the press already fired. Releasing during a pending hold
//     cancels the press timer with no PUT (intended).
//
// Value typing: press_value / release_value can be a bool, integer,
// float, or string in the JSON spec. We route to the matching put_*
// helper. If `bind` is omitted, the button is a no-op (action-only
// PUTs would require an `action.put.path` — that landed in v0.1
// designer but firmware never honoured it; reintroduce later if
// asked).

void put_json_value(const std::string& path, JsonVariantConst v) {
  if (v.isNull())   { put_null(path);              return; }
  if (v.is<bool>()) { put_bool(path, v.as<bool>()); return; }
  if (v.is<int>())  { put_int(path,  v.as<int>());  return; }
  if (v.is<float>()){ put_float(path,v.as<float>());return; }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s) put_string(path, s);
    return;
  }
  // Unknown non-null type — silently skip rather than crash.
}

struct ButtonCtx {
  std::string path;
  // Stored as the literal JSON tokens; re-parsed via a small
  // JsonDocument when fired. Simple, and avoids carrying a 4-way
  // variant union.
  std::string press_json;    // empty -> no press PUT
  std::string release_json;  // empty -> no release PUT
  uint32_t hold_ms;
  lv_timer_t* hold_timer;    // pending press; nullptr otherwise
  lv_obj_t* root;            // for redraw on press/release state
  bool press_fired;
};

void button_fire(const std::string& path, const std::string& json,
                 bool is_press) {
  // Local action sentinel: a button bound to "@drop_here" drops the
  // anchor at the boat's current fix (the panel can't compose a lat/lon
  // from a fixed press_value, so it fetches the position and PUTs it).
  // Fire on PRESS only — the release call would otherwise start a
  // second drop task. press_value is ignored; hold_ms latches as a
  // safety guard.
  if (path == "@drop_here") {
    if (is_press) drop_anchor_here();
    return;
  }
  if (json.empty()) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;
  put_json_value(path, doc.as<JsonVariantConst>());
}

lv_obj_t* build_button(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  const Colors colors = parse_colors(spec);
  const char* path = spec["bind"] | (const char*)nullptr;
  if (!path) { *err = "button: bind required"; return nullptr; }
  const char* caption = spec["label"] | "button";

  // Snapshot press/release values as JSON tokens so we can re-emit
  // them via the typed put_* helpers later. ArduinoJson's
  // serializeJson handles primitives + strings without quoting needs.
  //
  // A present-but-null value (e.g. RAISE = PUT null to raise the anchor)
  // must be captured as the literal token "null" — NOT skipped. isNull()
  // is true for both a missing key and an explicit JSON null, so we key
  // off whether the field is present: absent -> no PUT; null -> PUT null;
  // otherwise serialize the value. (button_fire treats an empty string
  // as "no PUT", so serialising null to "null" is what routes it through
  // put_json_value -> put_null.)
  std::string press_json;
  std::string release_json;
  if (spec["press_value"].is<JsonVariantConst>()) {
    serializeJson(spec["press_value"], press_json);  // null -> "null"
  }
  if (spec["release_value"].is<JsonVariantConst>()) {
    serializeJson(spec["release_value"], release_json);
  }
  uint32_t hold_ms = spec["hold_ms"] | 0;

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(root);
  lv_obj_set_style_text_color(lbl, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_text(lbl, caption);
  lv_obj_center(lbl);

  auto* bctx = new ButtonCtx{path, press_json, release_json, hold_ms,
                             nullptr, root, false};
  lv_obj_set_user_data(root, bctx);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* c = static_cast<ButtonCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
        if (c->hold_timer) lv_timer_delete(c->hold_timer);
        delete c;
      },
      LV_EVENT_DELETE, nullptr);

  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* c = static_cast<ButtonCtx*>(lv_event_get_user_data(e));
        c->press_fired = false;
        // Visual press feedback: dim the tile slightly.
        lv_obj_set_style_bg_opa(c->root, LV_OPA_70, LV_PART_MAIN);
        if (c->hold_ms == 0) {
          button_fire(c->path, c->press_json, true);
          c->press_fired = true;
          return;
        }
        // Hold-to-act: wait, then PUT.
        if (c->hold_timer) lv_timer_delete(c->hold_timer);
        c->hold_timer = lv_timer_create(
            [](lv_timer_t* t) {
              auto* c = static_cast<ButtonCtx*>(lv_timer_get_user_data(t));
              button_fire(c->path, c->press_json, true);
              c->press_fired = true;
              lv_timer_delete(t);
              c->hold_timer = nullptr;
            },
            c->hold_ms, c);
        lv_timer_set_repeat_count(c->hold_timer, 1);
      },
      LV_EVENT_PRESSED, bctx);

  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* c = static_cast<ButtonCtx*>(lv_event_get_user_data(e));
        lv_obj_set_style_bg_opa(c->root, LV_OPA_COVER, LV_PART_MAIN);
        if (c->hold_timer) {
          // Released before hold expired: cancel, no PUT.
          lv_timer_delete(c->hold_timer);
          c->hold_timer = nullptr;
          return;
        }
        if (c->press_fired) {
          button_fire(c->path, c->release_json, false);
        }
      },
      LV_EVENT_RELEASED, bctx);
  // PRESS_LOST handled like RELEASED (touch off the widget).
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* c = static_cast<ButtonCtx*>(lv_event_get_user_data(e));
        lv_obj_set_style_bg_opa(c->root, LV_OPA_COVER, LV_PART_MAIN);
        if (c->hold_timer) {
          lv_timer_delete(c->hold_timer);
          c->hold_timer = nullptr;
        }
      },
      LV_EVENT_PRESS_LOST, bctx);

  return root;
}

// ---- list ----
//
// Bound to the synthetic "notifications" virtual path: pulls rows
// from notifications_registry, re-renders on every registry change.
// Plain SK array paths are not supported in v1 — we don't have a
// generic array subject yet (every other widget kind subscribes
// scalar floats / ints / strings). Adding one is v2 work.
//
// Each row is a small label per column; columns are positioned by
// their `width` (defaults to 100px each). row_color_field is
// resolved per row to one of {alert, warn, alarm, emergency} and
// tints the row strip.

struct ListColumn {
  std::string label;
  std::string field;
  int width;
  std::string format;
};

struct ListCtx {
  std::vector<ListColumn> columns;
  std::string row_color_field;
  int max_rows;
  int row_height;
  Colors colors;
  lv_obj_t* tile;          // parent container, holds rows below header
  lv_obj_t* rows_box;      // child container we recycle on each rebuild
  int header_h;
  bool include_cleared;    // show normal/nominal rows too (audit view)
  NotificationsRegistry::ObserverToken obs_token;
};

// Resolve a dotted field against a row record. Notifications rows
// are flat ({path, state, message}) so this is overkill today, but
// keeps the door open for nested arrays in v2.
std::string read_field_str(const Notification& n, const std::string& f) {
  if (f == "path")    return n.path;
  if (f == "state")   return not_state_name(n.state);
  if (f == "message") return n.message;
  return "";
}

uint32_t row_color_for(const std::string& state_token) {
  NotState s = parse_not_state(state_token.c_str());
  switch (s) {
    case NotState::Nominal:
    case NotState::Normal:    return 0x3fb950;
    case NotState::Alert:     return 0xd29922;
    case NotState::Warn:      return 0xdb6d28;
    case NotState::Alarm:     return 0xf85149;
    case NotState::Emergency: return 0xa371f7;
  }
  return 0x161b22;
}

// Pick a contrasting text color for a row given its background.
// Light theme bg keeps the configured fg; tinted (state-coded) rows
// take dark text so the bright palette stays legible.
uint32_t row_text_color_for(uint32_t row_bg, uint32_t default_fg) {
  // The default tile bg is dark; any other color in our palette is
  // bright. A simple luminance test is more robust than enumerating
  // the palette since bg_color overrides can also be bright.
  uint32_t r = (row_bg >> 16) & 0xFF;
  uint32_t g = (row_bg >> 8) & 0xFF;
  uint32_t b = row_bg & 0xFF;
  // ITU-R BT.601 perceived brightness.
  uint32_t y = (r * 299 + g * 587 + b * 114) / 1000;
  return y >= 128 ? 0x0d1117 : default_fg;
}

void list_rebuild_rows(ListCtx* lc) {
  // Clear existing children of rows_box.
  lv_obj_clean(lc->rows_box);
  auto rows = notifications().snapshot(lc->include_cleared);
  int max_rows = lc->max_rows > 0 ? lc->max_rows : 8;
  int count = (int)rows.size();
  if (count > max_rows) count = max_rows;
  for (int r = 0; r < count; r++) {
    const Notification& n = rows[r];
    lv_obj_t* row = lv_obj_create(lc->rows_box);
    lv_obj_set_size(row, lv_pct(100), lc->row_height);
    lv_obj_set_pos(row, 0, r * lc->row_height);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t bg = lc->colors.bg;
    if (!lc->row_color_field.empty()) {
      std::string token = read_field_str(n, lc->row_color_field);
      if (!token.empty()) bg = row_color_for(token);
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    uint32_t fg = row_text_color_for(bg, lc->colors.fg);

    int x = 0;
    for (const ListColumn& col : lc->columns) {
      lv_obj_t* cell = lv_label_create(row);
      lv_obj_set_style_text_color(cell, lv_color_hex(fg), LV_PART_MAIN);
      lv_obj_set_style_text_font(cell, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_pos(cell, x, 4);
      lv_obj_set_width(cell, col.width);
      lv_label_set_long_mode(cell, LV_LABEL_LONG_DOT);
      // format strings are designer-only luxury; the firmware just
      // shows the raw field text.
      lv_label_set_text(cell, read_field_str(n, col.field).c_str());
      x += col.width;
    }
  }
  if (count == 0) {
    lv_obj_t* empty = lv_label_create(lc->rows_box);
    lv_obj_set_style_text_color(empty, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(empty, "(no pending notifications)");
    lv_obj_center(empty);
  }
}

lv_obj_t* build_notifications(BuildCtx& ctx, JsonObjectConst spec,
                              std::string* err) {
  const Colors colors = parse_colors(spec);
  JsonArrayConst columns = spec["columns"];
  if (columns.isNull() || columns.size() == 0) {
    *err = "notifications: columns[] required";
    return nullptr;
  }

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(root, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // Caption.
  const char* caption = spec["label"] | (const char*)nullptr;
  int header_h = 0;
  if (caption) {
    lv_obj_t* cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
    header_h = 20;
  }

  // Column header row.
  lv_obj_t* hdr = lv_obj_create(root);
  lv_obj_set_size(hdr, lv_pct(100), 20);
  lv_obj_set_pos(hdr, 0, header_h);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_style_outline_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  auto* lc = new ListCtx{};
  lc->tile = root;
  lc->max_rows = spec["max_rows"] | 8;
  lc->row_height = spec["row_height"] | 28;
  lc->row_color_field = spec["row_color_field"] | "";
  lc->colors = colors;
  lc->header_h = header_h + 20;
  lc->include_cleared = spec["include_cleared"] | false;

  int x = 0;
  for (JsonObjectConst c : columns) {
    ListColumn col;
    col.label = c["label"] | "";
    col.field = c["field"] | "";
    col.width = c["width"] | 100;
    col.format = c["format"] | "";
    lc->columns.push_back(col);

    lv_obj_t* h = lv_label_create(hdr);
    lv_obj_set_style_text_color(h, lv_color_hex(kMutedHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(h, x, 0);
    lv_obj_set_width(h, col.width);
    lv_label_set_text(h, col.label.c_str());
    x += col.width;
  }

  // Rows container — replaced wholesale on each registry change.
  // Vertical-only scroll so the operator can touch-drag through more
  // notifications than fit in the visible area. max_rows still caps
  // the number actually rendered (memory bound); when geometry is
  // smaller than max_rows × row_height the remainder scrolls.
  lv_obj_t* rows_box = lv_obj_create(root);
  lv_obj_set_size(rows_box, lv_pct(100),
                  (spec["h"] | 60) - lc->header_h - 16);
  lv_obj_set_pos(rows_box, 0, lc->header_h);
  lv_obj_set_style_bg_opa(rows_box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(rows_box, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(rows_box, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(rows_box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(rows_box, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(rows_box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(rows_box, LV_SCROLLBAR_MODE_AUTO);
  lc->rows_box = rows_box;

  // Stash ctx; deregister the observer + free on delete (the
  // registry-observer capture would otherwise dangle after teardown
  // and segfault on the next notification delta).
  lv_obj_set_user_data(root, lc);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* lc = static_cast<ListCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
        notifications().off_change(lc->obs_token);
        delete lc;
      },
      LV_EVENT_DELETE, nullptr);

  // Initial render + observe future registry changes.
  list_rebuild_rows(lc);
  lc->obs_token = notifications().on_change([lc]() { list_rebuild_rows(lc); });

  return root;
}

// ---- anchor watch -------------------------------------------------------
//
// A compass rose with a needle pointing to the dropped anchor plus a
// radius ring showing how close the boat is to the alarm limit. Binds
// four navigation.anchor.* paths (all published by an anchor-alarm SK
// plugin): apparentBearing (needle, boat-relative — up = anchor ahead),
// currentRadius + maxRadius (ring fill + centre distance), and state
// ("on"/"off"). The drag alarm itself rides notifications.navigation.
// anchor, handled by the notifications registry + alert overlay, so it's
// deliberately NOT part of this widget.
//
// Unlike the single-bind value widgets, this observes all four subjects;
// each observer updates its slice of one shared AnchorCtx and re-renders
// the composite from the last-known values.
struct AnchorCtx {
  lv_obj_t* needle;      // lv_line, recomputed from apparentBearing
  lv_obj_t* ring;        // lv_arc, value = currentRadius / maxRadius
  lv_obj_t* dist;        // centre distance label
  lv_obj_t* caption;     // "of NN m" range / "ANCHOR UP"
  lv_point_precise_t needle_pts[2];
  float cx, cy, needle_len;
  Disp dist_disp;        // display scaling for the centre distance text
  // Last-known values; any observer redraws from these.
  bool state_on = false;    // navigation.anchor.state == "on"
  bool have_bearing = false;
  float bearing_rad = 0.f;  // apparentBearing (0 = ahead), radians
  float cur_m = 0.f;
  float max_m = 0.f;
};

// Anchored if the plugin says state="on", OR we have a positive maxRadius.
// The state string only arrives over the WS (on drop + a slow periodic
// re-broadcast) and is skipped by the REST value seed, whereas maxRadius
// is a float the seed delivers and the plugin nulls on raise — so keying
// off it too lets the dial come alive from the cold-start fetch instead
// of waiting for the next state delta.
static bool anchor_is_watching(const AnchorCtx* a) {
  return a->state_on || a->max_m > 0.f;
}

// Warn margin (m): yellow once the boat is within this of the limit.
static constexpr float kAnchorWarnMarginM = 3.0f;

// Ring colour by absolute margin to the alarm limit, not a ratio — a
// fixed safety margin is what matters at anchor (3 m of slack means the
// same whether the rode is 20 m or 60 m):
//   red    — past the limit (currentRadius > maxRadius): dragging.
//   yellow — within kAnchorWarnMarginM of the limit but still inside.
//   green  — comfortably inside.
static uint32_t anchor_ring_color(float cur_m, float max_m) {
  if (cur_m > max_m) return 0xf85149;                        // red
  if (max_m - cur_m <= kAnchorWarnMarginM) return 0xd29922;  // yellow
  return 0x3fb950;                                           // green
}

static void anchor_render(AnchorCtx* a) {
  if (!anchor_is_watching(a)) {
    // Anchor up / no watch: dim everything, hide the needle, show a
    // placeholder instead of a stale bearing + distance.
    lv_obj_add_flag(a->needle, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(a->ring, 0);
    lv_obj_set_style_arc_color(a->ring, lv_color_hex(kMutedHex),
                               LV_PART_INDICATOR);
    // ASCII dash, not an em-dash: the compiled Montserrat glyph set
    // doesn't carry U+2014, so "—" renders as a tofu box on-device.
    lv_label_set_text(a->dist, "--");
    lv_label_set_text(a->caption, "ANCHOR UP");
    return;
  }

  // Ring FILL is still the fraction of the limit (0..1, clamped) — a
  // visual "how far out of my scope am I". COLOUR is by absolute margin
  // (see anchor_ring_color). max=0 guards a divide-by-zero.
  float frac = (a->max_m > 0.f) ? (a->cur_m / a->max_m) : 0.f;
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;
  lv_arc_set_value(a->ring, (int32_t)(frac * kBarSteps));
  // Until a positive maxRadius arrives the margin is undefined — keep the
  // ring muted rather than flashing red/yellow off a 0 limit. This can
  // happen when state="on" lands before the maxRadius delta.
  uint32_t ring_color = a->max_m > 0.f
                            ? anchor_ring_color(a->cur_m, a->max_m)
                            : kMutedHex;
  lv_obj_set_style_arc_color(a->ring, lv_color_hex(ring_color),
                             LV_PART_INDICATOR);

  // Centre distance (currentRadius) in display units.
  float dv = a->cur_m * a->dist_disp.scale + a->dist_disp.offset;
  lv_label_set_text_fmt(a->dist, "%.*f", a->dist_disp.decimals, dv);
  if (a->max_m > 0.f) {
    float mv = a->max_m * a->dist_disp.scale + a->dist_disp.offset;
    lv_label_set_text_fmt(a->caption, "of %.*f %s", a->dist_disp.decimals, mv,
                          a->dist_disp.unit);
  } else {
    lv_label_set_text(a->caption, a->dist_disp.unit);
  }

  // Needle: apparentBearing is boat-relative with 0 = dead ahead, so up
  // on the rose. Screen angle 0 points +x (right); rotate by -90° so 0
  // rad points up, then add the bearing (clockwise = starboard).
  if (a->have_bearing) {
    float scr = a->bearing_rad - (float)M_PI / 2.f;
    a->needle_pts[0].x = (lv_value_precise_t)a->cx;
    a->needle_pts[0].y = (lv_value_precise_t)a->cy;
    a->needle_pts[1].x =
        (lv_value_precise_t)(a->cx + a->needle_len * cosf(scr));
    a->needle_pts[1].y =
        (lv_value_precise_t)(a->cy + a->needle_len * sinf(scr));
    lv_line_set_points(a->needle, a->needle_pts, 2);
    lv_obj_clear_flag(a->needle, LV_OBJ_FLAG_HIDDEN);
  } else {
    // Anchored but no heading to reference apparentBearing against.
    lv_obj_add_flag(a->needle, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t* build_anchor(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  // Fixed path family — the widget owns its bindings rather than taking a
  // single `bind`, since it needs the whole navigation.anchor.* set.
  const char* kStatePath = "navigation.anchor.state";
  const char* kBearingPath = "navigation.anchor.apparentBearing";
  const char* kCurPath = "navigation.anchor.currentRadius";
  const char* kMaxPath = "navigation.anchor.maxRadius";

  lv_subject_t* s_state = ctx.reg.get_or_create(kStatePath, SubjectKind::String);
  lv_subject_t* s_brg = ctx.reg.get_or_create(kBearingPath, SubjectKind::Float);
  lv_subject_t* s_cur = ctx.reg.get_or_create(kCurPath, SubjectKind::Float);
  lv_subject_t* s_max = ctx.reg.get_or_create(kMaxPath, SubjectKind::Float);
  if (!s_state || !s_brg || !s_cur || !s_max) {
    *err = "anchor: subject kind conflict on navigation.anchor.*";
    return nullptr;
  }
  ctx.live_paths.insert(kStatePath);
  ctx.live_paths.insert(kBearingPath);
  ctx.live_paths.insert(kCurPath);
  ctx.live_paths.insert(kMaxPath);

  const Colors colors = parse_colors(spec);
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  int box_w = spec["w"] | 140;
  int box_h = spec["h"] | 140;
  int side = box_w < box_h ? box_w : box_h;
  float cx = box_w / 2.0f;
  float cy = box_h / 2.0f;

  // Full-circle rose backdrop (thin muted ring) — a plain bg arc.
  lv_obj_t* rose = lv_arc_create(root);
  lv_obj_set_size(rose, side, side);
  lv_obj_align(rose, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_bg_angles(rose, 0, 360);
  lv_arc_set_angles(rose, 0, 0);
  lv_obj_remove_style(rose, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(rose, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(rose, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_arc_width(rose, 2, LV_PART_MAIN);

  // Radius ring: a 270° arc (gap at the bottom) whose indicator fills
  // with the current/max fraction. Sits just inside the rose.
  lv_obj_t* ring = lv_arc_create(root);
  int ring_side = side - side / 8;
  lv_obj_set_size(ring, ring_side, ring_side);
  lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_range(ring, 0, kBarSteps);
  lv_arc_set_bg_angles(ring, 135, 45);
  lv_arc_set_angles(ring, 135, 135);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(ring, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, side / 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, side / 14, LV_PART_INDICATOR);

  // Needle — an lv_line from centre outward. lv_line stores the points
  // pointer (doesn't copy), so the backing buffer lives in AnchorCtx.
  lv_obj_t* needle = lv_line_create(root);
  lv_obj_set_style_line_color(needle, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_line_width(needle, 3, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(needle, true, LV_PART_MAIN);

  // Centre distance + caption.
  lv_obj_t* dist = lv_label_create(root);
  lv_obj_set_style_text_color(dist, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(dist, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_label_set_text(dist, "--");  // ASCII: U+2014 em-dash tofus on-device
  lv_obj_align(dist, LV_ALIGN_CENTER, 0, -4);

  lv_obj_t* caption = lv_label_create(root);
  lv_obj_set_style_text_color(caption, lv_color_hex(kMutedHex), LV_PART_MAIN);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(caption, "ANCHOR UP");
  lv_obj_align_to(caption, dist, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

  auto* a = new AnchorCtx{};
  a->needle = needle;
  a->ring = ring;
  a->dist = dist;
  a->caption = caption;
  a->cx = cx;
  a->cy = cy;
  a->needle_len = ring_side / 2.0f - side / 14.0f - 4.0f;
  a->dist_disp = parse_display(spec);
  // The centre text is currentRadius; if the layout gave no display
  // unit, default to metres (the SK unit for the radius paths).
  if (a->dist_disp.unit[0] == '\0') {
    snprintf(a->dist_disp.unit, sizeof(a->dist_disp.unit), "m");
  }

  // Ownership: root frees the AnchorCtx on delete; the four observers
  // are auto-removed by LVGL when their target object (root) is deleted.
  lv_obj_set_user_data(root, a);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        delete static_cast<AnchorCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);

  // state: "on"/"off" → anchored flag.
  lv_subject_add_observer_obj(
      s_state,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorCtx*>(lv_obj_get_user_data(w));
        const char* v = static_cast<const char*>(lv_subject_get_pointer(s));
        a->state_on = v && strcasecmp(v, "on") == 0;
        anchor_render(a);
      },
      root, nullptr);

  // apparentBearing (rad). The plugin sends null when there's no heading
  // to reference it against; SensESP's value listener drops nulls, so the
  // subject simply retains its last value. KNOWN LIMITATION: if a heading
  // source drops mid-watch, the needle keeps pointing at the last good
  // bearing rather than hiding — distinguishing that would need a
  // separate availability subject threaded through the listener. The
  // impact is cosmetic (the ring + distance stay correct, and the needle
  // hides entirely once the anchor is raised); deferred over that
  // plumbing.
  lv_subject_add_observer_obj(
      s_brg,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorCtx*>(lv_obj_get_user_data(w));
        a->bearing_rad = lv_subject_get_float(s);
        a->have_bearing = true;
        anchor_render(a);
      },
      root, nullptr);

  lv_subject_add_observer_obj(
      s_cur,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorCtx*>(lv_obj_get_user_data(w));
        a->cur_m = lv_subject_get_float(s);
        anchor_render(a);
      },
      root, nullptr);

  lv_subject_add_observer_obj(
      s_max,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorCtx*>(lv_obj_get_user_data(w));
        a->max_m = lv_subject_get_float(s);
        anchor_render(a);
      },
      root, nullptr);

  anchor_render(a);  // initial (idle) paint
  return root;
}

// ---- anchor track -------------------------------------------------------
//
// NORTH-UP swing plot: anchor at centre, watch-zone ring, and the boat's
// recent track around it — the "cycle" the anchor-alarm webapp shows.
// North is up and the frame is geographically fixed (unlike the `anchor`
// dial's boat-relative needle), so the swing pattern reflects real
// wind/current shifts on the ground, not the boat's heading. Live tail:
// every new currentRadius / bearingTrue sample appends a point at its
// polar position (radius = fraction of maxRadius, angle = bearingTrue,
// 0 = North). The track fades with age — newest bright, oldest dim — via
// a few opacity-banded polylines.
//
// Live-only by design: shows the swing since the widget loaded, not the
// whole session (that would need the SK History API). Binds
// navigation.anchor.{state,bearingTrue,currentRadius,maxRadius}.

// Fixed cap; a swing is slow, so a few hundred points cover a long tail.
static constexpr int kTrackMax = 256;
// Age bands: contiguous slices of the ordered buffer, each drawn as one
// lv_line at a decreasing opacity so the track fades oldest -> newest.
static constexpr int kTrackBands = 5;

struct AnchorTrackCtx {
  lv_obj_t* zone;                        // watch-zone boundary circle
  lv_obj_t* boat;                        // current-position dot
  lv_obj_t* chain;                       // chain-out (maxRadius) label on ring
  lv_obj_t* caption;                     // small "NN / MM m" readout / UP
  lv_obj_t* bands[kTrackBands];          // one lv_line per age band
  // lv_line keeps the points POINTER (no copy), so both buffers must be
  // PER-INSTANCE and outlive the widget: band_pts holds the samples in
  // ring order; `ordered` is the temporal-order layout each band's line
  // points into. A shared static buffer would let one widget's redraw
  // corrupt another's live line data.
  lv_point_precise_t band_pts[kTrackMax];
  lv_point_precise_t ordered[kTrackMax];
  int head = 0;                          // ring write index
  int count = 0;                         // valid points (<= kTrackMax)
  float cx, cy, plot_r, boat_r;          // centre, max plot radius, dot size
  uint32_t track_hex;
  Disp dist_disp;
  bool state_on = false;
  bool have_bearing = false;
  float bearing_rad = 0.f;
  float cur_m = 0.f;
  float max_m = 0.f;
};

static bool anchor_track_watching(const AnchorTrackCtx* a) {
  return a->state_on || a->max_m > 0.f;
}

// Boat-relative polar -> screen point, north up. bearingTrue 0 = North =
// up, 90° = East = right, on a screen whose +y points down.
static lv_point_precise_t anchor_track_xy(const AnchorTrackCtx* a, float frac) {
  if (frac > 1.f) frac = 1.f;
  float r = frac * a->plot_r;
  lv_point_precise_t p;
  p.x = (lv_value_precise_t)(a->cx + r * sinf(a->bearing_rad));
  p.y = (lv_value_precise_t)(a->cy - r * cosf(a->bearing_rad));
  return p;
}

// Rebuild the banded polylines from the ring buffer. Oldest points dim,
// newest bright, so the track fades with age.
static void anchor_track_redraw(AnchorTrackCtx* a) {
  if (!anchor_track_watching(a) || a->count < 2) {
    for (int b = 0; b < kTrackBands; ++b)
      lv_obj_add_flag(a->bands[b], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  int n = a->count;
  int start = (a->head - n + kTrackMax) % kTrackMax;
  for (int i = 0; i < n; ++i)
    a->ordered[i] = a->band_pts[(start + i) % kTrackMax];

  for (int b = 0; b < kTrackBands; ++b) {
    int lo = (int)((long)b * n / kTrackBands);
    int hi = (int)((long)(b + 1) * n / kTrackBands);
    if (lo > 0) lo -= 1;                  // overlap for continuity (never < 0)
    int len = hi - lo;
    if (len < 2) { lv_obj_add_flag(a->bands[b], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_line_set_points(a->bands[b], &a->ordered[lo], len);
    lv_opa_t opa = (lv_opa_t)(LV_OPA_20 +
                              (LV_OPA_COVER - LV_OPA_20) * b / (kTrackBands - 1));
    lv_obj_set_style_line_opa(a->bands[b], opa, LV_PART_MAIN);
    lv_obj_clear_flag(a->bands[b], LV_OBJ_FLAG_HIDDEN);
  }
}

static void anchor_track_render(AnchorTrackCtx* a) {
  if (!anchor_track_watching(a)) {
    lv_obj_add_flag(a->boat, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(a->caption, "ANCHOR UP");
    lv_label_set_text(a->chain, "");
    lv_obj_set_style_border_color(a->zone, lv_color_hex(0x30363d), LV_PART_MAIN);
    a->count = 0;  // reset the tail when the anchor comes up
    anchor_track_redraw(a);
    return;
  }

  // Boundary ring colour = drag margin (same palette as the boat dot and
  // the anchor dial's ring): green comfortably inside, yellow within the
  // warn margin, red once currentRadius exceeds maxRadius.
  uint32_t margin_color = a->max_m > 0.f
                              ? anchor_ring_color(a->cur_m, a->max_m)
                              : 0x30363d;
  lv_obj_set_style_border_color(a->zone, lv_color_hex(margin_color),
                                LV_PART_MAIN);

  // Chain-out label on the ring = maxRadius.
  if (a->max_m > 0.f) {
    float mv = a->max_m * a->dist_disp.scale + a->dist_disp.offset;
    lv_label_set_text_fmt(a->chain, "%.*f %s", a->dist_disp.decimals, mv,
                          a->dist_disp.unit);
  } else {
    lv_label_set_text(a->chain, "");
  }

  // Small readout at the bottom edge: current distance.
  if (a->max_m > 0.f) {
    float dv = a->cur_m * a->dist_disp.scale + a->dist_disp.offset;
    lv_label_set_text_fmt(a->caption, "%.*f %s", a->dist_disp.decimals, dv,
                          a->dist_disp.unit);
  } else {
    lv_label_set_text(a->caption, "");
  }

  // Current-position dot: placed at the boat's polar position, coloured
  // by drag margin (same palette as the boundary ring).
  if (a->have_bearing && a->max_m > 0.f) {
    lv_point_precise_t p = anchor_track_xy(a, a->cur_m / a->max_m);
    lv_obj_set_pos(a->boat, (int)(p.x - a->boat_r), (int)(p.y - a->boat_r));
    lv_obj_set_style_bg_color(a->boat,
                              lv_color_hex(anchor_ring_color(a->cur_m, a->max_m)),
                              LV_PART_MAIN);
    lv_obj_clear_flag(a->boat, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(a->boat, LV_OBJ_FLAG_HIDDEN);
  }
  anchor_track_redraw(a);
}

// Append the boat's current position as a track point (on a fresh
// bearingTrue or currentRadius). Skips near-duplicate points so a boat
// sitting still doesn't fill the buffer with one spot.
static void anchor_track_push(AnchorTrackCtx* a) {
  if (!anchor_track_watching(a) || !a->have_bearing || a->max_m <= 0.f) return;
  lv_point_precise_t p = anchor_track_xy(a, a->cur_m / a->max_m);
  if (a->count > 0) {
    int last = (a->head - 1 + kTrackMax) % kTrackMax;
    float dx = (float)(p.x - a->band_pts[last].x);
    float dy = (float)(p.y - a->band_pts[last].y);
    if (dx * dx + dy * dy < 4.0f) return;  // < 2 px moved: skip
  }
  a->band_pts[a->head] = p;
  a->head = (a->head + 1) % kTrackMax;
  if (a->count < kTrackMax) a->count++;
}

lv_obj_t* build_anchor_track(BuildCtx& ctx, JsonObjectConst spec,
                             std::string* err) {
  const char* kStatePath = "navigation.anchor.state";
  const char* kBearingPath = "navigation.anchor.bearingTrue";
  const char* kCurPath = "navigation.anchor.currentRadius";
  const char* kMaxPath = "navigation.anchor.maxRadius";

  lv_subject_t* s_state = ctx.reg.get_or_create(kStatePath, SubjectKind::String);
  lv_subject_t* s_brg = ctx.reg.get_or_create(kBearingPath, SubjectKind::Float);
  lv_subject_t* s_cur = ctx.reg.get_or_create(kCurPath, SubjectKind::Float);
  lv_subject_t* s_max = ctx.reg.get_or_create(kMaxPath, SubjectKind::Float);
  if (!s_state || !s_brg || !s_cur || !s_max) {
    *err = "anchor_track: subject kind conflict on navigation.anchor.*";
    return nullptr;
  }
  ctx.live_paths.insert(kStatePath);
  ctx.live_paths.insert(kBearingPath);
  ctx.live_paths.insert(kCurPath);
  ctx.live_paths.insert(kMaxPath);

  const Colors colors = parse_colors(spec);
  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  int box_w = spec["w"] | 140;
  int box_h = spec["h"] | 140;
  int side = box_w < box_h ? box_w : box_h;
  float cx = box_w / 2.0f;
  float cy = box_h / 2.0f;

  // Watch-zone boundary: a plain hollow circle (north up). frac 1.0 =
  // maxRadius lands on this circle. A radar/map look, not a gauge dial.
  int zone_side = side - side / 12;
  lv_obj_t* zone = lv_obj_create(root);
  lv_obj_set_size(zone, zone_side, zone_side);
  lv_obj_align(zone, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(zone, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(zone, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(zone, 2, LV_PART_MAIN);
  lv_obj_set_style_outline_width(zone, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(zone, 0, LV_PART_MAIN);
  lv_obj_clear_flag(zone, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

  // Anchor mark at centre (small filled dot).
  int anchor_d = side / 16; if (anchor_d < 4) anchor_d = 4;
  lv_obj_t* mark = lv_obj_create(root);
  lv_obj_set_size(mark, anchor_d, anchor_d);
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(mark, lv_color_hex(kMutedHex), LV_PART_MAIN);
  lv_obj_set_style_border_width(mark, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(mark, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(mark, 0, LV_PART_MAIN);
  lv_obj_clear_flag(mark, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

  auto* a = new AnchorTrackCtx{};
  a->zone = zone;
  a->cx = cx;
  a->cy = cy;
  // frac 1.0 (currentRadius == maxRadius) maps to the boundary circle.
  a->plot_r = zone_side / 2.0f;
  a->boat_r = (side / 22.0f); if (a->boat_r < 3.f) a->boat_r = 3.f;
  a->track_hex = colors.fg;
  a->dist_disp = parse_display(spec);
  if (a->dist_disp.unit[0] == '\0') {
    snprintf(a->dist_disp.unit, sizeof(a->dist_disp.unit), "m");
  }

  // Track band polylines (drawn above the zone circle, below the boat dot).
  for (int b = 0; b < kTrackBands; ++b) {
    lv_obj_t* ln = lv_line_create(root);
    lv_obj_set_style_line_color(ln, lv_color_hex(a->track_hex), LV_PART_MAIN);
    lv_obj_set_style_line_width(ln, 2, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
    lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
    a->bands[b] = ln;
  }

  // Current-position dot (drawn last = on top).
  lv_obj_t* boat = lv_obj_create(root);
  lv_obj_set_size(boat, (int)(a->boat_r * 2), (int)(a->boat_r * 2));
  lv_obj_set_style_radius(boat, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(boat, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(boat, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(boat, 0, LV_PART_MAIN);
  lv_obj_add_flag(boat, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(boat, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  a->boat = boat;

  // Chain-out (maxRadius) label sitting on the top of the boundary ring.
  lv_obj_t* chain = lv_label_create(root);
  lv_obj_set_style_text_color(chain, lv_color_hex(kMutedHex), LV_PART_MAIN);
  lv_obj_set_style_text_font(chain, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chain, lv_color_hex(0x0d1117), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chain, LV_OPA_COVER, LV_PART_MAIN);  // punch through the ring
  lv_obj_set_style_pad_hor(chain, 3, LV_PART_MAIN);
  lv_label_set_text(chain, "");
  // Centre it on the top edge of the boundary circle.
  lv_obj_align(chain, LV_ALIGN_CENTER, 0, -(int)(a->plot_r));
  a->chain = chain;

  // Small current-radius readout at the bottom edge.
  lv_obj_t* caption = lv_label_create(root);
  lv_obj_set_style_text_color(caption, lv_color_hex(kMutedHex), LV_PART_MAIN);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(caption, "ANCHOR UP");
  lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 0);
  a->caption = caption;

  lv_obj_set_user_data(root, a);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        delete static_cast<AnchorTrackCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);

  lv_subject_add_observer_obj(
      s_state,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorTrackCtx*>(lv_obj_get_user_data(w));
        const char* v = static_cast<const char*>(lv_subject_get_pointer(s));
        a->state_on = v && strcasecmp(v, "on") == 0;
        anchor_track_render(a);
      },
      root, nullptr);

  lv_subject_add_observer_obj(
      s_brg,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorTrackCtx*>(lv_obj_get_user_data(w));
        a->bearing_rad = lv_subject_get_float(s);
        a->have_bearing = true;
        anchor_track_push(a);
        anchor_track_render(a);
      },
      root, nullptr);

  lv_subject_add_observer_obj(
      s_cur,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorTrackCtx*>(lv_obj_get_user_data(w));
        a->cur_m = lv_subject_get_float(s);
        anchor_track_push(a);
        anchor_track_render(a);
      },
      root, nullptr);

  lv_subject_add_observer_obj(
      s_max,
      [](lv_observer_t* obs, lv_subject_t* s) {
        auto* w = lv_observer_get_target_obj(obs);
        auto* a = static_cast<AnchorTrackCtx*>(lv_obj_get_user_data(w));
        a->max_m = lv_subject_get_float(s);
        anchor_track_render(a);
      },
      root, nullptr);

  anchor_track_render(a);
  return root;
}

}  // namespace

namespace {
// Per-widget context for the voice mic button: a poll timer that reflects the
// satellite state (0 disc / 1 idle / 2 listening / 3 speaking). idle_text /
// idle_fg are the widget's configured label + colour, restored on the idle
// state so a custom label/fg_color isn't stomped by the state indicator.
struct VoiceCtx {
  lv_obj_t* root;
  lv_obj_t* label;
  lv_timer_t* timer;
  std::string idle_text;
  uint32_t idle_fg;
  int last = -1;
};
}  // namespace

// `voice` — a push-to-talk mic button. Tap to start a voice command; the
// tile reflects the satellite state. No SK bind — it drives the local
// Wyoming satellite's PTT directly (like @audio_mute is panel-local).
lv_obj_t* build_voice(BuildCtx& ctx, JsonObjectConst spec, std::string* err) {
  (void)err;
  const Colors colors = parse_colors(spec);
  const char* caption = spec["label"] | "TALK";

  lv_obj_t* root = lv_obj_create(ctx.parent);
  apply_geometry(root, spec);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root, 8, LV_PART_MAIN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(root);
  lv_obj_set_style_text_color(lbl, lv_color_hex(colors.fg), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_label_set_text(lbl, caption);
  lv_obj_center(lbl);

  auto* vc = new VoiceCtx{root, lbl, nullptr, caption, colors.fg, -1};
  lv_obj_set_user_data(root, vc);

  // Press-and-hold: hold the button to stream the mic, release to send. The
  // held state is level-triggered (set true on press, false on release), so
  // the exact ordering of LVGL press/release/press-lost events doesn't race —
  // whatever the last event says wins. Snappier than tap-then-wait-for-silence.
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        lv_obj_set_style_bg_opa(
            static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_OPA_70,
            LV_PART_MAIN);
        voice().set_ptt_held(true);
      },
      LV_EVENT_PRESSED, nullptr);
  // RELEASED (lift over the button) and PRESS_LOST (finger dragged off) both
  // end the hold.
  auto release_cb = [](lv_event_t* e) {
    lv_obj_set_style_bg_opa(
        static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_OPA_COVER,
        LV_PART_MAIN);
    voice().set_ptt_held(false);
  };
  lv_obj_add_event_cb(root, release_cb, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(root, release_cb, LV_EVENT_PRESS_LOST, nullptr);

  // Poll the satellite state ~4 Hz on the event_loop (LVGL) task and update
  // the caption/colour: LISTENING (green) while streaming mic, "…" when the
  // orchestrator is disconnected, else the caption.
  vc->timer = lv_timer_create(
      [](lv_timer_t* t) {
        auto* c = static_cast<VoiceCtx*>(lv_timer_get_user_data(t));
        int s = voice().state_code();
        if (s == c->last) return;
        c->last = s;
        switch (s) {
          case 2:  // listening
            lv_label_set_text(c->label, "LISTENING");
            lv_obj_set_style_text_color(c->label, lv_color_hex(0x33cc66),
                                        LV_PART_MAIN);
            break;
          case 0:  // disconnected
            lv_label_set_text(c->label, "\xE2\x80\xA6");  // …
            lv_obj_set_style_text_color(c->label, lv_color_hex(0x888888),
                                        LV_PART_MAIN);
            break;
          default:  // idle / speaking — restore the widget's own label + fg
            lv_label_set_text(c->label, c->idle_text.c_str());
            lv_obj_set_style_text_color(c->label, lv_color_hex(c->idle_fg),
                                        LV_PART_MAIN);
            break;
        }
      },
      250, vc);

  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        auto* c = static_cast<VoiceCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
        // If the widget is torn down mid-hold (a layout swap during a press),
        // no RELEASED/PRESS_LOST fires — clear PTT so it doesn't stay stuck.
        voice().set_ptt_held(false);
        if (c->timer) lv_timer_delete(c->timer);
        delete c;
      },
      LV_EVENT_DELETE, nullptr);

  return root;
}

lv_obj_t* build_widget(BuildCtx& ctx, JsonObjectConst spec,
                       std::string* err) {
  const char* type = spec["type"] | (const char*)nullptr;
  if (!type) { *err = "widget missing type"; return nullptr; }
  std::string t(type);
  if (t == "label")  return build_label(ctx, spec, err);
  if (t == "value")  return build_value(ctx, spec, err);
  if (t == "toggle") return build_toggle(ctx, spec, err);
  if (t == "arc")    return build_arc(ctx, spec, err);
  if (t == "bar")      return build_bar(ctx, spec, err);
  if (t == "bargroup") return build_bargroup(ctx, spec, err);
  if (t == "button")   return build_button(ctx, spec, err);
  if (t == "notifications") return build_notifications(ctx, spec, err);
  if (t == "anchor")   return build_anchor(ctx, spec, err);
  if (t == "anchor_track") return build_anchor_track(ctx, spec, err);
  if (t == "voice")    return build_voice(ctx, spec, err);
  // mute_speaker/mute_mic are the original names, kept as aliases: the
  // switches always read ON = working, so the mute_ prefix said the opposite
  // of what the tile does. Layouts saved with the old kinds keep rendering.
  if (t == "speaker" || t == "mute_speaker")
    return build_speaker(ctx, spec, err);
  if (t == "mic" || t == "mute_mic") return build_mic(ctx, spec, err);
  if (t == "volume")       return build_volume(ctx, spec, err);
  *err = std::string("unknown widget kind: ") + t;
  return nullptr;
}

}  // namespace jlp
