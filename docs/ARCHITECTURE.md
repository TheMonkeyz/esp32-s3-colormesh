# Architecture

<!-- How the firmware works and why it works that way. For each part: what it does, the rules that came from bugs
(with the numbers), and what to check after touching it. Framework components are described in docs/COMPONENTS.md;
describe here only how this app uses them. -->

ColorMesh: three (later more) boards share colours over Bluetooth LE. No Wi-Fi: forge_net is linked (forge_lvgl's
`screens.h` needs one of its structs) but never started. Configuration and updates come over USB from the Pages
site, or over Bluetooth from a peer in dev mode. Design chosen by [RESEARCH-BLE.md](RESEARCH-BLE.md).

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.75, three of them)

| Part | Details | Pins |
|---|---|---|
| MCU | ESP32-S3, 16 MB flash, 8 MB octal PSRAM; one 2.4 GHz radio, Bluetooth 5.0 LE only (no Classic) | |
| Display | CO5300 AMOLED 466×466 round, QSPI, RGB565 | CS 12, CLK 38, D0–D3 4/5/6/7, RST 39; column offset +6 |
| Touch | CST9217, I2C `0x5A` | SDA 15, SCL 14, RST 40 |
| Motion sensor | QMI8658, I2C `0x6B` (shared bus), unused | SDA 15, SCL 14 |
| Microphones | ES7210, unused | |
| USB | USB serial/JTAG: the test console, the Pages card, the flash helpers | |

Board support: espforge's `boards/ws_amoled175/board/`, at the tag in `main/idf_component.yml`. Board A is on COM5 of
the dev PC (the flash helper); B and C get one-shot flashes on their own ports (`flash_helper.py --port`).

## Modules (`main/`)

| File | Job |
|---|---|
| `main.c` | `nvs_init`, `app_text_init`, `diag_start(60)`, `board_init`, config load, `ui_init`, `mesh_start`, `testcon_start`, the pick screen then main, `diag_mark("app ready")`; rollback confirmation after 60 s |
| `ui.c` / `ui.h` | The screens: `pick`, `main`, `system`, `message`; the hold flag from the board's touch state; the mix read under the display lock |
| `config.c` | NVS: colour, dev mode, name (16 chars), brightness; defaults; the `cfg` JSON |
| `mesh_proto.c` / `.h` | Pure C (host-tested): payload encode/decode, the peer table (16 entries by address, self included), expiry, the mix, version compare through forge_core's `version.h` |
| `mesh_ble.c` | NimBLE: init, the public address, advertising (data updated in place), scanning, the `BLE_GAP_EVENT_DISC` queue, the mesh task |
| `mesh_fw.c` | Dev mode: the GATT service on the dev board, the sender on the serving board, OTA writes, verification, restart |
| `console.c` | USB console commands (`testcon_register`): `cfg`, `color`, `name`, `devmode`, `bright`, `peers`, `hold`, `fwpull`, `shot` |
| `app_text.c`, `i18n_strings.h` | Every display text, English and Québec French |

## Tasks

<!-- Every task: core / priority, stack size (from measured high-water marks: docs/LESSONS.md L63), its job. The
locking rule for LVGL calls from outside the LVGL task. -->

| Task | Core / priority, stack | Job |
|---|---|---|
| LVGL (forge_lvgl) | core 1 | Drawing; the mix is read under `display_lock()` |
| NimBLE host + controller | core 0 (Kconfig pinned) | The stack; the `DISC` callback copies 16 bytes to a queue and returns |
| mesh | core 0 | Drains the queue, updates the peer table, recomputes the mix on every report and on a 50 ms tick, updates the advertisement when the own state changes |
| fw (dev mode) | core 0 | The transfer: the sender streams from `esp_partition_mmap`; the receiver's write task feeds 4 KB sectors from a PSRAM ring into `esp_ota_write` |
| console (forge_core) | small, internal stack | One command per line; `shot` runs in its own task |

Stack sizes: fill from the `diag: tasks:` high-water marks of the first good build.

