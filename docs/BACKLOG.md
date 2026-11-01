# Backlog: changes from the apps that could go into espforge

Every project built on espforge (or that espforge came from) adds a line here when it changes something that could
help the others: framework code, board support, tools, tests, docs, a lesson learned. An alignment session works
through the list: port it (a PR here, then the apps take the new tag), or decide not to, and says so in the entry.
The rule is in each project's CLAUDE.md (esp32-s3-rtcquebec, weather_amoled, esp32-s3-meteobus, espforge).

How to write an entry: `- [ ] YYYY-MM-DD, <project> <version or commit>: what, where (files), why it matters.` Add
"PR #n" once a pull request exists here; tick it `[x]` with the espforge version that has it, or strike it with the
reason it stays in the app.

## Framework code

- [x] v0.6.0-rc.1, 2026-10-30, esp32-s3-meteobus port/esp32-p4 (3fb5c2d): diag's statistics hold interrupts off long enough to
  break an ESP32-P4 MIPI-DSI display: ESP-IDF's DPI driver restarts the frame DMA from an interrupt after every frame
  (~0.9 ms gap), and `uxTaskGetSystemState()` (diag's per-task lines: it measures every task's stack in a critical
  section) took 6.1 ms, `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` (the heap line; also slide.c's
  `room_for()`) 2.3 ms. A full-screen white flash every few minutes. MeteoBus's patch adds `CONFIG_FORGE_DIAG_TASK_STATS`
  (forge_core Kconfig, default y) and the P4 turns it off; it also stops walking PSRAM (a `--wrap` returning the free
  size) and sets the panel up on core 1. For espforge: the Kconfig, PSRAM's largest block in diag optional or
  cheaper, forge_lvgl's slide.c `room_for()` without the walk, and a LESSONS entry (time each statistic; on a board fed
  by a per-frame interrupt, nothing periodic may hold interrupts off past the frame gap).
  **→** PR #35: `FORGE_DIAG_TASK_STATS` and `FORGE_DIAG_PSRAM_LARGEST` (forge_core's Kconfig), `diag: stats cost` timing both
  at start. forge_lvgl's slide.c has no `room_for()` yet: the cache of FIX-PLAN-2026-10-30 D1 is built on the free size
  first. The lesson (L201): Group G.

- [x] v0.6.0-rc.1, 2026-10-29, esp32-s3-meteobus port/esp32-p4 (0177715): a second chip family. `p4/espforge-p4.patch` (against
  v0.3.0) holds what MeteoBus needed for an ESP32-P4 board (Guition JC1060P470C, Wi-Fi through an ESP32-C6 with
  esp-hosted / esp_wifi_remote): forge_core `diag.c`'s crash summary for RISC-V (it used the Xtensa backtrace struct);
  forge_net without Easy Connect where DPP isn't there (`FORGE_DPP` guard, `net_dpp_supported()` for the app's setup
  screen: esp_wifi_remote has no DPP); `CONFIG_FORGE_HTTPS_PORT` (443 by default) with `web_port()` for the addresses
  forge_net writes (the C6's esp-hosted 2.3 "network split" kept 443; no longer needed once the C6 was updated, kept
  for co-processors that need it). Port the three, then a board in `boards/` for this one (MeteoBus's docs/P4-PORT.md).
  **→** PR #35: the three ported. The patch's `#define FORGE_DPP (!CONFIG_ESP_WIFI_REMOTE_ENABLED)` did not compile on the S3
  (the symbol is undefined there and `net_dpp_supported()` returns it as C): 0 or 1 by `#ifdef` now. The starter hides
  the Easy Connect page when `net_dpp_supported()` is false. The board in `boards/`: its own session
  (FIX-PLAN-2026-10-30, follow-ups).
- [x] v0.6.0-rc.1, 2026-10-29, esp32-s3-meteobus port/esp32-p4: forge_net's settings page says "Could not reach the display" when a
  change is refused for a missing key (401): the page keeps the key per origin, so after the display's address or port
  changes the page has none. Say "scan the display's QR code again" on 401 (the user met it after a port change).
  **→** PR #35: the starter's page already says it (`bad_key`: "The key was refused: scan the QR code on the device again";
  `no_key` without a key); a Playwright case keeps it so. The wording is MeteoBus's and weather_amoled's to take (their
  `noReach` text), in their own repos.
- [x] v0.6.0-rc.2, 2026-10-29, esp32-s3-meteobus port/esp32-p4: the ES8311 as speaker and microphone at once (`main/audio_p4.c`):
  one `esp_codec_dev` IN_OUT device kept open, created under a lock, the amplifier switched by the app around each
  sound. esp_codec_dev's `es8311_open()` rewrites the chip, so a second device opened at each chime stops the
  microphone. For forge_presence's board hooks (a board with one codec for both).
  **→** PR #38: `components/forge_audio`, layout `FORGE_AUDIO_ES8311_BOTH` (MeteoBus's code: one IN_OUT device under a lock,
  `forge_audio_speaker_dev()` for the app's sound). Proven here on the ES7210 + ES8311 layout; the single-ES8311 one on
  the P4 board when it joins `boards/`.

