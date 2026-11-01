# Changelog

What changed in each release, written for the person holding a board. The installer page shows these notes for the
version it offers (`notes.json`, built by `tools/make_flasher_site.py`). Add a `## vX.Y.Z - YYYY-MM-DD` section at
the top before tagging, one `- ` line per change; release candidates get their own `## vX.Y.Z-rc.N` section and the
final release's section lists everything again.

## v0.2.0 - 2026-11-01
Second release. Everything in v0.1.0 plus:
- Waves: tap the main screen to send a bicolour wave to every board nearby (centre = rings growing outward;
  left, right, top or bottom = bands moving in from that side); colour B is the opposite of your colour; waves
  from several boards merge, and a held colour adds to them.
- A board with a saved colour starts on the main screen; the colour ring is one swipe to the right.
- Swipes start after 28 px of finger travel, so taps and holds with a slight move stay what they are.
- The installer page explains the gestures and lets you configure a board over USB.

## v0.2.0-rc.2 - 2026-11-01
- Swipes are less touchy: a finger must move 28 px (was 10) before a swipe starts, so taps and holds with a
  slight move stay taps and holds.

## v0.2.0-rc.1 - 2026-11-01
- Waves: tap the main screen to send a bicolour wave to every board nearby (centre = rings growing outward;
  left, right, top or bottom = bands moving in from that side). Colour B is the opposite of your colour.
  Waves from several boards merge, and a held colour adds to them. 4 s per wave, about 30 frames per second.
- After a restart a board with a saved colour starts on the main screen; the ring is one swipe to the right.
- Console: `wave centre|left|right|top|bottom`, `shot wave`; harness test `mesh.wave_tap`.

## v0.1.0 - 2026-10-31
First release. Everything from the release candidates below:
- Pick your colour on a ring of 12 at start-up, or any time with a swipe right from the main screen.
- Touch and hold the screen: every ColorMesh board nearby, yours included, shows the mix of the held colours
  within one 20 ms advertising event.
- Main screen: your colour as a ring, the mix in the centre, a dot per board nearby; a system page by swipe left.
- Configure over USB from the installer page (Chrome or Edge): colour, name, dev mode, brightness.
- Dev mode (on by default): a board that hears a nearby board with a newer firmware pulls it over Bluetooth
  (1.1 MB in about 21 s) and switches to it after a successful start.
- Tear-free display, swipes that send no colour, test harness over USB.

## v0.1.0-rc.7 - 2026-10-31
- A swipe no longer sends your colour: the hold ends the moment the finger starts a swipe (14 ms instead of 400).

## v0.1.0-rc.6 - 2026-10-31
- The colour ring is a page of the main screen: swipe right from the main screen to open it, tap a colour, you
  are back on the main screen (pages: ring | main | system). No restart needed to change colour.
- Dev mode is on by default: every board takes a newer firmware from a peer over Bluetooth.
- Tear-free display: frames are drawn whole and pushed right after the panel's tear signal (GPIO13).

## v0.1.0-rc.5 - 2026-10-31
- Dev-mode updates from another board are 2.3 times faster: the sending board scans for the others 30 ms of every
  100 ms while it sends (it still hears them, more slowly) and the receiving board stops scanning while it pulls.
  Measured: Full pulls of the rc.5 image (1,136,224 bytes) read on the sender: to B in 20.9 s (52.9 KB/s, stream 53.4, 0 resent, window wait 156 ms), to C in 22.1 s (50.1 KB/s, 0 resent), with `mesh: peer` lines from the third board arriving during both transfers; C's first attempt lost the link at 676 KB (the peer dropped out, cause not visible from A) and succeeded 30 s later. The radio link itself was already at 2M PHY with 251-byte packets.
- Tear-free screen refresh on fast colour changes (frames synced to the panel's TE signal).
- For developers: the sender logs what the link granted (`fw: link dle/phy/itvl/mtu`), and `fwtune` on the test
  console changes the send window, scan duty and connection settings in RAM or stops a transfer after N KB to
  compare settings. rc.2 to rc.4 were test labels on the boards, not releases.

## v0.1.0-rc.1 - 2026-10-31
- Pick your colour at start-up on a ring of 12; it is remembered.
- Touch and hold the screen: every ColorMesh board nearby, yours included, lights up in your colour over Bluetooth, within a blink. Several people holding at once mix their colours (red and blue make magenta, all three make white).
- The main screen shows your colour as a ring, the mix in the centre, and a dot for each board nearby in its colour (bright while it holds), with how many are near and your firmware version.
- A system page (swipe): version, Bluetooth address, memory, uptime, the boards nearby with their versions, dev mode.
- Configure over USB from the installer page (Chrome or Edge): colour, name, dev mode, brightness; it shows the firmware version and the boards nearby. The same console commands work from a serial terminal.
- Dev mode: a board that hears a nearby board with a newer firmware pulls it over Bluetooth, installs it and restarts; a firmware that fails to run for a minute goes back to the previous one by itself.
- No Wi-Fi at all: installs and updates come over USB from the installer page, or from another board in dev mode.
- For developers: the test harness runs without a network (`--no-net`), with suites for the pick screen, holding, the boards heard and screenshots over USB.
- Measured on three boards: a hold reaches the others' mix within one 20 ms advertising event (61 % of events
  received at a 100 ms scan window); the dev-mode pull moved 1.1 MB over Bluetooth in 51 s (21 KB/s, no resend)
  and the image confirmed itself 60 s after the restart.
- Known limits: the pull rate is half the goal (PHY and data length to check); a swipe on the main screen sends
  a short hold.
