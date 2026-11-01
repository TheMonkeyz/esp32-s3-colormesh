# Project history and retrospective

<!-- The story that connects the other docs, written for the next session. ARCHITECTURE says how things work,
TESTING how to check them, CLAUDE.md the lessons in short; this file says how the project got here. Update it at each
stable release and after each evaluation. -->

## In numbers

- 1 day so far (2026-10-31): research in the morning, the project in the afternoon; first release candidate
  v0.1.0-rc.1.
- 3 boards (Waveshare ESP32-S3-Touch-AMOLED-1.75), 1 user.
- Lines of C: to fill at the first stable release (`main/*.c`, `tests/host/test_mesh.c`).

## Timeline

### 2026-10-31: research, then the project from espforge v0.6.0 (v0.1.0-rc.1)

The user has three espforge boards and wanted a proof of concept of a Bluetooth mesh: pick a colour at boot, hold the
screen and the other boards light up in it, several holders mix. Decided with the user: a new project from the
espforge template with CI, releases and a Pages site like esp32-s3-rtcquebec; **no Wi-Fi at all** (configuration and
updates over USB from the Pages site); low latency first; dev mode where a board pulls a newer firmware from a peer
over Bluetooth; screens approved from mock-ups (a ring of 12 colours, a main screen with the own colour as the edge
ring and the mix in the centre, a DEV badge). Changed the same day: the holder's own colour mixes in on its own
screen too (every screen shows the mix of every holder).

A deep-research pass (six notes, one report: [RESEARCH-BLE.md](RESEARCH-BLE.md), [research/](research/)) compared
Bluetooth SIG mesh, a custom NimBLE advertising flood, GATT connections and ESP-NOW against the three boards, a
~85 ms latency target and the firmware pull. The flood won: nothing to provision, one advertisement every 20 ms
carries the state, SIG mesh caps a vendor message at 8 bytes and blocks a sender ~90 ms per message, GATT meshes
need an election at ten boards, ESP-NOW is Wi-Fi. The firmware pull is one GATT connection on demand from the same
stack. The design is in [ARCHITECTURE.md](ARCHITECTURE.md).

Built in one session by parallel agents: firmware (screens, config, mesh protocol, NimBLE, firmware pull, console),
the harness's `--no-net` mode and app suites, host tests, and this site/CI/docs set. The framework's screen code
still needs forge_net to compile (`screens.h` includes `web.h` for one struct), so forge_net is linked but never
started; the clean-up went to espforge's backlog.

## How the work is done now

As [WORKFLOW.md](WORKFLOW.md), without the network parts:

1. A change, a test build labelled above the release (`version.txt`), `idf.py -B build\v55 … build`.
2. Board A on the flash helper (COM5); boards B and C flashed one-shot with `flash_helper.py --port COMx` while the
   helper is stopped, or from the Pages site.
3. Prove it: the log lines of [PROTOCOL.md](PROTOCOL.md) §3 (`ble: advertising`, `mesh: peer …`, `mesh: mix …`),
   `shot` snapshots over USB, `harness.py --no-net`.
4. The user holds the boards: the only test of "within a blink".
5. Document and commit; Claude tags `vX.Y.Z-rc.N`; CI builds, publishes the pre-release and the Beta site; a board
   installed from the site, then the others pull it in dev mode.

## What paid off

- Researching before building: the report's eight risks became the first measurements (ARCHITECTURE.md, "Radio
  measurements") instead of surprises.