## Bluetooth design (summary of RESEARCH-BLE.md)

- **Transport: NimBLE, a connectionless advertising flood for state** (every board advertises its own state and
  scans continuously; nothing to provision, no phone) plus **one GATT connection on demand** for the dev-mode
  firmware pull. Not SIG mesh (8-byte vendor payload, ~90 ms per message, needs a provisioner, fights a GATT server),
  not ESP-NOW (Wi-Fi).
- **Advertising**: one legacy advertisement, one manufacturer-data AD, no Flags AD; non-connectable in normal mode
  (nobody can connect); connectable in dev mode wanting firmware, with the dev bit and a `target` byte naming the
  peer. Interval fixed at **20 ms** for three boards (the NimBLE floor; from about six boards: 20 ms for 300 ms
  after a change, then 100 ms). Data changes apply live with `ble_gap_adv_set_data` on a prebuilt 31-byte buffer;
  `seq` bumps only on a state change. Fallback if the controller refuses 20 ms non-connectable:
  `CONFIG_BT_NIMBLE_HIGH_DUTY_ADV_ITVL=y`, or connectable adverts with a Flags AD and connection rejection.
- **Payload** (30-byte AD structure): length, type `0xFF`, company id (2), protocol id, origin id (low byte of the
  address), seq (wrap-aware), TTL (0 = never relay), flags (bit0 holding, bit1 dev mode, bit2 image valid and
  servable, bit3 pending verify), R G B, firmware major/minor/patch, version kind (`-anything.N` → N, `-rc.N` →
  100+N, release → 200, post-release build → 201+N, the order of forge_core's `version.h`), target origin (`0xFF`
  unless dev mode), 14 reserved bytes (effects or relayed entries). On receive: `ble_hs_adv_find_field(MFG_DATA)`,
  check length, company id and protocol byte, copy out inside the callback.
- **Identity**: the public address (`ble_hs_util_ensure_addr(0)`, `ble_hs_id_infer_auto`), the peer table's key,
  shown on the system page and in `cfg`.
- **Scanning**: `ble_gap_disc` forever, passive, 100 ms window = interval, `filter_duplicates = 0`; the callback
  copies a 16-byte record into a FreeRTOS queue (microseconds). The scan is cancelled around `ble_gap_connect()` and
  restarted after `BLE_GAP_EVENT_CONNECT` (`CONFIG_BT_NIMBLE_HOST_ALLOW_CONNECT_WITH_SCAN` stays off, esp-idf
  issue 15258).
- **Peer table** (`mesh_proto.c`, 16 entries by address): RGB, holding, dev mode, version, seq, rssi, last heard
  tick; the own state is one more entry, mixed like a peer's. Release is explicit (every event after the finger
  lifts says `holding = 0`), so peers clear within one interval + 10 ms. Expiry only for a dead or out-of-range
  peer: 3 × steady interval + 100 ms (250 ms at 20 ms); a peer not heard for 10 s leaves the table. A 100 ms fade on
  add and remove hides jitter.
- **Mixing**: sum the holders' colours in linear light, then scale so the largest channel is full (keeps hue and
  brightness; averaging looks dim, clamping drifts to white). A board never receives its own advertisements, so self
  is added locally while its finger is down.
- **Timing targets**: remote light within 85 ms median, 150 ms worst. Three boards at 20 ms: the first report after
  a change arrives within one interval plus 0-10 ms; ten boards: ~24 % per-event loss, still every peer within
  ~150 ms.
