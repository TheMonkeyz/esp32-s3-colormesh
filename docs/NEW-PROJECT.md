# Starting a new project from espforge

The repository is a GitHub template and a buildable starter app. A new project is a copy with its names changed,
its history reset, and its own docs started. Claude can do all of it with the `new-project` skill; the GitHub
settings at the end need the owner.

## 1. Create the repository

On GitHub: **Use this template → Create a new repository** (public if devices should update from GitHub Pages:
Pages on a private repo needs a paid plan). Clone it on the PC, next to the others (e.g.
`C:\Users\<you>\ESPDEV\<name>`).

Pick the names once; they are hard to change later:

| Name | Example | Used for |
|---|---|---|
| app | `kitchen_timer` | CMake project name, image names, the updater's project-name check |
| repo | `TheMonkeyz/kitchen-timer` | CI, release links, the User-Agent of outgoing requests |
| ota_site | `https://themonkeyz.github.io/kitchen-timer/` | where devices look for updates **forever** (L130) |
| setup_ssid | `Timer-Setup` | the setup access point's name |

## 2. Rename checklist

- [ ] **`forge.json`**: `app`, `repo`, `ota_site` (ends with `/`), `setup_ssid`, `screens` (the app's screen names),
      `screen` (size, shape) if the board differs. Leave `idf`, `chip`, `build_dir` unless you mean it.
- [ ] **Root `CMakeLists.txt`**: `project(<app>)`. It must equal `forge.json` `app`: the updater refuses an image with
      another project name (L129).
- [ ] **`sdkconfig.defaults`**: the framework's Kconfig values (menuconfig "espforge"):
      ```
      CONFIG_FORGE_SETUP_SSID="Timer-Setup"
      CONFIG_FORGE_OTA_SITE="https://themonkeyz.github.io/kitchen-timer/"
      CONFIG_FORGE_TLS_NAME="Kitchen Timer"
      CONFIG_FORGE_REPO="TheMonkeyz/kitchen-timer"
      ```
      Then delete `build\v55\sdkconfig` and reconfigure (L12).
- [ ] **`README.md`**: what the device does, for its users first; the web flasher link ("Try it on the board") pointing at
  your `ota_site`, and your board; keep the developer sections.
- [ ] **`CHANGELOG.md`**: keep the `# Changelog` header, replace the sections with `## v0.1.0-rc.1 - <date>`.
- [ ] **`tools/harness/baseline.json`**: reset to `{}`, then run the harness on the first good build with
      `--update-baseline`, review `baseline.proposed.json` and copy it over (docs/TESTING.md §5). Until then the
      harness unit test of the snapshot metrics is skipped (an empty baseline), not failed.
- [ ] **`tools/harness/app_suites.py`**: replace the starter's suites with the app's. `pages()` lists the pager's
      screens for the navigation test: if the app's page count varies, read it from the device there.
- [ ] **`main/i18n_strings.h`** and **`I18N` in `main/web/index.html`**: the app's texts, every language.
- [ ] **`web/emu/`** (the display in the browser, the site's *Try it*; [web/emu/README.md](../web/emu/README.md)):
      `Makefile` lists the app's `main/` sources (`APP_SRC`), its emulator files (`APP_EMU`) and embedded files
      (`EMBED`); `emu_main.c` adds what a first visitor should see (demo data, `?parameters`) before
      `emu_start_app_main()`; `index.html` gets the app's name and words. After the first firmware build,
      `make -C web/emu config` regenerates `lv_kconfig.h` and `sdkconfig.h` (the new names); commit them. Each new
      embedded file read by the code gets the `#ifdef EMU_BUILD` `ui.c` has for the font. Hardware the board doesn't
      have in the template (speaker, microphones, motion sensor, a flash partition) gets a stand-in in `web/emu`
      (weather_amoled's `emu_audio.c`, `emu_imu.c`, `emu_partition.c` are examples). `web/emu/forge/` stays as is.
- [ ] **`CLAUDE.md`**: fill "Working setup" for this PC and board; leave "Bugs hit", "User preferences learned" and
      "Useful facts" to grow.
- [ ] **`docs/`**: copy `docs/templates/HISTORY.md` and `ARCHITECTURE.md` to `docs/` and start them;
      `DIAGNOSTICS.md` after the first diagnostics run; `IDEAS.md` when the backlog starts.
- [ ] **`LICENSE`**: your name and year. Keep `THIRD_PARTY_NOTICES.md` whatever licence you choose.
- [ ] Search for leftovers: `git grep -n -i "espforge\|Forge-Setup"` (the framework's own component names
      `forge_*` stay).

## 3. GitHub settings (the owner)

- [ ] `gh auth status` on the PC shows an account with `repo` and `workflow` scopes. On this PC `gh` is not on the
      PATH of older shells: Git Bash `"/c/Program Files/GitHub CLI/gh.exe"`, PowerShell
      `& "C:\Program Files\GitHub CLI\gh.exe"`; the tool below looks there by itself (or `--gh <path>`).
- [ ] **Copy the settings from a repository that has them, then check:**
      `python tools/setup_github.py --from TheMonkeyz/espforge --to <owner>/<repo>` adds what the new repository
      lacks through `gh api`, printing each call; with `--check` it changes nothing, lists every setting as same or
      different and exits 1 when one differs. It handles the repository rulesets ("protect main", "release tags"),
      Pages built by a workflow, the `github-pages` environment's deployment branch and tag policy (`main`, `v*`) and
      Actions' default workflow permissions (`read`). `--from` is never changed; a setting the reference lacks is not
      copied, so read the `--check` lines.
- [ ] The same by hand, when needed: **Settings → Pages → Build and deployment → Source: GitHub Actions**;
      **Settings → Environments → github-pages → Deployment branches and tags → Tag rule `v*`**;
      **Settings → Rules → Rulesets → New tag ruleset** on `v*`, only the owner may create, update or delete.

## 4. First build, first release

1. Copy `tools\esptool.exe` from espforge's checkout (git-ignored, so a fresh clone lacks it; without it the helper
   runs `python -m esptool` with whatever `python` is first on the PATH, and the Microsoft Store one has no esptool).
   `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`; flash with the helper (Claude starts it in the
   background); check `ota: Running` and that
   the setup network has the new name.
2. `python tools/harness/harness.py` (after resetting the baseline, with `--update-baseline`). Its settings page
   step needs Playwright's Chromium where the harness's Python can see it: `cd tools/webtest && npm ci && npx
   playwright install chromium`, then copy `%LOCALAPPDATA%\ms-playwright` to `tools/webtest/.browsers`
   (git-ignored). The Microsoft Store Python hides AppData\Local from its child processes (L146): without the copy
   every webtest fails in ~1 ms ("Executable doesn't exist") while `npm test` by hand passes.
3. Commit, push `main`: CI builds (no release).
4. Tag `v0.1.0-rc.1`: a pre-release and a Beta-only site (docs/RELEASING.md "First release"). With the board on a
   test build labelled `v0.1.0-rc.0` and the Beta channel: `harness.py --ota v0.1.0-rc.1`.
5. With the owner's OK, tag `v0.1.0`: Stable is offered too.

## Later: the framework at a tag

The copy starts with espforge's components and board in its own tree. Once the app is a project of its own, it can
take them from espforge at a release tag instead, as weather_amoled, esp32-s3-rtcquebec and esp32-s3-meteobus do: the
app only gets espforge versions that passed their own tests, and a framework fix reaches it by bumping one tag.

1. **`main/idf_component.yml`**: one entry per espforge component the app uses, and the board, all at the same tag.
   ESP-IDF's component manager fetches them into `managed_components/` (git-ignored); delete `dependencies.lock`
   when bumping the tag.
   ```yaml
     forge_core:
       git: https://github.com/TheMonkeyz/espforge.git
       path: components/forge_core
       version: v0.6.0
     board:
       git: https://github.com/TheMonkeyz/espforge.git
       path: boards/ws_amoled175/board
       version: v0.6.0
   ```
   Then delete the copies (`components/forge_*`, `boards/`, `web/emu/forge/`) and the `EXTRA_COMPONENT_DIRS` line of
   the root `CMakeLists.txt`.
2. **`tools/fetch_forge.py`** (in the template): clones espforge at that tag into `.espforge/` (git-ignored), for
   the builds outside idf.py, and prints the components folder. While the app still has copies (no espforge entry
   in the manifest) it says there is nothing to do.
3. **`tests/host/Makefile`**: the framework files the tests compile come from `managed_components` after an idf.py
   build, else from the clone:
   ```make
   FORGE ?= $(if $(wildcard ../../managed_components/forge_core/CMakeLists.txt),../../managed_components,../../.espforge/components)
   CORE := $(FORGE)/forge_core
   ```
4. **`web/emu/Makefile`**: the emulator's framework part, components and board from the clone, before the include:
   ```make
   ESPFORGE ?= ../../.espforge
   FORGE_EMU := $(ESPFORGE)/web/emu/forge
   COMP := $(ESPFORGE)/components
   BOARD := $(ESPFORGE)/boards/ws_amoled175/board/include
   include $(FORGE_EMU)/emu.mk
   ```
   Then `python3 ../../tools/fetch_forge.py && make -j8` (WSL, after `source ~/emsdk/emsdk_env.sh`).
5. **Before an espforge tag**, build the app against the local espforge checkout: `python tools/forge_local.py`
   (`--forge <path>`, default `../espforge`). For the length of the build every espforge entry of the manifest points
   into the checkout (`override_path`), the build goes to `build/forge`, then the manifest is put back exactly as it
   was; nothing is committed. Flash it with the harness (`python tools/harness/harness.py --flash build/forge`), run
   the suites, then tag espforge and bump the tags.

## A new board

Board support is an ESP-IDF component named `board` under `boards/<name>/board/`, selected by one line in the root
`CMakeLists.txt`:

```cmake
set(EXTRA_COMPONENT_DIRS boards/ws_amoled175)
```

A new board is a new `boards/<name>/board/` directory exposing the same `board.h` API as `boards/ws_amoled175/board/`
(see [COMPONENTS.md](COMPONENTS.md)): `BOARD_NAME`, `DISP_W`, `DISP_H`, `BOARD_ROUND`, `board_init()`,
`display_lock()` / `display_unlock()`, `display_brightness()`, the touch read and the shared I2C bus. Then switch the
`EXTRA_COMPONENT_DIRS` line and update `forge.json` `screen` and `chip`.

What to carry over from the first board (docs/LESSONS.md): panel quirks stay inside the board component (L88);
esp_lcd's interrupt on the LVGL task's core and no esp_lcd call while a band is in flight (L81, L82); a touch read
that treats repeated NACKs as "up", reads at most every 10 ms and accepts injected touches (L103-L105); draw buffers
in internal DMA RAM, sized deliberately (L86). Measure a full-screen render on the new panel before promising any
frame rate (L87).
