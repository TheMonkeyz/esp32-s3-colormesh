# espforge

espforge is a starting point for ESP32 firmware (ESP-IDF v5.5.4) developed mostly by an AI agent, Claude Code, with
a person deciding and trying things. It is the framework and the working method extracted from
[esp32-s3-weather](https://github.com/TheMonkeyz/esp32-s3-weather), a weather display built that way in six days
(~30 releases). It gives you: firmware components for Wi-Fi setup (setup network, captive portal, Easy Connect), a
settings web page over HTTPS with a per-device key, updates over Wi-Fi with rollback from a GitHub Pages site,
diagnostics and a USB test console; PC tools that let the agent flash, probe and test the board without a person
(a file-based flash helper, a test harness with a performance baseline, screenshots, host unit tests, browser tests
of the settings page); CI that builds, tests and publishes releases; and the lessons that made those tools necessary
([docs/LESSONS.md](docs/LESSONS.md)). The repository itself builds a small starter app for the Waveshare
ESP32-S3-Touch-AMOLED-1.75.

## Try it on the board

**[Install from the web flasher](https://themonkeyz.github.io/espforge/)**: plug the Waveshare
ESP32-S3-Touch-AMOLED-1.75 into a computer by USB, open the page in Chrome or Edge (desktop; they can talk to USB
devices), pick **Stable** or **Beta** (release candidates) and press Install. Then set up Wi-Fi from your phone with
the display's setup network or Easy Connect (press and hold the screen). Later versions arrive over Wi-Fi: the display
offers them on its system screen and on its settings page. Release notes: [CHANGELOG.md](CHANGELOG.md); all builds:
[Releases](https://github.com/TheMonkeyz/espforge/releases).

No board at hand? The flasher page links to **Try it in your browser**: the starter app's own screens and settings
page compiled to WebAssembly ([web/emu](web/emu/README.md)).

## The loop

```
  change ──► test build, labelled above the release ──► flash + probe (devloop, test console)
                                                              │
                                                              ▼
  the user tries it ◄── prove it on the device (harness, snapshots, baseline)
         │
         ▼
  "document and commit" ──► Claude tags vX.Y.Z-rc.N ──► CI ──► Pages Beta channel
                                                                     │
                                                                     ▼
                         stable vX.Y.Z (user's OK) ◄── harness --ota installs and tests it
```

Every few releases a read-only evaluation turns into a fix plan, worked one release candidate per group.
Details: [docs/WORKFLOW.md](docs/WORKFLOW.md).

## Quick start

Prerequisites (Windows; Linux works for everything except the PC-as-phone Wi-Fi tests; on a Mac,
[docs/MACOS.md](docs/MACOS.md) and `bash tools/mac/setup.sh`):

- [ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/) (the Windows
  installer puts it in `C:\Espressif\esp-idf`).
- Python 3.10+ (python.org rather than the Microsoft Store build, see L146), Node.js 22 for the page tests, WSL
  (Ubuntu) with gcc and make for the host tests, [GitHub CLI](https://cli.github.com/) for releases.
- The board on USB. [Claude Code](https://claude.com/claude-code) in the project folder.

Build (PowerShell):

```powershell
. C:\Espressif\esp-idf\export.ps1
idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build
```

Flash through the helper (it owns the COM port and keeps a log window open while the agent acts):

```powershell
tools\devloop\start_flash_helper.bat            # once, in its own window (Claude Code runs flash_helper.ps1 in the background instead)
python tools\devloop\stage.py                   # copy the parts under unique names, check md5
python tools\devloop\devloop.py flash 120       # flash, then log 120 s to .devloop\serial_log.txt
```

Test everything:

```powershell
python tools\harness\harness.py                 # all suites; report in tools\harness\reports\<date>\
```

On first boot the display opens its setup network (`Forge-Setup`); join it with a phone, or scan the Easy Connect
code, and give it your Wi-Fi. The settings page is then at `https://<ip>/` (the address is in the log line
`web: Settings page: https://<ip>/`).

## Layout

```
CMakeLists.txt                  ESP-IDF project; EXTRA_COMPONENT_DIRS selects the board
forge.json                      the project's config: app, repo, ota_site, screens, setup SSID, log patterns
sdkconfig.defaults              build config (CONFIG_FORGE_* values, PSRAM, QIO, -O2, rollback, core dump)
sdkconfig.debug                 extra checks for a debug build (heap poisoning, stack watchpoint)
partitions.csv                  two OTA slots, otadata, NVS, core dump
main/                           the starter app: hello | system pager, Wi-Fi setup pager, settings page
main/i18n_strings.h             every display text, English and Canadian French
main/web/index.html             the settings page
boards/ws_amoled175/board/      board support (component "board"): CO5300 display, CST9217 touch, QMI8658
components/forge_core/          version, http_once, utf8, png_rows, text fit, nvs_check, diag, test console, i18n core
components/forge_net/           Wi-Fi station, setup AP + captive portal, Easy Connect, TLS cert, web server, service health
components/forge_ota/           updates from the Pages site, rollback confirmation
components/forge_lvgl/          LVGL heap in PSRAM, pager, moves as pictures (slide.c), snapshots, touch injection, screens
components/dns_server/          captive-portal DNS (from ESP-IDF's example)
tools/devloop/                  flash helper (Windows .ps1, macOS / Linux .py), devloop.py, stage.py; files in .devloop/
tools/mac/                      setup.sh and doctor.sh for a Mac (docs/MACOS.md)
tools/harness/                  harness.py, board.py, core_suites.py, app_suites.py, baseline.json, reports/
tools/snapshot.py               a screen rendered off-display, saved as PNG
tools/diag_summary.py           summary of the diag: lines in a log
tools/make_flasher_site.py      release parts (dist) and the Pages flasher/OTA site (site)
tools/webtest/                  Playwright tests of the settings page against a mock device
web/flash/                      the web flasher page (the Pages site's index.html)
web/emu/                        the display in the browser (WebAssembly): the app's list, emu_main.c, its page
web/emu/forge/                  ...and the framework's stand-ins for the board and ESP-IDF (any app, unchanged)
tests/host/                     C unit tests with gcc + AddressSanitizer (WSL / Linux)
docs/                           workflow, testing, releasing, lessons, protocol, components, templates
.claude/                        Claude Code permissions and skills
.github/                        CI (build, tests, releases, Pages) and Dependabot
```

## The starter app

Two pages to swipe between, plus Wi-Fi setup on a long-press. Swipes follow the finger at ~66 fps: forge_lvgl's
`slide.c` draws them as pictures copied straight to the panel (LVGL's own scrolling managed ~24 fps).

- **hello**: clock and date; the place to start your own UI.
- **system**: firmware version, Wi-Fi, address, memory, uptime, update status, and the settings page's QR code.
- **setup** / **setup1**: Wi-Fi setup, two pages of their own pager: the setup network's QR code, and Easy Connect's
  (a faint placeholder until the code is ready, then it fades in).

The settings page (`https://<ip>/`) shows device info, Wi-Fi (scan and save), language (English / français) and
updates (channel, check, install, release notes). Everything visible goes through i18n; French is Canadian French.

## Documentation

| File | What |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Rules and setup for agent sessions; the project's bugs, preferences and facts grow here |
| [docs/WORKFLOW.md](docs/WORKFLOW.md) | The loop in detail, "document and commit", evaluation → fix plan, working with or without USB |
| [docs/TESTING.md](docs/TESTING.md) | Builds, devloop, test console, snapshots, harness, host and page tests, profiling |
| [docs/RELEASING.md](docs/RELEASING.md) | Version labels, CHANGELOG, rc and stable releases, CI, Pages, restoring a board |
| [docs/LESSONS.md](docs/LESSONS.md) | 150+ lessons by topic, each with its origin and how to check it |
| [docs/MACOS.md](docs/MACOS.md) | Working on a Mac: setup, the Python flash helper, what differs from the Windows PC |
| [docs/BACKLOG.md](docs/BACKLOG.md) | Changes from the apps that could come into espforge, for alignment sessions |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | What the PC tools rely on from the firmware: files, console, log lines, HTTP API, OTA site |
| [docs/COMPONENTS.md](docs/COMPONENTS.md) | The framework components and the board API |
| [docs/NEW-PROJECT.md](docs/NEW-PROJECT.md) | Start a project from this template; add a board |
| [docs/templates/](docs/templates/) | HISTORY, ARCHITECTURE, DIAGNOSTICS, IDEAS, EVALUATION, FIX-PLAN skeletons |

## Credits and licence

Extracted from [esp32-s3-weather](https://github.com/TheMonkeyz/esp32-s3-weather) (TheMonkeyz). MIT licence, see
[LICENSE](LICENSE); third-party parts in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