- **Firmware pull** (`mesh_fw.c`): every advertisement carries the version and a servable bit (only while the running
  image is `ESP_OTA_IMG_VALID`). A dev-mode board seeing a strictly newer servable version (4-byte pre-filter, then
  forge_core `cmp_ver` on the full strings once connected) picks that peer (highest version, then lowest address),
  advertises connectable with `target` = that peer, and waits. Only the named peer reacts: it connects as central
  (7.5 ms interval, supervision 6 s), then data length 251, MTU 512, `ble_gattc_exchange_mtu`, connection update, 2M
  PHY, and streams the image with Write Without Response (~500 kbps at 1M, up to ~1.3 Mbps at 2M: 2 MB in 12-34 s).
  GATT service (custom 128-bit UUID) on the dev board: `CTRL` (START with length and SHA-256, ABORT), `DATA`
  (509-byte writes: sector index, packet seq, 506 bytes; the last packet of each 4 KB sector tagged `0xFF` with the
  sector CRC16), `ACK` (notify, per sector); sender window of 4 sectors; `BLE_HS_ENOMEM` = back off one tick. Sender:
  `esp_image_get_metadata` for the exact length, read through `esp_partition_mmap`. Receiver: checks the first 288
  bytes (magic, same `project_name`, strictly higher version, not the last invalid partition's) before writing;
  `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)`, `esp_ota_end` (verifies the SHA-256), set boot partition, restart.
  The new image boots `PENDING_VERIFY` and confirms itself after 60 s of normal running, else the bootloader rolls
  back; never restart a board in its first 60 s after an update (espforge L23). One pull at a time; a failed pull
  waits 30 s. `fwpull <addr>` sets the target by hand.

### Radio measurements (the report's risks)

Measured on board A alone, 2026-10-31 (v0.1.0-rc.0 test builds):

- Risk 1: `ble_gap_adv_start()` at 0x20 (20 ms), non-connectable, returns 0; `where` shows `ble_adv=32`. No fallback.
- Risk 2: the first build heard neighbours only in bursts (3-4 reports/s, each device once every 2.7 s). Cause:
  `CONFIG_BT_NIMBLE_HOST_QUEUE_CONG_CHECK=y` (the research report's table recommended it) makes the host pass each
  address once per ~200 reports (`ble_hs_hci_evt.c`, `ble_check_adv_list`). With it off: 71-75 reports/s from ~13
  neighbours at a 100/100 ms scan; the strongest steady neighbour (-51 dBm) is heard 17.4 times/s, one report every
  ~57 ms, largest gap 163 ms, no multi-second hole. The true per-event percentage needs a second ColorMesh board
  (it sends exactly 50/s): fill in after the three-board test.
- Risk 3: at ~75 reports/s the report queue never holds more than 1 of 32, the GAP callback takes at most 67 us,
  drops 0 (`where`: `ble_q=1/32 ble_cb=67us ble_drop=0`).
- Risk 7: `heap` after host sync: internal 149 KB free, largest block 78 KB, PSRAM 5552 KB; app .bin 1,091,024 bytes
  (3 MB slot). NimBLE host pools stay in internal RAM (`MEM_ALLOC_MODE_INTERNAL`).
- Hold to mix on the own board: `mesh: hold on` to `mesh: mix` 1-2 ms; release 20 ms.
- Risk 5 (transfer rate with LVGL running): measured with the two-board firmware pull (see HISTORY once done).


## Display pipeline

espforge's forge_lvgl and the board's `display.c`: draw buffers in internal RAM, LVGL's heap in PSRAM, moves between
pages drawn as pictures (slide.c). Nothing here is DMA-read from flash, so `esp_ota_write` during a pull only pauses
the display (`CONFIG_SPI_FLASH_WRITE_CHUNK_SIZE=4096` keeps each pause short).

## Screens (`forge.json` `screens`: `pick main system message`)

- **pick**: 12 swatches on a ring (hue every 30°), centre "Pick your colour / Tap one"; the saved colour is
  pre-highlighted; a tap saves it (NVS) and shows `main`.
- **main**: edge ring 14 px in the own colour; centre = the mix (black when nobody holds, with a dim "hold to send
  your colour" hint); a row of small dots, one per peer in its colour, bright while it holds; a line "N near ·
  vX.Y.Z"; `DEV` badge in dev mode; a progress arc during a firmware pull. **Hold** = finger down on `main`, from
  the board's touch state (not a 400 ms long press: latency); off on release, on PRESS_LOST, or when a drag starts.
  The UI only sets a flag; the mesh task does the radio (espforge L164).
- **system**: second pager page (swipe): version, Bluetooth address, memory, uptime, the peers with versions, dev
  mode.
- **message**: as espforge's starter (a text, for the console and the harness).

## Settings (NVS)

`config.c`: colour (`#rrggbb`), dev mode (bool), name (up to 16 bytes, UTF-8), brightness (0-100); typed keys, not a
blob (L68); a missing key keeps its default. Changed only from the USB console (`color`, `devmode`, `name`,
`bright`), which the Pages card drives. `cfg` answers everything in one JSON line (PROTOCOL.md §2).

## Languages

English and French (Canada), espforge's i18n: display texts in `main/i18n_strings.h`. The Pages site is English.

## Updates

Partition table: two OTA slots, otadata, NVS, core dump (`partitions.csv`); `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
Three ways in: the Pages site over USB (esp-web-tools, the parts of a release at their offsets, NVS kept), esptool
with the full image (erases NVS), and the dev-mode pull from a peer (the app part only, verified by `esp_ota_end`,
rolled back by the bootloader if the new image doesn't confirm itself within 60 s). No board polls the site: the
site serves people, not devices. Version order as forge_core: `vX.Y.Z-anything < vX.Y.Z-rc.N < vX.Y.Z < vX.Y.Z-N-gHASH`;
a board pulls only a strictly higher version with the same `project_name` (`colormesh`).

## Memory budget

<!-- Table of big users (where, size), measured internal RAM free / min ever, PSRAM min. -->

Estimates from the research (to replace with `idf.py size` and the `heap` command of the first good build):

| Item | Where | Size |
|---|---|---|
| NimBLE controller | internal DMA RAM, >20 KB of IRAM | ~25-45 KB |
| NimBLE host pools (`MSYS_1_BLOCK_COUNT=50`) | internal (PSRAM with `MEM_ALLOC_MODE_EXTERNAL`) | ~21-30 KB |
| Flash | OTA slot (3 MB, 1.16 MB headroom before BLE) | +250-350 KB |
| Internal RAM free before BLE (espforge log at `mark board`, with Wi-Fi initialised there) | | ~174 KB |

## Log lines (the contract with the tools and the Pages card)

| Line | Meaning |
|---|---|
| `test: cfg {"color":"#rrggbb","dev":bool,"name":"…","bright":N,"version":"v…","addr":"aa:bb:cc:dd:ee:ff","peers":[{"addr","color","holding","ver"}]}` | The answer to `cfg` (the Pages card renders it) |
| `test: ok color #rrggbb`, `test: ok name …`, `test: ok devmode on\|off`, `test: ok bright N` | A setting saved; `test: error …` otherwise |
| `cfg: …` | The configuration loaded or changed |
| `ble: address aa:bb:cc:dd:ee:ff` | The public address (the identity) |
| `ble: synced` | NimBLE host synced with the controller |
| `ble: advertising` | The flood is on air (after `ble_gap_adv_start` succeeded) |
| `ble: scanning` | Continuous passive scan started |
| `mesh: hold on` / `mesh: hold off` | This board's finger down / up (the harness's `hold_sends`) |
| `mesh: peer <addr> v<ver> #rrggbb hold\|idle` | A peer's state changed (first heard, colour, holding, expired) |
| `mesh: mix #rrggbb n=<holders>` | The mix shown, and how many hold |
| `fw: …` | The dev-mode pull: target chosen, connected, `fw: 1234/2048 KB`, verified, restarting, confirmed |
| `SHOT name w h`, `S:<base64 row>`…, `SHOTEND`, then `test: ok shot name wxh` | A screen over USB (`tools/devloop/shot.py`) |
| `diag: mark app ready` | `forge.json` `ready_line` |

## Known issues / TODO

<!-- One bullet each, with the version it was seen in. -->

- forge_net is linked but never started (v0.1.0-rc.1): forge_lvgl's `screens.h` includes `web.h`; espforge backlog.
- The advertising interval is fixed at 20 ms (v0.1.0-rc.1); the burst-then-100 ms scheme for six boards and more is
  not built.
