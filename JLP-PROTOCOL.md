# JLP Protocol (schema 1)

The **JSON Layout Player (JLP)** is a runtime widget engine that lets a SignalK-aware display device load and re-render its UI from a JSON document, without a firmware rebuild. This document specifies the wire contract — the HTTP endpoints, the JSON schema, and the device behaviour a client (e.g. [signalk-hmi-designer](https://github.com/dirkwa/signalk-hmi-designer)) can rely on.

Reference implementation: this firmware. Any device that obeys this contract is a JLP device.

## Discovery

A JLP device announces itself on the local network via mDNS:

- **Service:** `_signalk-player._tcp`
- **Port:** the JLP HTTP API port (default 8081 on the reference firmware).
- **TXT records:**
  - `schema=1` — wire protocol version.
  - `widgets=label,toggle,arc,bar` — comma-separated widget kinds the device implements.
  - `firmware=<name>-<version>` — informational.
  - `api=/layout,/hello,/healthz` — informational.

Hosts iterate `_signalk-player._tcp.local` results and confirm capabilities with `GET /hello` before pushing.

## HTTP endpoints

All endpoints live on the same TCP port. The reference firmware uses **8081**.

### `GET /hello`

Capability descriptor. Always returns 200 with a JSON body:

```json
{
  "schema": 1,
  "name": "Main helm",
  "hostname": "p4-cockpit",
  "firmware": "p4-cockpit-jlp-1.0.1",
  "display": { "w": 1024, "h": 600 },
  "widgets": {
    "label":  { "fields": ["x","y","w","h","label","bind","display"] },
    "toggle": { "fields": ["x","y","w","h","label","bind"] },
    "arc":    { "fields": ["x","y","w","h","label","bind","display","min","max","start_angle","end_angle"] },
    "bar":    { "fields": ["x","y","w","h","label","bind","display","min","max","vertical"] }
  },
  "active_layout_name": "Main helm",
  "layout_source": "littlefs"
}
```

Field meanings:

| Field                | Meaning                                                                 |
|----------------------|-------------------------------------------------------------------------|
| `schema`             | Wire schema version. Currently always `1`.                              |
| `name`               | Human name of the device.                                               |
| `hostname`           | Network hostname (matches mDNS).                                        |
| `firmware`           | Firmware build tag.                                                     |
| `display.w` / `.h`   | Pixel dimensions of the renderable area.                                |
| `widgets`            | Dictionary of supported widget kinds. The `fields` list is the set of JSON keys the device recognises for that kind — clients MUST NOT emit unknown keys for a kind.                                                                              |
| `active_layout_name` | The `name` of the currently-rendered layout, or `""` if none.           |
| `layout_source`      | Where the active layout came from: `"littlefs"` \| `"default"` \| `"applicationData"` \| `"post"`. |

Clients SHOULD `GET /hello` before pushing and refuse to emit widgets/fields the device doesn't advertise.

### `POST /layout`

Apply a layout JSON. Request body: a JSON document conforming to the schema below. Max body size: **64 KB**.

The device parses, validates, builds the LVGL tree under a hidden staging parent, then atomically swaps it in. If parse, validate or build fails, the prior layout stays live — **the screen never blanks** — and the device returns a non-200 with an error body. On success the layout is persisted (LittleFS on the reference firmware) so it survives reboot.

Synchronous: the device replies only after the swap completes (typically <1s; absolute upper bound 10s before timing out).

Success response (200):

```json
{ "ok": true, "name": "Main helm", "screens": 2, "widgets": 14 }
```

`warning` MAY be present on success — typically `"persist failed"`, meaning the layout is live in RAM but won't survive reboot.

Failure response (400 / 422 / 500):

```json
{ "ok": false, "err": "screen[0].widgets[3]: unknown type 'nope'" }
```

### `GET /screenshot`

Returns the current framebuffer.

- Default (`/screenshot` or `?fmt=jpeg`): **JPEG** encoded in software via `esp_new_jpeg` from an RGB565→RGB888 conversion in PSRAM. Content-Type: `image/jpeg`. Designed for the designer's live-mirror preview mode.
- Legacy (`?fmt=bmp`): **16-bit RGB565 BMP** — a standard 70-byte BMP header (BITMAPINFOHEADER + BI_BITFIELDS RGB565 masks) followed by bottom-up raw pixels. Content-Type: `image/bmp`.

`/hello.screenshot.formats` advertises which encodings the device supports.

### `POST /screen`

Selects the active screen — the remote equivalent of tapping a tab.
Lets SignalK-side automation put the helm on the right page (e.g. a rule
that switches to the anchor screen when `navigation.state` becomes
`anchored`).

```json
{ "id": "anchor" }
```

Addressed by `screens[].id`, not by index: the index is not stable across
layout edits or reordering, while `id` is already required and unique.

- **200** `{"ok":true,"active":"anchor"}` — switched (or already there).
- **400** — body missing, larger than 256 bytes, not JSON, or no `id`.
- **404** `{"ok":false,"err":…,"screens":[…]}` — no screen with that id, or
  the active layout is single-screen and therefore has no tab strip. The
  `screens` array lists what *is* selectable, so a caller can recover
  without a separate `/hello`.

The switch is immediate and unconditional: a remote select overrides
whatever the crew last tapped, and the next tap overrides it back. There
is no lock-out window.

`GET /hello` reports `active_screen` (the current id, empty for a
single-screen layout) and `screens` (all ids in tab order), so a caller
can read the helm's state before changing it and restore it afterwards.

> This endpoint is unauthenticated, like the rest of the JLP API. Anything
> on the boat network can drive the display. See espOS issue #2.

### `GET /healthz`

Liveness probe. Returns 200 `{"ok":true}` if the device is up. No body fields are stable beyond `ok`.

## Layout JSON schema

```jsonc
{
  "schema": 1,                                  // required, must be 1
  "name": "Main helm",                          // required, free-form
  "status_overlay": true,                       // optional, default true
  "tab_strip_height": 56,                       // optional, default 56
  "screens": [                                  // required, ≥ 1
    {
      "id": "switches",                         // required, unique within layout
      "title": "SW",                            // optional, tab button text (falls back to id)
      "widgets": [                              // required, ≥ 1
        {
          "type": "toggle",                     // required
          "id": "nav_lights",                   // required, unique within screen
          "label": "Nav lights",                // optional
          "bind": "electrical.switches.bank.0.1.state",  // optional (required for most kinds)
          "x": 20, "y": 20, "w": 220, "h": 100  // optional, see defaults below
        }
      ]
    }
  ]
}
```

### Top-level

| Field             | Type    | Required | Default | Notes                                                |
|-------------------|---------|----------|---------|------------------------------------------------------|
| `schema`          | int     | yes      | —       | MUST be `1`.                                         |
| `name`            | string  | yes      | —       | Display name; returned in `/hello.active_layout_name`. |
| `status_overlay`  | bool    | no       | `true`  | Whether the device's status strip should be visible. |
| `tab_strip_height`| int     | no       | `56`    | Pixel height of the multi-screen tab strip. Ignored when there is only one screen. |
| `display`         | object  | no       | —       | Backlight power-save. `display.idle_timeout_sec` (uint): seconds of no touch before the backlight dims; `0` or omitted disables the dimmer. `display.idle_dim_pct` (uint 0-100, default 0 = fully off): brightness while idle. While dimmed the device covers the layout with a "TAP TO WAKE" overlay so the first wake-tap can't accidentally hit a widget underneath. **Day/night gating**: the dimmer only engages at night, driven by SignalK `environment.mode` (`"day"`/`"night"`, subscribed automatically) as published by the `signalk-derived-data` plugin's suncalc — which already applies civil-twilight phasing. Fail-safe to always-on until SK has delivered a value (so a missing/disabled `signalk-derived-data` just leaves the panel bright). Wake sources: any touch, any incoming notification (escalation only), a fresh layout push. All three re-arm the idle timer. Capability advertised by `/hello.display.idle_timeout = true` + `idle_dim_pct = true`. |
| `screens`         | array   | yes      | —       | At least one screen.                                 |

### Screen object

| Field     | Type    | Required | Notes                                |
|-----------|---------|----------|--------------------------------------|
| `id`      | string  | yes      | Unique within the layout.            |
| `title`   | string  | no       | Tab button text. Falls back to `id`. |
| `widgets` | array   | yes      | At least one widget.                 |

### Common widget fields

| Field      | Type    | Required | Default | Notes                                                  |
|------------|---------|----------|---------|--------------------------------------------------------|
| `type`     | string  | yes      | —       | Widget kind. Must be one the device's `/hello` lists.  |
| `id`       | string  | yes      | —       | Unique within the enclosing screen.                    |
| `x`, `y`   | int     | no       | `0`     | Top-left position in device pixels (origin top-left).  |
| `w`, `h`   | int     | no       | `120`, `60` | Pixel size.                                        |
| `label`    | string  | no       | —       | Caption text. Semantics depend on widget kind.         |
| `bind`     | string  | no       | —       | SignalK path. Required for everything except a label whose `label` is a fixed string. |
| `display`  | object  | no       | —       | Value formatting (see below).                          |
| `bg_color` | string  | no       | theme `#161b22` | Hex color (`#rrggbb` or `#rgb`) for the tile background. **SK zones still win** when the path matches one; this is the fallback. Use it for operator-meaningful fixed colors (STOP=red, ACK=yellow) that should be visible even without zone state. |
| `fg_color` | string  | no       | theme `#e6edf3` / `#58a6ff` | Hex color for the value text (label/bar) or arc indicator. Same zone-wins precedence. |

### `display` object

Used by label / arc / bar to format the bound numeric value.

| Field      | Type   | Default | Notes                                                |
|------------|--------|---------|------------------------------------------------------|
| `unit`     | string | `""`    | Suffix appended after the formatted number.          |
| `scale`    | float  | `1.0`   | Multiplier applied before display.                   |
| `offset`   | float  | `0.0`   | Added after `scale`.                                 |
| `decimals` | int    | `1`     | Decimal places.                                      |

Displayed value = `(raw × scale) + offset`, formatted to `decimals`, then `unit` appended.

### Widget kinds

#### `label`
- Extra fields:
  - `show_description` (bool, optional, default `false`) — show the SK meta `description` instead of the formatted value (e.g. a switch-state path reads "BMS DnC" instead of "1.0").
- Renders `label` (caption) above the formatted value from `bind`. If only `label` is set, renders a static caption.
- By default shows the formatted live value from `bind`, same as `value`. Set `show_description` to prefer the SK meta `description` text instead; if the path has no description, falls back to the formatted value.
- `display.font_size` (int, optional) — pick a compiled Montserrat size (14, 16, 20, 28, 36). Otherwise autoscales to the widget's height.
- Subject kind: `Float` if `display` set, else `String`.

#### `value`
- Extra fields: none beyond the common set.
- Big-number readout tile. Caption (`label`) sits at the top-left; the formatted value fills the centre; the `display.unit` is rendered separately at the bottom-right (so it stays visible even when the value has many digits).
- `display.font_size` (int, optional) — same compiled-Montserrat options as `label`.
- Background follows SK zone state of `bind` (alarm wins over `bg_color`).
- Subject kind: `Float`.

#### `toggle`
- Extra fields: none.
- Renders a caption (`label`) on the left and an LVGL switch on the right.
- Subject kind: `Int` (treats `0` as off, anything else as on).
- Tap → emits a SignalK PUT to `bind` with the inverted value. The visual switches optimistically and reconciles against the SK echo (~500 ms timeout); if no echo arrives, the switch snaps back to the authoritative subscription value.
- **Local action sentinel** `bind: "@audio_mute"` — instead of a SignalK path, the toggle becomes a **panel-local audio mute**. ON = muted: the alert chime is suppressed on this panel (current and future alarms) while alarms still show on the overlay; OFF re-arms sound. No PUT, no subscription — the state is local to this device. `label` defaults to `MUTE CHIME` — the caption names the *action*, so ON = "muting" reads correctly here, unlike the `SPEAKER`/`MIC` tiles which name the hardware and therefore run ON = working.

#### `arc`
- Extra fields:
  - `min`, `max` (float, required) — value range mapped to the arc sweep.
  - `start_angle` (int, default `135`), `end_angle` (int, default `45`) — degrees; 0° is east, sweep is clockwise.
  - `ticks` (int, optional) — N evenly-spaced major tick marks around the arc. `0` or omitted = none.
  - `tick_labels` (bool, optional, default `false`) — print min, max, and intermediate values next to each tick. Device firmware may omit labels for memory; the designer always shows them.
  - `bands` (array, optional) — advisory colored ring painted **behind** the live indicator. Each entry is `{from, to, color}` where `from`/`to` are in display-space (after the widget's `display.scale` / `offset`) and `color` is a hex string. Bands and SK zones coexist: bands are author-defined "good/warn/critical" ranges; the indicator color still follows live SK zones.
- Renders a circular arc forced into the largest square that fits inside `w × h`; caption (`label`) and formatted value are centred inside.
- Subject kind: `Float`.

#### `bar`
- Extra fields:
  - `min`, `max` (float, required) — value range.
  - `vertical` (bool, default `false`).
- Subject kind: `Float`.

#### `bargroup`
- Extra fields:
  - `bars` (array, required) — at least one sub-bar. Each entry is `{label, bind, min, max, display?}`. Each sub-bar binds to its own SK path with its own range and display formatting, and gets its own SK-zone tinting independently.
- Renders the group caption (top-level `label`) at the top-left and lays out the bars in equal-width slots beneath. Each bar fills bottom-up; the per-bar `label` prints below it.
- No top-level `bind` — the widget is a pure container. `bg_color` / `fg_color` apply to the container; per-sub-bar fill colors come from zones (or the theme accent fallback).

#### `notifications`
- Tabular viewer for the device's notifications registry (the set of
  `notifications.*` paths the device has seen, with their current
  `{state, message}`). No `bind` field — the data source is always
  the notifications registry.
- Extra fields:
  - `max_rows` (int, default `8`) — cap on rows rendered.
  - `row_height` (int, default `28`) — pixel height per row. When the
    rendered rows exceed the tile geometry, the body scrolls
    vertically (touch-drag on the device).
  - `columns` (array, required) — each entry `{label, field, width?, format?}`. `field` is a dotted path into the row object (`path`, `state`, `message`, `createdAt`). `format` is a printf-style template applied client-side (designer only; firmware shows raw text).
  - `row_color_field` (string, optional) — name of a row field whose value names a notification state (`alert`/`warn`/`alarm`/`emergency`, and optionally `normal`/`nominal` when `include_cleared` is set). When set, each row's background is tinted per the maritime palette and the row text auto-flips to dark for legibility.
  - `include_cleared` (bool, default `false`) — include rows in cleared states (`normal` / `nominal`) as well. Default is **pending only** so the list matches the device's "what needs attention" view; set `true` for an audit-style snapshot of every known notification path.
- Re-renders on every notification-registry change. Rows are sorted
  by severity descending (emergency / alarm / warn / alert /
  normal / nominal), so anything that needs attention floats to
  the top. Caption (top-level `label`) sits above a column-header
  row; rows fill the rest of the tile.

#### `button`
- Extra fields:
  - `bind` (string, required) — SK path to PUT.
  - `press_value` (bool|int|float|string, required) — value sent on press.
  - `release_value` (bool|int|float|string, optional) — value sent on release. Omit for one-shot actions like ACK.
  - `hold_ms` (int, optional) — when set, the press_value PUT only fires after the button has been held this long; releasing earlier cancels with no PUT. Use as a safety latch for STOP or anchor release.
  - `bg_color`, `fg_color` (string, optional) — fixed colors (see Common widget fields).
- Renders a centered caption on a tinted tile. Press dims the tile to ~70% opacity for visual feedback; release restores opacity.
- Touch-off-widget (press lost) cancels any pending hold and emits no release PUT.
- **Local action sentinel** `bind: "@drop_here"` — instead of a PUT, dropping the anchor at the boat's current fix. The panel can't compose a lat/lon from a fixed `press_value`, so on press it fetches `navigation.position` over authenticated SK REST and PUTs `{latitude, longitude}` to `navigation.anchor.position` (the anchor-alarm plugin drops there). `press_value` is ignored; set a `hold_ms` (e.g. 800) as a safety latch. Pairs with the RAISE button (`bind: navigation.anchor.position`, `press_value: null`).

#### `anchor`
- Anchor-watch dial: a compass rose with a needle pointing to the dropped anchor and a radius ring showing how close the boat is to the alarm limit.
- Takes **no `bind`** — it owns a fixed SK path family published by an anchor-alarm plugin:
  - `navigation.anchor.apparentBearing` (rad) — needle, boat-relative (0 = dead ahead, so up on the rose; clockwise = starboard). Needle hides when there's no value (no heading source).
  - `navigation.anchor.currentRadius` / `navigation.anchor.maxRadius` (m) — the ring fills `current / max` and the centre shows `currentRadius`. Ring **colour** is by absolute margin to the limit: green when comfortably inside, yellow within 3 m of `maxRadius`, red once `currentRadius` exceeds `maxRadius` (dragging).
  - `navigation.anchor.state` (`"on"` / `"off"`) — anchoring is active when `state` is `"on"` **or** `maxRadius` is positive (so the dial comes alive from the REST cold-start seed, which doesn't carry the string `state`). When neither holds, the dial dims, the needle hides, and the centre reads **ANCHOR UP**.
- Extra fields:
  - `display` (object, optional) — scales/units the centre distance text (defaults to metres, the SK unit).
  - `bg_color`, `fg_color` (string, optional) — fixed colors (see Common widget fields).
- The drag alarm itself is **not** part of this widget — it rides `notifications.navigation.anchor` and is handled by the notifications registry + alert overlay.

#### `anchor_track`
- Anchor-swing plot, **north up**: the anchor at centre, the watch-zone ring, and the boat's recent track around it (the "cycle" the anchor-alarm webapp shows). The frame is geographically fixed (north at top), so the swing reflects real wind/current shifts, not the boat's heading.
- Takes **no `bind`** — it owns the same SK path family as `anchor`, but uses the **true** bearing:
  - `navigation.anchor.bearingTrue` (rad, 0 = North) — plots each sample at the boat's compass position relative to the anchor.
  - `navigation.anchor.currentRadius` / `navigation.anchor.maxRadius` (m) — `current / max` positions the boat relative to the fixed boundary circle (`maxRadius` = the circle). The boundary circle is **labelled with `maxRadius`** (chain out) and the boundary + boat dot colour follow the same drag-margin palette as `anchor` (green inside, yellow near the limit, red once `currentRadius` exceeds `maxRadius`).
  - `navigation.anchor.state` — same `"on"` / `maxRadius > 0` gating as `anchor`; when idle the plot clears and the caption reads **ANCHOR UP**.
- **Live tail only**: the track is built from samples received since the widget loaded (a rolling buffer, ~256 points), not the whole anchoring session (which would need the SK History API). The track **fades with age** — newest bright, oldest dim.
- Extra fields: `display` (distance-text scaling), `fg_color` (track colour). Same as `anchor`.

#### `voice`
- Press-and-hold push-to-talk button for the on-board Wyoming voice satellite. The widget itself only does two things: it drives the satellite's PTT held-state — **held on press, released on lift or when the press is lost** — and it reflects the satellite's state in the caption. It takes **no `bind`** and never touches SignalK — it is a panel-local action, like the `@audio_mute` toggle.
- What happens while held is the satellite's job (see the sensesp-wyoming-satellite library), not the widget's: the satellite streams the panel mic to the orchestrator (signalk-wyoming / Home Assistant), which transcribes it and publishes the text to `voice.command`. The panel never publishes.
- Caption by satellite state: the configured `label` (default `TALK`) when idle, **`LISTENING`** (green) once a pipeline is active, `…` (grey) when no orchestrator is connected.
- Requires an orchestrator to have armed the satellite (`run-satellite`); when the satellite is output-only (`pause-satellite`) a press is a no-op.
- Extra fields: `label` (idle caption, default `TALK`), `bg_color`, `fg_color`.

#### `speaker`

- Panel-local speaker switch. **ON = you hear things, OFF = silent.** Off holds the audio power amp disabled, so **all** panel output — TTS/voice replies and alert chimes — goes quiet. Defaults to ON. No `bind`, no SignalK; a "quiet helm" switch.
- Extra fields: `label` (default `SPEAKER`), `bg_color`, `fg_color`.
- `mute_speaker` is accepted as a legacy alias so existing layouts keep rendering. New layouts should use `speaker`.

#### `mic`

- Panel-local mic switch / privacy control. **ON = mic live, OFF = the mic never streams** (push-to-talk and wake-word both suppressed). Defaults to ON. No `bind`, no SignalK.
- Extra fields: `label` (default `MIC`), `bg_color`, `fg_color`.
- `mute_mic` is accepted as a legacy alias. New layouts should use `mic`.

#### `volume`
- Draggable speaker-volume slider (0–100), applied at the codec. Panel-local; caption above the bar. No `bind`.
- Extra fields: `label` (default `VOLUME`), `bg_color` (tile), `fg_color` (slider indicator/knob).

### Validation rules

A layout MUST be rejected if any of the following holds:

1. `schema` is not `1`.
2. `screens` is missing or empty.
3. Two screens share an `id`.
4. Two widgets in the same screen share an `id`.
5. A widget's `type` is not advertised by the device's `/hello`.
6. A widget declares a `bind` whose subject kind conflicts with another widget binding the same path (e.g. one wants `Int`, another wants `Float`).
7. A widget references a field not in `/hello.widgets[type].fields`.

On rejection the device keeps the previously-active layout and returns `400` (or `422` for kind/field validation, `500` for build failures) with a human-readable `err`.

## Boot priority

A JLP device that has just booted resolves which layout to render in this order. The first one that builds successfully wins; the screen never blanks.

1. **`littlefs`** — the most recently persisted layout from a successful `POST /layout`.
2. **`default`** — a compiled-in minimal layout shipped with the firmware.
3. **`applicationData`** *(async, after boot)* — fetched from `http://<sk-host>:<sk-port>/signalk/v1/applicationData/global/<hostname>/1/layout.json`. If present and different from the active layout, it's applied after the synchronous boot path has already put something on screen.

`POST /layout` always takes precedence at runtime and overwrites the persisted layout on success.

## SignalK bindings

The device subscribes to every `bind` path in the active layout over a single SignalK WebSocket. Updates arrive as deltas; the device updates the corresponding widget on the next render tick.

`bind` values follow SignalK convention: dot-separated paths under `vessels.self.`, e.g. `environment.depth.belowKeel`, `electrical.switches.bank.0.1.state`.

### Meta and zones

The device requests meta in-stream (`sendMeta=all`) and uses two kinds of meta per path:

- **`displayUnits` formula** (preferred when available): inferred `scale` / `offset` / `unit` so a widget without an explicit `display` block still formats sensibly.
- **`zones[]`**: array of `{lower?, upper?, state}` entries where `state ∈ {nominal, alert, warn, alarm, emergency}`. Matching is in **raw SK units** (`lower ≤ raw_value < upper`, or `raw_value == lower` when `lower == upper` for point zones on bool/int paths). Zones are first-match in declaration order. Matching widget's background is tinted per the state, using a maritime-helm escalation palette: nominal/normal → green, alert → yellow, warn → orange, alarm → red, emergency → purple. Widgets without zone meta get the theme default. *Note: this palette is one severity step warmer than the SK spec defaults (which puts alert at blue) — chosen so a glance at the helm reads like a traffic light.*

When the layout's explicit `display` block is present it overrides the path's `displayUnits` formula.

## SignalK PUT (`toggle`)

Tap on a `toggle` widget emits:

```
PUT /signalk/v1/api/vessels/self/<dotted bind path>
{ "value": <inverted current> }
```

over the same WebSocket as a SignalK REST request envelope. The device does **not** send a `source` field — if the path has multiple cached sources, SK rejects the PUT; the layout author or operator is expected to clear stale sources (or the source-disambiguation work in [SK PR #2703](https://github.com/SignalK/signalk-server/pull/2703) needs to be live).

The visual flips optimistically on tap and reconciles against the authoritative subscription value 500 ms later. If no echo lands in time the switch snaps back.

## Alert overlay

A layout-level setting (NOT a widget) configures a full-screen modal that the device pops above the active screen whenever SK delivers a notification with state >= a configured threshold.

Top-level layout fields:

| Field                    | Type   | Default       | Notes                                                  |
|--------------------------|--------|---------------|--------------------------------------------------------|
| `notifications.enabled`  | bool   | `true`        | Master switch for the overlay.                         |
| `notifications.min_state`| string | `"alarm"`     | One of `alert` \| `warn` \| `alarm` \| `emergency`. Only notifications at or above this severity trigger the modal. |
| `notifications.ack_method`| string | `"modal"`    | v1: `"modal"` only. Future versions may add `"toast"` for non-blocking auto-dismiss alerts. |

The device subscribes to `notifications.*` and consumes every delta. Notifications in `state: "normal"` / `"nominal"` are treated as cleared and removed from the registry. The most-severe pending notification at or above `min_state` is shown.

ACK: tapping the on-screen ACK button sends a SignalK delta over the WebSocket with `{updates: [{values: [{path: "notifications.<...>", value: {state: "normal", message: "", method: []}}]}]}`. signalk-server's `filterNotifications` interceptor routes the delta to its NotificationManager, which syncs the alarm's state to `normal`; the server then echoes the cleared state back to all clients (including ours). The overlay observes the registry change and either refills with the next-most-severe pending notification or hides. (Note: the server's modern notification API does NOT honour a REST/WS PUT-with-state-normal against the path directly — only the inbound-delta route is wired through.)

The overlay z-order is above the layout content AND above the status overlay strip, so an unacknowledged alarm always remains visible.

## Versioning

Wire schema versions are integers. A schema change that breaks compatibility increments the integer; the device's `/hello.schema` is authoritative. Clients refuse to push to a device whose `schema` they don't understand.

Field additions inside a kind's `fields` list are not breaking — clients ignore unknown fields, devices ignore values they don't recognise. Removing a field, renaming one, or changing its semantics is a schema bump.
