# Architecture diagrams

Pictures of how ColorMesh fits together, in levels. Start with **The whole system**; under each diagram, **Zoom
in** lists the boxes that open into a diagram of their own. The prose version of the firmware is
`docs/ARCHITECTURE.md`, the research that chose the radio design `docs/RESEARCH-BLE.md`, what the PC tools read
from the firmware `docs/PROTOCOL.md`, and how a tag becomes a release `docs/RELEASING.md`.

**Interactive version:** `docs/diagrams.html` shows the same diagrams with pan, zoom and click-to-zoom-in. It
reads this file, so the two never disagree. Open it through any web server, for example
`python -m http.server -d docs 8000`, then <http://localhost:8000/diagrams.html> (or the preview config
`diagrams` in `.claude/launch.json`).

Map of the levels:
- [The whole system](#the-whole-system)
  - [One board](#one-board)
    - [The firmware, module by module](#the-firmware-module-by-module)
      - [The screens](#the-screens)
        - [From a finger to every screen](#from-a-finger-to-every-screen)
    - [Tasks and what they own](#tasks-and-what-they-own)
  - [The Bluetooth mesh](#the-bluetooth-mesh)
    - [The flood: one advertisement, every 20 ms](#the-flood-one-advertisement-every-20-ms)
      - [The 30 bytes on air](#the-30-bytes-on-air)
      - [The peer table and the mix](#the-peer-table-and-the-mix)
    - [Waves, calls and the game: effects in the flood](#waves-calls-and-the-game-effects-in-the-flood)
    - [Voice clips: the periodic train](#voice-clips-the-periodic-train)
  - [Updates](#updates)
    - [The dev-mode pull over Bluetooth](#the-dev-mode-pull-over-bluetooth)
      - [Who pulls from whom](#who-pulls-from-whom)
      - [The firmware link, packet by packet](#the-firmware-link-packet-by-packet)
      - [Boot, confirm or roll back](#boot-confirm-or-roll-back)
    - [Secure updates: the paired admin board](#secure-updates-the-paired-admin-board)
    - [Versions and their order](#versions-and-their-order)
  - [The PC side: USB, console and flash helper](#the-pc-side-usb-console-and-flash-helper)
  - [From a tag to the boards](#from-a-tag-to-the-boards)

Rules for editing (checked by `python tools/check_diagrams.py`, run by CI's host-tests job): one Mermaid diagram
per section; each **Zoom in** line names a box by its id in that diagram and links to a section heading; every
section can be reached from the top and is on the map above.

## The whole system

Three small round screens share colours over Bluetooth LE. No Wi-Fi, no phone, no app: every board shouts its
own state on the air every 20 ms and listens to the others all the time. A person installs and configures a board
over USB from a web page; after that, a board in dev mode takes a newer firmware from a nearby board by itself.

```mermaid
flowchart LR
  person(("Person<br/>fingers on the screens"))
  dev(("Developer<br/>Claude Code, the PC"))
  boards["The boards<br/>Waveshare ESP32-S3-Touch-AMOLED-1.75, three of them<br/>colour, hold, waves, voice, a game"]
  mesh["The Bluetooth mesh<br/>a flood of advertisements (state)<br/>a periodic train (voice)<br/>one connection on demand (firmware)"]
  updates["Updates<br/>USB from the web page, or a dev-mode pull<br/>from a peer over Bluetooth"]
  pc["The PC side<br/>the Pages site over Web Serial,<br/>the USB test console, the flash helper"]
  release["GitHub<br/>a tag → CI → a release → the Pages site"]

  person -- "tap, hold, swipe, talk" --> boards
  boards <-- "every 20 ms" --> mesh
  mesh -- "a newer image" --> updates --> boards
  pc -- "USB" --> boards
  dev --> pc
  dev -- "git tag" --> release -- "Install from the browser" --> pc
```

**Zoom in:**
- The boards (`boards`) → [One board](#one-board)
- The Bluetooth mesh (`mesh`) → [The Bluetooth mesh](#the-bluetooth-mesh)
- Updates (`updates`) → [Updates](#updates)
- The PC side (`pc`) → [The PC side: USB, console and flash helper](#the-pc-side-usb-console-and-flash-helper)
- GitHub (`release`) → [From a tag to the boards](#from-a-tag-to-the-boards)

## One board

One Waveshare ESP32-S3-Touch-AMOLED-1.75: an ESP32-S3 with 16 MB of flash and 8 MB of PSRAM, a 466×466 round
AMOLED, a touch controller, two microphones and a speaker, and USB serial/JTAG. The firmware is an ESP-IDF app
built on espforge (board support, LVGL pager, test console, diagnostics). The one 2.4 GHz radio runs Bluetooth LE
only: Wi-Fi is never started.

```mermaid
flowchart TB
  subgraph hw["Hardware"]
    mcu["ESP32-S3<br/>16 MB flash · 8 MB PSRAM<br/>one radio: BLE 5.0, no Wi-Fi"]
    panel["CO5300 AMOLED<br/>466×466 round, QSPI, RGB565<br/>tear signal on GPIO13"]
    touch["CST9217 touch<br/>I2C 0x5A"]
    audio["ES7210 microphones<br/>ES8311 speaker (amp on GPIO46)"]
    usb["USB serial/JTAG<br/>console · flash · Web Serial"]
  end
  subgraph fw["Firmware (ESP-IDF v5.5.4)"]
    app["main/: the app<br/>screens, config, mesh, firmware pull,<br/>voice, game, console"]
    forge["espforge components<br/>board · forge_lvgl (pager, slides) ·<br/>forge_core (testcon, diag, version)"]
    nimble["NimBLE<br/>host + controller, core 0"]
    tasks["Tasks<br/>LVGL · mesh · fw · clip · console"]
  end
  flash[("Flash layout<br/>ota_0 · ota_1 (3 MB each) · otadata ·<br/>NVS (settings, keys) · coredump")]

  mcu --- panel & touch & audio & usb
  app --> forge & nimble
  app --- tasks
  forge --> panel & touch & usb
  nimble --> mcu
  app --> flash
```

**Zoom in:**
- The app (`app`) → [The firmware, module by module](#the-firmware-module-by-module)
- Tasks (`tasks`) → [Tasks and what they own](#tasks-and-what-they-own)
- Flash layout (`flash`) → [Boot, confirm or roll back](#boot-confirm-or-roll-back)

## The firmware, module by module

Every file in `main/`, in groups. The pure C files (no ESP-IDF, time always an argument) are the ones the host
tests build with gcc and AddressSanitizer in `tests/host/`: the wire format, the peer table and the mix, the
waves, the game rules, the signed-pull messages, the clip chunks, the call state. An arrow means "calls".

```mermaid
flowchart TB
  main["main.c<br/>boot order: NVS, diag, board, config,<br/>ui, mesh, clip, console; app ready"]
  ui["ui.c<br/>the pager's five pages, the overlay,<br/>the hold flag, the wave loop"]
  config["config.c<br/>NVS: colour, colour B, dev mode, name,<br/>brightness, admin flag and keys"]
  console["console.c<br/>USB commands: cfg, color, devmode,<br/>peers, hold, fwpull, wave, game, admin, trust"]
  mesh["mesh.c<br/>NimBLE: advertising, scanning, the mesh<br/>task, waves, game and clip hooks"]
  proto["mesh_proto.c (pure C)<br/>30-byte payload, peer table,<br/>expiry, the additive mix, versions"]
  fw["mesh_fw.c<br/>the firmware pull: GATT service, sender,<br/>receiver, ECDSA, confirm after 60 s"]
  auth["fw_auth.c (pure C)<br/>challenge, proof, verdict, nonce,<br/>base64"]
  clip["clip.c<br/>record, send on the periodic train,<br/>sync and play, ring, calls"]
  pure["adpcm.c · clip_chunks.c · ringtone.c ·<br/>calls.c · layer.c (pure C)"]
  wave["wave.c (pure C)<br/>bicolour waves into RGB565 rows"]
  rps["rps.c (pure C)<br/>rock paper scissors rounds"]
  text["app_text.c · i18n_strings.h<br/>English and Québec French"]

  main --> ui & config & mesh & clip & console
  ui --> mesh & clip & wave & rps & text
  ui --> pure
  console --> config & mesh & fw & clip
  mesh --> proto & fw & rps & wave
  fw --> auth & proto & config
  clip --> mesh & pure & config
```

**Zoom in:**
- ui.c (`ui`) → [The screens](#the-screens)
- mesh.c (`mesh`) → [The flood: one advertisement, every 20 ms](#the-flood-one-advertisement-every-20-ms)
- mesh_proto.c (`proto`) → [The 30 bytes on air](#the-30-bytes-on-air)
- mesh_fw.c (`fw`) → [The dev-mode pull over Bluetooth](#the-dev-mode-pull-over-bluetooth)
- fw_auth.c (`auth`) → [Secure updates: the paired admin board](#secure-updates-the-paired-admin-board)
- clip.c (`clip`) → [Voice clips: the periodic train](#voice-clips-the-periodic-train)

## The screens

One LVGL pager with five pages, swiped left and right. Boot shows **main** when a colour is saved, else **pick**.
Every page but system shows the mesh behind it: the mix as a disc, and the waves, drawn by a 33 ms raw-frame loop
straight to the panel. An overlay on the top layer takes the screen for an incoming clip or call.

```mermaid
flowchart LR
  pick["pick<br/>12 swatches on a ring<br/>tap: your colour (saved)<br/>hold a swatch: wave colour B"]
  main["main<br/>edge ring = your colour<br/>centre = the mix<br/>a dot per board · N near · vX.Y.Z<br/>hold anywhere: send · tap: a wave"]
  voice["voice<br/>hold to talk (5 s max)<br/>the green Call button"]
  game["game<br/>tap: start a round<br/>3 s countdown, tap your pick<br/>result for 4 s"]
  system["system<br/>version, address, memory,<br/>uptime, the peers, dev mode"]
  overlay["overlay (top layer)<br/>Message · Incoming call ·<br/>Calling… · Answered"]
  loop["wave loop (33 ms)<br/>wave_fill → display_raw_frame<br/>the page's pixels layered over it"]

  pick <-- "swipe" --> main <-- "swipe" --> voice <-- "swipe" --> game <-- "swipe" --> system
  overlay -. "over any page" .-> main
  loop -. "behind pick, main,<br/>voice, game" .-> main
```

**Zoom in:**
- main (`main`) → [From a finger to every screen](#from-a-finger-to-every-screen)
- wave loop (`loop`) → [Waves, calls and the game: effects in the flood](#waves-calls-and-the-game-effects-in-the-flood)
- overlay (`overlay`) → [Voice clips: the periodic train](#voice-clips-the-periodic-train)

## From a finger to every screen

What happens when you hold the main screen, and why it reaches the other boards within about one 20 ms
advertising event. The touch handler only sets a flag. The mesh task re-encodes the advertisement in place. Every
other board's scan callback copies the 30 bytes into a queue, and its mesh task updates the table and the mix.
Release is explicit: every advertisement after the finger lifts says "not holding", so nothing sticks.

```mermaid
sequenceDiagram
  participant F as Finger (board A)
  participant UI as ui.c (LVGL task, A)
  participant M as mesh task (A)
  participant R as radio (A → air)
  participant CB as scan callback (B, host task)
  participant MB as mesh task (B)
  participant SB as screen (B)

  F->>UI: press on main
  UI->>M: mesh_set_holding(true), a flag + wake
  M->>M: seq++, flags holding, mp_encode
  M->>R: ble_gap_ext_adv_set_data (in place, no restart)
  loop every 20 ms, forever
    R-->>CB: ADV_NONCONN_IND, 30-byte manufacturer AD
  end
  CB->>MB: queue: addr, rssi, 28 bytes (microseconds, no log)
  MB->>MB: mp_table_update, mp_mix (linear light, normalised)
  MB->>SB: "mesh: peer … hold", "mesh: mix #rrggbb n=2"
  SB->>SB: centre disc = the mix (read under the display lock)
  F->>UI: release (or a drag starts)
  UI->>M: mesh_set_holding(false)
  M->>R: seq++, holding off
  R-->>CB: next event: holding = 0
  MB->>SB: the colour leaves the mix within one interval
```

Measured on three boards: a peer's hold reaches the mix 0-1 ms after its report; a missed event costs 20 ms;
about 61 % of events are heard at the 100/100 ms scan.

## Tasks and what they own

Who may call what. The rule from espforge's lesson L164: the UI only sets flags; the mesh task does the radio;
nothing is sent from a touch callback. The NimBLE host task only copies and posts. Every NimBLE procedure, flash
write and log line of the firmware pull is in the fw task.

```mermaid
flowchart LR
  lvgl["LVGL task (core 1)<br/>drawing, touch, the wave loop<br/>reads the mix under display_lock"]
  host["NimBLE host + controller (core 0)<br/>scan callback: copy 30 bytes to a queue<br/>GATT callbacks: copy and post"]
  meshT["mesh task (core 0, prio 5, 4 KB)<br/>peer table · expiry tick 50 ms · the mix ·<br/>advertising changes · 'mesh:' lines"]
  fwT["fw task (core 0, prio 4, 6 KB)<br/>connect, link set-up, the stream,<br/>esp_ota_write, ECDSA, confirm timer"]
  clipT["clip task (core 0, prio 4, 4 KB)<br/>I2S reads, ADPCM, the periodic train,<br/>sync, speaker writes, the ring"]
  con["console task (forge_core)<br/>one command per line; shot in its own task"]
  q[("queues and flags<br/>report queue 32 · fw events · clip events ·<br/>holding, refresh_req, effect_req")]

  lvgl -- "flags + wake" --> q
  host -- "reports, GAP events" --> q
  q --> meshT & fwT & clipT
  meshT -- "ble_gap_* under radio_lock" --> host
  fwT -- "gattc writes, notifies" --> host
  clipT -- "periodic adv data, sync create" --> host
  con -- "posts, never NimBLE directly" --> q
```

## The Bluetooth mesh

Three ways the radio is used, all from one NimBLE stack. The flood carries every board's state and is enough for
colours, holds, waves, calls and the game. Voice needs more bytes per second than a 31-byte advertisement gives,
so a clip rides a BLE 5 periodic advertising train that every listener syncs to: one send, every board. A firmware
image needs a reliable stream, so that one case opens a GATT connection between two boards. The research report
rejected SIG mesh (8-byte vendor payload, ~90 ms per message, a provisioner), GATT meshes (an election at ten
boards) and ESP-NOW (Wi-Fi).

```mermaid
flowchart TB
  flood["The flood<br/>legacy ADV_NONCONN_IND every 20 ms, 30 bytes:<br/>colour · holding · dev · version · target ·<br/>effect · colour B · game · clip id"]
  scan["Continuous passive scan<br/>100 ms window = interval, no duplicate filter<br/>every ColorMesh report → the mesh task"]
  train["The periodic train (voice)<br/>extended set, SID 1, 100 ms; periodic data every 10 ms,<br/>2M PHY, 244-byte chunks, 3 rounds"]
  sync["Periodic sync (voice)<br/>a listener syncs to the sender's train,<br/>collects the chunks, plays at once"]
  link["One GATT connection (firmware)<br/>central = the serving board, peripheral = the puller<br/>2M PHY · DLE 251 · MTU 512 · 7.5 ms"]
  table["Peer table + mix<br/>16 entries by address, self included<br/>hold expiry 250 ms, peer expiry 10 s"]
  effects["Effects in the flood<br/>waves (bytes 17-21), calls and answers (17-18, 28),<br/>the game (22-25): state, not events"]

  flood -- "heard by every board" --> scan --> table
  flood -- "effect bytes" --> effects
  flood -- "MP_F_CLIP + clip id" --> sync
  train --> sync
  flood -- "dev + target = a peer" --> link
```

**Zoom in:**
- The flood (`flood`) → [The flood: one advertisement, every 20 ms](#the-flood-one-advertisement-every-20-ms)
- Peer table + mix (`table`) → [The peer table and the mix](#the-peer-table-and-the-mix)
- Effects in the flood (`effects`) → [Waves, calls and the game: effects in the flood](#waves-calls-and-the-game-effects-in-the-flood)
- The periodic train (`train`) → [Voice clips: the periodic train](#voice-clips-the-periodic-train)
- One GATT connection (`link`) → [The dev-mode pull over Bluetooth](#the-dev-mode-pull-over-bluetooth)

## The flood: one advertisement, every 20 ms

How `mesh.c` keeps the state on the air. Since v0.6.0 the stack runs with extended advertising on: the flood is
extended advertising instance 0 with legacy PDUs, so on air nothing changed and older boards still hear it. The
advertisement is non-connectable in normal use (nobody can connect to a board); it turns connectable only while
the board wants firmware. A state change bumps `seq` and rewrites the data in place, no stop/start. The research
report's 20 ms is the NimBLE floor; the controller accepted it (risk 1), with a 100 ms fallback coded in.

```mermaid
flowchart LR
  state["Own state<br/>colour, holding, dev, admin, servable,<br/>pending, version, target, effect,<br/>colour B, game bytes, clip id"]
  refresh["do_refresh (mesh task)<br/>seq++, mp_table_set_self,<br/>mp_encode → 30 bytes"]
  inst0["Advertising instance 0<br/>legacy PDUs, 20 ms (0x20)<br/>non-connectable, or connectable while pulling"]
  air(("the air"))
  disc["BLE_GAP_EVENT_EXT_DISC (host task)<br/>legacy reports only; find the manufacturer AD;<br/>length 28, protocol 0x43: copy, enqueue"]
  queue[("report queue<br/>32 entries; never more than 1 used at 75 reports/s")]
  task["mesh task<br/>drain the queue, decode, mp_table_update,<br/>effect / game / clip hooks, then mix"]
  tick["50 ms tick<br/>expire holders and peers, end effects,<br/>game bytes, wave end, radio_check"]

  state -- "any setter: flag + wake" --> refresh --> inst0 --> air
  air --> disc --> queue --> task
  tick --> task
```

Rate limits measured on board A: 71-75 reports/s from ~13 neighbours, the GAP callback at most 67 µs, 0 drops. The
first build heard neighbours in bursts because `CONFIG_BT_NIMBLE_HOST_QUEUE_CONG_CHECK` passed each address once
per ~200 reports; it is off (risk 2).

**Zoom in:**
- Own state (`state`) → [The 30 bytes on air](#the-30-bytes-on-air)
- mesh task (`task`) → [The peer table and the mix](#the-peer-table-and-the-mix)

## The 30 bytes on air

One manufacturer-data AD structure fills the whole 31-byte legacy payload (no Flags AD: with non-discoverable
mode every flag is zero, so the spec allows leaving it out). The company id is `0xFFFF`; byte 4, the protocol id
`0x43`, is what really guards it. The origin id is the low byte of the board's public address. Every field is
state, never an event: a lost packet costs one interval, never a stuck colour.

```mermaid
flowchart TB
  hdr["0-4 · header<br/>AD length 29 · type 0xFF · company 0xFFFF · protocol 0x43"]
  id["5-7 · identity<br/>origin (low address byte) · seq (bumped on a state change, wrap-aware) · TTL (0 = never relay)"]
  flags["8 · flags<br/>bit0 holding · bit1 dev · bit2 servable · bit3 pending verify · bit4 admin · bit5 clip on the train"]
  colour["9-11 · R G B<br/>this board's colour"]
  ver["12-15 · version<br/>major · minor · patch · kind byte (test label 0-99, rc.N 100+N, release 200, post-release 201+N)"]
  target["16 · target<br/>the origin this board wants firmware from (0xFF = none)"]
  effect["17-18 · effect<br/>kind (1-5 waves, 6 ring, 7 answer) · effect seq (new per effect, held 1 s)"]
  b2["19-21 · colour B<br/>the wave's second colour (0,0,0 = automatic: the opposite hue)"]
  game["22-25 · game<br/>round seq · starter origin · time left to the reveal in 100 ms steps · pick"]
  clip["26-27 · clip<br/>clip id · length in 100 ms steps (with flag bit5)"]
  ans["28-29 · answer<br/>with effect 7: the caller's origin · reserved 0"]

  hdr --> id --> flags --> colour --> ver --> target --> effect --> b2 --> game --> clip --> ans
```

The game bytes and the clip bytes are stored but not part of "changed" in `mp_table_update`: the time left moves
every 100 ms and must not log a peer line each time.

## The peer table and the mix

`mesh_proto.c`, pure C and host-tested. The table has 16 entries keyed by address; a full table replaces the
least recently heard peer. A strictly older `seq` is ignored unless three come in a row (the peer rebooted). Self
is one more entry, so the holder sees its own colour in its mix too (a board never hears its own advertisements).
The mix adds the holders' colours in linear light and scales so the largest channel is full: red and blue make
magenta, all three make white, and nothing looks dim or drifts to white.

```mermaid
flowchart LR
  report["one report<br/>addr, rssi, 28 bytes"]
  update["mp_table_update<br/>new peer, or newer seq: store<br/>changed = colour, colour B, flags,<br/>version, target or effect differ"]
  table[("16 peers + self<br/>state, rssi, last heard, stale count")]
  expire["mp_table_expire (every 50 ms)<br/>holder silent 250 ms: holding off<br/>peer silent 10 s: gone"]
  mix["mp_mix<br/>sum the holders in linear light (gamma 2.2),<br/>scale so max channel = 65535, back to sRGB"]
  out["mesh: peer … hold|idle<br/>mesh: mix #rrggbb n=holders<br/>mix_packed for the UI"]

  report --> update --> table
  expire --> table
  table --> mix --> out
```

## Waves, calls and the game: effects in the flood

Three features ride on the same bytes. A **wave** (a tap on main) starts here at once and puts effect = kind with
a new effect seq in the advert for 1 s; a peer whose (effect, seq) is new starts that wave at its own clock, in
the sender's colours. A **call** is effect 6 the same way; a tap on the ringing board sends effect 7 with the
caller's origin in byte 28, and only the caller reacts. A **game round** is bytes 22-25: a tap starts a round with
the reveal 3 s away; a board that hears a round it does not know, with time left, joins it with the same reveal;
two rounds at once, the lower starter origin wins everywhere. 600 ms after the reveal the picks heard decide.

```mermaid
flowchart TB
  tap["tap on main<br/>centre: rings outward<br/>edge: a band from that side"]
  wave["mesh_start_wave<br/>own wave now; effect 1-5 + seq for 1 s"]
  peerw["peer_effect (receiver)<br/>new (effect, seq) from this address:<br/>wave_add at heard time, its colour A and B"]
  fill["wave_fill (33 ms loop)<br/>phase = (distance − 160 px/s · t) mod 64 px<br/>4 s, 500 ms fade; waves summed per pixel<br/>in linear light; holders' mix added"]
  ringbtn["Call button<br/>effect 6 + seq for 1 s"]
  ringp["every other board<br/>rings 3 phrases, 'Incoming call'<br/>tap: effect 7, byte 28 = caller"]
  caller["the caller<br/>'Calling…' → 'Answered', one dot and<br/>a doo-di-dit per board, ring + 2 s"]
  gtap["tap on game<br/>rps_start: round (origin, seq),<br/>reveal in 3000 ms"]
  gjoin["rps_join (receiver)<br/>unknown round with left > 0: join,<br/>reveal = heard time + left"]
  gres["rps_outcome (600 ms after)<br/>one kind or three: draw<br/>two kinds: the winner wins<br/>no own pick: late"]

  tap --> wave --> peerw --> fill
  ringbtn --> ringp --> caller
  gtap --> gjoin --> gres
```

Measured: one wave 29 fps, five waves 21 fps on the raw-frame loop; a round started on B was joined by A with
3000 ms left and both showed the result 3.6 s later.

## Voice clips: the periodic train

True multicast: one send reaches every board at once, not one link per board. Hold the voice page and talk (up to
5 s at 16 kHz, both microphones averaged, DC removed, a quiet clip scaled up). IMA ADPCM makes 5 s into 40,000
bytes. The sender starts extended advertising instance 1 (SID 1, 100 ms, 2M secondary PHY) with a periodic train
every 10 ms, and swaps the periodic data every 10 ms: one 224-byte chunk per packet, 3 rounds over all chunks, so
5 s of voice takes 5.4 s on air for any number of listeners. Meanwhile the flood advert carries flag bit5 and the
clip id. A board that hears a new clip id from that address syncs to the train, fills a bitmap of chunks, and plays
as soon as every chunk is in and the CRC-32 matches. A lost train, no sync within 4 s or a bad CRC drops it. One
hop only.

```mermaid
sequenceDiagram
  participant F as Finger (A)
  participant C as clip task (A)
  participant M as mesh (A)
  participant Air as the air
  participant MB as mesh task (B)
  participant HB as host task (B)
  participant CB as clip task (B)

  F->>C: hold the voice page, talk, release
  C->>C: level, IMA ADPCM 4:1, CRC-32, chunks of 224 bytes
  C->>M: mesh_clip_adv_start(10 ms), mesh_set_clip(id, on)
  M-->>Air: flood: MP_F_CLIP, clip id, length
  loop 3 rounds × every chunk, one every 10 ms
    C->>M: mesh_clip_adv_data(chunk)
    M-->>Air: AUX_SYNC_IND: 'V' · origin · id · chunk · total · len · crc · data
  end
  Air-->>MB: flood report with a new clip id
  MB->>CB: clip hook: addr, origin, id
  CB->>HB: ble_gap_periodic_adv_sync_create(addr, SID 1)
  Air-->>HB: PERIODIC_REPORT, one per packet
  HB->>HB: clip_parse, clip_rx_add (bitmap, under rx_lock)
  HB->>CB: every chunk in, CRC ok
  CB->>CB: terminate the sync, decode, 250 ms silence, play
  C->>M: mesh_set_clip(off), mesh_clip_adv_stop
```

Verified on three boards: B's 5 s clip reached A whole (179 chunks, 0 holes) and C. The flood stayed at 58-60
ColorMesh reports/s with extended advertising on (62 before).

## Updates

Three ways a new firmware gets onto a board, and one way back. No board ever polls the web site: the site serves
people, not devices. Every image carries its version and an appended SHA-256; the bootloader keeps two app slots
and rolls back a new image that does not confirm itself within 60 s of normal running.

```mermaid
flowchart LR
  site["The Pages site, over USB<br/>esp-web-tools installs the release's parts<br/>at their offsets; NVS (settings) kept<br/>Stable or Beta"]
  esptool["esptool, over USB<br/>the full merged image at 0x0<br/>erases NVS: the board asks for a colour again"]
  pull["The dev-mode pull, over Bluetooth<br/>a board in dev mode takes a strictly newer<br/>servable image from a peer (the app part only)"]
  signed["Secure updates<br/>a board out of dev mode takes an image only<br/>from the paired admin board, after a signed proof"]
  slots["The two OTA slots<br/>write the other slot, set it to boot,<br/>restart; PENDING_VERIFY → confirm after 60 s<br/>or the bootloader rolls back"]
  versions["Version order<br/>-x.N < -rc.N < release < -N-gHASH<br/>same project name, strictly newer"]

  site & esptool --> slots
  pull --> slots
  signed --> pull
  versions -. "gates the pull" .-> pull
```

**Zoom in:**
- The dev-mode pull (`pull`) → [The dev-mode pull over Bluetooth](#the-dev-mode-pull-over-bluetooth)
- Secure updates (`signed`) → [Secure updates: the paired admin board](#secure-updates-the-paired-admin-board)
- The two OTA slots (`slots`) → [Boot, confirm or roll back](#boot-confirm-or-roll-back)
- Version order (`versions`) → [Versions and their order](#versions-and-their-order)
- The Pages site (`site`) → [From a tag to the boards](#from-a-tag-to-the-boards)

## The dev-mode pull over Bluetooth

`mesh_fw.c`. The puller decides, the server obeys. Every advertisement carries the version and a servable bit
(only while the running image is valid). A dev-mode board that sees a strictly newer servable version picks that
peer (highest version, then lowest address), advertises **connectable** with `target` = that peer's origin, and
waits up to 60 s. Only the named peer reacts: it connects as central, sets the link up one procedure at a time,
discovers the three characteristics, and streams its own running image from a flash mmap. Then the receiver
verifies, sets the boot slot and restarts. One pull at a time; a failed pull waits 30 s.

```mermaid
flowchart TB
  seeA["Puller (dev mode)<br/>rx_poll every 1 s: a servable peer with a<br/>strictly newer version, same project?"]
  want["R_WANT<br/>advert: dev bit + target = peer's origin,<br/>connectable; 60 s to be connected"]
  seeB["Server<br/>tx_poll every 1 s: a peer whose target is me?<br/>(and I am servable, idle, synced)"]
  conn["Server connects as central<br/>scan paused around ble_gap_connect<br/>7.5 ms interval, supervision 6 s"]
  linked["R_LINKED (puller)<br/>only the target is accepted; advertising paused,<br/>scan paused: the radio is the link's"]
  setup["Link set-up, one procedure at a time<br/>data length 251 → 2M PHY → interval → MTU 512<br/>'fw: link …' on both boards"]
  stream["The stream<br/>START, then 4 KB sectors as 509-byte<br/>Write Without Response packets, ACK per sector"]
  done["esp_ota_end verifies the appended SHA-256,<br/>boot slot set, 'fw: pulled … restarting'"]

  seeA --> want --> seeB --> conn --> linked --> setup --> stream --> done
```

Measured: 1.1 MB in about 21 s at 52 KB/s, 0 resent, since rc.5. The link was never the limit (2M PHY, DLE 251,
MTU 512 all granted at 22 KB/s): the controller gave the connection one packet per event while the flood's
continuous scan was open. The fix is in the server: it scans 30 ms of every 100 ms while serving, and the puller
pauses its scan. `fwtune` changes these knobs in RAM and can stop a transfer after N KB to compare settings.

**Zoom in:**
- Puller (`seeA`) → [Who pulls from whom](#who-pulls-from-whom)
- The stream (`stream`) → [The firmware link, packet by packet](#the-firmware-link-packet-by-packet)
- esp_ota_end (`done`) → [Boot, confirm or roll back](#boot-confirm-or-roll-back)

## Who pulls from whom

The receiver's rule, run every second in the fw task. The same rule, with the admin bit, serves boards out of dev
mode. A version that rolled back on this board is never pulled again. `fwpull <addr>` on the console forces a
target and skips the version checks (dev mode only).

```mermaid
flowchart TB
  start(["rx_poll, every 1 s, state idle"])
  pend{"own image pending verify,<br/>host not synced, or in<br/>the 30 s back-off?"}
  mode{"dev mode?"}
  trusted{"a trusted admin key?"}
  cand["candidates: peers that are servable<br/>and strictly newer, same project,<br/>not the version that rolled back here"]
  adminf["…and advertise the admin bit"]
  best["best = highest version,<br/>then lowest address"]
  none(["nothing to do"])
  want(["R_WANT: target = best,<br/>connectable advert"])

  start --> pend
  pend -- "yes" --> none
  pend -- "no" --> mode
  mode -- "yes" --> cand
  mode -- "no" --> trusted
  trusted -- "no" --> none
  trusted -- "yes" --> adminf --> best
  cand --> best
  best -- "none" --> none
  best -- "one" --> want
```

A pull also needs the server to agree: it serves only a peer whose advertised `target` is its own origin, and a
peer out of dev mode only if it is the admin.

## The firmware link, packet by packet

The GATT service every board carries (custom 128-bit UUIDs `6c6d000N-8e6b-4c1b-9a35-2d5e7f0c4b31`), used only
while pulling. The server writes; the receiver answers with notifications on ACK, which the server reads without
any subscription. Framing is Espressif's ble_ota style without the component: a sector is packets seq 0, 1, …
then one seq 0xFF packet with the rest and the sector's CRC16. The server keeps 4 sectors in flight beyond the
last ACK; the receiver buffers 8 sectors in a PSRAM ring and a write task feeds them to `esp_ota_write`. A bad
CRC asks for that sector again (go-back-N). An mbuf shortage backs off one tick, never a loss.

```mermaid
sequenceDiagram
  participant S as Server (central, fw task)
  participant R as Receiver (peripheral, host task → fw task)

  S->>R: discover service 0001, CTRL 0002, DATA 0003, ACK 0004
  Note over S,R: an admin server first proves its image here (secure updates)
  S->>R: CTRL: START = 01 · length u32 · SHA-256[32] · version[4]
  R->>R: length 4 KB to the slot size, version strictly newer (or forced), signed values match
  loop each 4 KB sector, 4 in flight
    S->>R: DATA: sector u16 · seq 0 · 506 bytes
    S->>R: DATA: sector · seq 1 · …
    S->>R: DATA: sector · seq 0xFF · the rest · CRC16
    R->>R: sector 0 checks image magic, same project_name, newer version, not the rolled-back one, then esp_ota_begin
    R->>R: ring slot → esp_ota_write (hashed too, when signed)
    R-->>S: ACK notify: sector · status 0 (ok) | 1 (CRC: resend from here) | 2 (abort)
  end
  R->>R: esp_ota_end checks magic, segments, checksum and the appended SHA-256, and the signed digest is compared
  R->>R: esp_ota_set_boot_partition
  R-->>S: ACK notify: sector 0xFFFF · status 0
  R->>S: disconnect, then esp_restart
  S->>S: logs fw served in 21 s at 52 KB/s, scan back to 100/100 ms
```

Every packet and ACK refreshes a 15 s idle timer on both sides; silence ends the pull. `fwloop` on the console
feeds this board's sender output straight into its own receiver (with one injected CRC error) into the other slot,
without Bluetooth, to test the framing and the OTA writes.

## Boot, confirm or roll back

The flash has two 3 MB app slots and an otadata record that says which one boots. A pull writes the other slot
and marks it to boot; the new image starts as PENDING_VERIFY. 60 s after start the fw task checks that the display
answers and the host is synced, then marks the image valid and turns the servable bit on. If the new image
crashes, or never confirms, the bootloader goes back to the previous slot, and that version is remembered as bad
and never pulled again. Never restart a board in its first 60 s after an update. A USB install writes
`ota_data_initial.bin`, so the board boots `ota_0` again.

```mermaid
stateDiagram-v2
  [*] --> Running : ota_0 or ota_1, image VALID (servable bit on)
  Running --> Writing : a pull starts (other slot)
  Writing --> Running : failed, aborted, or verify failed (30 s back-off)
  Writing --> Pending : esp_ota_end ok, boot slot set, restart
  Pending --> Confirmed : 60 s up, display up, host synced → mark valid
  Pending --> RolledBack : crash, or not confirmed → bootloader boots the old slot
  Confirmed --> Running : servable bit on, fw confirmed vX
  RolledBack --> Running : old image, vX rolled back and never pulled again
```

While its own image is pending, a board neither serves nor pulls.

## Secure updates: the paired admin board

For boards that should not take firmware from anyone (dev mode off). The user chose a paired admin over signed
releases. One board is made the admin (`admin on`): it keeps an ECDSA P-256 key pair in NVS and sets the admin bit
in its advertisement. Each other board stores the admin's public key once over USB (`trust <base64>`). After the
link is up and before START, the admin asks for a challenge; the receiver notifies a fresh 32-byte nonce (single
use, 10 s); the admin answers with a proof: nonce, image length, image digest, version, and a deterministic ECDSA
signature over the SHA-256 of those 100 bytes. The receiver checks the nonce, that the version is strictly newer,
and the signature with the trusted key, then accepts only a START with the signed length and digest, requires the
image's own version to equal the signed one, hashes every byte it writes and compares with the signed digest
before `esp_ota_end`. In dev mode the proof is checked when offered, never required.

```mermaid
sequenceDiagram
  participant A as Admin board (server)
  participant B as Board out of dev mode (receiver, trusted key stored)

  Note over B: flood: it targets only an admin peer with a newer servable version
  A->>B: connect, link set-up, discovery
  A->>B: CTRL 04: a challenge, please
  B->>B: esp_fill_random → nonce[32], live 10 s, one use
  B-->>A: ACK notify: sector 0xFFFE · status 3 · nonce[32]
  A->>A: message = nonce ‖ len ‖ SHA-256(image) ‖ version[32], then sign SHA-256(message) with ECDSA P-256 (RFC 6979), ~110 ms
  A->>B: CTRL 03: message[100] ‖ signature r‖s[64]
  B->>B: nonce taken (constant-time compare), version strictly newer, verify with the trusted key, ~215 ms, keys zeroed
  B-->>A: ACK notify: sector 0xFFFE · status 4 · verdict (0 ok, 1 bad signature, 2 nonce, 3 not newer, 4 malformed, 5 unchecked)
  A->>B: CTRL START with the signed length and digest
  B->>B: START must match the proof, and in sector 0 the image's version must equal the signed one
  B->>B: SHA-256 of every byte written, which must equal the signed digest at the end, then esp_ota_end
  Note over A,B: any refusal logs fw refused (no proof, bad signature, nonce, START does not match), closes the link, 30 s back-off
```

Why the admin asks instead of the receiver pushing a nonce: the server only knows the ACK handle after discovery,
and a v0.3.0 server treated any unknown ACK status as an abort. Measured on three boards: both paired boards
targeted the admin within 4 s of its boot, proof accepted, 22 s pulls at 52 KB/s. Host tests:
`tests/host/test_fw_auth.c` (81 checks); `fwloop baddigest` and `fwloop badsig` on the console exercise the two
late failures on one board.

## Versions and their order

One order, used in three places: forge_core's `version.c` (the firmware comparing full strings), CI (which
release the site offers), and the advertisement's version-kind byte (a 4-byte pre-filter before any connection).
A board pulls only a version **strictly above** its own with the same project name. Test builds are labelled with
`version.txt` above the current release and below every rc of the same X.Y.Z.

```mermaid
flowchart LR
  test["vX.Y.Z-anything.N<br/>a test label (kind 0-99)<br/>e.g. v0.6.0-fw.3"]
  rc["vX.Y.Z-rc.N<br/>release candidate (kind 100+N)<br/>Beta on the site"]
  rel["vX.Y.Z<br/>release (kind 200)<br/>Stable on the site"]
  post["vX.Y.Z-N-gHASH<br/>N commits after the tag (kind 201+N)<br/>a git-describe build"]
  trap["Trap: vX.Y.Z-rc.N-M-gHASH<br/>reads as rc.N, not newer:<br/>use the next rc number for tests"]

  test -- "older than" --> rc -- "older than" --> rel -- "older than" --> post
  trap -. "equals" .-> rc
```

## The PC side: USB, console and flash helper

No network, so the USB serial/JTAG port is the only way into a board from a computer. Three things speak on it,
one at a time: the Pages site's *Configure over USB* card (Web Serial in Chrome or Edge), the test console used by
the harness and the devloop scripts, and esptool through the flash helper. The agent's shell may have no USB
access, so the flash helper runs on the PC, owns the COM port, and talks through plain files in `.devloop/`.

```mermaid
flowchart LR
  board["Board on USB<br/>test console at 115200:<br/>one command per line, 'test:' answers"]
  card["Pages card (browser, Web Serial)<br/>sends cfg, color, name, devmode, bright;<br/>renders the 'test: cfg {json}' line"]
  console["Test console commands<br/>cfg · color · color2 · devmode · name · bright · peers ·<br/>hold · fwpull · fwinfo · fwloop · fwtune · wave · game ·<br/>clip · ring · admin · trust · shot · + espforge's"]
  files[(".devloop/ files<br/>flash.request · serial.send · flash.status ·<br/>flash.done · serial_live.txt · stage/ · port")]
  helper["Flash helper (PC, owns the port)<br/>flash_helper.ps1 + monitor.ps1, or flash_helper.py<br/>stage parts under unique names, md5, esptool, log"]
  agent["Claude Code / devloop.py<br/>flash, send '<cmd>', read the log"]
  harness["harness.py --no-net<br/>suites: pick, hold, peers seen, waves,<br/>colour B, admin, page swipes; shot PNGs"]

  card <-- "USB, port free" --> board
  console --- board
  agent --> files --> helper <-- "USB" --> board
  harness --> files
```

What each line means is the contract in `docs/PROTOCOL.md`: the harness and the card parse `diag:`, `cfg:`,
`ble:`, `mesh:`, `fw:` and `test:` lines, so a line the firmware stops printing breaks a tool silently.

## From a tag to the boards

A tag is a release. CI builds it on the same ESP-IDF as the PC, runs the board-free tests, publishes the parts on
GitHub, and rebuilds the Pages site from the published releases only. `vX.Y.Z-rc.N` moves the Beta channel;
`vX.Y.Z` moves Stable. Claude tags release candidates; a stable tag needs the user's OK. Then one board is
installed from the site over USB, and the others pull it over Bluetooth in dev mode.

```mermaid
flowchart LR
  tag["git tag vX.Y.Z-rc.N / vX.Y.Z<br/>CHANGELOG section written first"]
  ci["CI: Firmware workflow<br/>config (forge.json) · build (git describe → version.txt,<br/>esp-idf-ci-action, dist parts, merged full.bin) ·<br/>host-tests (gcc + ASan, harness unit tests,<br/>config card, check_diagrams)"]
  rel["GitHub release<br/>bootloader · partition table · ota_data_initial ·<br/>colormesh-<ver>.bin · full.bin · flash-parts.json<br/>pre-release if the tag has a '-'"]
  pages["Pages site<br/>channels.json (stable, beta) · manifest.json per channel ·<br/>notes.json from the CHANGELOG · config.js"]
  install["One board installed from the page<br/>Stable or Beta, over USB, NVS kept"]
  peers["The other boards<br/>in dev mode: they hear the newer version<br/>and pull it, 21 s each"]
  check["harness.py --no-net --expect vX.Y.Z-rc.N<br/>diag: firmware <ver>, diag: running from ota_N"]

  tag --> ci --> rel --> pages --> install --> peers --> check
```

**Zoom in:**
- The other boards (`peers`) → [The dev-mode pull over Bluetooth](#the-dev-mode-pull-over-bluetooth)
