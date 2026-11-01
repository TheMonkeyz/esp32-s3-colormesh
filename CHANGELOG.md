# Changelog

What changed in each release. The display shows these notes on its update screen, and the settings page shows them
before you install; only the entries newer than the version you have are shown. The flasher site publishes them as
`notes.json`, built by `tools/make_flasher_site.py`.

How to write an entry: add a `## vX.Y.Z - YYYY-MM-DD` section at the top **before** tagging the release, with one
`- ` line per change, written for the person holding the display. Release candidates get their own
`## vX.Y.Z-rc.N - YYYY-MM-DD` section, shown only to Beta users; the final release's section lists everything again.

## v0.6.0 - 2026-10-31
- Screen off is a real off: the panel goes to sleep once the screen has faded out, and the processor slows down while it waits; a touch or a sound brings both back, with the screen redrawn before it shows.
- A touch on a dark screen only wakes it, every time; it never counts as a long press.
- A finger that moves while held is a swipe, not a long press: Settings and Wi-Fi setup no longer open from a slow swipe.
- The Settings list follows your finger smoothly (drawn as pictures, like the page swipes).
- The flash layout has a new mapcache partition for a map's home pictures: a board from v0.5.0 or older needs one install over USB; after that, updates over Wi-Fi again.
- For app developers: from MeteoBus's ESP32-P4 port. forge_core's diag has Kconfig switches for the two statistics that hold interrupts off (the per-task lines, PSRAM's largest block) and times them once at start; its crash summary reads a RISC-V core dump too. forge_net builds without Easy Connect where the Wi-Fi is a co-processor (net_dpp_supported, the starter hides that page), serves the settings page on CONFIG_FORGE_HTTPS_PORT and writes the port in every address (web_port), and checks the User-Agent's length at compile time (256 bytes).
- For app developers: forge_presence's screen_off hook; the board's display_sleep with a "screen" power lock, CONFIG_PM_ENABLE (80..240 MHz, no light sleep) and the console's power command; forge_audio, a board-free audio component (the Waveshare layout and MeteoBus's single-ES8311 one); 64-byte data cache lines.
- For app developers: forge_net has net_low_latency() (Wi-Fi power save off while a screen downloads much: ~100 ms less per request), one TLS download at a time (net_dl_begin/end, taken by the update check and install, the map tiles and the service probes) and a background worker for small network jobs (net_bg_post; the service probes run there instead of a task of their own). wifi status reports ps, lowlat, dl and bg; the console takes wifi lowlat on|off.
- For app developers: forge_lvgl allocates the neighbour pictures when needed instead of all four at start (1.7 MB less PSRAM for an app with two pages) with MeteoBus's rules for which picture gives way (host-tested), slide_release_unneeded() for a memory-hungry screen, slide_held_still(); forge_settings' settings_set_slider() for an app with its own slide.c.
- For app developers: forge_lvgl's list scrolls as pictures (the board's display_raw_area, the panel's raw_area hook); forge_map draws a path (a route) into its pictures with the pixels under it kept (fmap_set_path, no second canvas) and keeps the home place's pictures in the mapcache flash partition across boots (fmap_set_home, opts.flash_cache).
- For app developers: console shot (a screen over USB as a picture, for a session without HTTPS to the board; tools/devloop/shot.py) and netstat (lwIP's counters); the flash helpers pick the board's port by chip when several boards are plugged in.
- For app developers: the browser emulator runs screen dimming's real code (forge_presence's presence.c, its task, its saved settings, its /api/presence), with the board's microphones and motion sensor answering "not there"; CI builds the emulator from a pull request's own commit and runs its smoke test before merging.
- For app developers: three PC-side tools in the template. `tools/fetch_forge.py` clones espforge at the tag the app's manifest pins; `tools/forge_local.py` builds the app against a local espforge checkout before a tag and puts the manifest back; `tools/setup_github.py` copies a repository's GitHub settings to a new one and checks them.