- Mock-ups of the screens before writing them (the user's rule from MeteoBus).

## What cost time

- To fill after the first evaluation.

## Design rules that emerged

- State, not events: every advertisement carries the whole state (colour, holding, version, flags) and a sequence
  number; a lost packet costs one interval, never a stuck colour.
- The UI only sets a flag; the mesh task does the radio (espforge L164). Nothing is sent from a touch callback.
- Release is explicit (`holding = 0` in every event after the finger lifts); expiry is only for a dead peer.
- A board never serves or pulls a firmware while its own image is pending verification.

## Open threads

- The eight risks of RESEARCH-BLE.md are measured on three boards only; ten boards (24 % per-event loss at 20 ms in
  the model) are untested.
- Advertising interval fixed at 20 ms; the "20 ms burst, then 100 ms" scheme for six boards and more is designed,
  not built.
- forge_net linked but never started (espforge backlog: `web_image_t` out of forge_net).
- Effects (intensity, patterns, a shared clock) have 14 reserved bytes in the payload and no code.

## 2026-10-31: first three-board run

Board A (b8:1f:3f:c8:75:9a, COM5) on v0.1.0-rc.0, boards B (44:bd:8d:60:3c:76) and C (44:bd:8d:60:82:1a) flashed
by cable with v0.1.0-fw.1 and powered from chargers. A heard both within 2.5 s of boot; holds on B and C arrived
and mixed (blue + chartreuse = #80ffff) within 1 ms of the report; 61 reports/s of the 100/s sent. Forced pull
`devmode on` + `fwpull 76` on A: linked in 0.4 s, 1,124,336 bytes in 51.0 s (21.4 KB/s, 0 resent), restart on
ota_1, confirmed after 60 s. Tagged v0.1.0-rc.1. Open: pull rate (check PHY/DLE on the sender), a swipe sends
a short hold, scan 50/50 ms not tried.

## 2026-10-31: the pull rate, from the sender's side

Board A on the PC as the sender (test builds rc.3 to rc.5), B and C on chargers pulling. The link was never the
problem: `fw: link` lines showed 2M PHY, data length 251, 7.5 ms, MTU 512, all granted, and still 22.5 KB/s with the
sender spending 48 of 49 s waiting for mbufs: the controller sent one packet per connection event while the flood's
continuous scan was open. A `fwtune probe=200` stop (the sender aborts after 200 KB, the dev peer retries 30 s later)
let eight settings be compared in six minutes without re-flashing B or C: the scan window during the transfer is
the only knob that matters (100 ms 22 KB/s, 50 ms 40, 30 ms 52, none 75); advertising, the window, the event
length and the order of the link procedures do nothing. Shipped: 30 ms of scan per 100 ms while serving.
Full pulls of the rc.5 image (1,136,224 bytes) read on the sender: to B in 20.9 s (52.9 KB/s, stream 53.4, 0 resent, window wait 156 ms), to C in 22.1 s (50.1 KB/s, 0 resent), with `mesh: peer` lines from the third board arriving during both transfers; C's first attempt lost the link at 676 KB (the peer dropped out, cause not visible from A) and succeeded 30 s later. Labels rc.2 to rc.4 were used up by test builds the boards pulled (rc.2 by another agent's display
work, rc.3 and rc.4 by two pulls that completed before the probe knob was set), so the release candidate is rc.5.

## 2026-10-31, evening: rc.5 to rc.7

Tear-free display (TE on GPIO13, full PSRAM frame, pushes right after the tear pulse; rc.2 test label, in rc.5+),
dev mode on by default, pull rate 22 -> 52 KB/s (the sender scans 30 ms of 100 while serving; rc.5), the colour ring as
the pager's first page (swipe right from main; rc.6), a swipe no longer sends a hold (drag-start hook in a local
forge_lvgl copy; rc.7). rc.7 flashed on A by USB; B and C pulled it over Bluetooth 3 s after A's boot, 21 s each at
52 KB/s, 0 resent. Harness 15/15.

## 2026-11-01: waves, v0.2.0-rc.1

Bicolour waves from mockups (centre rings; bands from left, right, top, bottom; colour B = opposite hue; per-pixel
merge; a hold adds). Two agents: the pure-C renderer with 82 host checks (6 instructions per pixel for centre waves)
and the board side (tap zones, effect bytes 17-18 in the advert, 33 ms raw-frame loop: 29 fps with one wave, 21 fps
with five). First peer test: A received waves from both peers but sat on the ring page after its restart, so it drew
nothing; boot now goes to main when a colour is saved. v0.2.0-rc.1 flashed on A, pulled by B and C in 21 s each; the
user confirmed waves show and merge on every board.

## 2026-11-01: v0.2.0 stable, v0.3.0-rc.1 (wave colour B)

v0.2.0 tagged (waves, boot to main, swipes at 28 px, the installer page's gesture guide). Then the optional wave
colour B: a long press on a ring swatch (dot mark), `color2` on the console and the USB card, bytes 19-21 of the
advert; host tests mesh 923 / wave 95; harness 18/18. On the boards as v0.3.0-rc.1, confirmed by the user.

## 2026-11-01: paired admin board, v0.4.0-rc.1/rc.2

The user chose a paired admin over signed releases. A = admin (`admin on`, key wmg5gDSU), B and C paired over USB
(`trust <key>`, `devmode off`). A test label `v0.4.0-rc.1-1-gsigned` was NOT taken: the version order reads
`-rc.N-M-gHASH` as rc.N (not newer). With `v0.4.0-rc.2` on A: both boards targeted A within 4 s of its boot,
`fw: proof sent ... accepted (sign 108 ms)` for each, both pulled in 22 s at 52 KB/s, 0 resent. Security review by
the coordinator: nonce single use (constant-time compare), deterministic ECDSA, START gated on a verified proof
with the signed length and digest, the image's own version must equal the signed one, bytes written hashed and
compared before esp_ota_end. Lesson: a post-release test label only ranks above a release (`vX.Y.Z-N-g...`), never
above an rc; use the next rc number for signed-pull tests.

## 2026-11-01: the atlas, levelled diagrams of the whole project

The user asked for a thorough documentation in the zoomable style of aws-delocalized-backups: `docs/DIAGRAMS.md`,
21 Mermaid diagrams in levels (the whole system; one board, its modules, screens and tasks; the mesh: the 20 ms
flood, the 30 bytes on air, the peer table and the mix, effects, the voice train; updates: who pulls from whom,
the firmware link packet by packet, boot and rollback, the paired admin board, version order; the PC side; a tag
to the boards), each section with one diagram and a "Zoom in" list naming boxes by id; `docs/diagrams.html`, the
explorer copied from that project (pan, zoom, click-to-zoom-in, breadcrumbs, a map), reads the file as is;
`tools/check_diagrams.py` keeps the levels navigable (one diagram per section, real boxes and sections, every
level reachable and on the map, no `;` in a sequence message, no Mermaid keyword as a node id) and CI's host-tests
job runs it. All 21 diagrams render-checked in the browser.
