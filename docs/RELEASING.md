# Releasing

A tag is a release: CI builds it, publishes it on GitHub, and the Pages site offers it to every device on that
channel. Devices install what the tag builds. Read this before the first release of a project.

## Channels

| Tag | GitHub | Pages site | Devices |
|---|---|---|---|
| push to `main`, pull request | nothing (build check; images kept as a workflow artifact) | unchanged | nothing |
| `vX.Y.Z-rc.N` (any tag with a `-`) | pre-release | Beta channel moves to it, if it is above the stable | devices on Beta are offered it |
| `vX.Y.Z` | release | Stable channel moves to it; Beta disappears until the next newer rc | every device is offered it |
| *Run workflow* on `main` | nothing | rebuilt from the existing releases | nothing new |

The site is always assembled from files attached to releases (`flash-parts.json` and the `.bin` parts), never from a
branch, so people install exactly what was released.

## Version labels and their order

Order, as the firmware compares them (`forge_core/version.c`, mirrored in CI):

`vX.Y.Z-anything < vX.Y.Z-rc.N (by N) < vX.Y.Z < vX.Y.Z-N-gHASH (a git-describe build after the tag)`

- A device only installs a version **strictly above** what it runs, with the **same project name**
  (`forge.json` `app`).
- **Test builds** are labelled with `version.txt` (git-ignored; touch `CMakeLists.txt` after writing it) above the
  current release: with v0.1.0 out, `v0.2.0-graph.3`. A test label counts below every rc of the same X.Y.Z, so a board
  on `v0.2.0-graph.3` is offered `v0.2.0-rc.1`; on `v0.2.1-graph.3` it never is. Delete `version.txt` when done.
