# ColorMesh

Three small round screens that share colours over Bluetooth. Each one runs on a Waveshare ESP32-S3-Touch-AMOLED-1.75
and is built on [espforge](https://github.com/TheMonkeyz/espforge). No Wi-Fi, no phone, no app: the boards find each
other by themselves. Proof of concept of a low-latency Bluetooth "advertising flood" between ESP32-S3 boards
([docs/RESEARCH-BLE.md](docs/RESEARCH-BLE.md)).

## What it does

1. **Pick a colour** at start-up: a ring of 12 colours; tap one (the last one is pre-selected). It is saved.
2. **Hold to send**: on the main screen your colour is the ring around the edge. Touch and hold anywhere, and every
   board nearby, yours included, lights up in your colour.
3. **Mix**: while several people hold, every screen shows the mix of their colours (red and blue make magenta, red
   and green make yellow, all three make white). Lift the finger and your colour leaves the mix within a blink.

Small dots on the main screen show the boards nearby, in their colours, bright while they hold; a line says how many
are near and which firmware you run. Swipe for the **system** page: version, Bluetooth address, memory, uptime, the
boards nearby with their versions, dev mode.

## Install and configure

**[Install from the web flasher](https://themonkeyz.github.io/esp32-s3-colormesh/)**: plug the board into a computer
by USB, open the page in Chrome or Edge (desktop), pick **Stable** or **Beta** (release candidates) and press
Install. Release notes: [CHANGELOG.md](CHANGELOG.md); all builds:
[Releases](https://github.com/TheMonkeyz/esp32-s3-colormesh/releases).

**Configure over USB** on the same page: close the Install dialog, open *Configure over USB*, press Connect and pick
the board's port. The card reads the board's settings and lets you change its colour (a colour picker or the 12
presets), its name (up to 16 characters), **dev mode** and the screen brightness, and shows its firmware version,
its Bluetooth address and the boards it hears. Nothing leaves your computer: the page talks to the board through the
browser's serial port access.

**Updates** arrive over USB from the same page (Stable or Beta), or over Bluetooth: a board in **dev mode** that hears
a board nearby running a newer firmware asks that board for it, pulls the image over a Bluetooth connection (a
progress arc on screen, 15-40 s), installs it and restarts; a new firmware that fails to run for a minute goes back
to the previous one by itself. So one board flashed from the page updates the others. Dev mode off (the default) is
for boards that should only change by USB.

## For developers

The loop ([docs/WORKFLOW.md](docs/WORKFLOW.md)): a change, a test build labelled above the release, flash and probe
through the devloop helper and the USB test console, prove it on the boards (log, `shot` snapshots, the harness), the
user tries it, "document and commit", a release candidate tagged by Claude, a stable tag with the user's OK.

Prerequisites (Windows; Linux works too):

- [ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/) (the Windows
  installer puts it in `C:\Espressif\esp-idf`).
- Python 3.10+ (python.org rather than the Microsoft Store build, see L146), Node.js 22 for the flasher page's test,
  WSL (Ubuntu) with gcc and make for the host tests, [GitHub CLI](https://cli.github.com/) for releases.
- The boards on USB. [Claude Code](https://claude.com/claude-code) in the project folder.

Build (PowerShell):

```powershell
. C:\Espressif\esp-idf\export.ps1
idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build
```

Flash through the helper (it owns the COM port and keeps a log window open while the agent acts):

```powershell
tools\devloop\start_flash_helper.bat                        # once, in its own window (Claude Code runs flash_helper.ps1 in the background instead)
python tools\devloop\devloop.py flash 90 --build build\v55   # stage the parts under unique names (md5 checked), flash, log 90 s to .devloop\serial_log.txt
python tools\devloop\flash_helper.py flash 60 --port COM7    # a second board, one shot, while the helper is stopped
```

Test everything (no network: `--no-net`, or `"no_net": true` in `forge.json`):

```powershell
python tools\harness\harness.py --no-net        # all suites; report in tools\harness\reports\<date>\
wsl make -C tests/host                          # host unit tests (mesh payload, peer table, mixing, versions)
node web\flash\config.test.js                   # the flasher page's configuration card
```

The USB test console (`devloop.py send "<cmd>"`): `cfg`, `color RRGGBB`, `name <text>`, `devmode on|off`,
`bright 0-100`, `peers`, `hold on|off`, `fwpull <addr>`, `fwinfo`, `fwloop`, `fwtune …`, `shot [screen] [div]`, and espforge's `ping`, `help`,
`heap`, `where`, `screen`, `tap`, `swipe`, `reboot`. Replies are `test:` log lines
([docs/PROTOCOL.md](docs/PROTOCOL.md)).

Releases ([docs/RELEASING.md](docs/RELEASING.md)): a tag `vX.Y.Z-rc.N` builds a pre-release and moves the site's Beta
channel; `vX.Y.Z` a release and the Stable channel. Before the first stable release the site is Beta-only.

## Layout

```
CMakeLists.txt                  ESP-IDF project (colormesh)
forge.json                      the project's config: app, repo, ota_site, screens, no_net, log patterns
sdkconfig.defaults              build config (NimBLE, PSRAM, QIO, -O2, rollback, core dump; no Wi-Fi)
partitions.csv                  two OTA slots, otadata, NVS, core dump
main/                           the app: screens, config, mesh (payload, peer table, mix), NimBLE, firmware pull, console
main/i18n_strings.h             every display text, English and Canadian French
main/idf_component.yml          espforge's components and board at a tag
tools/fetch_forge.py            espforge at the pinned tag in .espforge/ (host tests, CI)
tools/devloop/                  flash helpers (PowerShell, Python), devloop.py, stage.py, shot.py; state files in .devloop/
tools/harness/                  harness.py, board.py, core_suites.py, app_suites.py, baseline.json, reports/
tools/diag_summary.py           summary of the diag: lines in a log
tools/make_flasher_site.py      release parts (dist) and the Pages flasher site (site)
web/flash/                      the Pages site: index.html, config.js (Configure over USB), config.test.js
tests/host/                     C unit tests with gcc + AddressSanitizer (WSL / Linux)
docs/                           architecture, history, research, workflow, testing, releasing, lessons, protocol
.claude/                        Claude Code permissions and skills
.github/                        CI (build, tests, releases, Pages) and Dependabot
```

## Documentation

| File | What |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Rules and setup for agent sessions; the project's bugs, preferences and facts grow here |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | The boards, the Bluetooth design, the modules, the log-line contract, memory |
| [docs/HISTORY.md](docs/HISTORY.md) | How the project got here; open threads |
| [docs/RESEARCH-BLE.md](docs/RESEARCH-BLE.md) | The research report that chose the design; its notes in [docs/research/](docs/research/) |
| [docs/WORKFLOW.md](docs/WORKFLOW.md) | The loop in detail, "document and commit", evaluation → fix plan |
| [docs/TESTING.md](docs/TESTING.md) | Builds, devloop, test console, `shot` snapshots, harness, host tests |
| [docs/RELEASING.md](docs/RELEASING.md) | Version labels, CHANGELOG, rc and stable releases, CI, Pages, restoring a board |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | What the PC tools and the Pages card rely on from the firmware: files, console, log lines |
| [docs/LESSONS.md](docs/LESSONS.md) | espforge's lessons by topic (the framework's history) |
| [docs/COMPONENTS.md](docs/COMPONENTS.md) | The framework components and the board API |

## Credits and licence

Built on [espforge](https://github.com/TheMonkeyz/espforge), itself extracted from
[esp32-s3-weather](https://github.com/TheMonkeyz/esp32-s3-weather) (TheMonkeyz). MIT licence, see
[LICENSE](LICENSE); third-party parts in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
