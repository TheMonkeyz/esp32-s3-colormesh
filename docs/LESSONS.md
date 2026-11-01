# Lessons

What the project this framework came from (weather_amoled, published as TheMonkeyz/esp32-s3-weather: an ESP32-S3
AMOLED weather display, built with Claude in six days, September 29 to October 4, 2026) learned the hard way. Each
lesson is a rule, where it came from, and how to check it. "bug N" refers to the numbered list in that project's
CLAUDE.md; "vX" is the release where it was found or fixed.

Cite lessons by number (L12) in commits and fix plans. Add new ones at the end of their section with the next free
number; never renumber.

Contents: [Flashing and transfers](#flashing-and-transfers) ·
[Builds, versions and releases](#builds-versions-and-releases) ·
[The harness as a release gate](#the-harness-as-a-release-gate) ·
[Flaky and time-window tests](#flaky-and-time-window-tests) · [Working with the user](#working-with-the-user) ·
[Debugging method](#debugging-method) · [ESP32 memory](#esp32-memory) · [RTOS and tasks](#rtos-and-tasks) ·
[Flash writes and caches](#flash-writes-and-caches) · [Display](#display) · [LVGL](#lvgl) ·
[Touch controllers](#touch-controllers) · [Wi-Fi setup and offline](#wi-fi-setup-and-offline) ·
[Web, HTTPS and captive portal](#web-https-and-captive-portal) · [OTA](#ota) ·
[Parsing and robustness](#parsing-and-robustness) · [i18n](#i18n) · [Tooling on Windows](#tooling-on-windows)

---

## Flashing and transfers

**L1. Stage every image under a unique name and compare md5 before flashing.**
Why: weather_amoled, September 29-30: twice a rebuilt file pushed from the *same* staging path delivered the previous
build; once a fresh staging directory did too. Each time a test "failed" on code that wasn't on the board.
Check: `python tools/devloop/stage.py` copies each part to `.devloop/stage/` under a new name and prints both md5s;
refuse to write `flash.request` unless they match.

**L2. Before testing, read which image is running, in a fresh log.**
Why: September 30 the board was found on an older release candidate in `ota_1`, not the test build just flashed;
the test was void.
Check: `ota: Running <ver> from ota_N, channel …` (docs/PROTOCOL.md §3) after the last flash or restart, and the
version is the one you built. The harness's `--expect vX` fails on any other version.

**L3. A USB flash writes `ota_data_initial.bin` too.**
Why: with two OTA slots, a board last updated over Wi-Fi keeps booting `ota_1`, whatever was written to `ota_0`.
Check: the flash helper flashes every part in `<build_dir>/flasher_args.json` (it includes `otadata`); after the
flash, L2.

**L4. Ask for a generous log window, then stop it as soon as the line you need is there.**
Why: waiting out a 330 s window for a line that came at 20 s cost minutes per iteration. `serial_log.txt` is only
written when the window ends (within ~1 s of a stop).
Check: tail `.devloop/serial_live.txt`; `python tools/devloop/devloop.py stop` (or Q / Esc in the helper window).

**L5. Read the helper's result before the log.**
Why: a failed flash followed by a log of the old firmware looks like a firmware bug.
Check: `flash.status` (`flash_failed`?), then `flash.done` (`exit=0`, `errors`, `warnings`, `resets`,
`stopped_early`), then grep the log. `flash.status` says `logging` ~2 s before `serial_live.txt` is recreated: delete
the old one before waiting on it.

**L6. The flash helper picks up changes to its own script only after a restart.**
Why: an edited `flash_helper.ps1` kept running the old code; `monitor.ps1` is reloaded on every run.
Check: after editing the helper, restart it: stop Claude's background helper task and start it again (or ask the
user to close and restart `start_flash_helper.bat` if they run it in a window).

**L7. Anything cached about the board is reset after a flash or an install.**
Why: v1.12.0-rc.3: a cached "no key" from the firmware before a flash made the harness skip its key-protected
checks.
Check: the tools clear `.devloop/ip` and `.devloop/key` after every flash and OTA install (docs/PROTOCOL.md §1).

**L8. Check whether someone else is using the board before flashing.**
Why: the board, the helper and the COM port are shared between the user and every session.
Check: `flash.status` is `idle` and `serial_live.txt` isn't growing, in every project that uses the board
(weather_amoled keeps those files in its repository root, espforge in `.devloop/`), and no `flash_helper` or
`monitor.ps1` process is running.

**L191. To keep a restart's boot log, restart through the test console with the port open.**
Why: on the ESP32-S3's own USB (Serial/JTAG), esptool's hard reset re-enumerates the device, and the ~2.5 s before the
port is back are lost (L154): the ROM banner, the reset reason, the bootloader's `SPI Mode` line, so the harness
printed `flash ?` instead of `flash QIO`. The test console's `reboot` is a software restart that leaves the USB
connected: with the port kept open the log is whole from `ESP-ROM:` on (weather_amoled October 6 on COM5, then
espforge's two helpers the same evening: `rst:0xc`, `SPI Mode : QIO`, `console ready` at 2.97 s). An RTS pulse with
the port open did not restart the board.
Check: `reboot.request` sends `reboot` on the open port and waits up to 4 s for `ESP-ROM:` or `rst:0x`, else falls
back to esptool (no console, a hung board); `flash_helper.log` says which. That restart is not counted in
`flash.done`'s `resets`. A flash still loses the first seconds: tests must not need lines printed before ~3 s (L154).

## Builds, versions and releases

**L9. Label every test build above the current release.**
Why: a board running a build labelled below the stable release is offered that release and may install it. The
firmware compares X.Y.Z first; for the same X.Y.Z a test label counts below any rc: `v1.12.0-fix.N` is offered
`v1.12.0-rc.N`, while `v1.12.1-x` never is.
Check: `version.txt` (git-ignored) holds e.g. `v0.2.0-name.3`; order in docs/PROTOCOL.md §5; delete `version.txt`
when done, or later builds keep the label.

**L10. `version.txt` is read at CMake configure time: touch `CMakeLists.txt` after changing it.**
Why: an unchanged CMakeLists kept the old label in the image. A build outside a git checkout without `version.txt`
reports `1`.
Check: `diag: firmware vX` / `test: pong vX` in the next log.

**L11. Test builds go to their own build directory with their own sdkconfig.**
Why: keeps the repo's `sdkconfig` out of the way and test config lines (debug prints, profiler) out of commits.
Check: `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`.

**L12. After changing `sdkconfig.defaults`, delete the build's sdkconfig and reconfigure.**
Why: an existing sdkconfig keeps its values; an option present as "not set" ignores the new default.
Check: grep `build\v55\sdkconfig` for the option after `idf.py … reconfigure`.

**L13. The PC and CI build with the same ESP-IDF version. Change both together.**
Why: weather_amoled CI used v5.4.2 while the PC had v5.5.4 until v1.9.0; test builds didn't match releases (Easy
Connect's failure event carries different data in the two, L113).
Check: `forge.json` `idf` drives CI; `idf.py --version` on the PC.

**L14. Build exactly what you commit.**
Why: v1.12.0-rc.1: work was split out of a tree that held the next group's changes by a script; it dropped an
`#include`, and the tag's CI build failed (no release).
Check: `git stash push -u` to set later work aside, build the tree as it will be committed, commit, `git stash pop`.

**L15. Run the host tests before tagging when a C file they compile changed.**
Why: v1.12.1-rc.2's release failed in CI: the host tests' HTTP client stand-in (`tests/host/shim/`) lacked an event
name the firmware had started using.
Check: `wsl make -C tests/host`.

**L16. The CHANGELOG section exists before the tag.**
Why: it becomes the "What's new" list on the display's update screen and the settings page (`notes.json`). Tags
without a section shipped with no notes. `-rc` sections are shown to Beta users only; the stable section repeats
everything.
Check: `## vX.Y.Z - YYYY-MM-DD` (or `## vX.Y.Z-rc.N - …`) at the top, one `- ` line per change, written for the person
holding the device.

**L17. Pick the version number by content: new features bump the minor.**
Why: the user asked "why .1?" for a release with features; the v1.10.1-rc series shipped as v1.11.0.
Check: before writing a stable section, list what the series added. A feature series starts at
`vX.(Y+1).0-rc.1`.

**L18. Claude publishes release candidates; a stable tag needs the user's OK.**
Why: the user's rule (October 2): "you may publish and test rc releases" but stable needs permission. A tag is a
release: every device installs what it builds.
Check: docs/RELEASING.md.

**L19. If CI fails but a local build works, suspect `main/idf_component.yml` versions.**
Why: CI uses the component manager; a cloud build without network uses vendored copies.
Check: compare the versions pinned there with what the local build resolved (`dependencies.lock`).

**L20. In a cloud build, changing sdkconfig re-runs CMake: keep `IDF_COMPONENT_MANAGER=0` in the environment.**
Why: otherwise CMake looks for `idf_component_manager` and fails (network allowlists blocked PyPI, the component
registry, dl.espressif.com, Docker Hub; only GitHub worked).
Check: docs/WORKFLOW.md "Working without USB".

## The harness as a release gate

**L21. A gate fails on what it didn't measure.**
Why: October 2 evaluation: a third of the metrics had no limit, and a reused log window dropped every boot metric
without a word; the run "passed".
Check: a baseline metric not measured is MISSING, a measured one without a limit is NEW; both fail like a
REGRESSION. A test that skips something on purpose says so with `ctx.skip(pattern, reason)`.

**L22. Tests keep their own log positions.**
Why: a test that moved the shared log mark hid a crash from the restart check.
Check: `at = len(ctx.log.lines())`, `wait(..., start=at)`; only the harness calls `log.mark()`.

**L23. Never restart a board in the first 60 s after an update.**
Why: October 1: the harness restarted rc.2 right after the user installed it; the bootloader rolled back, and the
harness tested the old build and reported a pass.
Check: `GET /api/update` `pending_verify` and `uptime_s`; wait for `marked valid`; `--expect vX`.

**L24. Give the mock every state the page can be in.**
Why: a JavaScript parameter named `t` hid the page's translation function `t()`; the Install button never showed.
It shipped in two releases because nothing tested the "update offered" state (L145).
Check: the mock server scripts every state (`POST /__update`); one Playwright test per state.

**L25. Straight synthetic touches pass while real fingers fail. The user tries it before it is done.**
Why: the harness's drags passed every time while the user's curved swipes were missed, quick flicks went to LVGL and
fast fingers produced false "up"s (bug 21h, bug 23).
Check: after the harness, ask the user to try the gesture during a log window, and read the log of that try.

**L26. A floor equal to the worst case seen is not a floor; sample the lowest path.**
Why: the internal RAM check read only the normal boot; the reconnect path (three TLS clients and a JSON parse) went
to 5 KB unseen.
Check: measure memory at the end of the heaviest path too (`internal_min_kb.reconnect`), set limits with margin.

**L27. `--update-baseline` proposes; a person reviews.**
Why: rewriting the baseline erased hand-tuned limits and their notes.
Check: it writes `baseline.proposed.json` (new `ref`s, limits and notes kept, new metrics marked); diff it, copy it
over `baseline.json`, say why in the commit.

**L28. Heavy test-console commands start their own task.**
Why: the render bench on the 3 KB console stack reset the board. A console task with a PSRAM stack crashed on an
NVS read (L64).
Check: the console task only parses and dispatches; `testcon` stack internal.

**L29. Match a command's reply from the command's position, events from the test's position.**
Why: an event can be logged before the console's reply to the command that caused it.
Check: `Log.wait(start=…)` with the right start.

**L30. A simulated tap lasts 120 ms.**
Why: a 60 ms tap fell entirely inside a 100 ms redraw and was never seen.
Check: `tap` / `press` defaults in the touch injector.

**L31. Background work on the device skews measurements: let it settle.**
Why: an automatic render bench 45 s after boot blocked the screen 1.5 s and swallowed the harness's swipes; a map
saved to flash after a place change made a measured drag crawl (3 fps, first frame after 2 s).
Check: no automatic benchmarks (on request only); wait until background writers have been quiet ~5 s.

**L32. Check the result of a move by state, and start where the move is possible.**
Why: a drag test checked the screen name, but a page change stays on the same screen; from the last page "drag up"
bounced and the check still passed.
Check: query the page/state through the console; go to a known start first.

**L33. An unexpected restart fails the test, even if every check passed.**
Why: restarts hide bugs (a rollback, a watchdog).
Check: the harness scans for `rst:0x` and decodes `Backtrace:` with `addr2line` against the ELF of the build on the
board (any other ELF gives wrong names).

**L34. An outage of an outside service is not a firmware failure.**
Why: October 3: Open-Meteo timed out 10 of 16 requests from the display while the PC got answers in 0.07 s; five
tests of an rc failed for it.
Check: wait out the device's "service unreachable" state (minutes), note it in the report, fail only on firmware
behaviour. A 200 can carry the service's own error (Open-Meteo's `allEndpointsUnavailable`, weather_amoled October 4): log
the status and size with an unexpected body, so nobody reads it as a rate limit.

**L35. A command that gets no answer makes the harness send `where`.**
Why: `where` takes no lock; the failure then says where the display stuck. That is how the raw-frame hangs were
found (L52).
Check: docs/PROTOCOL.md §2.

## Flaky and time-window tests

**L36. A wait outlasts the firmware's own retry.**
Why: after the network came back, the first fetch is retried after 30 s; a 30 s wait failed a run when one request
timed out.
Check: read the firmware's retry interval before choosing a timeout (75 s there).

**L37. A flaky check needs several runs before it is called fixed.**
Why: v1.10.0's Easy Connect channel pick passed sometimes; after the fix, 4 runs out of 4.
Check: run the suite 3-4 times; report n/N.

**L38. A rule with a time window needs a test that crosses the window.**
Why: v1.12.0-rc.3: "setup opens by itself for 15 minutes only" passed review and failed on the board: the window
ended while setup was open, and nothing closed it.
Check: a console switch that shortens the window for one boot (`wifi offline-boot-short`: 60 s), and a test past it.

**L39. Two snapshots of a live screen differ anyway: compare at one instant.**
Why: a status page ticking every second made every picture comparison fail.
Check: a firmware-side comparison (`pictest`) instead of two HTTP snapshots.

**L40. Probe right after the periodic event.**
Why: the minute tick and the update check each re-rendered whole pages; drags at random times rarely hit it.
Check: trigger the event (`POST /api/update {"action":"check"}`, wait for the minute line), then measure.

## Working with the user

**L41. Post a one-line progress note before anything that takes more than a minute.**
Why: long silences read as "stuck".
Check: "Building v0.2.0-x.3 (~2 min)", "Flashing, then a 120 s log".

**L42. Ask the user to touch the board during the log window, and say exactly when and what.**
Why: a gesture made before or after the window leaves no trace.
Check: "Now, for the next 60 s: swipe left twice, then long-press the centre."

**L43. Never take over the user's screen; prefer the flash helper.**
Why: the user's rule from the first day.
Check: no computer-use or terminal typing without asking first. Start the flash helper windowless in the
background (`run_in_background`), not with `start_flash_helper.bat`, which opens a window (user, 2026-10-06).

**L44. Relay `>>> ASK THE USER` from the harness at once.**
Why: steps like the Easy Connect phone scan have a time limit.
Check: watch the harness output (and `.devloop/harness.ask`) while it runs.

**L45. The user's words are the best signal for feel.**
Why: "follow the finger", "a 0.1 s delay before a drag", "corruption on the graph" each found a bug no frame-rate
number showed.
Check: turn each phrase into a measurement (drag start ms, pixel comparison) and a test.

**L46. Report what the log shows, not what a message suggests.**
Why: a harness line "Installing…" was relayed as an install while it meant something else.
Check: quote the log line.

**L47. Decisions that belong to the user are asked at the right moment, not up front.**
Why: the October 3 fix plan held four product questions (security model, alert re-chime, reboot on OOM, setup
timeout); everything else was done first.
Check: the fix plan marks items that depend on an answer.

**L48. Put the user's settings back after a test.**
Why: tests change language, presence delays, quiet hours, channels.
Check: read the values first (`GET`), restore them in a `finally`, even on failure.

**L49. Snapshots can't show touch problems: ask the user to tap through a new screen.**
Why: bug 18: a full-size arc caught every tap on a new screen (L91); the snapshot looked perfect.
Check: a log window while the user taps every row.

**L50. Sliders act while dragging and save on release.**
Why: the user expects the screen to change under the finger.
Check: a preview call during the drag, one NVS write at the end.

## Debugging method

**L51. Logs over guesses.**
Why: every hard bug in weather_amoled was found by reading or adding a log line: three Wi-Fi offline causes, the
esp_lcd race, drag delays, a double flash save.
Check: before a fix, a log line that shows the cause.

**L52. For hangs: breadcrumbs plus a lock-free `where`.**
Why: the display hangs of v1.11.0 took days of symptoms and minutes of fix once breadcrumbs (`slide_phase`,
`raw_phase`, `raw_band`, `lvgl_inflight`) printed by a command that takes no lock pointed at the race.
Check: `forge_core` breadcrumb providers; `where` on the console.

**L53. Measure the parts before optimising a part.**
Why: font kerning was 71% of rendering; LVGL's layout pass was 4.4 of 7.7 ms of a list's frame while the obvious
suspect (per-icon pictures) saved nothing; the flash mode was worth 30%.
Check: time each part in a throwaway build, or the LVGL profiler (docs/TESTING.md).

**L54. Throwaway builds for temporary logging and fake inputs. Never commit them.**
Why: the drag-delay culprits were found by logging each cache invalidation with `__builtin_return_address(0)` and
each redraw with `esp_backtrace_print()`; alerts, lightning and offline boots were tested with fake inputs.
Check: label them (`vX-alerttest.0`), `git checkout` the files after, flash a clean build.

**L55. Fake inputs live where the real ones do.**
Why: fake lightning marks at fixed screen positions didn't follow zooms and looked like a bug; at fixed lat/lon they
behaved.
Check: inject at the data layer, in the data's own units.

**L56. Read the source before relying on an API's behaviour.**
Why: `lv_obj_scroll_to_y()` clamps at the ends, `lv_obj_scroll_by()` sends bubbling events, esptool writes a QIO
bootloader's header as "dio", a static array is internal RAM. Each cost hours.
Check: `managed_components/lvgl__lvgl`, the ESP-IDF Kconfig and source.

**L57. Reproduce off the board when you can; check the test fails on the old code.**
Why: host tests reproduced parser crashes in seconds.
Check: `git show HEAD:main/x.c > /tmp/x.c`, run the new test against it, see it fail.

**L58. Debug build for memory corruption.**
Why: corruption crashes far from its cause.
Check: `sdkconfig.debug` (heap poisoning, end-of-stack watchpoint, stack protector); read the log for
`CORRUPT HEAP`, `Stack canary`, `Stack protection fault`. Its performance numbers don't count.

**L59. Keep crash details without a watcher.**
Why: crashes used to exist only if someone was watching USB at the time.
Check: core dump to flash, summarised at the next boot (`diag: coredump …`) and erased; decode with
`xtensa-esp32s3-elf-addr2line -pfC -e <elf>`. The partition comes with a USB install (L128).

## ESP32 memory

**L60. Internal RAM is the scarce resource; small `malloc`s land there first.**
Why: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` puts every allocation under 16 KB in internal RAM. LVGL's thousands
of small allocations left 10 KB free and 0 KB "min ever"; the HTTPS page then arrived truncated (L119).
Check: `forge_lvgl`'s `lvgl_mem` (LVGL heap in PSRAM via `CONFIG_LV_USE_CUSTOM_MALLOC`; the linker needs
`-u lv_malloc_core`); `heap` on the console; the harness's `internal_min_kb`.

**L61. A static array in a `.c` file is internal RAM. Big ones get `EXT_RAM_BSS_ATTR`.**
Why: 3.8 KB of zoom tables dropped the internal low point to 3 KB; 46 radar structs (48 KB of palettes) moved to
PSRAM raised it from 9 to 50 KB.
Check: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`; `idf.py size-components`; never touch such data from an ISR
(L80).

**L62. Big structs never on the stack.**
Why: bug 3: arrays of ~1 KB structs on the radar task's stack overflowed it.
Check: `static` (plus L61), or heap in PSRAM.

**L63. Size stacks from measured high-water marks; paths that end in a restart log their own.**
Why: six task stacks needed ~12.7 KB more than given; Easy Connect's success path ran on the 2.3 KB system event
task with ~600 B spare; a snapshot rendered a screen on a 7 KB HTTPS stack with 992 B spare. The periodic report
never sees a path that restarts.
Check: `diag: tasks` lines; `uxTaskGetStackHighWaterMark` logged at the end of rare paths;
`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`, `CONFIG_ESP_MAIN_TASK_STACK_SIZE`.

**L64. Task stacks that touch flash or NVS stay in internal RAM.**
Why: a PSRAM stack crashed on an NVS read (flash busy = PSRAM cache off). TLS key generation needs an 8 KB internal
stack for the same reason.
Check: `xTaskCreate` (internal) rather than a PSRAM stack for those tasks.

**L65. Big PSRAM users fragment PSRAM for the others.**
Why: with a 2.2 MB picture cache, lodepng's 2-3 MB decodes failed. A row-by-row PNG decoder using the ROM's inflate
needs ~50 KB.
Check: `forge_core/png_rows`; `heap` largest block; the harness's `psram_min_kb`.

**L66. Route library heaps to PSRAM where they allow it.**
Why: cJSON parse trees (~100 KB for a 10 KB forecast) and TLS sessions (~40-60 KB each) otherwise eat internal RAM.
Check: `cJSON_InitHooks` with PSRAM malloc; `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.

**L67. Count failed allocations and fallbacks; expect 0.**
Why: memory pressure shows up as odd behaviour long before a crash.
Check: `heap` reports `failed_allocs` and `lvgl_fallbacks`.

**L68. Don't grow a struct saved as one NVS blob; use typed keys.**
Why: a blob is accepted only if its size matches: the first field added would drop the user's settings. Places were
moved to typed keys in v1.12.0; new presence settings went into separate keys.
Check: `nvs_set_i32`/`nvs_set_str` per field; read old blobs once and rewrite them.

**L69. Check every NVS write.**
Why: a refused write silently lost a setting.
Check: `forge_core/nvs_check`; the API answers 500 "not saved" and the page shows it.

## RTOS and tasks

**L70. Waits that users may interrupt use task notifications, not `vTaskDelay`.**
Why: bug 12: after a dropped connection the radar slept 20 s and ignored swipes.
Check: `ulTaskNotifyTake(pdTRUE, ticks)`; long downloads check a "wanted elsewhere" flag and bail.

**L71. Anything that blocks the UI task for more than ~50 ms loses quick flicks.**
Why: rendering a whole cached picture at once blocked LVGL 60-180 ms; a flick could start and end unseen.
Check: background rendering in 64-row strips (~15-25 ms each); `diag: display` longest lock hold.

**L72. LVGL calls from outside the LVGL task hold the display lock, briefly.**
Why: LVGL isn't thread-safe; long holds stall the UI.
Check: `display_lock()` / `display_unlock()` (board API); `diag: display` names the longest holder.

**L73. Background work yields to the user.**
Why: flash writes and picture rendering during a drag made it stutter.
Check: background writers wait for "UI busy" to clear (finger down, move running).

**L74. Restarts asked for during the update-confirm window wait for it.**
Why: a restart before confirmation rolls the update back (L126).
Check: `forge_ota` restart-when-safe for every restart path (settings, Wi-Fi save, Easy Connect).

## Flash writes and caches

**L75. Flash writes stall the whole chip.**
Why: during an erase or write the code and PSRAM caches are off; a background map save made drags crawl.
Check: schedule writes when the UI is idle (L73).

**L76. Erase and write sector by sector, yielding between.**
Why: bug 10: one large `esp_partition_erase_range` tripped the task watchdog (IDLE0).
Check: 4 KB at a time with `vTaskDelay(1)` between.

**L77. Check what a save stores.**
Why: a download for one place finishing after a switch was saved as the other place's map.
Check: tag the data with its key at download start; compare at save time.

**L78. Write the header last.**
Why: a power cut mid-save must leave the old entry invalid, not a valid header over half the data.
Check: data sectors first, header (with a checksum) last.

**L79. QIO flash makes code from flash ~30% faster, but only a USB install changes the bootloader.**
Why: the program runs from flash through a 16 KB cache and LVGL's code is large; full-screen renders went 67 → 45 ms.
ESP-IDF writes a QIO bootloader's header as "dio" and the bootloader switches itself; boards updated over the air
keep their old bootloader (DIO speed).
Check: `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`; the boot log's flash mode; keep a DIO bootloader to reproduce OTA-updated
boards.

**L80. Code and data in PSRAM or flash are unreachable from an ISR while the cache is off.**
Why: an interrupt during a flash write that touches PSRAM data crashes.
Check: ISR code `IRAM_ATTR`, ISR data internal; `EXT_RAM_BSS_ATTR` data only from tasks.

## Display

**L81. `esp_lcd` isn't thread-safe: its transfer-done interrupt runs on the LVGL task's core.**
Why: v1.11.0: with the interrupt on the other core, raw frames hung for good every few slides.
Check: `isr_cpu_id` = the LVGL core in the panel IO config.

**L82. No `esp_lcd` call while a band is in flight; any call from outside LVGL waits for LVGL's last band.**
Why: v1.12.0: `display_brightness()` from the presence task (core 0) wrote a command while LVGL's last band was
still on the bus.
Check: an in-flight counter (`lvgl_inflight`) waited on, bounded (100-200 ms), before every out-of-LVGL call.

**L83. Every wait in the display path is bounded and leaves a breadcrumb.**
Why: an unbounded wait turns a race into a frozen device with no log.
Check: 200 ms caps; `where` prints the phase.

**L84. Compile with `-O2`.**
Why: bug 13: `-Og` made animations choppy.
Check: `CONFIG_COMPILER_OPTIMIZATION_PERF=y`.

**L85. Turn off image antialiasing for full-screen scaling.**
Why: antialiased scaling of a 466×466 image cost frames.
Check: `lv_image_set_antialias(img, false)`.

**L86. Draw buffers live in internal DMA RAM: size them deliberately.**
Why: two 32-line buffers (2 × 30 KB) were the balance between band count and internal RAM; bigger ones starved
TLS (L119).
Check: `heap` DMA free after boot.

**L87. A full LVGL redraw is the floor for animation speed: measure it before promising fps.**
Why: LVGL 9.2 can't redraw a full 466×466 screen in under ~65-85 ms on this chip (no single hot spot); the 60 fps
moves had to be pictures copied straight to the panel (~8 ms from PSRAM, ~11 ms on an 80 MHz QSPI bus).
Check: `fps`, the render bench; docs/TESTING.md profiling. weather_amoled measured LVGL 8, 9.2 and 9.6 and two draw
units on October 4: stay on 9.2.2.

**L88. Panel quirks belong to the board component.**
Why: the CO5300 needs areas starting on an even and ending on an odd pixel (a rounder callback) and a +6 column
offset; the app must not know.
Check: `boards/<name>/board/`.

## LVGL

**L89. Events bubble from children: check the target.**
Why: bug 16: a list's `LV_EVENT_SCROLL` bubbled up through the pager; its handler read state from
`lv_event_get_target()` and crashed on null user data.
Check: use `lv_event_get_current_target()` and ignore events whose target is another object.

**L90. Decorative objects are clickable by default and swallow presses.**
Why: bug 7: long-press never fired over icon blobs and boxes.
Check: clear `LV_OBJ_FLAG_CLICKABLE` (a `passthrough()` helper). After opening an overlay with a long-press, ignore
presses for 800 ms and call `lv_indev_wait_release()`, or the same release closes it.

**L91. A full-size `lv_arc` is a full-size touch target.**
Why: bug 18: on a settings screen it caught every tap and drag; rows, Done and scrolling did nothing.
Check: arcs non-clickable with a separate touch zone; L49.

**L92. Turn off font kerning.**
Why: kerning lookups (`stbtt_GetGlyphKernAdvance`, a linear scan) were 71% of all rendering.
Check: fonts created with `LV_FONT_KERNING_NONE`.

**L93. LVGL's bundled lodepng returns an `lv_draw_buf_t*`, not raw pixels.**
Why: bug 1.
Check: use `db->data` and `db->header.stride`; bytes are R,G,B,A; free with `lv_draw_buf_destroy`.

**L94. A redraw that changes nothing is not free.**
Why: setting the same label text, a flag already set, a style again, a theme transition or a pending layout each
invalidated a cached picture, and the next drag waited 0.2-0.5 s (bug 21f).
Check: compare before setting; set flags only when they change; `CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0`; find the
culprits with a throwaway build logging each invalidation with a backtrace (L54).

**L95. Mark only what changed, where it changed.**
Why: marking every picture out of date each minute and at each update check made the next drag render a whole page
(~0.13-0.2 s).
Check: dirty-rows API for clocks; mark only screens that show the changed data.

**L96. Content changed on a hidden screen marks its picture out of date.**
Why: otherwise a drag shows the old content for a moment.
Check: every `ui_*` setter for a hidden screen calls the cache's dirty function.

**L97. Drawing outside LVGL hides the touch from LVGL: reset its input after.**
Why: bug 21b: after a drag drawn outside LVGL, the next swipe was ignored, or the last point replayed as a stray tap.
Check: forget the touch, clear `wait_until_release`, `lv_indev_reset()`.

**L98. `lv_obj_scroll_to_y()` stops at the ends; `lv_obj_scroll_by()` sends bubbling events.**
Why: past the ends the list didn't move while the picture did (a smeared graph); `scroll_by`'s SCROLL_BEGIN/END
bubbled and re-triggered layout every frame (4.4 of 7.7 ms).
Check: `lv_obj_scroll_by_raw()` for moves you draw yourself.

**L99. Decide a touch's axis like LVGL, and before LVGL.**
Why: a 2:1 rule missed curved swipes on a round screen; deciding at 16 px while LVGL scrolled at 10 gave quick flicks
to LVGL's slow scroll.
Check: larger axis after 10 px, in the touch read hook; LVGL's scroll limit raised to 20
(`lv_indev_set_scroll_limit`).

**L100. Zoom out from a wider picture, not by shrinking the old one.**
Why: bug 11: shrinking exposed black borders.
Check: load the wider image first, animate it from 2× down to 1×.

**L101. The picture of the screen shown must equal the panel; test it pixel by pixel.**
Why: a pixel comparison found the smeared hourly graph that no frame-rate number showed.
Check: `pictest` on the console; snapshot `picture` against `current`.

## Touch controllers

**L102. The CST9217 needs an ack after each read.**
Why: bug 6: without writing `D0 00 AB` after each read it stops reporting. NACKs while idle are normal (silence the
`i2c.master` logs); coordinates are mirrored in X and Y.
Check: board component's touch read.

**L103. A touch chip that NACKs instead of reporting "up": 5 failed reads in a row = up, and every finger loop is
capped.**
Why: bug 21c, 22b: a loop waiting for a clean "up" never ended, once while holding the display lock.
Check: one `finger()`-style function used by every loop; caps (3 s for a pure wait, 20 s while following); log when
a cap fires.

**L104. Don't poll a touch chip faster than ~10 ms.**
Why: bug 23b: read every millisecond (each read acked), the CST9217 answered "not in contact" for long stretches with
the finger on it; every fix built on those false "up"s only moved the problem.
Check: reads at most every 10 ms; a "fresh reading" flag; speed samples from fresh readings only (a repeated point
measured a flick as 0 px/ms).

**L105. Inject simulated touches at the controller read.**
Why: then wake-up, long-press and gestures run the real code.
Check: `forge_lvgl` touch inject (`tap`, `press`, `swipe`, `drag` on the console).

**L106. To find lost touches, log every read with its time.**
Why: bug 23a: reads 60 ms apart showed LVGL had the touch (its frames took 35-60 ms).
Check: a throwaway build logging `t_ms x y state` per read.

## Wi-Fi setup and offline

**L107. No reconnect attempts while a setup mode is on.**
Why: bug 20a: reconnect attempts made the radio hop channels; phones dropped off the setup AP and Easy Connect
failed.
Check: pause the reconnect timer when the AP or DPP starts, resume when it stops; `wifi status` `retries`.

**L108. `stop_dns_server()` leaks its socket: start the DNS server once, never stop it.**
Why: bug 20b: port 53 stayed taken, the next setup AP had no captive portal (bind `errno 112`).
Check: `components/dns_server`; the harness re-joins the setup network after Easy Connect.

**L109. Easy Connect (DPP) listens on the phone's channel.**
Why: bug 20c: the phone stays on its network's channel; on another channel the exchange ended in
`ESP_ERR_DPP_AUTH_TIMEOUT`. It worked online only because the station was already there.
Check: listen on the saved (or strongest) network's channel.

**L110. Find the saved network by name before a broadcast scan.**
Why: bug 20d: a broadcast scan (40-80 ms per channel, 16 records kept) missed a router on a busy channel and picked
channel 11 instead of 1.
Check: probe requests with the SSID, 120 ms per channel; broadcast only as fallback.

**L111. `esp_supp_dpp_bootstrap_gen()` only queues work.**
Why: calling `esp_supp_dpp_start_listen()` right after it returns `ESP_FAIL`.
Check: start listening from the `ESP_SUPP_DPP_URI_READY` callback.

**L112. To see DPP steps, `CONFIG_ESP_WIFI_DEBUG_PRINT=y` in the test build's sdkconfig only.**
Why: lines `wpa: DPP: …` show each step; too noisy to ship. ESP-IDF 5.5 renamed the option (it was
`CONFIG_WPA_DEBUG_PRINT`): the old name is dropped silently when the build reconfigures, and no line appears (L171).
Check: `build\v55\sdkconfig` only (L11); the INFO lines (request received, response sent, confirm timeout) give the
timing; the hex dumps need `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` too and slow the answer ~4x.

**L113. `ESP_SUPP_DPP_FAIL` carries different data on IDF 5.4 and 5.5.**
Why: an error code on 5.4, a `wifi_event_dpp_failed_t *` on 5.5 (`failure_reason`).
Check: L13.

**L114. Test offline paths without touching the router.**
Why: the October 1 bugs only appeared with the network unreachable at boot.
Check: console `wifi offline` (fake SSID in the station config, saved credentials untouched), `offline-boot` (next
boot only, RTC flag); the PC's Wi-Fi card joins the setup AP like a phone.

**L115. Wi-Fi names and passwords go into the driver's fixed fields with their length.**
Why: a 32-byte SSID has no NUL there; `strlcpy` kept 31 bytes (a legal network saved but never joined) and Easy
Connect's handler read past it into the password.
Check: copy with explicit lengths; validate 1-32 byte names, passwords empty, 8-63 chars or 64 hex.

**L116. The setup AP has a per-device password and opens by itself only for a while.**
Why: a published shared password plus an AP that came up during every router outage exposed the API.
Check: password generated at first start (letters and digits without look-alikes), shown on screen and in the QR;
automatic opening limited (15 min there), long-press always works (L38).

**L117. Answer the OS connectivity check on the setup network, or Windows opens a browser.**
Why: a PC that is online through Ethernet opened msftconnecttest → msn.com when the harness joined.
Check: console `portal windows-quiet` before joining. `netsh` answers in the display language: check for English
(L147).

**L118. The captive portal's DNS answers only on the setup network.**
Why: it answered on the home LAN too.
Check: bind to the AP interface.

## Web, HTTPS and captive portal

**L119. Hardware AES needs internal DMA memory; without it HTTPS pages arrive truncated.**
Why: bug 4: the page's buttons "did nothing"; the AES peripheral failed to allocate DMA bounce buffers mid-response.
With LVGL's heap in PSRAM it was turned back on (snapshots 2.2 → 1.5 s).
Check: send pages in 1 KB chunks; if a page arrives truncated, look at internal DMA memory first; the harness checks
the page arrives whole.

**L120. Two httpd servers need different control ports.**
Why: bug 5: the HTTPS server's default control port is already 32769.
Check: HTTP 32768, HTTPS 32769; `max_uri_handlers` = routes + wildcard.

**L121. Serve the captive portal over HTTP on the AP interface; HTTPS on the home network.**
Why: bug 14: sign-in browsers won't accept a self-signed certificate. iOS needs a small HTML body with the 302.
Geolocation only works on HTTPS pages.
Check: decide with `getsockname()` on the socket; on the home network redirect to the device's own address, never
the Host header.

**L122. Every change carries the device's key; the Host must be the device.**
Why: "anyone on the LAN can reconfigure the display" (October 2 evaluation). A custom header also forces a CORS
preflight the server never answers, so another site's page can't send one; a foreign Host is DNS rebinding.
Check: `X-Key` on every POST and `/api/snapshot` (401), JSON only (415), Host check (421); key in the QR as a URL
fragment (`#k=`), compared in constant time. Get it with `key` on the console (docs/PROTOCOL.md).

**L123. Non-ASCII through curl from Git Bash is mangled.**
Why: bug 17: "Montréal" was stored as Latin-1.
Check: write the JSON to a file (`printf` with `\u00e9`) and `curl --data-binary @file`.

**L124. The certificate is per device: after an erase, the browser warning comes back.**
Why: a new key is generated on first start (~180 ms; 8 KB internal stack).
Check: expected; say so to the user.

**L125. A long blocking handler blocks the whole server.**
Why: a Wi-Fi scan blocks the httpd task 2-3 s.
Check: the page scans only on request.

## OTA

**L126. Confirm a new image only after it has run 60 s connected to Wi-Fi.**
Why: until v1.12.0 any image that stayed up 60 s was confirmed, before Wi-Fi had even started: one whose network
never worked would have been kept. Without Wi-Fi, confirm after 10 min (a home without Wi-Fi keeps a working device).
Check: `ota: New firmware ran N s…: marked valid`; tools wait for it (L23).

**L127. Say so when an update was rolled back.**
Why: a silent rollback looks like "the update didn't happen".
Check: `esp_ota_get_last_invalid_partition()` at boot; shown once (log, screen, `rolled_back` in the API).

**L128. The bootloader and partition table only change with a USB / web-flasher install.**
Why: rollback needs `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in the bootloader; QIO needs a QIO bootloader (L79); a new
partition (core dump) needs a new table.
Check: tell users when a release needs one USB install; the firmware copes without the partition.

**L129. The updater installs only the same project name and a strictly higher version.**
Why: protects against a wrong image on the site.
Check: `esp_app_desc.project_name` = `forge.json` `app`; the version offered = the image's.

**L130. Devices poll a fixed URL: never rename the repository or the account.**
Why: every device looks for updates at the Pages address built in.
Check: `forge.json` `ota_site`; docs/RELEASING.md.

**L131. A failed download keeps Install, and the device re-checks soon.**
Why: it hid the update until the next check, up to 6 h. A check missed during an outage waited 6 h too.
Check: back to "available" with the reason; re-check 2 min later and when the station comes back.

**L132. Keep the notes small.**
Why: the device reads `notes.json` into a fixed buffer (24 KB in weather_amoled).
Check: `forge.json` `notes_max_bytes`; `make_flasher_site.py` caps it.

## Parsing and robustness

**L133. `esp_http_client_init()` can return NULL.**
Why: seven unchecked calls in the October 2 evaluation.
Check: `forge_core/http_once`.

**L134. Every array a parser indexes is checked.**
Why: a reply missing `daily.time` crashed every fetch: a reboot loop that the rollback couldn't catch, because the
image was already confirmed.
Check: `cJSON_Is*` and sizes before every use; host tests with partial replies (`tests/host`).

**L135. Cap after sorting, never before.**
Why: alerts were capped at four before the severity sort; a red warning listed fifth was dropped.
Check: a host test with the important item last.

**L136. Identify a thing by its stable code, not by a feed's id.**
Why: Environment Canada re-issues change the feature id; "new alert" chimed again at every re-issue.
Check: the identity key is documented and host-tested.

**L137. Index by date, not by position, across midnight.**
Why: the hourly view and "Today" showed yesterday's data between midnight and the next refresh.
Check: find today's index by date; a host test at 00:05.

**L138. Bound every decoder and scanner.**
Why: a shape scanner looped forever on `-]`; a wrapped chunk length overflowed a 32-bit `size_t`.
Check: size limits (e.g. 4096 px, 8-bit, 256-entry palette), host tests with hostile inputs.

**L139. Cut UTF-8 strings without splitting a character.**
Why: truncated names showed garbage.
Check: `forge_core/utf8`; host test.

**L140. Reset backoff when the network comes back.**
Why: a failed fetch kept its backoff (up to 10 min) across a reconnect.
Check: fetch at once on `GOT_IP`.

## i18n

**L141. An empty translation is not a missing one.**
Why: `""` showed as nothing on screen.
Check: `tr()` falls back to English on empty too; a host test checks every text in every language, with the same
printf conversions as English.

**L142. Check fit with snapshots, not by estimate.**
Why: an Inuktitut draft looked fine in a width estimate; six labels collided or wrapped on the board. French is
longer and wraps badly on a round screen.
Check: set the language through the API, snapshot every screen (including scrolled and overlay pseudo-screens),
shorten, give long lines explicit `\n`.

**L143. Protect tokens from generated translations.**
Why: an automatic syllabics converter turned unit letters into syllabics ("56 ᐢ").
Check: unit letters in backticks in the source TSV.

**L144. Never a bare string literal on screen.**
Why: untranslated text slips through.
Check: display text in `main/i18n_strings.h` (`X(T_ID, "en", "fr")`); page text in `I18N` in `main/web/index.html`.
Framework components return codes; the app translates.

**L145. A JavaScript name that shadows the translation function breaks the page.**
Why: `function fwNotes(t)` hid `t()`; the Install button never showed (L24).
Check: never name a parameter `t`; Playwright fails on any page error.

## Tooling on Windows

**L146. Microsoft Store Python virtualises `AppData\Local`.**
Why: Playwright started from the harness couldn't see its browsers.
Check: copy `%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-*` to `tools/webtest/.browsers` after a Playwright
update.

**L147. `netsh` answers in the Windows display language.**
Why: the PC-as-phone code reads English words.
Check: the harness stops with a clear message on another language.

**L148. Don't write edit scripts in shell heredocs.**
Why: Python edit scripts in heredocs mangled `\n` and `\d` more than once.
Check: write scripts with the Write tool, then run them.

**L149. `.github/` is a protected path for remote file tools.**
Why: Claude in the cloud couldn't write workflow files into the PC folder.
Check: hand the file to the user, or edit on GitHub; Claude Code on the PC can write it.

**L150. Git on the PC folder from a Linux VM.**
Why: lock files and temporary objects need delete permission; modes and line endings showed every file changed.
Check: `git config core.fileMode false`, `core.autocrlf false`.

**L151. `gh` is not on the PATH of shells started before its install.**
Why: sessions predate the install.
Check: Git Bash: `"/c/Program Files/GitHub CLI/gh.exe"`; PowerShell: `& "C:\Program Files\GitHub CLI\gh.exe"`.

**L152. A Playwright fixture option must not be named `offline`.**
Why: it is Playwright's own option and takes the whole browser offline, mock server included.
Check: name it `noInternet`.

**L153. No PIL on the PC: parse images with the standard library.**
Why: snapshot comparisons need BMP → rows without extra packages.
Check: `struct` / `zlib` (`tools/snapshot.py`).

## Found while bringing up espforge (October 4)

**L154. A PC monitor misses the first ~2.5 s after a reset on USB Serial/JTAG.**
Why: the port re-enumerates at reset; ESP-IDF's boot lines and an early `ota: Running` line were lost in one run out
of two (espforge v0.1.0-rc.0).
Check: print the lines tests rely on after ~4 s of uptime (forge_ota, diag do); don't make a test require
ESP-IDF's own `App version:` line.

**L155. `esp_wifi_set_config()` refuses while the station is connecting.**
Why: "wifi online" arrived during a retry, the config change was refused (`sta is connecting, cannot set config`,
the return value ignored) and the device kept trying the fake network forever.
Check: stop the retry timer, `esp_wifi_disconnect()`, then set the config; log a refused config.

**L156. Every way out of a screen must stop what the screen started.**
Why: the setup screen starts the setup network, which pauses the saved network's retries; leaving it through
the test console's `screen hello` (not its own tap) left the network open and the device offline.
Check: one `leave` function called from every exit; the harness's `screens` suite runs before `wifi_runtime`.

**L157. A request file left from an earlier session runs when the helper restarts.**
Why: the harness gave up, the user later restarted the helper, and it flashed the hour-old request.
Check: the flash helper drops requests present at its start.

**L158. A new partition table leaves old data where the core dump now lives.**
Why: weather_amoled's map cache sat at espforge's coredump address; ESP-IDF logged
`Incorrect size of core dump image` at every boot, and the harness fails on E lines.
Check: diag erases a core dump partition that holds no valid dump.

**L159. A new project has release candidates before any stable release.**
Why: espforge's first tag (v0.1.0-rc.1) built and released, then the Pages job stopped on "no stable release yet",
a rule written for a project that already had one; and the firmware read a missing stable channel as a bad site.
Then rc.2's site job never started: a new repo's `github-pages` environment lets only `main` deploy, not tags.
Check: the site is Beta-only until vX.Y.Z (`make_flasher_site.py site --beta`); "nothing offered" = up to date;
the environment has a `v*` tag rule (docs/NEW-PROJECT.md checklist) before the first tag.

## Moves that follow the finger (espforge v0.1.0-rc.3, October 4)

**L160. Ship picture-based moves in the framework, not only in the app that found them.**
Why: espforge left weather_amoled's slide.c out as "too app-specific"; its pager scrolled with LVGL at ~24 fps and
the user found it sluggish at once. forge_lvgl's slide.c (pictures copied to the panel) runs at 65-68 fps.
Check: `perf` suite (`swipe_fps.*` ≥ 50) and the `slide: drag` log lines.

**L161. A drag must not render anything when it starts.**
Why: rendering the page coming in took 35-45 ms; the finger moved on meanwhile, the page started late and jumped
("hiccups"). Neighbour pictures are now kept ready (rendered while nobody touches, refreshed every 2 s): first frame
15 ms.
Check: `slide: drag: first frame after N ms ... renders 0`.

**L162. Don't freeze while a release is being confirmed.**
Why: the touch chip reports brief false "ups", so an "up" counts after 60 ms; the picture stood still during that
wait, then snapped: a stall at the end of every swipe. It now goes on at the finger's last speed (at most 80 ms).
Check: `finger still max` in the drag line (15 ms, was 74).

**L163. Synthetic swipes can't find hiccups; log what a real finger does.**
Why: the harness's swipes were smooth at every step while the user felt hiccups. The drag line now has gap max, held
reads (error / brief up), the longest hold, the longest still finger, samples and renders: two tries with the
user's finger located both stalls.
Check: ask the user to swipe during a log window, then read the `slide: drag` lines.

**L164. Radio work never runs in a touch handler.**
Why: switching setup pages stopped the setup network and scanned for Easy Connect's channel inside the swipe's
handler: the screen froze for seconds. A task does it now; only the latest request counts.
Check: `navigation.setup_pages_slide` (the page moves at once).

**L165. Easy Connect: wait for the listen to start before deinit.**
Why: stopping right after starting deinitialised DPP while its listen was still queued in the supplicant's task:
assert in `dpp_listen_start` (event group deleted), a restart. `net_dpp_stop()` waits for it (≤ 3 s).
Check: the setup page test switches back within a second, 3 runs in a row.

**L166. Connected already? The router's channel is only the fallback: still scan.**
Why: skipping the scan (the station knew the channel) showed Easy Connect's QR code in 0.15 s instead of ~2.1 s, but
every attempt made straight from a connection then timed out (`ESP_ERR_DPP_AUTH_TIMEOUT`), while attempts after a
scan worked; the scan was restored (517742e). (This lesson said the opposite until October 4: a lesson must follow its
code.) The wait is hidden behind a placeholder code (L170).
Check: `Easy Connect: channel N (saved network, ...)` even when online; the router's channel only when the scan by name
finds nothing.

**L167. A test keeps its own log position; `log.mark()` is the harness's.**
Why: a new test used `ctx.log.mark()` (returns nothing) as its start; its first wait moved the shared position past
the line the second wait needed: a false failure.
Check: `at = len(ctx.log.lines())`, then `ctx.log.wait(..., start=at)`.

**L168. Screens whose display changes the device's state are snapshotted, not shown.**
Why: showing the Easy Connect page took the radio off the home network: the snapshot request over HTTPS timed out.
`forge.json` `screens_not_shown` lists them; the firmware prepares their texts off-display.

**L169. Text from outside may not fit the font, and may not fit the console either.**
Why: a phone shared a network named with emoji: boxes on the display (TinyTTF draws a box for a missing glyph and
never says so); and the harness crashed printing it on the Windows console (cp1252).
Check: `textfit()` (reads the TTF's cmap; `tests/host/test_textfit.c`); the harness writes UTF-8 with replacement.

**L170. Something that arrives a moment later gets a placeholder of the same shape.**
Why: the Easy Connect code appeared 0.15 s after its page settled; the empty spot, then the pop-in, looked janky to
the user. A faint, grey placeholder code of the same size and density now holds its place (and is in the drag's
picture), and the real one fades up over 300 ms. Make the placeholder harmless if scanned or read (plain text here,
not a broken link).

## Easy Connect with a phone on 5 GHz (October 4)

**L171. A debug setting that "does nothing" may have been renamed.**
Why: `CONFIG_WPA_DEBUG_PRINT=y` (weather_amoled's note) built without a single `wpa:` line on IDF 5.5: the option is
`CONFIG_ESP_WIFI_DEBUG_PRINT` now. Then the DPP lines were DEBUG level and needed `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`.
Check: after the build, grep the generated sdkconfig for the option you set; grep the log for the tag.

**L172. Before blaming a change, run the old firmware the same way.**
Why: Easy Connect failed 5 times on espforge; three changes were suspected in turn (the skipped channel scan, power
save, timing). The user said it worked in weather_amoled: flashed with the same logging, it failed identically (phone
on the router's 5 GHz band: request received on channel 1, answer acknowledged, no Auth Confirm). One A/B try would
have saved four. (The 5 GHz explanation drawn from it was wrong too: the phone's own log found the cause, L174.)
Check: same build options, same steps, both firmwares, one log each; compare step by step.

**L173. Easy Connect's answer time: enable fixed-point ECC.**
Why: the board must create a key pair before answering the phone's request; a phone on a 5 GHz network only visits
the 2.4 GHz channel briefly. `CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM` (off by default) cut the answer to 236 ms
(INFO logging) from ~900 ms (debug logging). It was not the fix (L174); it can only help. (October 5: 235 ms on
both bands; the crypto library's memory in internal RAM instead of PSRAM made no difference. Faster is not what a
phone on 5 GHz needed: L188.)
Check: `wpa: DPP: Authentication Request` -> `Sending authentication response` times (debug build).

**L174. Easy Connect: keep the radio on the channel; the setup AP holds it there.**
Why: ESP-IDF's enrollee stops listening (ROC cancelled) when the phone's request arrives, computes its answer, and sends
it with a short wait on the channel. A Pixel 8 Pro confirmed 7 ms after receiving the answer: "no-ACK" in the phone's
log, Auth Confirm timeout on the board, 7 failures out of 7 (espforge and weather_amoled alike). Whether a phone's
confirmation lands is timing, which is why ESP-IDF issues #12151 / #17672 report 3-10 retries and why weather_amoled
saw it "only work online" (associated, the radio stayed on the router's channel). With the setup AP up on the Easy
Connect channel (`dpp_hold_channel` in forge_net), the confirmation is ACKed: 5 out of 5 (online, offline, switching
networks both ways).
Check: the phone's log shows `DPP-TX-STATUS ... type=2 ... result=SUCCESS`; the board logs `Easy Connect: received`.
(The day after, the same phone on 5 GHz failed every time again, with every firmware, the Oct 4 one included: L188.
Holding the channel is still needed, but a phone on another band can fail for reasons the board can't see.)

**L175. When the other side decides, read the other side's log.**
Why: the board's log could only say "no confirmation came"; three theories (channel scan, power save, 5 GHz) cost
seven tries. Android's own Wi-Fi log over wireless debugging (`adb pair` / `adb connect`, Wi-Fi verbose logging on)
showed in one try that the board's answer was correct and the phone's confirmation was not acknowledged.
Check: docs/TESTING.md, "Easy Connect: the phone's side".

**L176. A deployment that reports success may not be served: check what users get.**
Why: espforge v0.1.0's Pages deployment succeeded, yet the site kept serving rc.6 for 30 minutes (devices on Stable
saw nothing). Re-running only the `pages` job failed on a duplicate artifact; a fresh run from `main` fixed it.
Check: after every release, `curl -sI <ota_site>channels.json` (`Last-Modified`, `stable`); recovery in
docs/RELEASING.md, "The site still serves the old files".

## Aligning with weather_amoled (October 4, evening)

**L177. A framework and the app it came from drift within hours.**
Why: espforge was extracted at 13:33; by 20:00 the Easy Connect fix existed only here while weather_amoled still
failed with the user's phone, and weather_amoled's fps-gap and quick-swipe fixes existed only there. Nothing linked a
fix in one to the other.
Check: both CLAUDE.md files list the twin files; a fix in one gets ported in the same session or a task for the other
(the plan: weather_amoled takes forge_core, forge_net and forge_ota as a git submodule).

**L178. A wrapping label must not have a fixed-position neighbour below it.**
Why: weather_amoled's alert title wrapped to two lines and ran into the line under it (v1.12.2). Its test's first run
then read `lv_obj_get_y()` = 80 right after `lv_obj_set_y(111)`: that is the last layout's position until the next one.
Check: lay out what follows from the label's real height; read `lv_obj_get_style_y()` or call
`lv_obj_update_layout()` first. A console command that lays a screen out with worst-case texts and reports the
geometry makes it testable (weather_amoled's `alert sample`).

**L179. A blank core dump partition and junk in it look the same to ESP-IDF.**
Why: `esp_core_dump_image_check()` returns `ESP_ERR_INVALID_SIZE` for a size word of 0xFFFFFFFF, which is both an
empty partition and what `esp_core_dump_image_erase()` leaves: diag (L158) erased the partition and warned at every
boot after the first erase.
Check: read the size word; erase only when it isn't 0xFFFFFFFF (diag.c). Junk test: TESTING.md.

**L180. A queue where "only the latest counts" must never drop a stop.**
Why: the setup radio task (L164) replaced any older request with a newer one; a stop queued by one exit could be
replaced, while main.c stopped Easy Connect from its own task at the same moment: two deinits racing. Only page
requests may be replaced; a stop that someone waits for goes through the same task and signals when done.
Check: `ui_wifi_setup_end()` returns after the radio task stopped Easy Connect.

**L181. After a move, forget the press you were tracking.**
Why: LVGL reads nothing while slide.c moves the pictures, so read_hook never saw the press end; a finger landing
during the release animation was taken for the old press and never became a drag: quick successive swipes were lost.
weather_amoled had the same bug and fixed it with a counter (v1.12.1).
Check: `touch_resync()` clears `track.down`; `swipe left right` (150 ms apart) makes two drags (navigation.quick_swipes).

**L182. A test that depends on the time of day fails in the evening.**
Why: weather_amoled's hourly-list test opened today's list; at 20:30 only a few hours were left and the whole list
scrolled 14 px, so every flick "barely moved" (4 runs, the code unchanged). The log line said `now at 14 of 0..14`.
Check: test on data whose size doesn't depend on the clock (tomorrow's list); read the log line before suspecting
the change under test.

**L183. A wait that accepts several lines can use up the line the next wait needs.**
Why: start_log waited for "console ready", "ota: Running" or the ready line; when "console ready" was lost (L154) the
first match was the ready line itself, the read position moved past it, and harness.py's wait for it failed 90 s
later with the line in the log (v0.1.1-align.1).
Check: a wait for a line that may already have been passed searches from a known start (`start=0`, or the test's
own position, L167); `test_harness.py` LogWaits.

**L184. A test that finds a firmware bug must not take the next tests down with it.**
Why: weather_amoled's `online_during_attempt` failed on v1.12.3 as it should (ESP-IDF refused the config, L155), but
left the board on the fake network; its cleanup then failed too, hid the real message, and the next test failed for
the leftover state. And a console command line is occasionally lost on the USB console with the board fine.
Check: clean up in a way that never replaces the test's own failure (weather_amoled `recovering()`, a restart as the
last resort); a lost read-only command is sent again once when `where` answers (board.py `READ_ONLY`). Prove a new
test against the old firmware: it must fail there for the reason it names.

## An app on the framework's components (weather_amoled v1.14.0, October 5)

**L185. A component's static buffers cost every app internal RAM.**
Why: weather_amoled moved onto forge_core / forge_net / forge_ota and its harness failed `internal_min_kb.reconnect`
(25 KB, limit 28): 2.2 KB more static internal RAM (`idf.py size-components`: the console's help buffer and command
table, the 12 service records, the update URL, answer buffers), all in plain `static` arrays. They now carry
`EXT_RAM_BSS_ATTR` (the app needs `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`); none is touched with the flash cache
off. The app ended 900 B below its own copies.
Check: `idf.py size` (DIRAM) before and after a component change; the harness's `internal_min_kb` floors. Buffers
used from an ISR or while the flash cache is off stay internal.

**L186. Test an unreleased component with its app through the manifest, and let the script clean up.**
Why: an app takes these components at a release tag (component manager). `-DEXTRA_COMPONENT_DIRS=<checkout>` was
documented to win over a managed component, but the managed copy was built (no "overrides" notice). `override_path:`
in the manifest is what the manager honours; weather_amoled's `tools/forge_local.py` swaps it in for one build and
puts the manifest back. Once a PowerShell pipeline (`| Select-Object -First 10`) killed the script mid-build: the
`finally` never ran and the manifest kept the local paths.
Check: the build log says "Using component placed at <checkout>"; the script restores a manifest left by a killed run
(its backup) before anything else; capture long builds to a file, not through a pipeline that stops early.

**L187. Rendering a band of rows alone misses what reaches into it from outside.**
Why: weather_amoled renders pictures and list scrolls a strip of rows at a time (`lv_obj_redraw` with a clip). LVGL
skips a label whose box misses the clip, but glyphs can reach past the box (Inuktitut's syllabics come from a
fallback font drawn 5/4 larger): after a Settings scroll in Inuktitut, the picture lacked one faint row of glyph tips
(`pictest` 1 row off; English and French were clean). Fix: clear and draw a margin of rows (8) around each strip,
as far as the buffer has room, and keep only the strip.
Check: a partial renderer's output against a full render, in every language and font the app uses, not only
English. espforge's slide.c renders whole screens (no partial strips): nothing to change there today.

## Easy Connect from a phone on 5 GHz, again (weather_amoled, October 5)

**L188. When both logs say "sent, never received", read the phone's kernel log; then design for the failure.**
Why: a Pixel 8 Pro connected on 5 GHz failed Easy Connect 9 times in a row ("Couldn't add device"); on 2.4 GHz it
worked at once. The same firmware had passed 3/3 with that phone on 5 GHz the day before. Five theories cost an
evening: a stage-2 regression (A/B with the old firmware failed the same way, L172), the band itself (the docs said
5 GHz had worked), a slower answer (235 ms on both days, measured with `CONFIG_ESP_WIFI_DEBUG_PRINT` alone: the
debug log level's hexdumps made it 835 ms), the crypto library's memory, and a lost answer (an ESP-IDF patch that
sent the answer again every 300 ms after its ACK changed nothing). The board's log said its answer was ACKed by
the phone; Android's log said no answer came. `adb bugreport` right after a failure settled it: the phone's
Broadcom driver logs `TX DPP_AUTH_REQ ... dwell time : 400 wait_afrx:1`, then `RX DPP_AUTH_RESP` 260 ms later
(inside the dwell), then `ACTION_FRAME_OFFCHAN_COMPLETE` at 414 ms, and never hands the answer to wpa_supplicant
(no `NL80211_CMD_FRAME`). The day before, it did. Same Android build, same access point, no restart in between,
and Wi-Fi off/on did not clear it: the cause is inside the phone's Wi-Fi driver or firmware.
Check: the kernel log (bugreport, "KERNEL LOG (dmesg)", lines tagged `[cfgp2p]`/`[dhd]`, stamped in UTC) and
Android's (`nl80211: ... NL80211_CMD_FRAME) received`, `RX frame ... sa=<board>`) side by side; a frame logged by
the driver but missing from wpa_supplicant is the phone's. Then make the failure easy to get out of: the setup page
says what to do when Easy Connect fails (swipe right and join the setup network, which works on any band).
Delete the bugreport after use: it holds the phone's personal data (and `adb bugreport` leaves a copy on the phone).
Also: a harness failure in the same session was the forecast service (HTTP 503), not the firmware: read the log.

## Rows rendered alone, and a decoder's bit depths (weather_amoled v1.14.2-rc.2, October 6)

**L189. Size a partial renderer's margin from the fonts, and re-render what a moved picture got wrong.**
Why: L187's 8-row margin was not the whole fix. (1) A list scroll moves the picture and renders only the rows coming
in; the rows just before them were drawn while the next label was still past the list's edge (LVGL clips children to
the list, and `lv_draw_label` returns unless the clip meets the label's own box), so its glyphs' tops never reached
them, and moving kept them missing: in Inuktitut a slow drag (1 px a frame) left 6-7 rows off every time, a quick
flick 1-2 rows one time in four (rows at the same two places in the list each time). (2) A margin row is right only
if every label reaching it was drawn: the outermost ones can lose a neighbour's top or descender, and writing them
into a whole picture erased what it had right. Fix: compute how far glyphs reach past a label's box from the TTFs
(TinyTTF's stb_truetype placement: rise = `ceil(yMax x fallback scale) + 1 - main ascent`; Noto's syllabics at 5/4:
9 rows at 28 px, 7 at 20; Montserrat alone 4 up, 1 down), draw `up + down` margin rows, take from the strip the new
rows plus `up` rows before and `down` after, and put the outermost margin rows back as they were. Lists hold
syllabics only in Inuktitut, so the reach is per language there: English scrolls got faster (render 3.7 -> 3.2 ms a
frame), Inuktitut's cost ~1.2 ms more.
Check: a slow drag (2.5 s) and a flick, each followed by a picture-vs-render test, in every language; an
intermittent one-row miss is worth making deterministic before fixing (here: slow, so every label enters a row at a
time). espforge's slide.c renders whole screens: nothing to change today (as L187).

**L190. A row decoder must take every bit depth its sources send.**
Why: `png_rows` took 8-bit samples only. OpenStreetMap saves tiles with few colours as 4-bit palettes (`4/5/6.png`,
ocean with an island, 321 bytes): weather_amoled's zoom-4 map of its first place had 8 of 9 tiles (a dark square),
was never cached (only complete maps are), and downloaded again at every boot and every return to that place,
on top of other downloads at internal RAM's low point. The log said so for days (`png: unsupported: depth 4`).
Fix: 1, 2 and 4-bit grey and palette, the depths PNG allows for them (scanline `ceil(w x depth / 8)` bytes, filters
bytewise with a 1-byte distance, samples from the high bits, grey scaled `x 255 / (2^depth - 1)`).
Check: grep the log for what a decoder refuses; test with the sources' real files (the host test decodes that tile
against a Python zlib reference), not only generated ones.

## An app with a varying list of pages (esp32-s3-rtcquebec v0.2.0-v0.3.0, October 7)

**L192. During `lv_screen_load_anim()` the active screen is still the old one.**
Why: LVGL 9.2 switches `lv_screen_active()` only when the load animation ends (200 ms here), and has no public getter
for the screen that is loading. esp32-s3-rtcquebec's map had a timer asking "is the map shown?"; it ran during the
fade-in, read the stop page, and took the map for closed as it opened.
Fix: note when the screen was opened (`lv_tick_get()`) and let such checks ignore the first 500 ms (or keep your own
"screen I asked for" variable).
Check: open the screen and read the log for anything that acts on "not shown" in the first frames.

**L193. `esp_http_client` sends at most 512 bytes of request line and headers by default.**
Why: the RTC's notices query is ~1.3 KB of query string; the request failed before reaching the server.
Fix: `.buffer_size_tx = 2048` (or what the longest URL needs) in `esp_http_client_config_t`.
Check: log the URL length next to the request; a request that fails without any HTTP status with a long URL is this.

**L194. A label sized to its content in `LV_LABEL_LONG_DOT` mode shows only "...".**
Why: LVGL 9 measures a `LV_SIZE_CONTENT` label with nothing to cut against, so the dots replace the text.
Fix: give the label a width **and a height**: the text's own from `lv_text_get_size()`, the width capped at the room
there is, the height one line. With a fixed width but a content height LVGL 9.2 wrapped the text and ran into the line
below ("Terminus Chute-Montmorency", MeteoBus v0.2.0-rc).
Check: a snapshot with the longest text in every language (French is longer).

**L195. Never pass a piece that holds `%` (an encoded query) as `snprintf`'s format.**
Why: `%5B` (an encoded `[`) and the like were read as conversions: the URL came out wrong, and an argument list that
doesn't match is undefined behaviour.
Fix: such pieces go in as `%s` arguments; the format is a literal.
Check: `-Wformat-nonliteral` flags a non-literal format; a host test of the URL builder with an encoded piece.

**L196. Emscripten's `localtime_r` and `strftime("%z")` ignore `TZ`.**
Why: they use the browser's zone and offset; the emulator's clock showed the visitor's time and `%z` wrote +00:00.
Fix: web/emu/forge/emu_time.c applies the firmware's TZ to `localtime_r`; for an offset, compute it from the date
(`mktime` of the local fields against the UTC time) rather than `%z`.
Check: run the emulator with the browser in another time zone (Playwright's `timezoneId`).

**L197. Code a test console command reaches runs on the console task's 4 KB stack.**
Why: esp32-s3-rtcquebec's `screen stop2` (v0.4.0-nav.1) ran the app's page refresh in the `testcon` task: a whole
departures board (`dep_entry_t`) and a 500-byte buffer on its stack, twice down the call chain: "A stack overflow in
task testcon", a restart. The same refresh from the LVGL task (a bigger stack) had always been fine.
Fix: large locals in code that a `screens_def_t` show / prepare reaches are `static` (one caller at a time: the
display lock is held), or the work goes to the LVGL task.
Check: the harness's screens and navigation suites drive every screen through the console; `where` and the panic line
name the task.

**L198. A tool that flashes or reads a build must follow the build that was staged, not forge.json's.**
Why: `stage.py --build build/forge` (a build against an unreleased espforge, `tools/forge_local.py`) staged the right
parts, then the flash helper checked them against `build/v55` and refused them ("changed since it was staged"), and
`devloop.py flash` without `--no-stage` staged `build/v55` again: the board ran the previous firmware until the log's
`ota: Running` line said so (L2). The harness then looked for the ELF in `build/v55` and couldn't decode a panic.
Fix: `stage/manifest.json` records the build folder; the helpers (`flash_helper.ps1` / `.py`) and the harness's
`elf_path()` read it (espforge v0.5.0). Flash another folder with `stage.py --build <dir>` then
`devloop.py flash N --no-stage`, or `harness.py --flash <dir>`.
Check: `tools/harness/test_flash_helper.py` `test_another_build_folder_staged_is_flashed`.

**L199. The Claude desktop app's shell redirects AppData\Local: a component manager fetch fails.**
Why: Claude Code in the desktop app runs inside its MSIX package, which redirects `AppData\Local` to
`Packages\Claude_...\LocalCache\Local` for its child processes, not for all of them alike: ESP-IDF's component manager
put its git cache in one view and git ran in the other ("'git init --bare' failed ... unable to get current working
directory"), on the first build that fetched a new espforge tag (esp32-s3-rtcquebec v0.4.0, 2026-10-09). L146 is the
same trap with the Microsoft Store Python.
Fix: `$env:IDF_COMPONENT_CACHE_PATH="$HOME\.espressif\component_cache"` (any folder outside AppData) before `idf.py`.
Check: a build after bumping the espforge tags, from the desktop app's shell.

**L200. A page whose frames take over ~100 ms loses drags.**
Why: LVGL reads the touch between frames; esp32-s3-rtcquebec's map page (a 466 px image under two 700-point, 6 px
`lv_line`s) took 170-200 ms a frame, redrawn for every tile that arrived. slide.c saw a swipe's first point late (x 130
of a 388 -> 78 swipe) or not at all: no drag, and LVGL fired a long press (Settings opened), since it fires one for a
press held 400 ms however far it moved.
Fix: draw what doesn't change into one picture (a canvas: the map and the path, composed once per change), don't
redraw a busy page while a finger is down, and accept a long press only from a finger that stayed within ~24 px
(forge_lvgl's `slide_held_still()` since v0.6.0).
Check: the `fps` console command on that page (`render_avg_ms`), and a harness swipe on it right after it opens.

## Alignment with MeteoBus v0.2.1-v0.3.1 and its ESP32-P4 port (espforge v0.6.0, October 30-31)

**L201. Time each statistic: on a board fed by a per-frame interrupt, nothing periodic may hold interrupts off past the
frame gap.**
Why: ESP-IDF's MIPI-DSI DPI driver restarts the frame DMA from an interrupt after every frame (~0.9 ms between frames on
the ESP32-P4). diag's per-task lines (`uxTaskGetSystemState()`: every task's stack measured in a critical section) took
6.1 ms with 25 tasks, and `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` (a walk of the whole heap) 2.3 ms with
32 MB: a full-screen white flash every few minutes, matched to the diag period in the log (MeteoBus's P4 port).
Fix: `CONFIG_FORGE_DIAG_TASK_STATS` and `CONFIG_FORGE_DIAG_PSRAM_LARGEST` (forge_core), off on such a board; the free
size instead of the largest block wherever it will do (slide.c's room check reads the free size first). `diag: stats
cost` at start times both on the board at hand (an S3 with 8 MB: 1.5 and 0.8 ms).
Check: match each flash's time to the log, then time the suspects (`esp_timer_get_time()` around each).

**L202. A console command that reads `esp_clk_cpu_freq()` always sees the maximum: it runs.**
Why: with dynamic frequency scaling a running task holds ESP-IDF's own "rtos" lock, so the clock is 240 MHz whenever
anything measures it. "CPU at 80 MHz while off" failed in MeteoBus's harness because the command that asked ran at 240.
Fix: check the lock that lets the CPU idle slower (the board's `power` reports `screen_lock`), or profile the time spent
in each mode (`CONFIG_PM_PROFILING`, `esp_pm_dump_locks`), never the frequency at an instant.
Check: harness `presence.dim_off_wake` checks `screen_lock=0` with the screen off, `1` awake.

**L203. Measure the split before guessing, again.**
Why: three plausible speed-ups of MeteoBus's zoom measured nothing (a column table in internal RAM and an unrolled loop:
fill 185 vs 188 ms; the other core's TLS and tile decoding held back: 54 fps both ways; 64-bit divisions out of the
per-frame tables: none), while a cycle counter around the overlay blend found 40 % of the fill at once (42-43 -> 54-58
fps). And a harness check can be wrong in the test, not the firmware (L202).
Fix: a cycle-counter split in the loop's log line before any change; change one thing; measure again.
Check: the log line carries the split; a change that doesn't move its numbers is reverted.

**L204. Slow requests: compare the board with the PC on the same URLs, and try `WIFI_PS_NONE`.**
Why: ESP-IDF's default station power save (`WIFI_PS_MIN_MODEM`) makes each reply wait for the router's next beacon,
~100 ms a request. MeteoBus's radar (28 small requests on one kept-alive connection) took 4.9-6.6 s, 3.1-3.7 s with power
save off; the PC took ~90 ms a request. The server and the parsing were blamed first.
Fix: `net_low_latency(true)` while a screen that downloads much is open (forge_net), `false` when it closes.
Check: time the same requests from the PC (`curl -w`) and from the board (`svc: probe ... N ms`); the gap is the
beacon interval.

**L205. A flaky UI test may be a real race: hold the reply back to make it deterministic.**
Why: MeteoBus's settings page removed the wrong stop when a tap landed between a save the display accepted and the page's
redraw (its rows acted on the list they were drawn from). It showed as a flaky Playwright test on the Mac runner, and
reproduced every time once the display's answer was held back 1.5 s (`page.route` + `route.fetch`, a delay, then
`route.fulfill`).
Fix: disable the rows during a save and show the saved list at once; in the test, hold the reply back.
Check: the Playwright case with the delayed reply (MeteoBus `stops.spec.js`; esp32-s3-rtcquebec's PR #11 has both).

**L206. Identify a new board from its product page or a photo before porting.**
Why: MeteoBus's P4 port started from a guessed evaluation board and the V1 panel's init table; the real board (Guition
JC1060P470C, a V2 panel) showed a white streaked band until it was identified. Hours.
Fix: the product page, a photo of the board's markings, then the vendor's BSP for exactly that revision.
Check: the first picture on the panel is the vendor's test pattern, not the app.

**L207. Take pins from a working setup for the same board and an I2C scan, not its schematic.**
Why: the JC1060P470C's schematic showed the amplifier on GPIO 20 and an ES7210; the board has the amplifier on GPIO 11,
no ES7210, and its one ES8311 does both ways (an ESPHome setup for the same board had the pins right).
Fix: an I2C scan at boot during a port; pins from a setup proven on that very board.
Check: every address the scan finds has a driver, and no driver waits for an address the scan didn't find.

**L208. "Unreachable" over the network: count packets on the board first.**
Why: the P4's settings page was unreachable twice for two different reasons: the C6 co-processor's esp-hosted "network
split" kept port 443 for itself, then the router isolated its Wi-Fi clients. Guessing at the firmware cost a day; lwIP's
counters showed at once whether anything arrived.
Fix: a `netstat` console command (lwIP stats, `CONFIG_LWIP_STATS`) before touching the server code;
`CONFIG_FORGE_HTTPS_PORT` for a co-processor that keeps 443.
Check: the counters move when the PC sends; if not, the problem is below the firmware.

**L209. LVGL sends DRAW_POST only when the band meets the object itself, not its ext draw size.**
Why: a badge drawn past its object's edge in a DRAW_POST handler was cut at the band that no longer met the object
(7 rows missing, found by `pictest`; a `shot` of the frame looked right because the panel kept the old rows).
Fix: `LV_OBJ_FLAG_OVERFLOW_VISIBLE` on the object (with its ext draw size), or draw the badge as a child object.
Check: `pictest` (the picture against a fresh rendering), not a screenshot of the panel.

**L210. esp_hosted's SDIO pools are allocated before the scheduler and can starve the idle task.**
Why: the P4's esp_hosted (SDIO to the ESP32-C6) allocates its queues (12 + 12) before FreeRTOS runs; with large pools
the idle task had no RAM to start: a boot loop with no useful line.
Fix: smaller queues in esp_hosted's Kconfig; read the first `heap` line after boot on a new co-processor set-up.
Check: `diag: boot` shows a normal internal free size on the first boot.

**L211. On an ESP32-P4, move frames with the 2D-DMA.**
Why: a 1024x600 RGB565 frame copied by the CPU took 26-28 ms, by the PPA scaler 26.5 ms, by the 2D-DMA
(`esp_async_fbcpy`) ~8 ms; and the DSI's DMA needs the bus first (`axi_icm_ll_set_dw_gdma_qos_arbiter_prio`) or it
underruns.
Fix: `esp_async_fbcpy` for frame moves on the P4, the panel's DMA given bus priority; MeteoBus's `blitbench` console
command measures the three.
Check: `blitbench` before choosing a path on a new chip.

**L212. One codec, one device.**
Why: esp_codec_dev's `es8311_open()` rewrites the chip's set-up: a second device for the speaker, opened at each chime
while the first one read the microphone, stopped the microphone (the P4 board's single ES8311).
Fix: one `ESP_CODEC_DEV_TYPE_IN_OUT` device kept open, created under a lock (the presence and sound tasks start
together); the app writes its sound to it and switches the amplifier itself (forge_audio's `FORGE_AUDIO_ES8311_BOTH`).
Check: the microphone's level line (`presence: level`) keeps moving across a chime.

**L213. Two watchers of one touch: the slow one must still get its say.**
Why: the touch that wakes a dark screen must do nothing else. The press filter (in the LVGL task) decides that from
the presence state; but the presence task also polls the touch controller every 100 ms, and while the panel sleeps
LVGL reads touch only every 100 ms too (v0.6.0-rc.2). About one wake in ten, the poll won: the state was ACTIVE when
the filter looked, the press was not swallowed, and a long press opened the setup page (harness `dim_off_wake`,
rc.6: "no log line presence: touch on a dark screen").
Fix: the state machine remembers a wake made by the poll from OFF for `PRESENCE_DARK_TOUCH_S` (0.6 s);
`presence_sm_take_dark_touch()` answers the filter from the state or that grace, and clears it.
Check: `tests/host/test_presence.c` (the grace, taken once, expired); on the board `presence.dim_off_wake` ten times.