- [x] v0.6.0-rc.2, 2026-10-11, esp32-s3-meteobus v0.2.1-power (not committed yet): forge_presence's "off" is only brightness 0; the
  panel kept refreshing and the CPU stayed at 240 MHz for the hours the screen was dark. MeteoBus makes it a real off
  in its `set_brightness` hook (`main/main.c`) and `display_sleep()` (`main/display.c`): DISPOFF + SLPIN from the
  presence task (after LVGL's last band, as `display_brightness()`); the way back is split, SLPOUT from the presence
  task, then the LVGL task (core 1, the transfer-done interrupt's core) waits the 120 ms, sends a whole frame and
  DISPON (a whole refresh from core 0 would be lesson L81's race). Power management for frequency scaling only
  (`CONFIG_PM_ENABLE`, 80..240 MHz, no light sleep) with an `ESP_PM_CPU_FREQ_MAX` "screen" lock held while the panel is
  awake: profiled, 100 % at 240 MHz with the screen on, 72 % at 80 MHz with it off (ESP-IDF's per-core "rtos" locks
  give a running task 240 anyway). For espforge: a `screen_off(bool)` hook in `presence_hooks_t` (or document
  `set_brightness(0)` as "off"), panel sleep in the board component (L88), the PM recipe in forge_core, and the harness
  check (`presence.dim_off_wake`: console `power` reports `panel=` and `screen_lock=`). Pitfall met on the way: a
  console command that reads `esp_clk_cpu_freq()` always sees 240 MHz (it runs); check the lock, profile the time.
  **→** PR #38: `screen_off(bool)` hook in `presence_hooks_t`; the board's `display_sleep` (panel sleep, the split wake, the
  "screen" lock); `CONFIG_PM_ENABLE` 80..240 MHz in the board; console `power`; harness `dim_off_wake` checks
  `panel=asleep screen_lock=0` then `awake`/`1`. The pitfall: L202 (Group G).
- [x] stays in the app (v0.6.0 alignment), 2026-10-11, esp32-s3-meteobus v0.2.1-power: slide.c's zoom 42-43 -> 54-58 fps (radar 57.8, bus map 54.5). One
  pass per pixel (gather two pixels, swap to the panel's byte order in the same word: `copy_swap()` over the band again
  is gone), a row whose source row is the one above is a `memcpy`, and the overlays (pill, labels, ring) are runs of
  one colour and alpha blended with one multiply a pixel (RGB565 spread over 32 bits, 5-bit alpha: at most one step
  off, checked against the old code at 110 scales on the PC). The overlays were ~40 % of the fill (found with a
  cycle-counter split in the zoom's log line); the frame is now bound by PSRAM reads and the bus's waits (fill ~11 ms,
  bus ~11 ms, overlapping in two 32-row buffers). forge_lvgl's slide.c has the same zoom: port `zoom_fill()`, the
  overlay runs and the split log.
  **→** forge_lvgl's slide.c has no zoom (nor a `zoom_fill`): nothing to port until it gets one. What the work taught,
  measure the split, is L203.

- [x] v0.6.0-rc.3, 2026-10-10, esp32-s3-meteobus v0.2.1 (fix/radar-wifi-power-save): forge_net leaves the station in ESP-IDF's
  default power save (`WIFI_PS_MIN_MODEM`, and `net_dpp_stop()` sets it back): each reply then waits for the router's
  next beacon, ~100 ms a request. The radar's 14 past frames (28 small GeoMet requests on one kept-alive connection)
  took 4.9-6.6 s, 3.1-3.7 s with power save off (the PC: ~90 ms a request). MeteoBus turns it off while a map screen
  is open (`netq_awake()` in `main/netq.c`) and leaves forge_net alone. For espforge: a `net_low_latency(bool)` (a
  count of users, kept off while Easy Connect holds the radio) and a LESSONS entry: before blaming a server or the
  parsing for slow requests, compare the board with the PC on the same URLs, and try `WIFI_PS_NONE`.
  **→** PR #39: `net_low_latency(on)` (counted; Easy Connect puts it back when it stops), `wifi status ps= lowlat=`, console
  `wifi lowlat on|off`, harness `console.wifi_low_latency`. The lesson: L204 (Group G).
- [x] v0.6.0-rc.4, 2026-10-09, esp32-s3-meteobus v0.2.0-rc (dbd61f2): slide.c's `get()` with `force` (a slide that needs a picture
  now) allocated a new 434 KB buffer even when `room_for(1)` said no: PSRAM's low point fell from ~450 to 268 KB the
  first time a screen not kept as a neighbour (the radar, opened on top since MeteoBus) slid in. Now it takes the
  buffer of the picture worth least (not a spare). forge_lvgl's slide.c has the same `!room_for(1) && !force` test:
  port the fix (`main/slide.c`, `get()`).
  **→** PR #40: forge_lvgl's slide.c had no cache at all (four buffers taken at init): now on demand, and a forced
  picture with no room takes a less useful picture's buffer (`slide_slot_reuse`, host-tested).
- [x] v0.6.0-rc.4, 2026-10-09, esp32-s3-meteobus v0.2.0-rc: slide.c's idle work never took a slot holding a needed picture, even
  one worth less than the picture it had to render, nor reused a less useful picture's buffer when PSRAM had no room
  for another: with five neighbours to keep (MeteoBus's weather screen: itself, the next place, extras, the stop page,
  the hourly view) the next place's picture, evicted during a visit to the extras page, never came back, and place
  drags waited ~0.12 s (harness drag_start_ms.drag_place 14 -> 121 ms). `slot_for(..., want_p)` and the idle path's
  buffer reuse in `main/slide.c`; forge_lvgl's slide.c has the same rule.
  **→** PR #40: `slide_slot_pick(..., want_p)`: a wanted slot gives way to a picture worth more; the idle work reuses a less
  useful picture's buffer when PSRAM has no room (`slide_slots.c`, 16 host checks).
- [x] v0.6.0-rc.4, 2026-10-09, esp32-s3-meteobus v0.2.0-rc: `slide_cache_release_unneeded()`: frees the pictures not in the keep
  list, for a screen that needs PSRAM for a while (MeteoBus's bus map: its 434 KB picture and its tiles exist only
  while it is open). A small addition to forge_lvgl's slide.c for any app with a memory-hungry screen on top.
  **→** PR #40: `slide_release_unneeded()` (the pages not wanted now: not a neighbour of a pager on view).
- [x] v0.6.0-rc.3, 2026-10-09, esp32-s3-meteobus v0.2.0-rc: `netq.c`: who is downloading now (flags set around the weather loop's,
  the radar's and the bus map's downloads), so a lower-priority poller (departures.c) waits before each request: each
  TLS download holds 10-15 KB of internal RAM, and two at once took a place switch's low point from ~40 to 29 KB.
  A forge_net-level "download slot" would let every app (and svc probes, OTA checks) take turns. MeteoBus went
  further (rc.13): no task for the poller at all, its requests run in the radar task's idle time
  (`radar_set_side_work`), after a PSRAM-stack TLS task stopped for good once and an internal one cost 6 KB. A
  framework "background download worker" with a queue would serve every app's pollers the same way.
  **→** PR #39: `net_dl_begin/end` (one TLS download at a time; the update check and install, the map tiles and the
  probes take it; `net_dl_busy()` for a poller), `net_bg_post` (one short-lived worker task, jobs one at a time in the
  slot; svc's probes run there). `wifi status dl= bg=`.

- [x] v0.6.0-rc.1, 2026-10-09, esp32-s3-meteobus v0.1.0-rc.0 (3bbe34a): forge_net `svc_user_agent()` builds the User-Agent in a
  128-byte buffer, and `-Werror=format-truncation` fails the build once `CONFIG_FORGE_PRODUCT`, `CONFIG_FORGE_REPO` and
  `CONFIG_FORGE_UA_COMMENT` are a little longer ("esp32-s3-meteobus", "open-source weather and bus display").
  MeteoBus shortened its comment to "open-source weather/bus". Make the buffer larger (256) or check the lengths
  with a `_Static_assert` with a clear message, in `components/forge_net/svc.c`.
  **→** PR #35: 256 bytes and the `_Static_assert` (the three Kconfig strings plus 96 must fit).

- [x] v0.3.0-rc.1, 2026-10-06, esp32-s3-rtcquebec v0.1.0: forge_lvgl `pager_set_count()`: build every page once, show as many as
  the app has data for (pages past n hidden, out of the scroll range and of slide.c's drags). PR #8; review found
  slide.c's idle loop still rendered the hidden page after the last one (`pager_shown()` now), host test
  `tests/host/test_pager.c`.
  **→** Merged (PR #8).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0 (f71f9fd): forge_lvgl `pager_set_order()`, on top of
  `pager_set_count()`: the app gives the order of the pages it shows (a summary page that stays last after a
  varying list), the others follow hidden. `components/forge_lvgl/pager.c`. Added to PR #8, with a fix: a page
  listed twice overran the order array (ASan in `test_pager.c`); esp32-s3-rtcquebec's call never repeats one.
  **→** Merged (PR #8).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0: a tap on a page of a slide.c pager right after a drag can be the next
  quick swipe's press (it reached LVGL as a short click: the app opened its map). The app ignores a tap within
  600 ms of a page settling (`stop_tapped` in its ui.c); slide.c / pager could offer that ("last settle" tick, or
  swallow the click) so every app gets it.
  **→** `slide_tap_ok()` in forge_lvgl, PR #12 (no automatic swallowing: a tap can be wanted); esp32-s3-rtcquebec's
  `stop_tapped` takes it when it moves onto espforge's components, its harness `quick_swipes` the proof.
- [x] v0.4.0-rc.1 (phase 1), 2026-10-07, esp32-s3-rtcquebec v0.2.1: a street-map module: OpenStreetMap tiles around a place at zoom z,
  dimmed for the AMOLED, decoded a row at a time (`png_rows`), kept in PSRAM by zoom (`main/map.c`, `main/geo.c`
  with a host test). weather_amoled's radar.c does the same with a flash cache: a shared `forge_map` component
  would serve both (and the next app with a map).
  **→** Later (user, 2026-10-07): a plan first, after forge_presence. PR #20 (phase 1): `components/forge_map`
  (`forge_geo.h` + `forge_map.h`: the map task with PSRAM slots, `fmap_render` for another task, URL templates,
  host test `tests/host/test_map.c`); no flash cache yet (phase 2: weather_amoled's MAP7 layout). Then
  esp32-s3-rtcquebec moves its map.c/geo.c onto it, and weather_amoled's radar.c its tile code. In v0.4.0-rc.1, with
  the emulator's `USE_MAP := 1` (PR #22); esp32-s3-rtcquebec's map on it (its v0.4.0-rc.1). Still to do: phase 2 (the
  flash cache) when weather_amoled's radar.c moves.
  **→** Phase 2 in PR #41 (v0.6.0-rc.5): `map_cache.c` over an I/O table (host-tested), `fmap_set_home`, `opts.flash_cache`,
  the `mapcache` partition in the starter's table; weather_amoled's radar.c moves onto it in its own session.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0 (5114f19): screen dimming by sound, ported from weather_amoled's
  presence.c: typed NVS keys instead of its `cfg` blob (a blob that changes size drops the settings on an update), the
  state machine as pure C with a host test (`main/presence_sm.c`, `tests/host/test_presence.c`), touch counted as
  activity through `touch_idle_ms()`. A shared presence component would serve both apps; weather_amoled should move
  to typed keys too.
  **→** PR #14: `components/forge_presence` (user's OK for A+B, 2026-10-07), rtcquebec's code as the base, hooks for
  the hardware; harness `presence.dim_off_wake` passed on COM5 (v0.3.0-rc.0 test build).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0: board API `touch_set_press_filter()` (ws_amoled175 `touch.c` /
  `board.h`, and the emulator's touch): a press the app swallows whole before LVGL and slide.c see it (the touch
  that wakes a dark screen must not also open a map or move a page). forge_lvgl owns the read hook, so a hook of
  its own was needed.
  **→** PR #12 (board and emulator).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0: the board's microphones (ES7210 over I2S, pins MCLK 42, BCLK 9, WS 45,
  DIN 10; esp_codec_dev 1.5.11) set up in the app (`main/presence.c`): move into the board component
  (`board_mic_open()`), keeping I2S TX for the ES8311 speaker.
  **→** PR #14: `board_audio.h` (I2S0 both ways, `board_mic_open/read`, `board_audio_data_if` for a speaker).
- [x] weather_amoled v1.15.0-rc.1, 2026-10-07, espforge PR #14 (the other way: espforge → weather_amoled): weather_amoled's presence.c onto
  forge_presence: its `cfg` blob read once into the typed keys (then erased), its speaker (`sound.c`) on
  `board_audio_data_if()`'s twin, its preview brightness and console commands kept in the app, its page for the
  200 + `"ok":false` answers and the `cal` message (en, fr, iu). A plan first (its own board and audio code).
  **→** weather_amoled PR #28 (branch `feat/forge-presence`, for v1.15.0-rc.1): on forge_presence v0.3.0 with hooks
  in its main.c; the blob is imported by the component and kept (not erased: a rollback reads it); preview brightness
  and the console commands are the component's; host test `test_presence_blob.c` pins the old struct's layout.
  Merged; on COM5 the import read the real blob (every value as v1.14.4 showed it) and v1.15.0-rc.0 passed 22/22.
- [x] v0.6.0-rc.2, 2026-10-08, weather_amoled PR #28: `board_audio.c` copied into the app as `main/audio.c` only to use the app's own
  I2C bus (`touch_i2c_bus()` instead of `board_i2c_bus()`) and to open I2S both ways at start. A board-free audio
  helper (the bus as a parameter, e.g. `forge_audio_init(bus, speaker)`) would let an app with its own board code
  share it.
  **→** PR #38: `components/forge_audio` (`forge_audio_init(cfg, speaker)` with the bus, port and pins); the board's
  `board_audio_*` are thin wrappers. weather_amoled's `main/audio.c` can call it with `touch_i2c_bus()` (its session).
- [x] v0.5.0-rc.1, 2026-10-09, esp32-s3-rtcquebec v0.4.0 (the user's new navigation, as weather_amoled's places): a
  pager on a page of another, across it (the stops in a column, in the middle of a row alerts | stops | map), each
  axis dragging its own: `pager_on_view()` (pager.c, host test), slide.c picking the pager by the drag's axis, four
  neighbour pictures instead of two, `slide_stale()` for neighbours whose contents the app changed.
  **→** PR #26.
- [x] v0.5.0-rc.1, 2026-10-09, esp32-s3-rtcquebec v0.4.0 (the user: "maybe the whole settings screen should be
  standardized in espforge for current and future projects"): weather_amoled's on-device Settings (`cfg_*` in its
  ui.c) as `components/forge_settings`: the screen (Done, rows in sections, brightness arc), ready-made rows (dimming,
  pick-up, timing, language, phone QR, Wi-Fi, updates, restart, About), the app's own rows, texts by code.
  **→** PR #26; the starter opens it with a long press (harness `long_press_opens_settings`, `settings_row_acts`).
- [x] v0.6.0-rc.4 (the hook), 2026-10-09, espforge v0.5.0-rc.1: weather_amoled onto forge_settings (its `cfg_*`, main/ui.c:2347-2672). It has
  its own slide.c, not forge_lvgl's, and forge_settings slides with forge_lvgl's `slide_to()`: give forge_settings a
  hook for the slide (or move weather_amoled to forge_lvgl first). Its rows of its own: units, sound.
  **→** PR #40: `settings_set_slider(fn)`. The migration itself is weather_amoled's session (FIX-PLAN-2026-10-30,
  follow-ups).
- [x] v0.6.0-rc.5, 2026-10-09, espforge v0.5.0-rc.1: list scrolls drawn as pictures (weather_amoled's `slide_scroll`) in forge_lvgl,
  for forge_settings' list and apps' lists (esp32-s3-rtcquebec's alerts): LVGL scrolls them at ~25 fps.
  **→** PR #41: a vertical drag on a scrollable list with no pager that way scrolls it as pictures (the board's
  `display_raw_area`, the panel's `raw_area` hook); harness `perf.settings_scroll`.
- [x] v0.6.0-rc.4, 2026-10-09, esp32-s3-rtcquebec v0.4.0-rc.1: a long press only from a finger that stayed put (its ui.c
  `long_pressed`: within 24 px of where it came down). LVGL fires LONG_PRESSED for a press held 400 ms however far it
  moved: a swipe read late on a slow page opened Settings (L200). A forge_lvgl helper (`touch_held_still()`), used by
  the starter and forge_settings' openers; weather_amoled's `open_cfg` has the same exposure.
  **→** PR #40: `slide_held_still(px)`, `SLIDE_HOLD_PX` 24; the starter's long presses ask it. The exposure in espforge: a
  screen without a pager (the start-up message): harness `moving_press_is_no_long_press` failed on the old firmware.
- [x] v0.6.0-rc.5, 2026-10-09, esp32-s3-rtcquebec v0.4.0-rc.1: its map composes picture + route path in a canvas (`map_compose`,
  434 KB PSRAM) because `lv_line`s over the image cost 170-200 ms a frame (L200). forge_map could draw polylines into
  its own picture (an overlay list per view): no second 434 KB buffer, and weather_amoled's radar could use it too.
  **→** PR #41: `fmap_set_path()` draws the path into the picture after the tiles, the pixels under it kept (an undo
  list, at most 192 KB) so it can change without a download; `map_path.c` host-tested. rtcquebec drops its canvas in
  its own session.

## Tools and tests

- [x] v0.6.0-rc.5, 2026-10-29, esp32-s3-meteobus port/esp32-p4: console commands for a board the PC's HTTPS can't reach yet:
  `shot SCREEN [DIV]` (a screenshot over USB as base64 RGB565, `tools/shot.py` makes PNGs; `shot panel` sends the frame
  buffer as shown), `netstat` (lwIP counters: does anything from outside reach the board?), `blitbench` (frame copy
  timings: CPU, PPA, async memcpy, 2D-DMA), `c6update SIZE` (an ESP32-C6 co-processor's esp-hosted firmware streamed
  from the P4's spare app slot through `esp_hosted_slave_ota_*`; the image padded 4 KB so esptool writes it). And the
  P4 flash helper (`p4/flash_helper_p4.ps1`: `--chip esp32p4`, bootloader at 0x2000, finds the P4's port among
  several boards). Candidates for forge_core's console and tools/devloop.
  **→** PR #43: `shot SCREEN [DIV]` (forge_lvgl's screens.c; `shot panel` only on a board with a frame buffer) and
  `tools/devloop/shot.py`; `netstat` (forge_net, `CONFIG_LWIP_STATS`); the flash helpers (`.ps1`, `.py`) pick the
  port by chip among several boards (`.devloop/port`). `blitbench` and `c6update` are the P4 board's: its session.

- [x] v0.6.0-rc.2, 2026-10-11, esp32-s3-meteobus v0.2.1-power: `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` (32 by default, same 32 KB
  cache): every PSRAM-bound frame got faster, none slower in the harness (zoom fill -5 %, bus map drawn 1725 -> 1663 ms,
  snapshots ~4 % faster). A board default for any app that copies pictures out of PSRAM.
  **→** PR #38: in `sdkconfig.defaults`. Harness: swipes 52-61 fps, snapshots 1.7-2.1 s, none slower than the baseline.

- [x] v0.6.0-rc.6, PR #37, 2026-10-09, esp32-s3-meteobus v0.1.0-rc.0: this was the third new repo (after esp32-s3-weather and
  esp32-s3-rtcquebec) set up by hand: the "protect main" and "release tags" rulesets, Pages built by a workflow, the
  `github-pages` environment's branch and tag policy (`main`, `v*`) and Actions' default read permissions. A
  `tools/setup_github.py` (or a step in the new-project skill) that copies these from a reference repo with `gh api`,
  then checks they match, would make this repeatable.
  **→** PR #37: `tools/setup_github.py --from A --to B [--check]` (rulesets by name, Pages, the github-pages
  policies, Actions permissions; `--check` read-only, exit 1 on a difference), a step in the new-project skill and
  NEW-PROJECT.md §3. espforge itself has no rulesets (the apps do): copy them from esp32-s3-weather, the owner's call.

- [x] v0.3.0-rc.1, 2026-10-06, esp32-s3-rtcquebec v0.1.0: the flash helper started by Claude in the background, windowless;
  `tools\esptool.exe` to copy into a new project (git-ignored). PR #7.
  **→** Merged (PR #7).
- [x] v0.3.0-rc.1, 2026-10-06, esp32-s3-rtcquebec v0.1.0: a new project's CI fails `test_snapshot_metrics_follow_forge_screens`
  until the first harness run writes a baseline (NEW-PROJECT resets it to `{}`): say so in docs/NEW-PROJECT.md, or
  let the test skip an empty baseline.
  **→** PR #13: the test skips an empty baseline (a unit test for it), NEW-PROJECT says so.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.1.0: on a fresh clone the harness's Playwright step fails every test in ~1 ms
  ("Executable doesn't exist") while `npm test` by hand passes: the Microsoft Store Python hides AppData\Local from
  its children (L146); copy `%LOCALAPPDATA%\ms-playwright` to `tools/webtest/.browsers`. Add to NEW-PROJECT.md.
  **→** PR #13: docs/NEW-PROJECT.md §4.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0: the harness's navigation test hard-coded the starter's two pages; the
  app's version reads the pages from the device (`/api/favs`) and swipes through all of them (`pages()` in
  `tools/harness/app_suites.py`): a pattern for apps whose page count varies.
  **→** PR #13: the starter's `pages()` in `tools/harness/app_suites.py`, the navigation test walks it; TESTING §5.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0: the webtest mock's `/api/presence` and `/__presence` hook and
  `tools/webtest/tests/screen.spec.js`: a template for a settings section with live state.
  **→** PR #14 (mock, `screen.spec.js`, with a case for each calibration result).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.1 (ca8d132): `git add -A` committed an agent's worktree
  (`.claude/worktrees/agent-...`) as an embedded repository: the template's `.gitignore` should list
  `.claude/worktrees/`.
  **→** PR #13: `.gitignore`.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0: the settings page's `.row { align-items: flex-end }`, so a French label
  that wraps to two lines doesn't push its field out of line.
  **→** PR #14 (the Screen section brings `.row`).
- [x] done in weather_amoled (4c7a947), 2026-10-07, espforge PR #6 (the other way: espforge → weather_amoled): the PowerShell helpers restart through
  the test console with the port open (`monitor.ps1 -Reboot`, exit 3 = esptool instead; the COM port found by VID
  303A in `flash_helper.ps1`), so a restart's boot log is whole from `ESP-ROM:` (L191). weather_amoled's
  `monitor.ps1` / `flash_helper.ps1` (repository root) still use esptool's reset and lose the first ~2.5 s (L154);
  its Python `tools/flash_helper.py` already does it (22f76f1). Twins: port, then prove on COM5 (`flash QIO`).
  **→** For weather_amoled (twins): ported in its espforge-bump release.
  **→** Checked 2026-10-31: weather_amoled's root `flash_helper.ps1` / `monitor.ps1` restart through the console (`-Reboot`,
  exit 3 = esptool), its commit 4c7a947.
- [x] v0.6.0-rc.6, PR #37, 2026-10-07, esp32-s3-rtcquebec (alignment): an app made from the template can take espforge at a tag
  instead of copies (as weather_amoled and now esp32-s3-rtcquebec): `tools/fetch_forge.py`, the Makefiles'
  `FORGE ?=` (managed_components, else `.espforge`), emu.mk's `FORGE_EMU`/`COMP`/`BOARD` pointed at the clone. Say
  so in docs/NEW-PROJECT.md (a section "Later: the framework at a tag") and ship `tools/fetch_forge.py` here.
  **→** esp32-s3-rtcquebec moved (its PR #1, v0.3.1-rc.1: 20/20 on COM5); espforge's side: `gen_config.py` reads
  espforge's own Kconfig (v0.3.0-rc.1), `smoke.js` finds Playwright from the current directory (PR #16). Still to
  do here: NEW-PROJECT's section and `tools/fetch_forge.py` in the template.
  **→** PR #37: both done (NEW-PROJECT.md "Later: the framework at a tag", `tools/fetch_forge.py`, `.espforge/`
  git-ignored).
- [x] v0.6.0-rc.1, 2026-10-07, espforge v0.3.0-rc.1 (harness on COM5): `perf.page_swipes` runs right after
  `navigation.setup_pages_slide`, whose Easy Connect page leaves the home network: the first swipe can land while
  Wi-Fi rejoins (46 fps and a 146 ms gap once, a console reply lost once; clean on re-runs). Wait for `net:
  Connected` (or `wifi status`) before the perf suite measures.
  **→** PR #35: `Board.wait_online()` (wifi status connected=1) in `measure()` and after the setup pages; unit test.
- [x] v0.3.0-rc.2, 2026-10-07, espforge v0.3.0-rc.1: internal RAM since forge_presence: free 125 -> 111 KB, largest block 47-55 ->
  36-41 KB (floor 40), most likely I2S0's DMA buffers both ways (4 x 320 frames each, internal) and the presence
  task's stack. To measure (a build without presence_start), then: open TX only when an app plays sound, or fewer
  DMA frames for the microphones. Before the stable v0.3.0.
  **→** Measured (no presence 128/58 KB, without microphones 123/52, rc.1 112/41) and fixed in v0.3.0-rc.2: the
  speaker's direction only on request (117/46). The rest (~6 KB) is the microphones' own DMA and the codec.
- [x] v0.6.0-rc.1, 2026-10-08, espforge v0.4.0 (harness on COM5): `memory.heap_numbers` in an `--ota` run measures right after the
  update installed (the image confirmed, the update's TLS connection maybe still held): internal_largest_kb 35 there,
  46 three times on the same firmware at rest and after a restart (36-46 across tonight's runs). Measure memory after
  the OTA suite's settle, or let `--ota` runs wait for the update's connection to close before the memory suite.
  **→** PR #35: in an `--ota` run `heap_numbers` reads until two readings agree within 2 KB (`heap_settled`, 30 s at most);
  unit test.
- [x] v0.5.0-rc.1, 2026-10-09, esp32-s3-rtcquebec v0.4.0: the flash helpers (`flash_helper.ps1`, `.py`) checked the
  staged parts against forge.json's build_dir and the harness looked for the ELF there: a build staged from another
  folder (`stage.py --build build/forge`) was refused, and its panic not decoded. Both follow
  `stage/manifest.json`'s `build_dir` now; test `test_another_build_folder_staged_is_flashed` (LESSONS L198).
  **→** PR #26.
- [x] v0.6.0-rc.6, PR #37, 2026-10-09, esp32-s3-rtcquebec v0.4.0: its `tools/forge_local.py` (weather_amoled's, with every espforge
  component of the manifest and the board, `COMPONENTS` a name -> folder map): ship it in the template with the
  "framework at a tag" section of NEW-PROJECT.md (the open entry above).
  **→** PR #37: shipped, the component map read from the manifest's espforge entries (the board included),
  `--forge` (default `../espforge`); "nothing to do" in espforge itself.
- [x] v0.6.0, 2026-10-09, esp32-s3-rtcquebec v0.4.0-rc.1: building from the Claude desktop app's shell needs
  `IDF_COMPONENT_CACHE_PATH` outside AppData for the component manager's git fetches (L199): say so in WORKFLOW.md
  and the build skill (or set it in the devloop / CI helper scripts that build).
  **→** docs/WORKFLOW.md (the PC way) and the build-flash-probe skill set `IDF_COMPONENT_CACHE_PATH` before `idf.py`.

- [ ] 2026-10-31, espforge v0.6.0-rc.4 (harness on COM5): `perf.page_swipes`' worst gap counts an LVGL redraw of the
  page landed on when it comes within 250 ms of the drag's repaint (the system page's 1 s refresh, its RSSI text):
  `swipe_gap_max_ms.hello_to_system` 222 and `drag_slow` 99 in two of five runs, 35-41 in the others, "lvgl>lvgl". The
  `fps` counters should stop counting a move's animation at its end (slide.c knows it), or the harness read the
  `slide: drag:` line's own gap max for the move and keep the frame stats for the landing separately.
- [ ] 2026-10-31, espforge v0.6.0-rc.2 to rc.4 (harness `--ota` on COM5): `memory.heap_numbers`, read right after an
  OTA install, reports the largest free internal block at 32-39 KB (baseline min 40). Every run on a flashed build
  and a re-run of the suite a few minutes later read 43-44 KB, on rc.2, rc.3 and rc.4 alike; v0.3.0-rc.1's `--ota`
  run showed 36. Something the install leaves behind (the updater's buffers, the first-minute check) fragments the
  heap for a while. `heap_settled()` only waits for the free size to settle: in an `--ota` run also wait past the
  first minute (as `first_minute_wait_s()` does for perf) or for the largest block to settle, and find out which
  allocation it is.
- [ ] 2026-10-31, espforge v0.6.0-rc.6 to rc.7 (harness on COM5, `presence.dim_off_wake` about 25 times): while the
  panel sleeps (CPU at 80 MHz, the "screen" lock released) a test-console command was lost twice (`power`, `presence`:
  no reply within 15 s; the next command answered), and once the HTTPS PUT restoring the settings right after the
  wake ended in `TimeoutError: The read operation timed out` although the device applied them (`presence: config`).
  All three sit on power management's frequency changes (Group B, rc.2). Find out whether the USB console or lwIP
  loses a packet across a DFS switch (an `esp_pm_lock` for the console while a command is pending?); meanwhile the
  harness could resend a console command once when no reply comes, and retry an idempotent PUT once.
- [ ] 2026-10-31, esp32-s3-meteobus v0.3.2-rc.1 (harness `--flash` on COM5, PR #25): the display's lowest internal free
  RAM read 18 KB (its limit 25; the run before, 25 exactly; the rc's `--ota` run, 26) in the minute where the
  harness's HTTPS snapshot (an incoming TLS session on the settings server) overlapped a bus request's TLS GET right
  after the bus map closed. The download gate (`net_dl_begin/end`, rc.3) keeps the display's own TLS downloads to one
  at a time but does not count an incoming HTTPS session: a request served by `esp_https_server` can land on top of a
  download. Measure the two handshakes' peak, then either make the server's session wait for the gate (a hook in
  forge_net's web.c around the session open) or give the gate a `net_dl_server_busy()` the pollers check.

## Emulator (web/emu)

- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.1.1: the browser emulator. PR #9 (generic part). The app's port adds, worth
  having in the generic part: `app_main` run as a task with Wi-Fi stubbed "up" (main.c unchanged, no routes moved);
  real binary semaphores in `emu_tasks.c` (a task waiting for another's answer never got it); `emu_time.c` applying
  the firmware's TZ (Emscripten's `localtime_r` uses the browser's zone); `make_flasher_site.py --emu` copying
  `settings.html` and `emu-settings.js`; miniz's tinfl for `png_rows`.
  **→** Merged (PR #9, which already has app_main as a task, binary semaphores, TZ, `--emu` copying the settings
  files). miniz for `png_rows`: PR #12, with a fix to `emu.mk` (an app's `FORGE_CORE +=` replaced the list).
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0: Emscripten's `strftime("%z")` ignores TZ too (it wrote local time as
  +00:00): the app computes the offset from the date (`rtc_iso_local`). A lesson for web/emu/README.md.
  **→** PR #12 (web/emu/README.md), PR #13 (LESSONS L196).
- [x] v0.6.0-rc.6, PR #36, 2026-10-08, weather_amoled PR #28: its emulator builds forge_presence's real `presence.c` (not a stand-in like
  `web/emu/forge/emu_presence.c`) with the app's hooks: it needed only a `testcon_register` / `testcon_add_where` stub
  and NVS `i16` in the emulator's NVS. espforge's emulator could do the same and drop `emu_presence.c`.
  **→** PR #36: `emu.mk` builds `presence.c`, `emu_presence.c` is gone, the board's microphones and motion sensor answer
  "not there" from `emu_stubs.c`, `emu_nvs.c` has `i16`; the smoke test checks `/api/presence` and the task's start
  line. The starter's `main.c` already passes the hooks from `app_main()`, so `emu_main.c` passes none.
- [x] v0.6.0-rc.6, PR #36, 2026-10-09, esp32-s3-rtcquebec v0.4.0-rc.1: the CI emulator job builds the latest stable release (the site's
  "Try it"), so a pull request that changes the emulator or the UI is never built for the browser in CI (rtcquebec's
  PR #8 passed "emulator" on v0.3.3 while its Makefile lacked USE_SETTINGS). Build the branch too on pull requests
  (and keep publishing the stable one).
  **→** PR #36: a pull request builds its own commit and runs the smoke test on it (job `emulator`, `THIS_COMMIT`);
  `main` and tags keep building the latest stable release for the site. The `push` trigger is still `main` only.

## Lessons (docs/LESSONS.md)

- [x] v0.6.0, 2026-10-29, esp32-s3-meteobus port/esp32-p4 (docs/P4-PORT.md "Lessons"): identify a new board from its product
  page or a photo before porting (a guessed EV board and V1 panel table cost hours); take pins from a working setup for
  the same board and an I2C scan, not its schematic (amplifier GPIO 11 not 20, no ES7210); "unreachable" over the
  network: count packets on the board first (`netstat`: a co-processor's network split, then a router isolating Wi-Fi
  clients); LVGL sends DRAW_POST only when the band meets the object itself, not its ext draw size, so a badge past
  its object's edge needs `LV_OBJ_FLAG_OVERFLOW_VISIBLE` (pictest found 7 rows missing); esp_hosted's SDIO pools are
  allocated before the scheduler and can starve the idle task (boot loop: queues 12 + 12); on the P4 move frames with
  the 2D-DMA (~8 ms a 1024x600 frame; CPU 26-28 ms, PPA scaler 26.5 ms).
  **→** L206-L211 (docs/LESSONS.md, the v0.6.0 section).

- [x] v0.6.0, 2026-10-11, esp32-s3-meteobus v0.2.1-power: measure the split before guessing, again (L54/LESSONS 29): three
  plausible zoom speed-ups measured nothing (the column table in internal RAM and an unrolled loop: fill 185 vs
  188 ms; the other core's TLS and tile decoding held back until the zoom ended: 54 fps both ways; 64-bit divisions in
  the per-frame tables: none), while a cycle counter around the overlay blend found 40 % of the fill at once. And a
  harness check can be wrong in the test, not the firmware: "CPU at 80 MHz while off" failed because the command that
  asks runs at 240 MHz.
  **→** L203, and L202 for the harness check that was wrong in the test.

- [x] v0.6.0, 2026-10-10, esp32-s3-meteobus (PR #13): a settings-page list whose rows act on the list they were drawn from
  (My stops: `rm.onclick = () => saveFavs(favs.filter((_, k) => k !== i))`) removes the wrong item when a tap lands
  between a save the display accepted and the page's redraw (Remove right after Move up: the old first stop went).
  Found as a flaky Playwright test on the Mac runner; reproduced every time by holding the display's answer back 1.5 s
  (`page.route` + `route.fetch`, then a delay, then `route.fulfill`). Fix: disable the rows during a save and show the
  saved list at once. Lesson: a flaky UI test may be a real race; hold the reply back to make it deterministic.
  **esp32-s3-rtcquebec's `main/web/index.html` has the same handlers** (its `saveFavs`): port the fix there.
  **→** L205. esp32-s3-rtcquebec already has the fix (its PR #11 `fix/stops-tap-during-save`, in v0.4.1, with the delayed-reply
  test): nothing to port.
- [x] v0.6.0, 2026-10-09, esp32-s3-meteobus v0.2.0-rc: LVGL 9.2's `LV_LABEL_LONG_DOT` needs a fixed width **and** a fixed
  height: with the width set by its content (a pill sized to its text) it showed only "…"; with a fixed width but a
  content height it wrapped and ran into the line below ("Terminus Chute-Montmorency"). Size a one-line label from
  `lv_text_get_size()` (width capped, height one line). A lesson for docs/LESSONS.md (LVGL).
  **→** L194 amended: the height too (one line).
- [x] v0.6.0, 2026-10-09, esp32-s3-meteobus v0.2.0-rc: a weather app and a bus app merged into one firmware (docs/MERGE-PLAN.md
  there): the memory plan that held (one picture cache, the two maps taking turns, a poller's stack in PSRAM, one TLS
  download at a time) is worth a page in docs/ for future merges of espforge apps.
  **→** docs/MERGING-APPS.md, with the framework pieces that carry the plan (`slide_release_unneeded`, `net_dl_begin`,
  `net_bg_post`).

- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.1: during `lv_screen_load_anim()` (200 ms) `lv_screen_active()` is still
  the previous screen, and LVGL 9.2 has no public getter for the one loading: a timer that checks "is my screen
  shown?" took the map for closed as it opened. Note when the screen was opened and ignore the first 500 ms.
  **→** PR #13: LESSONS L192.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0: `esp_http_client`'s send buffer is 512 bytes by default: a ~1.3 KB query
  string can't be sent; set `buffer_size_tx`.
  **→** PR #13: LESSONS L193.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.1: an LVGL 9 label sized to its content (`LV_SIZE_CONTENT`) in
  `LV_LABEL_LONG_DOT` mode measures nothing and shows only "...": give it a width from `lv_text_get_size()`.
  **→** PR #13: LESSONS L194.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.2.0: building URLs with `snprintf` from pieces that hold `%5B` (encoded
  query strings) reads them as conversions: pass such pieces as `%s` arguments.
  **→** PR #13: LESSONS L195.
- [x] v0.3.0-rc.1, 2026-10-07, esp32-s3-rtcquebec v0.3.0: the sound baseline is the 90th percentile of a 5 s calibration: speech
  during it set it 30 dB too high (-35 instead of -67 dBFS) and the screen would never have dimmed. The settings
  page should say "quiet" plainly, or the calibration use the median, or reject a spread that wide.
  **→** PR #14: the median, a spread over 12 dB refused with the old baseline kept (`"cal":"noisy"`, shown
  on the page); host test.
- [x] v0.5.0-rc.1, 2026-10-09, esp32-s3-rtcquebec v0.4.0: L197 (code a console command reaches runs on the
  console's 4 KB stack: a `screen stop2` overflowed it), L198 (tools follow the staged build).
  **→** PR #26.
- [x] 2026-10-09, esp32-s3-rtcquebec v0.4.0-rc.1: L199 (the desktop app's shell and the component manager's cache), L200
  (a page with slow frames loses drags; long presses from moved fingers).