## v0.6.0-rc.7 - 2026-10-31
- A touch on a dark screen only wakes it, every time: about one in ten also acted as a long press (the screen's own watcher saw the finger first and lit the screen before the touch filter looked).

## v0.6.0-rc.6 - 2026-10-31
- For app developers: the browser emulator runs screen dimming's real code (forge_presence's presence.c: its task, its saved settings, its /api/presence), with the board's microphones and motion sensor answering "not there"; its NVS stand-in gains the i16 keys.
- For app developers: CI builds the browser emulator from a pull request's own commit and runs its smoke test, so a change to the emulator or the screens is checked before merging; main and tags still build the latest stable release for the site's "Try it".
- For app developers: three PC-side tools in the template. `tools/fetch_forge.py` clones espforge at the tag the app's manifest pins (for the host tests and the display in the browser); `tools/forge_local.py` builds the app against a local espforge checkout before a tag, every espforge entry of the manifest and the board included, and puts the manifest back; `tools/setup_github.py` copies a repository's GitHub settings (rulesets, Pages, the github-pages environment's branch and tag policy, Actions permissions) to a new one and checks them.

## v0.6.0-rc.5 - 2026-10-31
- The Settings list follows your finger smoothly (drawn as pictures, like the page swipes).
- For app developers: console shot (a screen over USB as a picture, for a session without HTTPS to the board; tools/devloop/shot.py) and netstat (lwIP's counters); the flash helpers pick the board's port by chip when several boards are plugged in.
- For app developers: forge_lvgl's list scrolls as pictures (the board's display_raw_area, the panel's raw_area hook); forge_map draws a path (a route) into its pictures with the pixels under it kept (fmap_set_path, no second canvas) and keeps the home place's pictures in a mapcache flash partition across boots (fmap_set_home, opts.flash_cache); the partition table has that partition: a board on the old table needs one USB install.

## v0.6.0-rc.4 - 2026-10-31
- A finger that moves while held is a swipe, not a long press: Settings and Wi-Fi setup no longer open from a slow swipe.
- For app developers: forge_lvgl allocates the neighbour pictures when needed instead of all four at start (1.7 MB less PSRAM for an app with two pages) with MeteoBus's rules for which picture gives way (host-tested), slide_release_unneeded() for a memory-hungry screen, slide_held_still(); forge_settings' settings_set_slider() for an app with its own slide.c.

## v0.6.0-rc.3 - 2026-10-31
- For app developers: forge_net has net_low_latency() (Wi-Fi power save off while a screen downloads much: ~100 ms less per request), one TLS download at a time (net_dl_begin/end, taken by the update check and install, the map tiles and the service probes) and a background worker for small network jobs (net_bg_post; the service probes run there instead of a task of their own). wifi status reports ps, lowlat, dl and bg; the console takes wifi lowlat on|off. From MeteoBus v0.2.0-v0.2.1.

## v0.6.0-rc.2 - 2026-10-31
- Screen off is a real off: the panel goes to sleep once the screen has faded out, and the processor slows down while it waits; a touch or a sound brings both back, with the screen redrawn before it shows. From MeteoBus v0.2.1.
- For app developers: forge_presence's screen_off hook; the board's display_sleep with a "screen" power lock, CONFIG_PM_ENABLE (80..240 MHz, no light sleep) and the console's power command; forge_audio, a board-free audio component (the Waveshare layout and MeteoBus's single-ES8311 one); 64-byte data cache lines.

## v0.6.0-rc.1 - 2026-10-31
- For app developers: from MeteoBus's ESP32-P4 port. forge_core's diag has Kconfig switches for the two statistics that hold interrupts off (the per-task lines, PSRAM's largest block) and times them once at start; its crash summary reads a RISC-V core dump too. forge_net builds without Easy Connect where the Wi-Fi is a co-processor (net_dpp_supported, the starter hides that page), serves the settings page on CONFIG_FORGE_HTTPS_PORT and writes the port in every address (web_port), and checks the User-Agent's length at compile time (256 bytes).

## v0.5.0 - 2026-10-09
- Touch and hold opens a Settings screen on the display: screen dimming and its timing, wake on pick-up, brightness (the arc along the bottom), language, the phone's settings page as a QR code, Wi-Fi, updates, restart, and an About section. Done or a swipe right closes it.
- For app developers: the Settings screen as a component (forge_settings) with ready-made rows and room for the app's own; a pager can sit on a page of another one across it (a column of pages in the middle of a row), each direction dragging its own (pager_on_view, slide_stale).
- For app developers: the flash helper and the test harness follow the build that was staged (stage.py --build), so a build against an unreleased espforge can be flashed and its crashes decoded.

## v0.5.0-rc.1 - 2026-10-09
- Touch and hold opens a Settings screen on the display: screen dimming and its timing, wake on pick-up, brightness (the arc along the bottom), language, the phone's settings page as a QR code, Wi-Fi, updates, restart, and an About section. Done or a swipe right closes it.
- For app developers: the Settings screen as a component (forge_settings) with ready-made rows and room for the app's own; a pager can sit on a page of another one across it (a column of pages in the middle of a row), each direction dragging its own (pager_on_view, slide_stale).
- For app developers: the flash helper and the test harness follow the build that was staged (stage.py --build), so a build against an unreleased espforge can be flashed and its crashes decoded.

## v0.4.0 - 2026-10-08
- For app developers: a street map component (forge_map): OpenStreetMap tiles around a place, dimmed for the dark screen, kept in memory by place and zoom so going back downloads nothing; also in the browser emulator (USE_MAP).
- For app developers: the test tools are faster (the settings page tests in parallel, a quick harness mode for small changes).

## v0.4.0-rc.1 - 2026-10-08
- For app developers: a street map component (forge_map): OpenStreetMap tiles around a place, dimmed for the dark screen, kept in memory by place and zoom so going back downloads nothing; also in the browser emulator (USE_MAP).
- For app developers: the test tools are faster (the settings page tests in parallel, a quick harness mode for small changes).

## v0.3.0 - 2026-10-07
- The screen dims, then turns off, when the room stays quiet, and lights up again for voices, a touch, or when the display is picked up (settings page: Screen). The touch that wakes a dark screen does nothing else.
- Measuring the background noise can no longer go wrong when someone talks during it: the page says the room wasn't quiet enough and keeps the previous level.
- No board at hand? The installer page offers "Try it in your browser": the display's own screens and its settings page, running in your browser. Swipe, press and hold, change the language on the settings page and watch the screen follow.
- For app developers: screen dimming as a component (forge_presence, with a brightness slider and the settings weather_amoled saved before) and the board's microphones; pages shown or hidden and reordered (pager_set_count, pager_set_order); a tap right after a swipe can be told apart (slide_tap_ok); a press the app swallows (touch_set_press_filter); every app gets the "Try it" page (web/emu).
- For app developers: the flash helper runs on macOS and Linux too, and a restart keeps the whole start-up log.

## v0.3.0-rc.2 - 2026-10-07
- About 5 KB more of the scarcest memory left free: the speaker's half of the sound hardware is opened only by an app that plays sound.
- For app developers: screen dimming takes a brightness slider (presence_preview_brightness), tells the app when the settings page changed its settings, and reads the settings weather_amoled saved before it used forge_presence.

## v0.3.0-rc.1 - 2026-10-07
- The screen dims, then turns off, when the room stays quiet, and lights up again for voices, a touch, or when the display is picked up (settings page: Screen). The touch that wakes a dark screen does nothing else.
- Measuring the background noise can no longer go wrong when someone talks during it: the page says the room wasn't quiet enough and keeps the previous level.
- No board at hand? The installer page offers "Try it in your browser": the display's own screens and its settings page, running in your browser. Swipe, press and hold, change the language on the settings page and watch the screen follow.
- For app developers: screen dimming as a component (forge_presence) and the board's microphones; pages shown or hidden and reordered (pager_set_count, pager_set_order); a tap right after a swipe can be told apart (slide_tap_ok); a press the app swallows (touch_set_press_filter); every app gets the "Try it" page (web/emu).
- For app developers: the flash helper runs on macOS and Linux too, and a restart keeps the whole start-up log.

## v0.2.1 - 2026-10-06
- Map tiles and pictures saved with few colours are shown: such a map tile appeared as a dark square, and its map was downloaded again at every start.

## v0.2.1-rc.1 - 2026-10-06
- Map tiles and pictures saved with few colours are shown: such a map tile appeared as a dark square, and its map was downloaded again at every start.

## v0.2.0 - 2026-10-05
- Two quick swipes in a row both move the screen: the second one, landing while the first was still settling, was ignored.
- The settings page no longer tells someone on the setup network the name and address of your home network.
- When Wi-Fi Easy Connect doesn't work (some phones on a 5 GHz network say "Couldn't add device"), the display now says what to do: swipe right and join its setup network, which works with any phone.
- The settings screen shows a new update channel at once, even offline.
- For app developers: the languages are the app's (add one without changing the framework); service failure reasons and update errors can be shown in the display's language; names in the User-Agent, the certificate and the setup sign-in page are settings.
- Behind the scenes: about 2 KB more of the scarcest memory left free; crash-report cleanup no longer repeats at every start; quick setup-page switches can no longer stop Easy Connect twice at once; the test tools no longer miss the display starting up.

## v0.2.0-rc.2 - 2026-10-05
- Behind the scenes: about 2 KB more of the scarcest memory left free (buffers moved to the large external memory). Nothing changes on the screens.

## v0.2.0-rc.1 - 2026-10-05
- For app developers: the languages are the app's (add one without changing the framework); service failure reasons and update errors can be shown in the display's language; names in the User-Agent, the certificate and the setup sign-in page are settings. Nothing changes on the starter app's screens.
- The settings screen shows a new update channel at once, even offline.

## v0.1.1-rc.2 - 2026-10-04
- Behind the scenes: the test tools no longer miss the display starting up, and the quick-swipe test checks what it should. Nothing changes on the display itself.

## v0.1.1-rc.1 - 2026-10-04
- Two quick swipes in a row both move the screen: the second one, landing while the first was still settling, was ignored.
- The settings page no longer tells someone on the setup network the name and address of your home network.
- Behind the scenes: crash-report cleanup no longer repeats at every start; quick setup-page switches can no longer stop Easy Connect twice at once; a smoother-swipe measurement that is no longer fooled by an unrelated screen update.

## v0.1.0 - 2026-10-04
- Two screens to swipe between, following your finger smoothly: a welcome screen with the time and date, and a system screen with the firmware version, network, memory and update status.
- Press and hold anywhere for the Wi-Fi setup screen.
- Wi-Fi setup from a phone: join the display's own setup network (its password is on the screen), or scan the Easy Connect code with an Android phone. Swipe between the two.
- If the Wi-Fi is unreachable at start-up, the setup network opens by itself for a while.
- A settings page for Wi-Fi, language and updates: scan the code on the system screen with your phone.
- English and French (Canada), on the display and on the settings page.
- Updates over Wi-Fi from the Stable or Beta channel, with release notes before you install. A new version that fails to start goes back to the previous one by itself.

## v0.1.0-rc.6 - 2026-10-04
- Easy Connect works reliably: the display used to miss the phone's last message, so the phone said "That didn't work".
- The setup network stays available while the Easy Connect page is shown.
- Swiping to the system screen shows its current details right away.

## v0.1.0-rc.5 - 2026-10-04
- Easy Connect answers the phone faster.
- A network name made only of emoji shows as "…" instead of nothing.

## v0.1.0-rc.4 - 2026-10-04
- The Easy Connect code shows as a faint placeholder until it is ready, then fades in.

## v0.1.0-rc.3 - 2026-10-04
- Swiping between screens follows your finger smoothly (about 66 frames a second instead of 24), and the Wi-Fi setup pages swipe the same way.
- The Easy Connect code appears at once instead of after about 2 seconds.
- Network names with emoji show without empty boxes.
- Fixed: switching quickly back from the Easy Connect page could restart the display.
- Fixed: the Easy Connect page title overlapped the line below it.

## v0.1.0-rc.2 - 2026-10-04
- Before the first stable release, a display on the Stable channel says it is up to date instead of showing an error.

## v0.1.0-rc.1 - 2026-10-04
- Two screens to swipe between: a welcome screen with the time and date, and a system screen with the firmware version, network, memory and update status.
- Press and hold anywhere for the Wi-Fi setup screen.
- Wi-Fi setup from a phone: join the display's own setup network (its password is on the screen), or scan the Easy Connect code with an Android phone.
- If the Wi-Fi is unreachable at start-up, the setup network opens by itself for a while.
- A settings page for Wi-Fi, language and updates: scan the code on the system screen with your phone.
- English and French (Canada), on the display and on the settings page.
- Updates over Wi-Fi from the Stable or Beta channel, with release notes before you install. A new version that fails to start goes back to the previous one by itself.
