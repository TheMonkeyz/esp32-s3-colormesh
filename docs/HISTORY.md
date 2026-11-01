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
