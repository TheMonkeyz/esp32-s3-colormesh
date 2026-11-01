---
name: release-rc
description: Publish and test a release candidate (vX.Y.Z-rc.N) - CHANGELOG check, build what you commit, host tests, tag and push, watch CI with gh, check the Pages Beta channel, install it on the device with harness.py --ota, report. Use after "document and commit" when the user says "release rc" or a fix plan group is done. Also covers preparing a stable release, which needs the user's OK before tagging.
---

# Release candidate

Claude may tag, push and test **release candidates** without asking. A **stable** tag (`vX.Y.Z`) needs the user's
explicit OK first (step 11). Details: docs/RELEASING.md. In Git Bash use `"/c/Program Files/GitHub CLI/gh.exe"` for
`gh`; `<repo>` and `<ota_site>` come from `forge.json`.

1. **Number:** last tag `git describe --tags --abbrev=0`. Same series → next `-rc.N`; a series with new features
   starts at `vX.(Y+1).0-rc.1` (L17). A tag whose build failed is never reused: take the next N.
2. **CHANGELOG:** `CHANGELOG.md` has `## vX.Y.Z-rc.N - <today>` at the top, one `- ` line per change, written for the
   person holding the device (L16). Add and commit it if missing ("Changelog: vX.Y.Z-rc.N notes").
3. **Clean tree:** `version.txt` deleted. Other work in progress? `git stash push -u` (L14). `git status` shows nothing
   uncommitted.
4. **Build exactly what is committed:** `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build` must succeed.
5. **Board-free tests:** C files compiled by `tests/host` changed since the last tag (`git diff --stat <last-tag>`)?
   `wsl make -C tests/host` (L15). `main/web/index.html` or the mock changed? `cd tools/webtest && npm test`.
   Harness code changed? `python -m unittest discover -s tools/harness -p "test_*.py"` (from an ESP-IDF shell).
6. **Push:** `git fetch`; `git status` must not say "behind"; `git push origin main`; `git tag vX.Y.Z-rc.N`
   (lightweight); `git push origin vX.Y.Z-rc.N`. Then `git stash pop` if you stashed.
7. **Watch CI:** `gh run list --repo <repo> --limit 5` (the tag's run shows the tag as its branch), then
   `gh run watch <id> --repo <repo> --exit-status` in the background. On failure: `gh run view <id> --repo <repo>
   --log-failed`, fix, and go back to step 1 with the next N.
8. **Pages:** `curl -s <ota_site>channels.json` lists the rc as `beta` (~5 min after the tag). The `pages` job fails
   until a first stable release exists: that is expected for a new project.
9. **Install and test on the device:** the device must be on the Beta channel and run a version below the rc (a test
   build labelled above it is never offered it, L9). Note to the user ("Installing vX over Wi-Fi and running every
   suite, ~20 min"), then `python tools/harness/harness.py --ota vX.Y.Z-rc.N`. Relay any `>>> ASK THE USER` at once.
10. **Report:** CI result, release URL (`gh release view vX.Y.Z-rc.N --repo <repo>`), the harness's "Testing vX" and
    summary lines, notes and skips. Ask the user to try the change on their device if it is about feel.
11. **Stable (only after the user says yes):** propose the number (features → minor) and list what is in it; on OK,
    write `## vX.Y.Z - <today>` repeating the rc lines in user words, commit, repeat steps 3-9 with `vX.Y.Z`
    (`git push origin main vX.Y.Z`), check `channels.json` `stable`, report.

Never: `git push --force`, moving or deleting a tag, `gh release create` by hand (CI makes releases), renaming the
repository.
