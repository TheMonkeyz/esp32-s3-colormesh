# Testing on the device

How a change is built, flashed, observed and proved before it is called done. Every change is verified on the board
(log, snapshot, or both). The Windows PC drives the board on its USB serial port; the board is on the home network.
`<ip>` below is the display's address (`net: Connected, IP …` or `web: Settings page: https://<ip>/` in the log).
What the tools read from the firmware is specified in [PROTOCOL.md](PROTOCOL.md); project values come from
`forge.json`.

A project adds its own section at the end ("Project checks") for screens, suites and recipes of its own.

## 1. Build

Test build on the PC, in its own folder so the repo's `sdkconfig` is never touched (PowerShell):

```powershell
Set-Content version.txt "v0.2.0-graph.3" -NoNewline -Encoding ascii   # label; git-ignored
(Get-Item CMakeLists.txt).LastWriteTime = Get-Date                     # version.txt is read at configure time
. C:\Espressif\esp-idf\export.ps1
idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build
```

- **Label test builds above the current release** (L9). Delete `version.txt` when done.
- **To test a release candidate over Wi-Fi** the board must run something *below* it: flash an older test build first
  (with a new unique name).
- After changing `sdkconfig.defaults`, delete `build\v55\sdkconfig` and run `idf.py … reconfigure` (L12).
- Keep the PC and CI on the same ESP-IDF (`forge.json` `idf`, L13).
- **Debug build** (memory checks, test builds only): `sdkconfig.debug` adds light heap poisoning, a watchpoint at the
  end of each task's stack and GCC's stack protector:
  `idf.py -B build\debug -D SDKCONFIG=build\debug\sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug" build`.
  Slower and hungrier, so its performance numbers don't count; run it after changes to memory handling and read the
  log for `CORRUPT HEAP`, `Stack canary`, `Stack protection fault` and panics.

## 2. Flash and log (devloop)

The flash helper (`tools/devloop/`) runs on the PC, owns the COM port, and talks through files in `.devloop/`
(PROTOCOL.md §1). Claude Code starts it in the background, windowless, so nothing opens on the user's screen:
`powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1` with `run_in_background` (its output arrives in
that task; stop the task to stop it). `tools\devloop\start_flash_helper.bat` runs the same helper in a window, for
a person who wants to watch (Q / Esc stop a log there). Restart it after editing `flash_helper.ps1`; `monitor.ps1`
is reloaded on every run. On macOS / Linux the same files are served by
`python tools/devloop/flash_helper.py`, from an ESP-IDF shell (docs/MACOS.md). A restart request goes through the
test console's `reboot`, so its log is whole from `ESP-ROM:` on (L191); after a flash the first ~2.5 s are still
lost (L154).

```bash
python tools/devloop/stage.py                 # copy the parts in build/v55/flasher_args.json to .devloop/stage/
                                              # under unique names, print md5 of source and copy
python tools/devloop/devloop.py flash 120     # flash the staged parts, then log 120 s
python tools/devloop/devloop.py reboot 300    # restart without flashing (test console, else esptool), log 300 s
python tools/devloop/devloop.py send "screen" # a test-console line (echoed in the log as "> screen")
python tools/devloop/devloop.py stop          # end the log window now
python tools/devloop/devloop.py status        # flash.status + flash.done
python tools/devloop/devloop.py wait-done     # block until the request is finished
```

1. Stage, and **refuse to flash unless the md5s match** (L1).
2. `flash.status` goes `flashing` → `logging` → `idle` (or `flash_failed`). Ask the user to touch the board *during*
   `logging`, and say what to do.
3. Tail `.devloop/serial_live.txt` while it logs; stop early once the lines you need are in (L4).
4. `flash.done`: `exit`, timings, `errors` (`E (` lines), `warnings`, `resets`, `stopped_early`. Then grep
   `.devloop/serial_log.txt` (written when the window ends).

Read before testing: `ota: Running vX from ota_N, channel …` in the fresh log (L2).

Normal noise: `mbedtls_ssl_handshake returned -0x7780` (a phone rejecting the self-signed certificate),
`read error :-0x0050` (peer closed), `failed to load RF calibration data` (first boot after a flash).

## 3. Test console

