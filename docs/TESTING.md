# Testing on the device

How a change is built, flashed, observed and proved before it is called done. Every change is verified on a board
(log, `shot` snapshot, or both). The Windows PC drives the boards on their USB serial ports; there is no network and
no address: everything goes through the USB console. What the tools read from the firmware is specified in
[PROTOCOL.md](PROTOCOL.md); project values come from `forge.json`.

## 1. Build

Test build on the PC, in its own folder so the repo's `sdkconfig` is never touched (PowerShell):

```powershell
Set-Content version.txt "v0.1.0-rc.0" -NoNewline -Encoding ascii          # label; git-ignored
(Get-Item CMakeLists.txt).LastWriteTime = Get-Date                     # version.txt is read at configure time
. C:\Espressif\esp-idf\export.ps1
idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build
```

- **Label test builds above the current release** (L9). Delete `version.txt` when done. The label matters here more
  than elsewhere: a board in dev mode pulls any strictly newer version it hears (the version-kind byte orders
  `-fw.2` below `-rc.1` below the release), so a test build on board A is pulled by B and C in dev mode.
- **To test a release candidate** flash it from the Pages site (Beta) on one board, or `harness.py --flash` with
  the release's parts, and let the others pull it in dev mode.
- After changing `sdkconfig.defaults`, delete `build\v55\sdkconfig` and run `idf.py … reconfigure` (L12).
- Keep the PC and CI on the same ESP-IDF (`forge.json` `idf`, L13).
- **Debug build** (memory checks, test builds only): `sdkconfig.debug` adds light heap poisoning, a watchpoint at the
  end of each task's stack and GCC's stack protector:
  `idf.py -B build\debug -D SDKCONFIG=build\debug\sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug" build`.
  Slower and hungrier, so its performance numbers don't count; run it after changes to memory handling and read the
  log for `CORRUPT HEAP`, `Stack canary`, `Stack protection fault` and panics.

## 2. Flash and log (devloop)

The flash helper (`tools/devloop/`) runs on the PC, owns the COM port of the main board (A, COM5), and talks through
files in `.devloop/` (PROTOCOL.md §1). Claude Code starts it in the background, windowless:
`powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1` with `run_in_background`.
`tools\devloop\start_flash_helper.bat` runs the same helper in a window, for a person who wants to watch. Restart
it after editing `flash_helper.ps1`; `monitor.ps1` is reloaded on every run. The Python `flash_helper.py` serves
the same files on any OS. A restart request goes through the test console's `reboot`, so its log is whole from
`ESP-ROM:` on (L191); after a flash the first ~2.5 s are still lost (L154). Several agents share board A: take
`.devloop\board.lock` before a flash (CLAUDE.md).

```bash
python tools/devloop/devloop.py flash 90 --build build/v55   # stage the parts (unique names, md5), flash, log 90 s
python tools/devloop/stage.py                 # (stage alone: copy the parts in build/v55/flasher_args.json to .devloop/stage/)
python tools/devloop/devloop.py reboot 300    # restart without flashing (test console, else esptool), log 300 s
python tools/devloop/devloop.py send "cfg"    # a test-console line (echoed in the log as "> cfg")
python tools/devloop/devloop.py stop          # end the log window now
python tools/devloop/devloop.py status        # flash.status + flash.done
python tools/devloop/devloop.py wait-done     # block until the request is finished
```

**The other boards** (B, C): while the helper is stopped (it holds only its own port, but esptool's port search can
pick the wrong board), `python tools/devloop/flash_helper.py flash 60 --port COM7` flashes the staged parts to that
port and logs 60 s; or install them from the Pages site; or flash one and let the others pull it in dev mode. The
Pages card can't connect to a port the helper is logging: stop the log window first.

1. Stage, and **refuse to flash unless the md5s match** (L1).
2. `flash.status` goes `flashing` → `logging` → `idle` (or `flash_failed`). Ask the user to touch the board *during*
   `logging`, and say what to do.
3. Tail `.devloop/serial_live.txt` while it logs; stop early once the lines you need are in (L4).
4. `flash.done`: `exit`, timings, `errors` (`E (` lines), `warnings`, `resets`, `stopped_early`. Then grep
   `.devloop/serial_log.txt` (written when the window ends).

Read before testing: `diag: firmware vX` and `diag: running from ota_N` in the fresh log (L2), then `ble: address`,
`ble: advertising`, `ble: scanning`.

Normal noise: `failed to load RF calibration data` (first boot after a flash).

## 3. Test console

