# Changelog

What changed in each release, written for the person holding a board. The installer page shows these notes for the
version it offers (`notes.json`, built by `tools/make_flasher_site.py`). Add a `## vX.Y.Z - YYYY-MM-DD` section at
the top before tagging, one `- ` line per change; release candidates get their own `## vX.Y.Z-rc.N` section and the
final release's section lists everything again.

## v0.6.0-rc.7 - 2026-11-01
- Calls: the caller sees everyone who answers, one dot per board in its colour, with the doo-di-dit for each; an
  answer that comes after "Answered" closed opens it again, as long as the others can still ring.
- The Call button is easier to hit: a bigger touch area, mostly below it, and a press near it never starts a
  recording.

## v0.6.0-rc.5 - 2026-11-01
- Calls: the ring now starts on its first note (the speaker wakes up on a short silence first).
- Calls: the ringing board says "Touch the screen to answer"; a touch answers. The caller shows "Calling…" in its
  colour, then "Answered" in the colour of the board that answered, with a short rising doo-di-dit.
- Console: `ring answer`, `ring cancel`, `ring answered` (plays the answered sound here).

## v0.6.0-rc.3 - 2026-11-01
- Voice clips: swipe to the last page ("Voice"), hold the screen and talk (up to 5 s), let go. Every board in range
  gets the clip at the same time and plays it at once, with "Message" in your colour.
- Call: the green Call button on the same page makes every other board ring with the classic phone tune and show
  "Incoming call" in your colour; touch the screen to stop it.
- Console: `clip rec <ms>`, `clip send`, `clip play`, `clip info`, `ring`, `ring local`, `ring stop`.
- Bluetooth now uses extended advertising (the colour flood sends the same packets as before; older boards still
  hear it). Needs this version on every board for clips and calls.

## v0.5.0-rc.3 - 2026-11-01
- Rock paper scissors: the title at the top of the game page sits lower, clear of the coloured ring.

## v0.5.0-rc.1 - 2026-11-01
- Rock paper scissors: swipe left twice from the main screen. Tap to start a round on every board near you; "Rock,
  Paper, Scissors!" counts down 3 s; tap your pick before 0; every board shows the result at the same moment (your
  pick, won / lost / draw, the others' picks with their colour). Console: `game`, `game start`, `game pick <x>`.

## v0.4.0 - 2026-11-01
Fourth release. Everything in v0.3.0 plus:
- Secure updates for boards not in dev mode: make one board the admin (`admin on`, or the USB card), paste its key
  into the other boards once (`trust <key>`), then they take new firmware only from that admin, after it proves
  the image with its key. Dev mode is unchanged. ADMIN pill on the main screen.

## v0.4.0-rc.2 - 2026-11-01
- Same code as rc.1; the label the paired boards took in the first signed pull (rc.1 was the pairing build).

## v0.4.0-rc.1 - 2026-11-01
- Secure updates for boards NOT in dev mode: make one board the admin (`admin on`, or the USB card), paste its key
  into the other boards once (`trust <key>`), then they take new firmware only from that admin, after it proves
  the image with its key (a fresh challenge, an ECDSA P-256 signature over length, digest and version, and the
  received bytes checked against the signed digest). Dev mode is unchanged. ADMIN pill on the main screen.

## v0.3.0 - 2026-11-01
Third release. Everything in v0.2.0 plus:
- Choose the wave's second colour: hold a swatch on the colour ring (a dot marks it); hold it again for automatic
  (the opposite of your colour). Also `color2 RRGGBB|auto` on the console and a "Wave colour" row on the USB card.
  Your choice travels with the wave, so every board draws your pair.

## v0.3.0-rc.1 - 2026-11-01
- Choose the wave's second colour: hold a swatch on the colour ring (a dot marks it); hold it again for automatic
  (the opposite of your colour). Also `color2 RRGGBB|auto` on the console and a "Wave colour" row on the USB card.
  Your choice travels with the wave, so every board draws your pair.

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
