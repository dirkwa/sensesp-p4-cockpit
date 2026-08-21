# espos-p4-cockpit

ESP32-P4 firmware that runs the **JSON Layout Player (JLP)** — a
runtime-loadable widget engine on a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
panel. The UI is JSON pushed via HTTP and rendered live with LVGL; no
firmware rebuild per layout change. The device also acts as a SignalK ↔
NMEA 2000 gateway (TWAI rx/tx + a candump TCP server on port 2599).

2.x is a pure ESP-IDF 6 project on **espOS** (`espos/` submodule): WiFi,
provisioning portal, config store + web UI, SignalK discovery / token /
stream in and out (`espos_sk_subscribe`, `espos_sk_put`), logs, core dump
and signed OTA are espOS; this repo is the panel on top. SensESP and
PlatformIO are gone (1.x lives on `master`).

Companion projects:
- **signalk-hmi-designer** — the SignalK webapp that designs and pushes
  layouts. Lives at `../signalk-hmi-designer`.
- **espOS** — `../espOS` is the working checkout; `espos/` here is the
  submodule pinned by commit. Generic device features go into espOS
  (with host tests), panel-specific ones stay here. Fixes found while
  debugging this panel follow the same rule — see
  [Fixing espOS from here](#fixing-espos-from-here).
- **sensesp-cockpit-display / -n2k-gateway** — the 1.x Arduino libraries,
  now folded into `components/` here (`cockpit_hal`, `cockpit_n2k`). They
  stay in place for 1.x on `master`; 2.x does not link them.
  **sensesp-wyoming-satellite** is archived: the voice satellite moved to
  espOS as `espos_voice` (protocol, TCP server, esp-sr wake engine) plus
  `espos_audio` (the `AudioDriver` contract). The board driver stays here
  as `cockpit_hal::WaveshareAudio`, which implements that contract.
  **sensesp-ble-gateway** is deliberately not ported: 1.x linked it but
  never instantiated it, and BLE scanning through the C6 is blocked
  upstream.

## Where to start reading

| File / dir                                            | Why                                       |
|-------------------------------------------------------|-------------------------------------------|
| [README.md](README.md)                                | High-level overview + endpoint table      |
| [JLP-PROTOCOL.md](JLP-PROTOCOL.md)                    | **The wire contract** — schema, endpoints, widget catalogue, alert overlay |
| [main/app_main.cpp](main/app_main.cpp)                | Boot sequence — single source of truth    |
| [main/jlp/](main/jlp/)                                | All player code                           |
| [main/jlp/widgets/widget_factory.cpp](main/jlp/widgets/widget_factory.cpp) | Every widget kind in one file |
| [main/jlp/layout/layout_manager.cpp](main/jlp/layout/layout_manager.cpp)   | Apply pipeline + atomic swap   |
| [main/jlp/net/http_api.cpp](main/jlp/net/http_api.cpp)                    | `/hello`, `/layout`, `/screenshot`, `/healthz` |
| [main/jlp/notifications_registry.cpp](main/jlp/notifications_registry.cpp)| Notifications + ack state              |
| [main/jlp/alert_overlay.cpp](main/jlp/alert_overlay.cpp)                  | Full-screen alarm modal                |
| [components/cockpit_hal/](components/cockpit_hal/)    | Display/touch HAL, LVGL + the UI task (`ui::post/after/every`) |
| [CMakeLists.txt](CMakeLists.txt) / [sdkconfig.defaults](sdkconfig.defaults) / [main/idf_component.yml](main/idf_component.yml) | Toolchain pin, config, registry deps |

## Architecture invariants

These are non-negotiable. Maintaining them is more important than any
single feature.

1. **Never blank the helm.** Layout parse + LVGL build happens under a
   hidden staging parent. Only after the new tree is ready do we swap
   it onto the live screen and delete the old one. A failed push
   returns 400/422/500 and the previous layout keeps rendering.
2. **No optimistic switch latch.** Toggle visual state derives from the
   subscription only. Press handlers PUT and rely on the SK echo to
   flip; a 500 ms reconciliation timer snaps back if no echo arrives.
3. **The DSI flush is asynchronous.** `esp_lcd_panel_draw_bitmap()` only
   queues the copy; the flush callback must wait for `on_color_trans_done`
   (`DisplayDriver::wait_flush_done()`) before `lv_display_flush_ready()`,
   or LVGL overwrites the draw buffer mid-DMA and the panel flashes stale
   strips.
4. **LVGL is single-writer** on the `ui` task (`cockpit_hal::ui`). The
   HTTP task parses + validates, then marshals build/swap with
   `ui::post(...)`. espOS SignalK callbacks (`espos_sk_subscribe`) fire on
   the stream task: copy the strings and `ui::post` before any `lv_*`
   work. `ui::after/every` are LVGL timers (UI task). No `lv_*` call from
   any other task. Ever.
5. **OSS only**, programmatic LVGL API only — no LVGL Pro / XML
   runtime / GPL deps.
6. **Wire format is additive.** New optional fields are fine. Removing
   a field, renaming, or changing semantics bumps `schema` from 1 → 2.

## Pipeline: parse → validate → stage → swap → persist

`LayoutManager::apply()` ([src/jlp/layout/layout_manager.cpp](src/jlp/layout/layout_manager.cpp)):

1. **Parse** the JSON via ArduinoJson, capped at 64 KB POST body.
2. **Validate** schema version, widget kinds, duplicate ids, kind
   conflicts on shared paths, geometry.
3. **Build** the LVGL tree under a hidden staging parent.
4. **Re-configure layout-level chrome** — `status_overlay`,
   `alert_overlay`.
5. **Atomic swap**: un-hide staging, set as current root, delete the
   previous root.
6. **Persist** to `/lfs/layout.json` via tmp + rename — only for
   `ApplySource::PostLayout` after a successful swap.

`ApplySource`: `BootStore` (LittleFS) → `BootDefault`
(`default_layout.h`) → `BootFetched` (async GET from SK
`applicationData`) → `PostLayout`. Boot priority: store > default;
applicationData lands afterwards if different. `POST /layout` always
wins at runtime.

## SignalK wiring

- espOS opens the stream with `sendMeta=all`, so metadata deltas arrive
  in-stream; `espos_sk_subscribe(path, …)` delivers values AND meta items
  for that path to one callback.
- `zone_registry` caches `{zones, description}` per path. The metadata
  is fed by the subscription `SubjectRegistry` opens for each bound path
  (the REST cold-start fetch in `zone_fetch.cpp` also calls `apply_meta`).
  Widgets that bind a path read from it on every value change. Zones
  live in **raw SK units**; match against the raw value, not the
  display-scaled one.
- `notifications_registry` observes the whole `notifications.*` family
  via one family subscription (`espos_sk_subscribe("notifications.*")` —
  they're dynamic, no per-path subscription can cover them). Each notification is keyed by the
  path-after-prefix; the registry tracks `{state, message}` and an
  `acked_` map for the local-ack feature.
- **ACKing a notification** sends an inbound SK delta with
  `state: "normal"` via `espos_sk_send_raw`. SignalK PUT-to-path is
  **not** wired through the server's notification manager — only the
  delta route is.
- Toggles/buttons emit SK PUT via `espos_sk_put` (requestId tracked,
  response logged). Per-kind helpers in
  [main/jlp/net/sk_put.{h,cpp}](main/jlp/net/sk_put.h): `put_bool`,
  `put_int`, `put_float`, `put_string`, `put_null`, `put_position`,
  `put_notification_ack` (delta-based).

## Subjects, listeners, lifetimes

- `SubjectRegistry::get_or_create(path, kind)` lazily creates an
  `lv_subject_t` per bound path and opens an espOS subscription whose
  callback pushes incoming values into the subject (on the UI task).
  `garbage_collect` unsubscribes paths no layout uses (subjects stay).
- Subjects survive layout swaps; same path bound to the same kind is
  reused. **Kind conflicts** (e.g. one widget wants Float, another
  wants Int on the same path) are caught at validate-time and reject
  the layout.
- `NotificationsRegistry::on_change` returns an `ObserverToken`. **Any
  widget that captures a pointer in the callback MUST deregister via
  `off_change(token)` on `LV_EVENT_DELETE`**, otherwise the next
  notification delta after teardown will use-after-free. See
  `build_list()` for the pattern. The alert overlay is exempt — it's a
  process-lifetime singleton.

## Build / flash / debug

```bash
. ~/esp-idf-v6.0.2/export.sh           # the version in .idf-version
scripts/build-ui.sh                    # nice'd espOS web UI → espos/ui/dist-gz
scripts/build.sh                       # nice'd idf.py build, one at a time
idf.py -p /dev/ttyACM0 flash monitor
curl -sf http://<device-ip>:8081/hello | jq .
curl -sf "http://<device-ip>/api/v1/logs?limit=200" | jq -r '.lines[]'   # espOS log ring
```

Prefer the wrappers over bare `idf.py build` / `npm run build` on a small
machine: a full build saturates every core and the editor/SSH session
stops being scheduled. Both run at `nice -n 15`, `ionice -c3`;
`build.sh` caps ninja at `-j 3` (override with `BUILD_JOBS`) and holds a
lock so two builds never race on `build/`.

Boards: the 7B (1024×600 EK79007) is the target; the 4B HAL is parked
until a board is on hand (`components/esp_lcd_st7703` is excluded from the
build in CMakeLists.txt). `/hello` reports the live panel geometry.

If flashing dies with `OSError: [Errno 71] Protocol error` on
`_setDTRandRTS`, the cdc_acm CDC state is stuck. Manual download mode
(hold BOOT, tap RESET, release BOOT) always works; replug also helps.

### Dependencies

Registry components are pinned in `main/idf_component.yml` and
`components/*/idf_component.yml` (lvgl 9.x, ArduinoJson 7, esp_new_jpeg,
mdns, esp_hosted + esp_wifi_remote for the P4's C6 radio); espOS pins its
own. LVGL is configured by `components/cockpit_hal/lv_conf.h`
(`LV_CONF_PATH`, Kconfig LVGL is switched off). `managed_components/` is
not committed.

## Adding a widget kind

Each kind lives as a `build_<kind>(BuildCtx&, JsonObjectConst,
std::string* err)` function in
[widget_factory.cpp](src/jlp/widgets/widget_factory.cpp).

Mandatory:

1. Call `parse_colors(spec)` for `bg_color` / `fg_color` overrides;
   zone match wins when both apply.
2. Create the LVGL tree under `ctx.parent`. Zero the default outline
   and shadow (`lv_obj_set_style_outline_width/shadow_width(... 0
   ...)`); LVGL's default theme draws them and the designer won't
   match.
3. If you call `ctx.reg.get_or_create(path, kind)`, also
   `ctx.live_paths.insert(path)` so the registry knows which subjects
   are live for the new layout (for future GC).
4. Free all heap-allocated context in an `LV_EVENT_DELETE` handler. If
   you registered a `notifications().on_change(...)` observer, also
   call `off_change(token)` first.
5. Add the kind to the `/hello` widgets dictionary in
   [http_api.cpp](src/jlp/net/http_api.cpp) with its supported fields.
6. Dispatch in `build_widget()` at the bottom of widget_factory.cpp.
7. Document the kind in [JLP-PROTOCOL.md](JLP-PROTOCOL.md).

The same fields list must appear in the designer's `schema.ts`. The
designer refuses to push widget kinds the device doesn't advertise.

## Threading model

| Task              | Touches LVGL? | Notes |
|-------------------|---------------|-------|
| `ui` task         | yes           | `lv_timer_handler`, `ui::post` queue, `ui::after/every` timers, layout build/swap, alert overlay |
| `httpd_api` (8081)| no directly   | Parses + validates POSTs, marshals to the UI task, waits on completion semaphore |
| espOS httpd (80)  | no            | web UI + REST (config, OTA, logs, core dump) |
| `esp_timer` 1 ms  | no            | `lv_tick_inc(1)` only — lock-free |
| `espos_skws` task | no            | espOS SignalK stream; subscription callbacks run here and `ui::post` their work |
| `audio` task      | no            | Drains the chime clip queue; blocking I2S write to the ES8311. `WaveshareAudio::play_pcm` (called from the UI task) copies + enqueues, never blocks |
| `wyoming_*` tasks | no            | Voice satellite (`espos_voice`): TCP server, mic streaming, wake feed/fetch (esp-sr AFE) |
| `twai_rx` / candump | no          | N2K receive + per-client fan-out to the candump TCP server |

## Fixing espOS from here

Most espOS bugs surface here first — this is the loudest consumer, and
some of them (WiFi wedges, stream stalls) only reproduce on real hardware
with a real SignalK server. That is fine. What is not fine is leaving the
fix here.

**If a fix belongs to espOS, it lands in espOS.** A change is espOS's when
it is about WiFi, provisioning, config, the config web UI, SignalK
discovery / token / stream, logs, core dump or OTA — regardless of which
repo you were staring at when you found it. Panel-specific means display,
touch, audio, LVGL, JLP, voice or N2K.

The trap is `sdkconfig.defaults`. A radio or hosted-transport setting
fixed only here is invisible to every other espOS board, and the next
project rediscovers the same wedge from scratch. `CONFIG_WIFI_RMT_RX_BA_WIN`
was fixed in this repo first and had to be upstreamed afterwards
(cockpit #72 → espOS #1); do it in the other order.

Order of work:

1. **Fix it in `../espOS`**, on a branch, with a host test where the logic
   is testable (parsers, state machines, cadence). Explain *why* in the
   commit — a bare Kconfig line with no rationale gets reverted by the next
   person who reads the IDF default and disagrees.
2. **Document the constraint** in the matching `espos/docs/*.md`
   (`wifi.md`, `signalk.md`, …) when the fix encodes a hardware or
   protocol limit rather than a plain bug. Other boards need the reasoning,
   not just the value.
3. **Verify on this panel** against your espOS branch — point the submodule
   at it locally, or build with `../espOS` checked out to the branch.
4. **Merge espOS first**, then bump the submodule here in its own
   `chore: bump espos to <sha>` PR. Never merge a cockpit PR whose
   behaviour depends on unmerged espOS work; the pinned sha is what CI and
   every other checkout actually build.

A temporary workaround in this repo is acceptable only when the panel is
unusable without it and the upstream fix is genuinely still open. Say so
in the code comment, name the espOS issue or PR, and remove the workaround
when the bump lands.

## Repo conventions

- **Build/test gate**: `scripts/build.sh` must succeed. There are no host
  tests in this repo (espOS has them) — verify on device via the espOS
  log ring + curl probes.
- **Commits and PR titles**: Angular Conventional Commits —
  `type(scope): subject`, imperative, subject ≤ 50 chars. Types:
  `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`,
  `chore`. Scope is optional (`fix(wake): ...`). The release notes are
  generated from PR titles, so a vague title becomes a vague changelog
  entry. Commits stay focused and atomic.
- **Never commit local/boat configuration.** No WiFi SSIDs or
  passwords, no server IPs, no personal wake words — not in source or
  sdkconfig. `strings` on a firmware image prints every one of them, and
  the merged binary is a public release asset. Site config lives in NVS
  (espOS provisioning image or the setup portal); the signing key is
  git-ignored.
- **Never auto-commit, never auto-push.** Do both only when the user
  explicitly asks.
- **No release-flow work** (version bumps, tags) unless the user says
  release.
- **No AI attribution anywhere.** No "Co-Authored-By: Claude", no
  CLAUDE.md content in the repo body, no AI-tool mentions in commits
  / PRs / code.
- **Code review**: `cr review --plain --type committed --base master`
  on a feature branch. Save output the first time; `cr` is rate-limited
  ~50 min between runs.
- **Comments**: WHY only. No echo comments, no "added for issue #X"
  rot bait, no multi-paragraph docstrings.

## Out of scope (deferred to v0.3+)

- Map / chart widget (raster or vector).
- Polar / AIS-radar plot.
- Media player binding.
- Text-input + canned-reply pills.
- List widget v2: vessels.\* iterator for AIS.
- Alert overlay `ack_method: "toast"`.
- LVGL-WASM pixel-perfect preview.
