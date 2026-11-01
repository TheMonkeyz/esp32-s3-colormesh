# Working on a Mac

The rest of the docs were written on the owner's Windows PC (COM5, PowerShell, WSL, `C:\` paths). This file says
what changes on a Mac (most of it holds on Linux too), for a person and for the AI agent helping them. Where it
disagrees with CLAUDE.md or docs/TESTING.md, **this file wins on a Mac**.

**Status (October 6, 2026):** ported from weather_amoled (`TheMonkeyz/esp32-s3-weather`, its docs/MACOS.md), where
the same Python flash helper ran the harness on the real board from the Windows PC (smoke + navigation 6/6, `flash
QIO`). Here, both helpers restarted the board through the test console on COM5 with the whole boot log. GitHub's
macOS runner (`.github/workflows/macos.yml`, Apple Silicon) runs the setup script on a fresh machine, the firmware
build, the host tests, the harness's and the helper's unit tests and the page tests on every push to a branch other
than main. **Nobody has used it on a real Mac with the board yet:** section 5 lists what is most likely to break.

| | On the Mac | Checked by |
|---|---|---|
| Firmware build (ESP-IDF at forge.json's `idf`) | yes | macOS CI |
| Flash + serial log (`tools/devloop/flash_helper.py`) | yes | loopback and fake-board tests in CI; the board from Windows |
| Host unit tests (`tests/host`, Apple's clang) | yes | macOS CI |
| Settings page tests (Playwright) | yes | macOS CI |
| Harness on the board | all suites except `wifi_setup` | not yet from a Mac |
| Snapshots from the board (`tools/snapshot.py`) | yes | not yet from a Mac |
| `wifi_setup` suite | **no**: it puts the computer's Wi-Fi on the display's setup network (netsh); a MacBook has no Ethernet, so it would lose the display and the internet. Left out of the default run off Windows | |

---

## 1. One-time setup (about an hour, mostly waiting)

1. **Xcode command line tools:** `xcode-select --install`.
2. **Homebrew** (https://brew.sh): paste its install line in Terminal, then run the two "Next steps" lines it prints.
3. **The project**, in a folder **without spaces** (ESP-IDF doesn't support them) and **outside Desktop / Documents**
   (iCloud may sync or offload build files): `git clone https://github.com/<owner>/<repo>.git ~/<repo>`.
4. **Everything else:** `bash tools/mac/setup.sh` (30-60 min, ~3 GB, safe to run again). It installs the build tools,
   ESP-IDF at forge.json's version in `~/esp/esp-idf-<version>` (`IDF_DIR=` to change), the page tests' browser, sets
   `core.autocrlf false` for this checkout and adds a `get_idf` command to your shell. **Open a new terminal after.**
5. **GitHub:** `gh auth login`; `git config --global user.name` / `user.email`.
6. **Check:** `bash tools/mac/doctor.sh` (`--board` with the board plugged in). Every line PASS or INFO; a FAIL says
   what to run. Paste its whole output when asking for help.

## 2. Build

```bash
get_idf
idf.py -B build/v55 -D SDKCONFIG=build/v55/sdkconfig build      # build_dir from forge.json
```

Label test builds above the newest release as on the PC (`version.txt`, then `touch CMakeLists.txt`; L9, L10).

## 3. The flash helper

The board's USB port belongs to one program, the flash helper; everything else (devloop.py, the harness, snapshot.py,
an agent) talks to it through the files in `.devloop/` (docs/PROTOCOL.md §1). On the PC that is
`start_flash_helper.bat`; on a Mac, in its own terminal, left running:

```bash
get_idf
python tools/devloop/flash_helper.py          # Ctrl-C stops it; q or Esc ends a log window early
```

Then, in another terminal, the same commands as on the PC:

```bash
python tools/devloop/stage.py                 # unique names, md5 (L1)
python tools/devloop/devloop.py flash 120     # flash the staged parts, log 120 s to .devloop/serial_log.txt
python tools/devloop/devloop.py reboot 60     # restart through the test console (whole boot log), log 60 s
python tools/harness/harness.py               # every suite but wifi_setup
```

One-shots without the helper running: `flash_helper.py flash 60`, `reboot 60`, `monitor 60`, `stage`, and `ports`
(lists the serial ports and marks the board's).

**The port.** `--port`, else `$FORGE_PORT`, else forge.json's `port`, else the ESP32-S3's own USB found by its vendor
id (0x303A): `/dev/cu.usbmodem…` on a Mac (the helper prefers `cu.` to `tty.`). macOS numbers usbmodem ports by USB
location, so the name changes with the socket; after a hard reset the helper finds the board again by vendor id.

## 4. Troubleshooting

| Symptom | Fix |
|---|---|
| `pyserial is missing` | Run it from a terminal where `get_idf` ran (ESP-IDF's Python has esptool and pyserial). |
| No port / esptool `No serial data received` | A charge-only cable is the usual cause: try another. `flash_helper.py ports`. Else hold BOOT, tap RESET, release BOOT, request again. |
| `Resource busy` | Another program has the port: `idf.py monitor`, a second helper, a serial terminal. Close it. |
| The harness waits for the helper | Is `flash_helper.py` running in its own terminal? `devloop.py status`. |
| `NOT FLASHING: ... changed since it was staged` | The build is newer than the stage: `stage.py` again (L1). |
| Snapshots / harness can't reach the display | Same Wi-Fi? macOS 15's Local Network permission for your terminal app (System Settings → Privacy & Security → Local Network). `curl -sk https://<ip>/api/info` tells. |
| `make: *** missing separator` or `bad interpreter` | CRLF in a script: `git config core.autocrlf false; git checkout -- .` (doctor.sh checks it). |

## 5. Not yet tried on a real Mac (most likely to break first)

1. **The port coming back after esptool's reset** under another `cu.usbmodem` name: handled by the vendor id lookup
   (tested with a fake port only).
2. **DTR/RTS when the port opens.** The helper opens with both low, as ESP-IDF's monitor does. If the board resets
   each time a log starts, or stays in `waiting for download`, that's it; compare with `idf.py -p <port> monitor`.
3. **The first seconds of log.** A restart request goes through the test console's `reboot` with the port kept open,
   so the log is whole from `ESP-ROM:` on (L191). After a **flash**, esptool's reset re-enumerates the USB and the
   first ~2.5 s are lost (L154): the harness then prints `flash ?` (QIO limits apply). If a restart falls back to
   esptool (`.devloop/flash_helper.log` lacks "Restarted through the test console"), the console didn't answer: a
   hung board, or firmware without it.
4. **Local Network permission** for HTTPS to the display (row above).
5. **Performance limits** in `baseline.json` come from the owner's board; the same model should hold them.

## Windows → Mac

| Windows (the rest of the docs) | Mac |
|---|---|
| `. C:\Espressif\esp-idf\export.ps1` | `get_idf` |
| `tools\devloop\start_flash_helper.bat` | `python tools/devloop/flash_helper.py` (own terminal) |
| `COM5` | `/dev/cu.usbmodem…` (`flash_helper.py ports`) |
| `wsl make -C tests/host` | `make -C tests/host` |
| `"/c/Program Files/GitHub CLI/gh.exe"` | `gh` |
| `harness.py` (all suites) | `harness.py` (wifi_setup left out) |
