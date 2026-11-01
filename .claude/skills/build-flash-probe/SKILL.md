---
name: build-flash-probe
description: Build a labelled test build, flash it to the board through the devloop flash helper and read the log or drive the test console. Use for any firmware change before calling it done, or whenever the user asks to flash, try or check something on the device.
---

# Build, flash, probe

Rules behind each step: docs/LESSONS.md L1-L10. Protocol: docs/PROTOCOL.md.

1. **Label** the build above the current release (`git describe --tags --abbrev=0`; e.g. `v0.2.0-<topic>.<n>`):
   `Set-Content version.txt "<label>" -NoNewline -Encoding ascii`, then touch `CMakeLists.txt`
   (`(Get-Item CMakeLists.txt).LastWriteTime = Get-Date`).
2. **Note to the user** ("Building <label> (~2 min)"), then build:
   `. C:\Espressif\esp-idf\export.ps1; $env:IDF_COMPONENT_CACHE_PATH="$env:USERPROFILE\.espressif\cm_cache"; idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`
   (the cache path: L199, the desktop app's shell).
   Changed `sdkconfig.defaults`? Delete `build\v55\sdkconfig` first and reconfigure.
3. **Board free?** `python tools/devloop/devloop.py status` shows `idle` and `.devloop/serial_live.txt` isn't growing.
   If the helper isn't running, start it yourself in the background, windowless (Bash or PowerShell tool with
   `run_in_background`): `powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1`. Nothing opens on the user's screen;
   its output arrives in that background task. Only one helper at a time: check no other one holds COM5. On a Mac:
   `python tools/devloop/flash_helper.py`, in the background the same way (docs/MACOS.md).
4. **Stage:** `python tools/devloop/stage.py`. It copies every part under a unique name and prints the md5 of source
   and copy. Stop if any pair differs.
5. **Flash:** note to the user ("Flashing, then a 120 s log"), then `python tools/devloop/devloop.py flash 120`.
   If a gesture is needed, tell the user exactly when and what ("when the screen lights up: swipe left twice").
6. **Check the result:** `devloop.py wait-done`, then `.devloop/flash.done` (`exit=0`, errors, resets). Then the log:
   - `ota: Running <label> from ota_0` must be the build you just made (L2);
   - `diag: mark app ready`, no `rst:0x` after boot, no `Guru Meditation`.
7. **Probe:** `python tools/devloop/devloop.py send "<command>"` (see `help`), read the `test:` reply in
   `.devloop/serial_live.txt`. Stop the window early with `devloop.py stop` once you have the lines.
   Hang? `send where` (it takes no lock). Memory? `send heap`.
8. **Forget cached board facts** if the tools didn't: `.devloop/ip`, `.devloop/key` (L7).
9. **Report** what the log shows, quoting the lines. Next: `snapshot` and `harness` skills to prove it.

When done with test builds, delete `version.txt` (and touch `CMakeLists.txt`) so later builds don't keep the label.
