# Device ↔ tools protocol

Everything the PC-side tools (`tools/devloop`, `tools/harness`) and the Pages site's *Configure over USB* card
(`web/flash/config.js`) rely on from the firmware. Keep this file and the firmware in step: a tool that parses a line
the firmware no longer prints fails silently or, worse, waits out a timeout. Project values (app name, screen size,
ready line…) come from `forge.json`. ColorMesh has no Wi-Fi: no HTTP API, no settings page, no update polling; the
USB console is the only way in, and the Pages site serves people, not devices.

## 1. Files between the agent and the flash helper (`.devloop/`)

The agent's shell may have no USB access (Claude desktop / cloud). The flash helper (`tools/devloop/`) runs on the PC,
owns the port and talks through plain files in `.devloop/`. Two helpers speak it: Windows' `flash_helper.ps1` +
`monitor.ps1` and the Python `flash_helper.py` (any OS; also one-shot flashes of the other boards with `--port`):

| File | Direction | Meaning |
|---|---|---|
| `flash.request` | agent → helper | Body: log seconds (default 60). Flash the parts listed in `<build_dir>/flasher_args.json` from the staged copies in `.devloop/stage/`, then log. |
| `reboot.request` | agent → helper | Body: log seconds. Restart (no flash), then log: the test console's `reboot` on the open port, so the boot log is whole (LESSONS L191); esptool's hard reset when no boot follows within 4 s. |
| `stop.request` | agent → helper | End the log window now (`stopped_early=1`). Q / Esc in the helper window does the same. |
| `serial.send` | agent → monitor | Each line is written to the port with `\n` and echoed in the log as `> line`. Write `serial.send.tmp`, then rename. |
| `flash.status` | helper → agent | `idle` / `flashing` / `logging` / `flash_failed` |
| `flash.running` | helper | Present while a request is handled. |
| `flash.done` | helper → agent | One line: `exit=0 port=COM5 flash_s=25 total_s=45 errors=0 warnings=0 resets=0 stopped_early=1 started=… finished=…` (errors = `E (` lines, warnings = `W (`, resets = `rst:0x`). |
| `serial_live.txt` | monitor | Grows line by line while logging (tail this). |
| `serial_log.txt` | monitor | The whole window, written when it ends. |
| `flash_log.txt`, `flash_helper.log` | helper | esptool's output; the helper's own history. |
| `harness.ask` | harness → person | A step only a person can do (hold a board, plug a second one). |
| `stage/` | agent | Staged parts, each copied under a **unique name** and checked by md5 before `flash.request`. |
| `port` | helpers | The board's COM port once found among several boards (the chip that answered esptool's `chip_id`); asked again when it is gone. |
| `board.lock` | agents | Several agents share board A: created (with the agent's name) before a flash, only if absent; deleted when the log window ends (CLAUDE.md). |

## 2. Test console (USB Serial/JTAG, `forge_core/testcon`)

One command per line, 115200 baud. Every answer is a log line with the tag `test`: `I (…) test: ok …`,
`test: <name> k=v …`, `test: cfg {json}` or `W (…) test: error …`. Commands are registered by the components and the
app (`testcon_register`). An unknown command answers `test: error unknown command '<name>' (help lists them)`. The
Pages card parses these lines with the ESP-IDF prefix (`I (1234) `) stripped (`web/flash/config.js`,
`config.test.js`).

| Command | Answer | From |
|---|---|---|
| (boot) | `test: console ready on USB (send 'help')` | forge_core |
| `ping` | `test: pong <version>` | forge_core |
| `help` | `test: commands: a, b, …` | forge_core |
| `heap` | `test: heap internal=K min=K largest=K psram=K psram_min=K uptime_s=S failed_allocs=N lvgl_fallbacks=N` (KB) | forge_core |
| `where` | `test: where <k=v from each breadcrumb provider> task_lvgl=<state>` — takes **no lock**, answers when the rest hangs | forge_core |
| `memspeed` | `test: memspeed …_ms=…` | forge_core |
| `reboot` | `test: ok restarting` | forge_core |
| `fps [reset]` | `test: fps frames=… render_avg_ms=… render_max_ms=… anim_frames=… anim_fps=… gap_max_ms=… …` | board display |
| `tap X Y`, `press X Y [ms]`, `swipe left\|right\|up\|down`, `drag X1 Y1 X2 Y2 [ms]` | `test: ok tap` … (simulated touches enter at the touch controller read, L105) | forge_lvgl |
| `screen` | `test: screen <name>` (`forge.json` `screens`: `pick main system message`) | forge_lvgl |
| `screen <name>` | `test: ok screen <name>` | forge_lvgl |
| `power` | `test: power cpu_mhz=N panel=asleep\|awake screen_lock=0\|1` | board |
| `shot [SCREEN [DIV]]` | `SHOT name w h`, one `S:<base64 RGB565 row>` line per row, `SHOTEND` (raw, straight to USB), then `test: ok shot name wxh`: the screen rendered off-display, every DIV-th pixel (1..4, default 2). `tools/devloop/shot.py` makes PNGs from a log. | app (`console.c`, from forge_lvgl's) |
| `cfg` | `test: cfg {"color":"#rrggbb","dev":true\|false,"name":"<up to 16 chars>","bright":0-100,"version":"v…","addr":"aa:bb:cc:dd:ee:ff","peers":[{"addr":"…","color":"#rrggbb","holding":true\|false,"ver":"v…"}]}` — one line, everything the Pages card shows | app |
| `color RRGGBB` | `test: ok color #rrggbb` (saved; the ring and the advertisement change at once) or `test: error …` | app |
| `name <text>` | `test: ok name <text>` (up to 16 characters, UTF-8; `name ` with nothing clears it) | app |
| `devmode on\|off` | `test: ok devmode on\|off` (saved; the advertisement's dev bit) | app |
| `bright N` | `test: ok bright N` (0-100, saved, applied) | app |
| `peers` | `test: peers n=N` then one `test: peer <addr> v<ver> #rrggbb hold\|idle rssi=-NN age_ms=N` per peer | app |
| `hold on\|off` | `test: ok hold on\|off`: as a finger down / up on `main` (test only) | app |
| `fwpull <addr>` | `test: ok fwpull <addr>`: dev-mode pull from that peer by hand (test only) | app |
| `fwtune [k=v …]` | `test: fwtune window=4 probe=0 scan=30 adv=1 rxscan=0 itvl=6 mince=12 maxce=24 seq=1`: the sender's RAM knobs (test only); `probe=KB` stops a transfer after that many KB | app |

## 3. Log lines the tools read

| Line | Meaning |
|---|---|
| `diag: firmware <ver>`, `diag: running from ota_0\|ota_1` | Which image and slot booted (the harness's `--no-net` boot check; this app links no forge_ota, so there is no `ota: Running` line). Check them in a fresh log **before** testing. |
| `diag: mark <stage>` | Boot stages; `diag: mark app ready` = `forge.json` `ready_line`. |
| `cfg: …` | The configuration loaded (`cfg: color #rrggbb dev 0 name "…" bright N`) or a value changed. |
| `ble: address aa:bb:cc:dd:ee:ff` | The public address: the board's identity, the peer table's key. |
| `ble: synced` | The NimBLE host is synced with the controller. |
| `ble: advertising` | `ble_gap_adv_start` succeeded: the flood is on air (the harness's `hold_sends` waits for it). |
| `ble: scanning` | The continuous passive scan started. |
| `mesh: hold on` / `mesh: hold off` | This board's finger down / up on `main` (also from `hold on\|off`). |
| `mesh: peer <addr> v<ver> #rrggbb hold\|idle` | A peer's state changed: first heard, new colour, hold on or off, or expired (the harness's `peers_seen`). |
| `mesh: mix #rrggbb n=<holders>` | What the centre shows and how many hold (self included). |
| `fw: …` | The dev-mode pull: `fw: want <ver> from <addr>`, `fw: linked to <addr>`, `fw: receiving N bytes`, `fw: 1234/2048 KB`, `fw: pulled <ver> from <addr> in S s (R KB/s, N resent), restarting`, `fw: confirmed <ver>` (after 60 s), `fw: failed: <why>`. Sender: `fw: serving <addr> <ver> N bytes`, `fw: link dle/phy/itvl/mtu …` (what the link granted), `fw: sent N/M KB in S s (R KB/s)`, `fw: served <addr> in S s (R KB/s; stream R KB/s, N resent, N ENOMEM N ms, window wait N ms; flood N reports/s)`, `fw: probe <addr>: N KB in S s (…)` after `fwtune probe=`. |
| `SHOT name w h` … `SHOTEND` | A screen over USB (§2 `shot`). |
| `slide: drag: first frame after N ms, F frames in T ms (X fps), …` | Each drag drawn as pictures (forge_lvgl). |
| `diag: mark <stage> internal N KB free (largest N), DMA N KB, PSRAM N KB` | A boot stage. |
| `diag: heap: internal N KB free (min ever N, largest block now N / worst N) \| DMA N KB … \| PSRAM N KB free … \| failed allocs N, LVGL in internal RAM N` | Every period (`tools/diag_summary.py`). |
| `diag: tasks: name(cC pP) X.X% NB \| …`, `diag: cpu: core0 N% busy, core1 N% busy (window N s)` | Per task: core, priority, CPU, stack bytes never used. |
| `diag: display: N frames, render avg N ms max N ms, … \| LVGL lock wait max N ms, longest hold N ms by <task>` | From the board (diag hook). |
| `App version: <ver>` | ESP-IDF's own boot line. |
| `diag: boot reset=<reason> …`, `diag: coredump …` | Boot info; the last crash, then erased. |
| `rst:0x…` | A restart (the harness fails a test that didn't expect one). |
| `Guru Meditation`, `Backtrace:` | A crash (the harness decodes it with addr2line when the ELF matches). |

## 4. HTTP API

None: ColorMesh has no Wi-Fi. forge_net is linked (forge_lvgl's `screens.h` needs one of its structs) but never
started; no port is open. Everything the settings page did in other espforge apps goes through §2.

## 5. The Pages site (GitHub Pages, `tools/make_flasher_site.py`)

`<ota_site>/channels.json` (`{"stable":{"version","built","manifest"},"beta":{…}|null}`),
`<ota_site>/<channel>/manifest.json` (esp-web-tools: the parts at their offsets; NVS is not among them, so an install
keeps the saved colour, name, dev mode and brightness), `<ota_site>/notes.json` (from `CHANGELOG.md`), `site.json`
(`{app, repo, fonts}`), and `config.js` beside `index.html` for the *Configure over USB* card. No board reads the
site: it is for the person at the computer. Version order (`forge_core/version.c`, mirrored in CI and in the
advertisement's version-kind byte): `vX.Y.Z-anything < vX.Y.Z-rc.N < vX.Y.Z < vX.Y.Z-N-gHASH`.

## 6. The Pages card over Web Serial (`web/flash/config.js`)

Chrome / Edge on a computer, https or localhost; the port must be free (the Install dialog of the same page holds it
while open). The card opens the port at 115200, reads lines (CRLF or LF, ANSI colour codes stripped, the ESP-IDF
prefix `I (1234) ` optional), sends `cfg` and renders the JSON of the `test: cfg {…}` line; each control sends its
command (`color RRGGBB`, `name <text>`, `devmode on|off`, `bright N`) and `cfg` again 300 ms later, so what it shows
is what the board saved; a `test: error …` line is shown as is. It never sends anything else.