USB Serial/JTAG, one command per line, answers are `test:` log lines (PROTOCOL.md §2): espforge's `ping`, `help`,
`heap`, `where` (no lock: answers when the display hangs), `memspeed`, `reboot`, `fps [reset]`, `tap X Y`,
`press X Y [ms]`, `swipe left|right|up|down`, `drag X1 Y1 X2 Y2 [ms]`, `screen [<name>]`, `power`; this app's
`cfg`, `color RRGGBB`, `name <text>`, `devmode on|off`, `bright 0-100`, `peers`, `hold on|off`, `fwpull <addr>`,
`fwinfo`, `fwloop`, `fwtune [window=N probe=KB scan=MS adv=0|1 rxscan=0|1 itvl=N mince=N maxce=N seq=0|1]`,
`shot [screen] [div]`, `clip rec <ms>|send|play|info`, `ring [local|stop|answer|cancel|answered]`.

- **Voice clips and calls** (three boards; only A's log is visible): on A, `clip rec 2000` records 2 s and sends it
  (`clip: sent <id> in ~2200 ms, 0 data refused`); `clip play` plays it on A. For reception, ask the user to hold the
  voice page of B or C and talk: A logs `mesh: clip <id> from <addr>`, then `clip: clip <id> from <origin> complete,
  <bytes> bytes, 0 holes` and `clip: played …` (rc.3: 5 s, 40,000 bytes, 0 holes). `ring` on A: ask the user whether B
  and C ring; a call from B or C shows `ui: overlay call` on A, a tap `clip: ring end after N, answered`. Answer test: `ring` on A, ask the user to tap B or C once while it rings; A logs `mesh: answer from <addr>`, `clip: call answered by <origin>`, `ui: overlay answered`, `clip: answered sound played` (rc.5). The tone's
  timing and spectrum are host-tested (`test_voice.c`); a WAV of it can be made on the PC from `main/ringtone.c`.

- Simulated touches enter at the touch controller read, so wake-up, hold and gestures run the real code (L105). A tap
  lasts 120 ms (L30). `hold on` sets the hold flag the way a finger on `main` does, without the touch.
- Heavy commands start their own task; the console task's stack is small and internal (L28).
- Settings from the PC (for a test; put the user's values back after): `devloop.py send "color ff8000"`,
  `send "devmode off"`, then `send cfg` and read the `test: cfg {…}` line. The same commands from a serial terminal
  (115200) or the Pages card.

## 4. Snapshots (`shot`, over USB)

There is no HTTPS to the board, so the console's `shot <screen> [DIV]` sends the screen over USB (every DIV-th pixel,
default 2, as `SHOT name w h`, `S:` rows, `SHOTEND`), and `python tools/devloop/shot.py .devloop/serial_log.txt`
(or `serial_live.txt`) makes PNGs from the log.

```bash
python tools/devloop/devloop.py send "shot main"      # then shot.py on the log
python tools/devloop/devloop.py send "shot pick 1"    # full resolution
```

- Screens: the names in `forge.json` `screens` (`pick`, `main`, `system`, `message`), plus `current`.
- Check every screen a change touches, **in every language** (French is longer and wraps; L142).
- A snapshot shows state, not interaction: for holds, mixes and drags ask the user to act during a log window and
  read `mesh: hold`, `mesh: mix`, `slide: drag` lines.

## 5. Harness (`tools/harness/`): everything, without a person

One command tests a board end to end. Needs the flash helper running and firmware with the test console. This
project runs it **without a network**: `--no-net`, or `"no_net": true` in `forge.json` (no IP wait, the boot check
reads `diag: running from ota_N`, the console check has no `key` / `wifi`; the `web`, `update`, `wifi_*` and `ota`
suites don't run).

```bash
python tools/harness/harness.py --no-net                          # all suites on the firmware on the board
python tools/harness/harness.py --no-net --flash                  # stage the build in forge.json build_dir, flash, test
python tools/harness/harness.py --no-net --flash --quick          # iterating: no idle minute
python tools/harness/harness.py --no-net boot console memory      # some suites (or --suite boot,console)
python tools/harness/harness.py --no-net --expect v0.1.0-rc.1     # fail unless the board runs this version
python tools/harness/harness.py --no-net perf --update-baseline   # propose new reference numbers
```

| Flag | Meaning |
|---|---|
| `--no-net` | No network: see above (`forge.json` `no_net` makes it the default). |
| `--flash [BUILD_DIR]` | Stage and flash a build folder first (default `forge.json` `build_dir`; every part, md5 checked). |
| `--suite <name>` (or positional names) | Run only these suites. |
| `--expect vX` | Fail unless the board runs vX. Always use it when testing a release. |
| `--update-baseline` | Write `tools/harness/baseline.proposed.json` (see below). |
| `--phone` | Include steps only a person can do (holding a second board); the harness prints `>>> ASK THE USER` (relay it at once). |
| `--minutes` | Log window to request (default 40 min). |

Suites. Generic ones live in `tools/harness/core_suites.py` and test the framework; this project's are in
`tools/harness/app_suites.py`.

| Suite | What it proves |
|---|---|
| `boot` | the start-up: no `E (` line, `diag: running from ota_N` / `App version` / the console agree, boot stage times; a crash kept from before is noted |
| `console` | `help` lists the commands; an unknown command is refused; `shot` sends a screen over USB that decodes to a picture |
| `memory` | internal RAM and PSRAM free and low points, failed allocations, against `baseline.json` |
| `screens` | each screen in `forge.json` `screens` snapshotted over USB (size, not blank) |
| `idle_stable` | 60 s idle: internal RAM doesn't drop |
| `navigation`, `perf` | swipes between `main` and `system`, fps and the drag's first frame against the baseline |
| `pick_then_main` | the pick screen at boot, a tap on a swatch saves the colour (`cfg` agrees) and shows `main` |
| `hold_sends` | `hold on` → `mesh: hold on` and the advertisement's holding bit (`mesh: mix … n=1`); `hold off` clears it within the budget |
| `peers_seen` | with a second board advertising (`--phone`: the harness asks for it): `mesh: peer <addr> …` within 2 s, `peers` lists it, `cfg`'s `peers` has it |
| `shot` | `pick`, `main`, `system` snapshots over USB |

Reports: `tools/harness/reports/<date>/report.md` (git-ignored) with `results.json`, screenshots and the log of each
failed test. Exit code 0 = all passed, no regression. Read the "Testing vX" line first.

How the gate works (L21-L35):

- **Baseline** (`baseline.json`): per metric `min`/`max`, `ref` (value when set), `note` (why that limit). A metric
  whose suite ran but wasn't measured is **MISSING**, a measured one without a limit is **NEW**; both fail like a
  **REGRESSION**. A test that skips a measurement on purpose calls `ctx.skip(pattern, reason)`. The project starts
  with `{}`: the first good build's `--update-baseline` run proposes the numbers.
- **Updating the baseline:** run the suites on a known-good build with `--update-baseline`; it writes
  `baseline.proposed.json`. Diff it against `baseline.json`, adjust limits (leave margin, L26), copy it over, and say
  why in the commit.
- **Log cursors:** the harness marks the log at the start of each test and fails a test on `rst:0x` after it
  (decoding the backtrace with `addr2line` against the ELF in `build_dir`).
- **No answer or `display busy`** → the harness sends `where` and puts the breadcrumbs in the failure.
- The harness's own logic has unit tests: `python tools/harness/test_harness.py`. With the flash helper's
  (`test_flash_helper.py`: loopback port, fake board; they need pyserial, so run them from an ESP-IDF shell) and the
  PC-side tools' (`test_forge_local.py`, `test_setup_github.py`):
  `python -m unittest discover -s tools/harness -p "test_*.py"`. CI runs them all (`host-tests`).

## 6. Host unit tests (`tests/host/`)

This app's pure C built with gcc against small shims of the ESP-IDF headers (`tests/host/shim/`) and espforge's
components at the tag `main/idf_component.yml` pins (`managed_components/` after an idf.py build, else
`python tools/fetch_forge.py` clones espforge into `.espforge/`), with AddressSanitizer and UBSan.
`test_mesh.c`: payload encode/decode round trip, peer expiry, mixing (red + blue = magenta, three holders), version
comparison, relay dedupe.

```bash
wsl make -C tests/host      # FORGE= to point at another espforge checkout
```

- A fix that can be reproduced off the board gets a case here; check that the case fails on the old code
  (`git show HEAD:path/x.c`) before calling it a test (L57).
- When the firmware starts using a new ESP-IDF symbol in a file compiled here, extend the shim, and run the tests
  before tagging (L15).
- CI runs them on every push; a release needs them to pass.

## 7. The Pages site's configuration card (`web/flash/`)

`node web/flash/config.test.js`: the line parser and the JSON extraction of `config.js`, in Node, no dependencies
(CI runs it in `host-tests`). The card itself is tried by hand: `python tools/make_flasher_site.py dist --build
build/v55 && python tools/make_flasher_site.py site --stable dist && python -m http.server -d _site 8000`, then
Chrome at `http://localhost:8000/` with a board on USB and the flash helper stopped (it holds the port): Connect,
read, change the colour, see the ring change.

## 7b. The Android app (`android/`)

Tools on this PC (2026-11-01): Temurin JDK 17 (`C:\Program Files\Eclipse Adoptium\jdk-17.0.20.101-hotspot`), the
Android SDK in `%USERPROFILE%\AppData\Local\Android\Sdk` (platform 35, build tools, `platform-tools\adb.exe`),
no Android Studio. In PowerShell, from `android/`:

- `$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-17.0.20.101-hotspot"; .\gradlew.bat test assembleDebug`:
  the JVM tests (`app/src/test`: the protocol, mix, expiry, ADPCM, CRC and chunks against `tests/fixtures/*.txt`,
  calls, waves, ring length) and `app/build/outputs/apk/debug/app-debug.apk` (`ca.themonkeyz.colormesh.debug`, beside
  a release install).
- The phone (wireless debugging, paired once with `adb pair <ip:port> <code>`; `adb mdns services` shows the ports):
  `adb connect <ip:port>`, `adb -s <ip:port> install -r app-debug.apk`, `adb shell pm grant
  ca.themonkeyz.colormesh.debug android.permission.BLUETOOTH_SCAN` (and `BLUETOOTH_ADVERTISE`, `RECORD_AUDIO`),
  `adb logcat -s colormesh`: `advertising 100 ms`, `peers=N holders=N sent=N heard=N` every 10 s, `advert origin=..
  seq=.. hold=.. rgb=.. effect=../..` per change, `ring from`, `answer from`, `wave k from`, `clip N: ms, bytes,
  packets`, `clip sent ok=true packets=n/n refused=0`.
- A tap or a hold from the PC: `adb shell input tap X Y`, `adb shell input swipe X Y X Y 2000` (a 2 s press), with X Y
  from `adb exec-out screencap -p > shot.png`.
- On the boards: A's log shows the phone as `mesh: peer <random addr> v0.0.0-0 #rrggbb hold|idle`, its waves and
  rings as from any peer, and its clips as `mesh: clip <id> from phone <origin> ...`.

## 8. Diagnostics

`python tools/devloop/devloop.py reboot 300`, wait for `idle`, then `python tools/diag_summary.py
.devloop/serial_log.txt`: memory, render timing, per-task CPU and stack high-water marks from the `diag:` lines. Keep
this project's reference numbers in `docs/DIAGNOSTICS.md` (template in `docs/templates/`) and compare after changes
that touch memory or rendering; the radio measurements go to `docs/ARCHITECTURE.md`.

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

## 10. Throwaway builds for fake inputs

Some paths can't be reached on demand: a peer with a newer firmware, a controller refusing an interval, ten boards.
Make a throwaway build that fakes the input, label it (`v0.1.0-fw.2`, `v0.1.0-loss.0`), test, then `git checkout`
the changed files and flash a clean build. **Never commit it.** Fake inputs at the data layer, in the data's own
units (L55); prefer a test-console switch in the real firmware when the path matters enough (`hold on|off`,
`fwpull <addr>`).

## Project checks

- **Screens**: `pick`, `main`, `system`, `message` (`shot` each, in both languages after a text change).
- **The flood on three boards** (the user holds): B holds → A, B, C in B's colour within a blink; B (blue) and C (red)
  → all three magenta; A alone → A's own colour; the dots and "2 near" on each. Log: `mesh: peer …`, `mesh: mix …`.
- **Dev mode**: `devmode on` on B; flash A with a build labelled above B's; B's log: `fw: target …`, `fw: N/M KB`,
  `fw: verified, restarting`, `diag: firmware <new>`, then `fw: confirmed` after 60 s. Never restart B in those 60 s.
- **Pull rate** (after a change to mesh_fw.c, mesh.c's radio code or the Bluetooth Kconfig): A on the PC serving a
  dev peer; A's log `fw: link mtu=512 phy=2M dle=251 itvl=7.50 ms` and `fw: served … (stream ≥ 40 KB/s, 0 resent)`,
  and `mesh: peer` lines from the third board during the transfer. To compare settings: `fwtune probe=200` right
  after the first logged line (the transfer stops at 200 KB, the peer retries after 30 s), then one `fwtune …` per
  setting and read the `fw: probe … KB/s` lines (2026-10-31: scan 100 ms 22 KB/s, 50 ms 40, 30 ms 52, 0 ms 75).
- **The Pages card**: in Chrome on the published site (or the local preview), connect, read, change the colour, see
  it on the board.
- **Radio measurements** after a change to `mesh_ble.c` or the Kconfig: the risks table in ARCHITECTURE.md.
