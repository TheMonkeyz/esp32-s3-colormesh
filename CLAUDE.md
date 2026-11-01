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
- **Check `diag: firmware <ver>` and `diag: running from ota_N` in a fresh log before testing** (L2; this app
  links no forge_ota, so there is no `ota: Running` line).
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
- **Never restart a board in the first 60 s after an update** (it rolls back) (L23).
- **Report what the log shows**, not what a message suggests (L46).
- **Backlog** (the user's rule, 2026-10-07, for esp32-s3-rtcquebec, weather_amoled and espforge): when a
  change could go into espforge (framework code, board support, tools, tests, docs, a lesson), add an entry to
  `docs/BACKLOG.md` (`C:\Users\lmathieu\ESPDEV\espforge\docs\BACKLOG.md`, its format at the top) in the
  same session, before calling the work done. Alignment sessions work through it for future projects.
  Here: also what the apps should take (new tags to bump, fixes to port).

Lessons by topic (memory, LVGL, touch, Wi-Fi, OTA, Windows tooling…): `docs/LESSONS.md`. Add a lesson there when a
bug teaches something general; add the project-specific fact below.

## Working setup (this PC)

- **No Wi-Fi at all.** Bluetooth LE between the boards (NimBLE, `docs/RESEARCH-BLE.md`); configuration and updates
  over USB serial (the Pages site, the test console) or, in dev mode, from a peer over Bluetooth. forge_net is linked
  only because forge_lvgl requires it; it is never started. So: no IP, no key, no HTTPS snapshots (`forge.json`
  `no_net`); the harness runs with `--no-net`.
- **Boards:** three Waveshare ESP32-S3-Touch-AMOLED-1.75 on a Windows 11 PC. Board A is on **COM5** (`forge.json`
  `port` empty = auto); the others get one-shot flashes (`flash_helper.py flash 60 --port COMx` while the helper is
  stopped) or the Pages flasher.
- **ESP-IDF v5.5.4** at `C:\Espressif\esp-idf` (CI uses the same version, from `forge.json` `idf`). In PowerShell:
  `. C:\Espressif\esp-idf\export.ps1`, then
  `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`. The framework comes from espforge at a tag
  (`main/idf_component.yml`, v0.5.0); delete `dependencies.lock` when bumping it. From the Claude desktop app's
  shells, `%LOCALAPPDATA%` is redirected into the app's package folder and the component manager's git fetch fails
  ("'git init --bare' failed ... unable to get current working directory"): set
  `$env:IDF_COMPONENT_CACHE_PATH = "<checkout>\build\v55\cm_cache"` in that shell before `idf.py`.
- **Flashing:** Claude starts the flash helper itself, in the background (`run_in_background`:
  `powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1`), so no window opens on the
  user's screen (user's rule, 2026-10-06); restart it the same way after editing `flash_helper.ps1` (L6). Then
  `python tools\devloop\devloop.py flash 90 --build build\v55` (stages under unique names with md5 checks, flashes,
  logs to `.devloop\serial_log.txt`), wait for `.devloop\flash.done`; during the log window
  `python tools\devloop\devloop.py send "ping"` (test console). Check that `.devloop\flash_log.txt` names COM5.
- **Board lock** (several agents share the board): before a flash, create `.devloop\board.lock` holding your name,
  only if it does not exist (else wait 30 s and retry, up to 15 min); delete it when your log window ends.
- **Other projects' helpers** run on this PC too (esp32-s3-meteobus: `flash_helper.ps1`, `p4\flash_helper_p4.ps1`).
  An idle helper does not hold the port, but one flashing does: check their `flash.status` is `idle` before flashing.
- **GitHub CLI** signed in as TheMonkeyz; not on the PATH of older shells: Git Bash
  `"/c/Program Files/GitHub CLI/gh.exe"`, PowerShell `& "C:\Program Files\GitHub CLI\gh.exe"`.
- **Host tests** run in WSL (Ubuntu): `wsl make -C tests/host`. **Harness + flash helper unit tests** need pyserial:
  `~/.espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe -m unittest discover -s tools/harness -p "test_*.py"`.

## How the user likes to work

- Short progress notes; no screen takeover; changes verified on the device (log + snapshot) before they are called
  done.
- Stable releases only with their OK; rc releases Claude publishes and tests, then reports.
- New features → minor version bump.
- **French = Canadian French (Québec), standard written:** no anglicisms, no slang; **1er** for the first of the
  month; check grammar agreement when a noun changes.
- New display text goes in `main/i18n_strings.h` (`X(T_ID, "English", "Français")`), never a bare literal on screen.
  The device has no web page (the Pages site is the configuration UI). Check French in snapshots: it is longer and
  wraps.

## Bugs hit, and their fixes

Number them; one entry per bug: symptom, cause, fix, the test that guards it. General lessons go to docs/LESSONS.md.

## User preferences learned

Add each preference the user states (gestures, wording, timing, look) with the date, in their words when possible.
- 2026-10-31: screen texts in the **vous** form with an infinitive: the main screen's hint is
  "Toucher l'écran pour envoyer votre couleur" / "Touch the screen to send your colour" (the user replaced
  "Gardez le doigt pour envoyer votre couleur"); the pick screen "Choisissez votre couleur" / "Touchez une pastille".
- 2026-10-31: the peer count on the main screen reads "N à portée" in French (not "N proches"); English "N near".


## Useful facts

Board facts, API quirks, numbers measured on this hardware, test recipes: anything a future session would otherwise
rediscover.
- Boards (2026-10-31): A b8:1f:3f:c8:75:9a (origin 9a, COM5 on this PC), B 44:bd:8d:60:3c:76 (origin 76),
  C 44:bd:8d:60:82:1a (origin 1a). The PC has one free USB port: B and C run from chargers during tests; only A's
  log is visible. A board plugged alone showed up as COM8 (B) and COM3 (C).
- Flood numbers: 20 ms non-connectable advertising accepted; 100/100 ms scan hears 61 % of two peers' events;
  `CONFIG_BT_NIMBLE_HOST_QUEUE_CONG_CHECK` must stay off (one report per address per 2.7 s with it on).
- Pull over Bluetooth: 21.4 KB/s receiving with the display on (1.1 MB in 51 s); `fwloop` self-test 115 KB/s on
  the flash side. Internal RAM at app ready 135-137 KB free, largest block 64-66 KB.
