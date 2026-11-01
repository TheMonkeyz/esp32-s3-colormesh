# Changelog

What changed in each release, written for the person holding a board. The installer page shows these notes for the
version it offers (`notes.json`, built by `tools/make_flasher_site.py`). Add a `## vX.Y.Z - YYYY-MM-DD` section at
the top before tagging, one `- ` line per change; release candidates get their own `## vX.Y.Z-rc.N` section and the
final release's section lists everything again.

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