- CI writes `git describe --tags --always` into `version.txt`. A build outside a git checkout without it shows `1`.
- **Pick the number by content:** fixes only → patch; new features → minor, even if the rc series started as a
  patch (weather_amoled's v1.10.1-rc series shipped as v1.11.0). A feature series starts at `vX.(Y+1).0-rc.1`.
- Tags are lightweight.

## CHANGELOG

Before every tag, `CHANGELOG.md` needs the section, or devices show no "What's new":

```markdown
## v0.2.0-rc.1 - 2026-10-12
- The clock shows seconds.
- Wi-Fi setup no longer stops when the router restarts.
```

- `## vX.Y.Z - YYYY-MM-DD` (or `## vX.Y.Z-rc.N - …`), one `- ` line per change, written for the person holding the
  device. No "unreleased" sections: the tooling reads every `## v…` header as a release.
- `make_flasher_site.py site` turns it into `notes.json` (capped at `forge.json` `notes_max_bytes`); the device shows
  the sections between its version and the offered one before installing; when a stable release is offered, `-rc`
  sections are skipped, so the stable section repeats everything.

## Release candidate (Claude does this)

The step list is `.claude/skills/release-rc/SKILL.md`. In short:

1. The `## vX.Y.Z-rc.N - <date>` section is committed; `version.txt` deleted.
2. The committed tree is what was built and tested (`git stash push -u` for anything else, L14).
3. Host tests pass if a C file they compile changed (`wsl make -C tests/host`, L15); `cd tools/webtest && npm test`
   if the page changed.
4. `git fetch`; `main` isn't behind `origin/main`; `git push origin main`; `git tag vX.Y.Z-rc.N`;
   `git push origin vX.Y.Z-rc.N`.
5. Watch CI: `gh run list --repo <repo> --limit 5`, `gh run watch <id> --repo <repo> --exit-status`
   (`gh run view <id> --log-failed` on failure). The workflow is "Firmware"; a tag's run shows the tag as its branch.
6. Check the site: `<ota_site>/channels.json` lists the rc as `beta`.
7. Device on the Beta channel, running something below the rc: `python tools/harness/harness.py --ota vX.Y.Z-rc.N`.
8. Report: CI result, the harness summary line, anything noted.

A failed tag build is not reused: fix, then tag the next N (weather_amoled's v1.12.0-rc.1 and v1.12.1-rc.2 failed and
were published as rc.2 and rc.3). The CHANGELOG keeps no section for a tag that never released.

## Stable release (only with the user's OK)

1. Ask the user, saying what is in it (the rc series' changelog lines) and the proposed number.
2. Write the stable `## vX.Y.Z - <date>` section (everything from the rc sections, in user words), commit.
3. Same checks as an rc, then `git tag vX.Y.Z` on that commit and `git push origin main vX.Y.Z`.
4. Watch CI; check that the site **serves** it: `curl -sI <ota_site>channels.json` (its `Last-Modified` after the
   deploy) and its `stable`. Then `harness.py --ota vX.Y.Z` on a device on Stable (or Beta: Beta falls back to
   Stable when no newer rc exists).

### The site still serves the old files

espforge v0.1.0: the tag's `pages` job and its deployment both reported success, and 30 minutes later GitHub Pages still
served the rc.6 files (`Last-Modified` of the previous deployment, fetched again from the origin). Never re-run only
the `pages` job of that run: the re-run uploads a second `github-pages` artifact into the same run and
`deploy-pages` fails ("Multiple artifacts named github-pages"). Deploy afresh instead:
`gh workflow run firmware.yml --ref main` (the `pages` job builds the site from the published releases); served
within a minute.

## CI (`.github/workflows/firmware.yml`)

| Job | When | What |
|---|---|---|
| `config` | always | Reads `forge.json` (`app`, `idf`, `chip`, `ota_site`) into outputs for the other jobs. |
| `build` | push, PR, tag | `git describe` → `version.txt`; ESP-IDF build in `espressif/esp-idf-ci-action` (version and target from `forge.json`); `make_flasher_site.py dist` → release parts + `flash-parts.json`; `esptool merge_bin` → `<app>-<version>-full.bin`. Uploaded as an artifact. |
| `host-tests` | push, PR, tag | `make -C tests/host` (cJSON at ESP-IDF's version) and the harness's unit tests. |
| `webtest` | push, PR, tag | Playwright against the mock device; screenshots uploaded. |
| `emulator` | push, PR, tag | The display in the browser (`web/emu`) built with Emscripten, then its smoke test in headless Chromium. A pull request builds its own commit; `main` and tags build the latest stable release, for the site's *Try it* (artifacts `emulator`, `emulator-smoke`). A release doesn't need it: when it fails, the site goes out without *Try it*. |
| `release` | `v*` tags, after the three above pass | GitHub release with the parts; pre-release if the tag has a `-`. |
| `pages` | after a release, or *Run workflow* on `main` | Newest stable release + newest pre-release above it → `make_flasher_site.py site` → GitHub Pages; Beta only before the first stable release (L159). |

Actions are pinned to commit SHAs (the comment names the tag); Dependabot proposes updates monthly
(`.github/dependabot.yml`). esptool is pinned too.

### First release

Before the first stable release the site is Beta-only: `channels.json` has `"stable": null` and the flasher page
offers Beta; a device on Stable reports "up to date". So a new project's `v0.1.0-rc.1` is already installable over
USB (flasher page) and over Wi-Fi on Beta: flash a test build labelled `v0.1.0-rc.0`, set the channel to Beta, tag the
rc and run `harness.py --ota v0.1.0-rc.N`. Tag `v0.1.0` with the user's OK; from then on Stable is offered too.
(espforge itself: rc.1's site job failed on a missing tag rule and a stable-only site generator, L159; rc.2 was the
first published to Beta.)

## One-time GitHub setup

- **Settings → Pages → Build and deployment → Source: GitHub Actions.**
- **Settings → Environments → github-pages → Deployment branches and tags → add a Tag rule `v*`** (by default only
  `main` may deploy to Pages, and every release would fail at the `pages` job).
- **Settings → Rules → Rulesets → New tag ruleset** on `v*`: only the owner may create, update or delete. A tag is a
  release that devices install.
- **Never rename the repository or the account.** Every device polls the Pages address built into its firmware
  (`forge.json` `ota_site`, `CONFIG_FORGE_OTA_SITE`); a rename strands them on their current version until a USB
  install.

## Release files

| File | Use |
|---|---|
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`, `<app>-<version>.bin` | Separate parts at the offsets in `flash-parts.json` (from `build/flasher_args.json`): an install that keeps NVS (Wi-Fi, settings, the TLS certificate). The web flasher and the updater use these; the updater only takes the app part (offset 0x10000). |
| `<app>-<version>-full.bin` | Merged image, flash at 0x0: a first install with esptool. Overwrites everything. |
| `flash-parts.json` | Chip, version, part names and offsets; the site builder reads it. |

Only a USB or web-flasher install changes the bootloader and partition table (L128): say so in the changelog when a
release needs one.

## Restoring a board

v0.6.0 changed the partition table (a `mapcache` partition for forge_map's flash cache at 0x620000, the crash dump
moved to 0xEE0000): a board on an older table needs one USB or web-flasher install (`harness.py --flash`); an update
over Wi-Fi keeps the old table, and the firmware then finds no `mapcache` and caches nothing (it says so in the log).

- **Bad update:** a new image that restarts (or never reaches Wi-Fi) within its confirm window rolls back by itself
  (L126); the device reports it once.
- **A board on the wrong image:** flash a known build over USB with the helper (`stage.py`, `devloop.py flash`); the
  helper writes `ota_data_initial.bin`, so it boots `ota_0` (L3).
- **Back to a release:** the web flasher (`<ota_site>`, `?channel=beta` for Beta), or
  `esptool.py --chip esp32s3 write_flash 0x0 <app>-<version>-full.bin` (erases settings).
- **Board doesn't enumerate / boot loop:** hold BOOT while pressing RESET for download mode, then flash.
- After an erase the TLS certificate is regenerated, so the browser warns again (L124); the setup network password
  and the settings key are new too.

## Preview the site locally

```bash
python tools/make_flasher_site.py dist --build build/v55 --out dist
python tools/make_flasher_site.py site --stable dist --out _site
python -m http.server -d _site 8000     # Web Serial works on localhost
```
