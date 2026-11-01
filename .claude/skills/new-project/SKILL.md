---
name: new-project
description: Turn a fresh copy of the espforge template into a new project - rename app, repo, OTA site and setup network, reset the changelog and baseline, start the project docs, and copy the GitHub settings from a reference repository, then check them. Use when the user starts a new device project from this template or asks to rename the project.
---

# New project from the template

Follow `docs/NEW-PROJECT.md`; this is the order of work.

1. **Ask the user** for the four names if not given: app (C identifier, e.g. `kitchen_timer`), repo
   (`owner/name`), OTA site (`https://<owner>.github.io/<name>/`, derived from repo unless they say otherwise),
   setup network name. Warn that the OTA site can never change once devices are out (L130).
2. **`forge.json`:** `app`, `repo`, `ota_site` (trailing `/`), `setup_ssid`, `screens`; `screen` and `chip` only for a
   different board.
3. **Root `CMakeLists.txt`:** `project(<app>)`; board line `set(EXTRA_COMPONENT_DIRS boards/<board>)`.
4. **`sdkconfig.defaults`:** `CONFIG_FORGE_SETUP_SSID`, `CONFIG_FORGE_OTA_SITE`, `CONFIG_FORGE_TLS_NAME`,
   `CONFIG_FORGE_REPO`. Delete `build\v55\sdkconfig`.
5. **Reset:** `CHANGELOG.md` to the header plus `## v0.1.0-rc.1 - <today>`; `tools/harness/baseline.json` to `{}`;
   `tools/harness/app_suites.py` to the app's suites (or an empty registry).
6. **Docs:** copy `docs/templates/HISTORY.md` and `ARCHITECTURE.md` to `docs/`, fill the hardware and the first
   timeline entry; README's user section; CLAUDE.md "Working setup" for this PC and board (leave the three growing
   sections empty with their one-line instructions).
7. **Leftovers:** `git grep -n -i "espforge\|Forge-Setup"` and fix each hit except the framework's own `forge_*`
   names and docs/LESSONS.md's history.
8. **Build and flash** with the `build-flash-probe` skill; check the setup network's name and `ota: Running`.
9. **Baseline:** `python tools/harness/harness.py --update-baseline`, review `baseline.proposed.json`, copy it over.
10. **Commit** (with the user's go-ahead).
11. **GitHub settings** (docs/NEW-PROJECT.md §3): `python tools/setup_github.py --from TheMonkeyz/espforge --to
    <owner>/<repo>` copies the rulesets, Pages, the github-pages policies and the Actions permissions through `gh api`,
    printing each call; then the same with `--check` (read-only, exit 1 on a difference). `gh` is not on the PATH of
    older shells on this PC: the tool looks in `C:\Program Files\GitHub CLI\gh.exe`, or pass `--gh <path>`. What the
    reference repository lacks is not copied: read the `--check` lines and give the owner what is left. First release:
    docs/RELEASING.md "First release".
