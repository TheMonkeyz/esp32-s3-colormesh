# The display in the browser

The app's own screens, compiled to WebAssembly with Emscripten: LVGL at the firmware's version with the display's
settings, the app's `main/` sources (`main.c`, `ui.c`, `app_text.c`), the framework's components (forge_lvgl's pager
and moves drawn as pictures, forge_core's i18n and text fit, forge_net's service statuses, forge_ota's settings page
routes), all unchanged. Only the hardware is replaced. The settings page (`main/web/index.html`, unchanged) runs
beside it in an iframe, served by the firmware's own route handlers. On the flasher site it is *Try it in your
browser* (`try/`). It came from [weather_amoled](https://github.com/TheMonkeyz/esp32-s3-weather)'s `web/emu`
(October 4-6) and rtc_quebec's port of it (October 6).

```
web/emu/                  the app's: a new app changes these (docs/NEW-PROJECT.md)
  Makefile                what to build: APP_SRC (main/'s files), APP_EMU (emu_main.c ...), EMBED (the font)
  emu_main.c              main(): what only the browser needs, then app_main() as a task and the loop
  index.html              the page: the app's words and look, the canvas, the settings page's frame
  lv_kconfig.h            LVGL's settings, generated from the firmware's sdkconfig (make config)
  sdkconfig.h             the framework's and the app's Kconfig values, generated with it
web/emu/forge/            the framework's: the same in every app
  emu.mk                  the build rules (Emscripten, LVGL, cJSON, the components, the page)
  emu.h                   what emu_main.c calls: emu_start_app_main, emu_loop / emu_step, emu_param
  emu_*.c, shim/          the hardware and ESP-IDF, in the browser (below)
  emu-page.js             draws the screen, the mouse or finger is the touch, queues the settings page's requests
  emu-settings.js         loaded first in the settings page: its /api/ requests go to the emulator
  gen_config.py           sdkconfig -> lv_kconfig.h + sdkconfig.h
  embed.py                the files main/CMakeLists.txt embeds (EMBED_FILES), as C arrays
  smoke.js                the smoke test: headless Chromium, real mouse events (below)
```

## What stands in for what

| File | Stands in for |
|---|---|
| `emu_display.c` | the panel (`board.h`'s display half and `board_init()`): a `DISP_W` x `DISP_H` RGB565 framebuffer that `emu-page.js` copies to the canvas when it changed; slide.c's raw frames; `display_brightness()` dims the canvas (a CSS filter, 0 is black) |
| `emu_touch.c` | the touch chip (`board.h`'s touch half): the mouse or a finger on the canvas, for LVGL and for slide.c's own reads |
| `emu_http.c` | `esp_http_client` (and so forge_core's `http_once`): a `fetch()`, polled until it answers (ASYNCIFY) |
| `emu_nvs.c` | NVS: a small table saved in the page's `localStorage` (`<app>_emu_nvs`) on each commit |
| `emu_tasks.c` | FreeRTOS tasks, queues, binary semaphores: each task an Emscripten fiber, run by the main loop between LVGL frames; a wait inside a task goes back to the main loop |
| `emu_time.c` | `localtime_r` following `TZ` as the firmware sets it (Emscripten's uses the browser's zone) |
| `emu_web.c` | the web server: the settings page's `/api/` requests, queued by the page and served between LVGL frames by the app's and forge_ota's handlers; `/api/info` as forge_net's; Wi-Fi scan and save answer that they need the real display |
| `emu_stubs.c` | Wi-Fi (a saved network, joined at once: `main.c` goes straight to what it does once Wi-Fi is up), the setup network and Easy Connect (never start), updates (none; Restart reloads the page), diagnostics and the test console (none: the log is the browser's console), the NTP service (the browser's clock), the board's microphones and motion sensor (not there: Notes) |
| `emu_loop.c` | the LVGL task and the scheduler: `app_main()` as a task, then tasks, settings page requests, `lv_timer_handler()`, `emscripten_sleep()`, forever |
| `shim/` | the ESP-IDF and FreeRTOS headers the firmware includes; `vTaskDelay` hands control back to the browser |

The app has one `#ifdef EMU_BUILD`: `ui.c` finds its font as an array (`ttf_start[]`, `*ttf_end`) instead of
ESP-IDF's `asm("_binary_montserrat_ttf_start")` symbols, which WebAssembly can't place (`embed.py`).

## Build (WSL)

Emscripten 6.0.11 (the version CI uses):

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/emsdk && ~/emsdk/emsdk install 6.0.11 && ~/emsdk/emsdk activate 6.0.11
```

From Windows, after one firmware build (it fetches LVGL into `managed_components/lvgl__lvgl`):

```bash
wsl bash -lc 'source ~/emsdk/emsdk_env.sh && make -C /mnt/c/Users/<you>/ESPDEV/<app>/web/emu -j8'
```

A build from scratch takes about a minute (LVGL, over `/mnt/c`); then `build/` has `index.html`, `emu.js`, `emu.wasm`
(~0.9 MB), `emu-page.js`, `settings.html`, `emu-settings.js` and `fonts/`. cJSON is ESP-IDF's (`IDF_PATH`, default
`/mnt/c/Espressif/esp-idf`); `make LVGL=<lvgl> CJSON=<folder>` points elsewhere. Serve `build/` over HTTP
(`file://` can't load WebAssembly): `.claude/launch.json` "emulator" (port 8767), or
`python -m http.server 8767 -d web/emu/build`. Another session may already serve another checkout on that port: pick
another one, or the page shows the other checkout's build.

- `make config`: after changing LVGL options or the app's Kconfig values, regenerates `lv_kconfig.h` and
  `sdkconfig.h` from `build/v55/sdkconfig` (`SDKCONFIG=` for another); commit both, CI doesn't build the firmware
  first.
- `make try F=../../main/ui.c`: compile one file, show its first errors.
- `node web/emu/forge/smoke.js` (Playwright from `tools/webtest`, `npm ci` there): serves `build/` and checks in
  headless Chromium, with real mouse events, that the screen draws, a drag to the left shows another page, a press and
  hold changes the screen, the settings page reads `/api/info`, screen dimming's `/api/presence` answers with a state
  (forge_presence's real code runs), choosing its second language changes the screen, and that nothing logs an error.
  Screenshots of each step in `build/smoke/`: look at them. CI runs it after the build.
- From a git worktree without `managed_components` of its own: `make LVGL=<main checkout>/managed_components/lvgl__lvgl`.

## What an app provides

1. `Makefile`: `APP_SRC` (the `main/` files that build for the browser: usually all of `main/CMakeLists.txt`'s SRCS),
   `APP_EMU` (its `emu_*.c` files), `EMBED` (`name=path` for each embedded file its code reads), and if it uses more
   of the framework than the starter, `FORGE_CORE` / `FORGE_NET` ... (e.g. `FORGE_CORE += png_rows.c`; `http_once.h` is header-only).
2. `emu_main.c`: `main()`. Usually demo data a first visitor should see (a place, a stop), `?parameters` from the
   page's address (`emu_param`), then `emu_start_app_main(); emu_loop();`. An app whose `main.c` can't run here writes
   that part itself (weather_amoled's `emu_main.c` fetches and shows the forecast in its own loop, `emu_step()`).
3. `index.html`: the page's words (what to try, what needs the real display) and look; keep the element ids
   `emu-page.js` uses (`screen`, `status`, `settings`) and its two scripts.
4. The one `#ifdef EMU_BUILD` for each embedded file (`embed.py`'s docstring).
5. Its own hardware: a stand-in in `web/emu` (in `APP_EMU`) and, if the driver's headers aren't in `shim/`, a header
   next to it (`web/emu` comes first on the include path). weather_amoled's are examples: `emu_audio.c` (the speaker
   and microphones through `esp_codec_dev`, played with Web Audio, the browser's microphone when the visitor turns it
   on), `emu_imu.c` (the motion sensor: a phone's `devicemotion`, or a "Pick it up" button) and `emu_partition.c` (a
   flash partition, in memory). A C function the page calls goes in `APP_EXPORTS` (`APP_EXPORTS := ,_emu_mic`); the
   page reaches the module through `window.emuReady`.

What fails to link is what the app uses and the browser lacks: stub it in the app's `web/emu` file, or, if every
app would need it, in `forge/`.

## On the flasher site

`python tools/make_flasher_site.py site --stable dist --emu web/emu/build` copies `build/`'s files to the site's
`try/` and adds `"try": "try/"` to `channels.json`; the flasher page then links to it ("No board at hand? Try it in
your browser"). `index.html` uses the site's font from `../fonts/` (the build copies it to `build/fonts/` for local
use).

CI (`.github/workflows/firmware.yml`, job `emulator`) builds it with LVGL from GitHub at `main/idf_component.yml`'s
version, cJSON 1.7.19 (ESP-IDF 5.5.4's) and Emscripten 6.0.11, then runs the smoke test (screenshots: artifact
`emulator-smoke`). A pull request (or a push to a branch other than `main`) builds its own commit, so a change to the
emulator or the screens is checked before it is merged. A push to `main` or a tag builds the latest stable release
(the tag itself when a stable tag is pushed; a release older than the emulator has no `web/emu`, and then the pushed
commit is built, said in the log): the `pages` job adds that one to the site, and if its build or its smoke test
fails, the site is published without it.

## Notes

- One thread: the tasks, the settings page's requests and LVGL take turns; LVGL never runs while a task does, as the
  display lock guarantees on the board. A task's wait (`vTaskDelay`, `ulTaskNotifyTake`, a queue, a semaphore, an
  HTTP request) switches back to the main loop; a wait in the main loop is an `emscripten_sleep`. At most 6 tasks
  (`MAX_TASKS`), 256 KB of C stack each whatever the firmware asks.
- A click can start and end between two of LVGL's touch reads: `emu_touch.c` holds a new press until LVGL has read
  it. Only a new press is held, never a move, and `touch_forget()` drops it: a drag (slide.c) reads the finger itself,
  and a press held from its moves reached LVGL after the drag as a tap (weather_amoled: every day swipe closed the
  hourly view, October 4).
- The settings page's requests are served by the main loop (`emu_web_poll()`), never inside a call from JavaScript:
  a handler may wait (forge_ota's update POST does), and ASYNCIFY can't unwind a call made while the main loop is
  itself suspended. The frame can start before the page around it has run its script: `emu-settings.js` waits for
  `emuApi`. Its key is `#k=0000000000000000` (`web_key()`); no key is checked.
- A request is a plain `fetch()` with no header of ours: a page can't set `User-Agent`, and a custom header turns
  every request into a CORS preflight. The service must answer cross-origin requests (`Access-Control-Allow-Origin`).
  A request polls for its answer: an await inside a fiber isn't safe with ASYNCIFY. The screen pauses while the main
  loop waits for one.
- Emscripten's `strftime("%z")` ignores `TZ` too (it wrote local time as +00:00, esp32-s3-rtcquebec): compute an
  offset from the date (`mktime` of the local fields against the UTC time), as its `rtc_iso_local` does (LESSONS L196).
- `png_rows.c` (map tiles, pictures) inflates with the ROM's tinfl on the board: an app that adds it
  (`FORGE_CORE += png_rows.c`) gets miniz 3.0.2's tinfl here, downloaded once into `build/miniz`.
- forge_map (a street map): `USE_MAP := 1` in the app's Makefile builds the whole component (its task, the tiles
  as `fetch()`: tile servers like OSM's answer cross-origin requests) with `png_rows.c` and miniz.
- The board's `touch_set_press_filter()` works here too (`emu_touch.c`): a press the app refuses is no press.
- Screen dimming (forge_presence) is the real `presence.c`, as on the board: its task ticks every 100 ms, its settings
  are saved in the emulated NVS (namespace `presence`, one typed key each; the baseline is an `i16`, which `emu_nvs.c`
  has with the other sizes), its console commands register (nowhere to type them) and `/api/presence` answers the
  settings page. The hooks come from the app's `main.c` through `app_main()`, as on the board: the starter passes
  `board_mic_open`, `imu_init`, `touch_idle_ms` and `display_brightness`, and `emu_stubs.c` answers for the
  microphones and the motion sensor that they are not there, so the state machine never runs, the screen stays on and
  the page shows "No microphone". An app whose `main()` doesn't run `main.c` (weather_amoled) passes its own
  `presence_hooks_t` to `presence_start()` in its `emu_main.c` (`mic_open` and `accel_open` returning false,
  `mic_read` and `accel_read` unused, `touch_idle_ms` from `emu_touch.c`, `set_brightness` on `display_brightness()`,
  `settings_changed` NULL or its own) and calls `presence_web_routes()`.
- A hidden tab pauses `requestAnimationFrame` (the canvas updates only when shown) and slows timers.
- `?lang=fr` in the address starts the starter app in French (`emu_main.c`), as if chosen on the settings page.