USB Serial/JTAG, one command per line, answers are `test:` log lines (PROTOCOL.md §2): `ping`, `help`, `heap`,
`where` (no lock: answers when the display hangs), `memspeed`, `reboot`, `key`, `wifi status|offline|online|
offline-boot|offline-boot-short`, `portal windows-quiet`, `fps [reset]`, `tap X Y`, `press X Y [ms]`,
`swipe left|right|up|down`, `drag X1 Y1 X2 Y2 [ms]`, `screen [<name>]`, `profile`. Components and the app add their
own with `testcon_register`.

- Simulated touches enter at the touch controller read, so wake-up, long-press and gestures run the real code (L105).
  A tap lasts 120 ms (L30).
- Heavy commands start their own task; the console task's stack is small and internal (L28).
- Settings from the PC (for a test; put the user's values back after):
  `curl -sk -X POST -H 'Content-Type: application/json' -H "X-Key: $FORGE_KEY" -d '{"lang":"fr"}' https://<ip>/api/settings`.
  The key: `devloop.py send key`, then `test: key …` in the log (the tools cache it in `.devloop/key`; L7).
  Non-ASCII from Git Bash: put the JSON in a file, `--data-binary @file` (L123).

## 4. Snapshots (`tools/snapshot.py`)

`GET /api/snapshot?screen=<name>` renders a screen **off-display** and streams a 24-bit BMP; the tool saves a PNG with
the area outside a round panel tinted red, so anything the circle cuts off stands out.

```bash
python tools/snapshot.py hello                  # -> snapshot_hello.png (address: .devloop/ip)
python tools/snapshot.py system out.png --ip <ip>
python tools/snapshot.py setup --key <key>      # else $FORGE_KEY (forge.json key_env), else .devloop/key
```

- Screens: the names in `forge.json` `screens` (registered by the app with the screen registry), plus `current`.
  Projects may register pseudo-screens (a list scrolled by one page, an overlay) for text-fit checks.
- Check every screen a change touches, **in every language** (French is longer and wraps; L142).
- A snapshot shows state, not interaction: for gestures, scrolling and animations ask the user to act during a log
  window. It can't show touch-target problems either (L49).
- Cost on weather_amoled: ~434 KB of PSRAM while rendering, ~650 KB sent, 2-3 s.
- Without HTTPS to the board (the cloud's shell, a board behind a co-processor): the console's `shot <screen> [DIV]`
  sends the screen over USB (every DIV-th pixel, default 2), and `python tools/devloop/shot.py .devloop/serial_log.txt`
  makes PNGs from the log.

## 5. Harness (`tools/harness/`): everything, without a person

One command tests the board end to end. Needs the flash helper running and firmware with the test console.

```bash
python tools/harness/harness.py                                  # all suites on the firmware on the board
python tools/harness/harness.py --flash                          # stage the build in forge.json build_dir, flash, test
python tools/harness/harness.py --flash --quick                  # iterating: no idle minute, setup network, Playwright
python tools/harness/harness.py --flash build/debug              # another build folder
python tools/harness/harness.py boot web                         # some suites (or --suite boot,web)
python tools/harness/harness.py wifi_setup --phone               # + Easy Connect with a real phone (asks the user)
python tools/harness/harness.py --expect v0.2.0-rc.1             # fail unless the board runs this version
python tools/harness/harness.py --ota v0.2.0-rc.1                # install a published release with the updater, test it
python tools/harness/harness.py perf --update-baseline           # propose new reference numbers
```

| Flag | Meaning |
|---|---|
| `--flash [BUILD_DIR]` | Stage and flash a build folder first (default `forge.json` `build_dir`; every part, md5 checked). |
| `--suite <name>` (or positional names) | Run only these suites. |
| `--expect vX` | Fail unless the board runs vX. Always use it when testing a release. |
| `--ota vX` | Ask the device to check its channel every minute until it offers vX (CI + Pages take ~5 min after a tag; `--ota-wait` minutes, default 20), install with the device's own updater, wait for `ota: Running vX` and `marked valid`, then run the suites with `--expect vX`. An rc needs the device on the Beta channel; the harness doesn't change it. |
| `--update-baseline` | Write `tools/harness/baseline.proposed.json` (see below). |
| `--phone` | Include steps only a person can do; the harness prints `>>> ASK THE USER` (relay it at once). |
| `--ip`, `--minutes` | Override the IP from the log; log window to request (default 40 min). |

Suites. Generic ones live in `tools/harness/core_suites.py` and test the framework; each project adds its own in
`tools/harness/app_suites.py`.

| Suite | What it proves |
|---|---|
| `boot` | the start-up: no `E (` line, `ota: Running` / `App version` / the console agree, boot stage times; a crash kept from before is noted |
| `console` | `help` lists the commands; an unknown command is refused; `wifi lowlat on\|off` switches Wi-Fi power save (`ps=`, `lowlat=`); `shot` sends a screen over USB that decodes to a picture |
| `memory` | internal RAM and PSRAM free and low points, failed allocations, against `baseline.json` |
| `screens` | each screen in `forge.json` `screens` snapshotted (size, not blank); `screens_not_shown` are prepared off-display |
| `web` | the Playwright suite (`tools/webtest`), `GET /api/info`, the page whole; 403 for a POST over plain HTTP, 302 to the device itself, 401 without or with a wrong key, 415 for non-JSON, 421 for another Host |
| `update` | an update check ends in "up to date" or "available" (network errors tolerated) |
| `idle_stable` | 60 s idle: internal RAM doesn't drop |
| `wifi_runtime` | network lost while running: retries go on; setup pauses them; back online |
| `wifi_setup` | the PC joins the setup network like a phone (DNS answers every name, the captive-portal redirect, the page loads, `/api/info` without the home network's name or address), Easy Connect on its channel with the setup network held there; `--phone` for a real scan |
| `ota` | (on request) the installed release is confirmed and nothing was rolled back |
| `navigation`, `perf` | the starter app's: swipes between pages (quick ones too; `pages()` lists them, read from the device in an app whose page count varies), long-press to Settings, a moving press is no long press (the message screen), setup pages, fps and the drag's first frame against the baseline, the Settings list scrolled as pictures |
| *app suites* | whatever `app_suites.py` registers |

Reports: `tools/harness/reports/<date>/report.md` (git-ignored) with `results.json`, screenshots and the log of each
failed test. Exit code 0 = all passed, no regression. Read the "Testing vX" line first: it says which version and
bootloader flash mode it tested.

How the gate works (L21-L35):

- **Baseline** (`baseline.json`): per metric `min`/`max`, `ref` (value when set), `note` (why that limit). A metric
  whose suite ran but wasn't measured is **MISSING**, a measured one without a limit is **NEW**; both fail like a
  **REGRESSION**. A test that skips a measurement on purpose calls `ctx.skip(pattern, reason)`.
- **Updating the baseline:** run the suites on a known-good build with `--update-baseline`; it writes
  `baseline.proposed.json` (this run's numbers as `ref`, limits and notes kept, proposed limits marked for new
  metrics). Diff it against `baseline.json`, adjust limits (leave margin: a floor equal to the worst case seen is no
  floor, L26), copy it over, and say why in the commit.
- **Log cursors:** the harness marks the log at the start of each test and fails a test on `rst:0x` after it
  (decoding the backtrace with `addr2line` against the ELF in `build_dir`: wrong for any other build). Tests keep
  their own positions (`at = len(ctx.log.lines())`).
- **No answer or `display busy`** → the harness sends `where` and puts the breadcrumbs in the failure.
- **After an update** the harness waits for `marked valid` before any restart (L23).
- **Outside outages** (a cloud API down) are waited out and noted, not failed (L34).
- The harness's own logic has unit tests: `python tools/harness/test_harness.py`. With the flash helper's
  (`test_flash_helper.py`: loopback port, fake board; they need pyserial, so run them from an ESP-IDF shell) and the
  PC-side tools' (`test_forge_local.py`: `tools/forge_local.py` overrides a sample manifest for a fake idf.py run and
  puts it back, `tools/fetch_forge.py` reads the pinned tag, both say "nothing to do" in espforge itself;
  `test_setup_github.py`: `tools/setup_github.py`'s comparison of two repositories' GitHub settings on saved `gh api`
  answers, and its `--check` writing nothing): `python -m unittest discover -s tools/harness -p "test_*.py"`. CI runs
  them all (`host-tests`).

Pitfalls (Windows): Microsoft Store Python hides `%LOCALAPPDATA%` from child processes, so Playwright's browsers go in
`tools/webtest/.browsers` (L146); `netsh` must answer in English for the PC-as-phone tests (L147); before joining the
setup network the harness sends `portal windows-quiet` (L117).

## 6. Host unit tests (`tests/host/`)

Framework and app C files built with gcc against small shims of the ESP-IDF headers (`tests/host/shim/`) and a scripted
HTTP client (`fake.c`: each request gets one reply, an HTTP status, a transport error, or "no client"; or a reply
function per request, from its URL, with a body that may hold NULs, and counts of clients made and cleaned up:
`test_map.c` serves map tiles that way; `osm_tile.h` is a real OSM tile for such tests), with
AddressSanitizer and UBSan. forge_lvgl's `pager.c` builds against a fake of the LVGL calls it makes
(`tests/host/lvfake/lvgl.h`, implemented in `test_pager.c`: objects, flags, a scroll range that skips hidden
children); a new LVGL call in pager.c needs a line there. forge_net's download-slot bookkeeping (`dl_gate.c`) has
`test_dlgate.c`; slide.c's slot choice (`slide_slots.c`) has `test_slide_slots.c`; forge_map's path overlay is in
`test_map.c` and its flash cache (`map_cache.c` over a RAM-backed I/O table) in `test_map_cache.c`.

```bash
wsl make -C tests/host      # IDF_PATH defaults to /mnt/c/Espressif/esp-idf (for cJSON)
```

- A fix that can be reproduced off the board gets a case here; check that the case fails on the old code
  (`git show HEAD:path/x.c`) before calling it a test (L57).
- When the firmware starts using a new ESP-IDF symbol in a file compiled here, extend the shim, and run the tests
  before tagging (L15).
- CI runs them on every push; a release needs them to pass.

## 7. Settings page tests (`tools/webtest/`)

Playwright tests of `main/web/index.html` on a phone-sized Chromium against a mock device, no board needed:

```bash
cd tools/webtest
npm install                       # first time
npx playwright install chromium   # first time
npm test
```

- `mock-server.js` serves the real page and answers `/api/*` like the firmware (state in memory; `POST /__reset`,
  `GET /__state`). Keep it in step with the API (PROTOCOL.md §4) and **give it every state the page can be in** (L24).
- Any page error or console error fails a test. Every test saves a screenshot in `tools/webtest/shots/`: look at them
  before flashing a page change.
- Don't name a fixture option `offline` (L152).
- **4 workers, one mock display each** (`fixtures.js` starts `mock-server.js` on port 8100 + the worker's index, at
  127.0.0.1): ~15 s for the suite instead of ~45 with one shared mock. `WEBTEST_WORKERS=1 npm test` to debug one at a
  time. A test may rely only on its own worker's mock (`POST /__reset` before each test). The harness starts the suite
  in the background when a run begins (it needs no board) and collects it at `web.settings_page_tests`.

## 7b. The display in the browser (`web/emu/`)

The app's screens and settings page compiled to WebAssembly ([web/emu/README.md](../web/emu/README.md)): a quick look
at a screen change, and the site's *Try it in your browser*. It runs the same `main/` code with the hardware replaced,
so it shows layout, texts, swipes and the settings page's round trips, not timing, memory or radio behaviour: it never
replaces the board.

```bash
wsl bash -lc 'source ~/emsdk/emsdk_env.sh && make -C /mnt/c/Users/<you>/ESPDEV/<app>/web/emu -j8'
```

Then `node web/emu/forge/smoke.js` (Playwright from `tools/webtest`): in headless Chromium, with real mouse events,
the screen draws, a drag moves between the pages, a press and hold changes the screen, the settings page beside it
answers (its second language: the screen follows; `/api/presence`: screen dimming's real code runs), and nothing logs
an error; screenshots in `web/emu/build/smoke/`. CI runs it too, on a pull request's own commit (job `emulator`,
docs/RELEASING.md). To try it by hand, serve `web/emu/build` (`.claude/launch.json` "emulator", port 8767) and open
it in the built-in browser; a hidden browser pane doesn't paint the canvas (`requestAnimationFrame` pauses). After a
change to `main/` that adds a file or a framework function, the emulator may stop linking: CI's `emulator` job says
so on the pull request (it builds the pull request's own commit); on `main`, the site then goes out without *Try it*.

## 8. Diagnostics

`python tools/devloop/devloop.py reboot 300`, wait for `idle`, then `python tools/diag_summary.py
.devloop/serial_log.txt`: memory, render timing, per-task CPU and stack high-water marks from the `diag:` lines. Keep
this project's reference numbers in `docs/DIAGNOSTICS.md` (template in `docs/templates/`) and compare after changes
that touch memory or rendering.

## 9. Profiling LVGL rendering

A local experiment, never committed in a build:

1. `python tools/harness/lvgl_profile_patch.py apply`: tags each draw task type (`t_fill`, `t_label`, `t_image`…) in
   `managed_components` (not in git; `revert` undoes it).
2. In `build\v55\sdkconfig` only: `CONFIG_LV_USE_PROFILER=y`, `CONFIG_LV_USE_PROFILER_BUILTIN=y`,
   `CONFIG_LV_PROFILER_INCLUDE="src/misc/lv_profiler_builtin.h"`. Build and flash.
3. `python tools/devloop/devloop.py send profile`: one render-only frame of each screen, between
   `PROFILE-BEGIN <screen>` and `PROFILE-END <screen>`, then `test: ok profile done`.
4. `python tools/harness/profile.py .devloop/serial_live.txt`: per screen, each marker's count, inclusive and self
   time.
5. Revert the patch and the sdkconfig lines (the profiler slows rendering), rebuild.

On weather_amoled it showed no single hot spot (L87): the time was spread over the 15 bands and the glyphs, which is
why moves became pictures sent straight to the panel.

## 10. Throwaway builds for fake inputs

Some paths can't be reached on demand: a network that disappears at boot, a weather alert, lightning, a rare server
answer. Make a throwaway build that fakes the input, label it (`v0.2.0-alerttest.0`), test, then `git checkout` the
changed files and flash a clean build. **Never commit it.** Fake inputs at the data layer, in the data's own units
(L55); prefer a test-console switch in the real firmware when the path matters enough (`wifi offline-boot`).

Examples from weather_amoled: fake orange alerts appended after a real fetch at 30 s and 60 s of uptime (checks "beep
once per alert" and quiet hours); `net_begin()` with a bogus SSID, the real one restored with `esp_wifi_set_config()`
after N seconds (the offline setup path, before `wifi offline-boot` existed); fake lightning marks at fixed lat/lon.

## Project checks

Each project lists here its screens, pseudo-screens, app suites and recipes (alert sounds, presence, sensors).

## Easy Connect: the phone's side

The board's log says what the board saw; only the phone's log says why the phone stopped. Android (11+), no cable:

1. On the phone: Developer options (tap Build number 7 times) → **Wireless debugging** on → **Pair device with pairing
   code**. Note the IP:port and the code; also turn on **Enable Wi-Fi verbose logging**.
2. On the PC (Android SDK Platform-Tools, unzipped anywhere; on this PC `C:\Users\lmathieu\ESPDEV\platform-tools`): `adb pair <ip>:<pairing port> <code>` (use the IP, not
   the mDNS name; a failed try uses the code up: ask for a new one), then `adb mdns services` for the connect port and
   `adb connect <ip>:<port>`.
3. `adb logcat -c`, then `adb logcat -v time > phone_dpp.txt` in the background; the user scans; then
   `grep -E "DPP|DppManager" phone_dpp.txt`.
4. Read: `DPP-TX ... type=0` (the phone's request), `DPP: Authentication Response from ...` (the board's answer
   arrived), `type=2 ... result=no-ACK` (its confirmation not received: the board's radio was elsewhere),
   `DPP-CONF-SENT` (the network was sent). The link drops when the phone changes network; the phone keeps its log.
5. When the board says its answer was ACKed and Android never saw it (no `RX frame ... sa=<board>`): `adb bugreport
   br.zip` within a minute of the failure (the kernel's ring buffer covers ~10 minutes), then read "KERNEL LOG
   (dmesg)" in the main .txt: the Wi-Fi driver's `TX DPP_AUTH_REQ` / `RX DPP_AUTH_RESP` lines (UTC times) show
   whether the phone's driver received the answer (L188). The bugreport holds the phone's personal data: keep it out
   of every repository, delete it after use, and ask the owner before taking one.

Together with the board's side (`CONFIG_ESP_WIFI_DEBUG_PRINT=y` in the test build's sdkconfig only, LESSONS L112) this
located the Easy Connect failure in one try (L174, L175).
