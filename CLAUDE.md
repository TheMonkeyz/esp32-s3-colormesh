# Notes for AI-assisted sessions

Read first, in this order: `README.md`, `docs/WORKFLOW.md` (the loop), `docs/TESTING.md` (how to prove a change),
`docs/LESSONS.md` (what went wrong before, L1..), `docs/PROTOCOL.md` (what the tools read from the firmware),
`docs/COMPONENTS.md` (the framework's components), then this project's `docs/HISTORY.md` and
`docs/ARCHITECTURE.md` once they exist (templates in `docs/templates/`). Project values (app name, repo, OTA site,
screens, build dir) are in `forge.json`.

## The loop

1. Change, then a test build labelled above the current release (`version.txt`, touch `CMakeLists.txt`).
2. Flash and probe: `python tools/harness/harness.py --flash <bin>` or the devloop helper + test console.
3. Prove it on the device: log lines, snapshots, the harness's checks and baseline.
4. The user tries it (real fingers, real phone).
5. "Document and commit": ARCHITECTURE, TESTING, this file, CHANGELOG.
6. Claude tags and tests a release candidate (`harness.py --ota vX.Y.Z-rc.N`); a stable tag only with the user's OK.

Details: `docs/WORKFLOW.md`, `docs/RELEASING.md`. Skills in `.claude/skills/` hold the step lists.

## Rules that are not negotiable

- **Stage parts under unique names and compare md5 before flashing** (`tools/devloop/stage.py`). A reused path has
  delivered a stale file (L1).
- **Check `ota: Running <ver> from ota_N` in a fresh log before testing** (L2).
- **Label test builds above the current release**; delete `version.txt` when done (L9, L10).
- **Build exactly what you commit.** Set later work aside with `git stash push -u`, build, commit, `git stash pop`
  (L14).
- **Run the host tests before tagging** when a C file they compile changed: `wsl make -C tests/host` (L15).
- **Ask before a stable tag.** Release candidates Claude may tag, push and test itself (L18).
- **One-line progress note before anything that takes more than a minute** (build, flash, log window) (L41).
- **Ask the user to interact during the log window, and say exactly when and what** (L42).
- **Never take over the user's screen.** Ask first; prefer the flash helper (L43).
- **Relay `>>> ASK THE USER` from the harness at once** (L44).
- **Every fix gets a test that fails on the old code** (harness, Playwright or host test) (L57).
- **Reset cached board facts (`.devloop/ip`, `.devloop/key`) after a flash or an install** (L7).
- **Never restart a board in the first 60 s after an update** (it rolls back) (L23).
- **Report what the log shows**, not what a message suggests (L46).
- **Backlog** (the user's rule, 2026-10-07, for esp32-s3-rtcquebec, weather_amoled and espforge): when a
  change could go into espforge (framework code, board support, tools, tests, docs, a lesson), add an entry to
  `docs/BACKLOG.md` (`C:\Users\lmathieu\ESPDEV\espforge\docs\BACKLOG.md`, its format at the top) in the
  same session, before calling the work done. Alignment sessions work through it for future projects.
  Here: also what the apps should take (new tags to bump, fixes to port).

Lessons by topic (memory, LVGL, touch, Wi-Fi, OTA, Windows tooling…): `docs/LESSONS.md`. Add a lesson there when a
bug teaches something general; add the project-specific fact below.

## Shared with weather_amoled

[weather_amoled](https://github.com/TheMonkeyz/esp32-s3-weather) (`C:\Users\lmathieu\ESPDEV\weather_amoled`) is the
app espforge was extracted from (October 4). Since its v1.14.0 it **uses forge_core, forge_net, forge_ota and
dns_server at an espforge release tag** (its `main/idf_component.yml`); it keeps its own display, touch, slide.c and
app code (forge_lvgl and the board are not used there).

- **Before tagging an espforge release that changes those components**, build weather_amoled against this checkout
  (`python tools/forge_local.py` there: a temporary `override_path`, nothing committed) and run its harness; then tag
  here, and bump the four tags in its manifest. Keep components generic: app texts, names and languages come in
  through the hooks (i18n descriptors, `svc_set_why_text`, `ota_set_err_text`, Kconfig `FORGE_*`).
- **Still twins** (same code in both, not shared): forge_lvgl's `lvgl_mem.c` and `pager.c`, the board's `display.c`,
  `touch.c`, `imu.c`, the harness's `board.py` / `harness.py` and the flash helper (espforge's
  `tools/devloop/flash_helper.py` is weather_amoled's `tools/flash_helper.py` with `.devloop/` and forge.json; the
  PowerShell helpers restart through the console too since October 6). The browser emulator: its
  `web/emu/emu_*.c` are the originals of espforge's `web/emu/forge/` (generic since October 7; weather_amoled
  still has its own copy, with its audio, motion and partition stand-ins). A fix in one gets ported, or a
  task for the other. Its `slide.c` is the larger original: port ideas, not the file.

## Working setup (this PC)

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 on **COM5** of a Windows 11 PC (`forge.json` `port` empty = auto).
- **ESP-IDF v5.5.4** at `C:\Espressif\esp-idf` (CI uses the same version, from `forge.json` `idf`). In PowerShell:
  `. C:\Espressif\esp-idf\export.ps1`, then
  `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`.
- **GitHub CLI** signed in as TheMonkeyz; not on the PATH of older shells: Git Bash
  `"/c/Program Files/GitHub CLI/gh.exe"`, PowerShell `& "C:\Program Files\GitHub CLI\gh.exe"`.
- **Browser emulator** (`web/emu`): Emscripten 6.0.11 in WSL at `~/emsdk`;
  `wsl bash -lc 'source ~/emsdk/emsdk_env.sh && make -C /mnt/c/<checkout>/web/emu -j8'` (LVGL from
  `managed_components` after an idf.py build; ~1 min from scratch, 5 s with nothing to do), then
  `node web/emu/forge/smoke.js`.
- **Host tests** run in WSL (Ubuntu): `wsl make -C tests/host`. **Harness + flash helper unit tests** need pyserial:
  `~/.espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe -m unittest discover -s tools/harness -p "test_*.py"`
  (plain Python skips the helper's). **Settings page tests**: `cd tools/webtest && npm test`
  (Node in `C:\Program Files\nodejs`).
- **Two ways to work:**
  1. **Claude Code on the PC** (preferred): builds, `idf.py`, the harness and `gh` run directly. The flash helper
     still owns the COM port while it is logging; the harness talks to it. **Claude starts the helper itself, in the
     background** (`run_in_background`: `powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1`), so no window
     opens on the user's screen (user's rule, 2026-10-06, as weather_amoled does); restart it the same way after
     editing `flash_helper.ps1` (L6). `start_flash_helper.bat` (a window) only when the user asks for one.
  2. **Claude desktop app / cloud**: builds run in a cloud container, the shell has **no USB** and can't type into
     Windows terminals. The user starts `tools\devloop\start_flash_helper.bat` once; Claude stages the parts, runs
     `python tools/devloop/devloop.py flash 120` (or writes `.devloop/flash.request`), waits for
     `.devloop/flash.done` and reads `.devloop/serial_log.txt`. The cloud can't reach the board's IP: no
     snapshots, no harness. Cloud build recipe: `docs/WORKFLOW.md`.
- The display's IP is in the log (`web: Settings page: https://<ip>/`); its key comes from the console (`key`).

## How the user likes to work

- Short progress notes; no screen takeover; changes verified on the device (log + snapshot) before they are called
  done.
- Stable releases only with their OK; rc releases Claude publishes and tests, then reports.
- New features → minor version bump.
- **French = Canadian French (Québec), standard written:** no anglicisms, no slang; **1er** for the first of the
  month; check grammar agreement when a noun changes.
- New display text goes in `main/i18n_strings.h` (`X(T_ID, "English", "Français")`), never a bare literal on screen;
  page text goes in `I18N` in `main/web/index.html`. Check French in snapshots: it is longer and wraps.

## Bugs hit, and their fixes

Number them; one entry per bug: symptom, cause, fix, the test that guards it. General lessons go to docs/LESSONS.md.

## User preferences learned

Add each preference the user states (gestures, wording, timing, look) with the date, in their words when possible.

## Useful facts

Board facts, API quirks, numbers measured on this hardware, test recipes: anything a future session would otherwise
rediscover.
