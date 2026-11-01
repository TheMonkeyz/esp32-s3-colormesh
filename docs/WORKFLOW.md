# Workflow

How a change goes from an idea to a release on the devices, with Claude doing most of the work and the user
deciding. This is the loop weather_amoled settled on after four days and ~30 releases (its docs/HISTORY.md, "How the
work is done now"). Commands are in [TESTING.md](TESTING.md) and [RELEASING.md](RELEASING.md); the reasons behind
the rules are in [LESSONS.md](LESSONS.md).

## The loop

```
  change ──► test build (label above the release) ──► flash + probe ──► prove on the device
     ▲                                                                        │
     │                                                                        ▼
  fix plan ◄── evaluation          release candidate ◄── document and commit ◄── the user tries it
                                         │
                                         ▼
                                harness --ota vX.Y.Z-rc.N ──► stable (only with the user's OK)
```

### 1. Change, then a test build

- Work on `main` unless the user asks for a branch.
- Label the build above the current release: `version.txt` holds e.g. `v0.2.0-graph.3` (git-ignored), then touch
  `CMakeLists.txt` (it is read at configure time). For the same X.Y.Z a test label counts below any rc, so a board on
  `v0.2.0-graph.3` is offered `v0.2.0-rc.1`, while `v0.2.1-graph.3` would hide it (L9).
- Build in `build\v55` with its own sdkconfig (`forge.json` `build_dir`):
  `idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build`.
- Settings page changed? `cd tools/webtest && npm test` and look at `tools/webtest/shots/` before building firmware.
- A C file the host tests compile changed? `wsl make -C tests/host`.

### 2. Flash and probe

- Post a one-line note first ("Flashing v0.2.0-graph.3, then a 120 s log").
- `python tools/harness/harness.py --flash build/v55/<app>.bin <suites>` flashes through the devloop helper and runs
  suites; or stage and flash by hand (`tools/devloop/stage.py`, `devloop.py flash 120`) and act through the test
  console (`devloop.py send "screen"`).
- Read `ota: Running … from ota_N` in the fresh log before anything else (L2).
- For "why is X slow": time the parts in a throwaway build with temporary logging (L53, L54). For hangs:
  breadcrumbs and `where` (L52).

### 3. Prove it on the device

- Behaviour: the harness's suites, or a probe script in the scratchpad that imports `Board` / `Log` from
  `tools/harness/board.py`.
- Looks: `python tools/snapshot.py <screen> [out.png] --ip <ip>` for every screen the change touches, in every language.
- Speed and memory: the harness's `perf` against `tools/harness/baseline.json`.
- Every fix adds a check (harness test, Playwright test or host test) that fails on the old code.

### 4. The user tries it

Synthetic touches and phone-less tests miss things (L25). Ask the user to try it during a log window and say exactly
what to do and when: "For the next 60 s: swipe left twice, then long-press the centre." Their words ("follow the
finger", "a delay before it moves") are the most useful signal you will get; turn each into a measurement.

### 5. "Document and commit"

When the user says it (often "document and commit, then release rc"):

| File | What goes in |
|---|---|
| `docs/ARCHITECTURE.md` | How it works now, and why that way (the alternative that failed, with numbers). |
| `docs/TESTING.md` (project section) or the harness | How to check it again. |
| `CLAUDE.md` | The bug (symptom, cause, fix, test), a new user preference, a useful fact. A general lesson goes to `docs/LESSONS.md`. |
| `docs/DIAGNOSTICS.md` | New reference numbers, if memory or timing changed. |
| `CHANGELOG.md` | One line per change for the person holding the device, no jargon (L16). |

Then delete `version.txt`, build the tree exactly as it will be committed (L14: `git stash push -u` sets other work
aside), commit. Usual shape: one code + docs commit, then one "Changelog: vX notes" commit.

### 6. Release candidate, then stable

Claude tags, pushes and tests release candidates (`.claude/skills/release-rc`): CI builds, the Pages site moves the
Beta channel, `python tools/harness/harness.py --ota vX.Y.Z-rc.N` installs it with the device's own updater and runs
every suite with `--expect`. Report the result. A **stable** tag needs the user's OK; say what is in it when asking.
Details: [RELEASING.md](RELEASING.md).

## The evaluation → fix-plan cycle

Every few releases (or when the user asks), a read-only review finds what the loop missed. weather_amoled's first one
found 183 issues, one shipped bug among them; its fix plan became v1.12.0-rc.1 to rc.8, worked through in one
session, then shipped as v1.12.0.

1. **Evaluate** (`.claude/skills/evaluate`): read code, docs, the latest harness reports and logs. Change nothing,
   build nothing, flash nothing. Split by area; verify every medium/high finding twice (one reader tries to refute it,
   one judges its severity for this project); a critic checks coverage. Write
   `docs/EVALUATION-<YYYY-MM-DD>.md` from `docs/templates/EVALUATION.md`.
2. **Plan**: `docs/FIX-PLAN-<YYYY-MM-DD>.md` from `docs/templates/FIX-PLAN.md`. Each work item is self-contained
   (where, what to change, how to prove it, the changelog line), sized S/M/L, grouped by value; **one release
   candidate per group**. Questions that only the user can answer are listed apart and asked at the moment they
   block, not up front (L47).
3. **Work the plan** in a fresh session, group by group, through the loop above. Tick items off in the plan file;
   when done, a line in `docs/HISTORY.md`.

## Two ways to work

### Claude Code on the PC (preferred)

Claude runs in the project folder on the Windows PC: `idf.py`, Python, `gh`, WSL and the board's IP are all reachable.
The devloop flash helper owns the COM port while logging; the harness and `devloop.py` talk to it through
`.devloop/` (docs/PROTOCOL.md §1). Claude starts the helper itself in the background, windowless
(`run_in_background`: `powershell -NoProfile -ExecutionPolicy Bypass -File tools\devloop\flash_helper.ps1`): nothing opens on the
user's screen. If the user prefers, Claude may also flash with `idf.py -p COM5 flash` while the
helper isn't running; the helper is still the way to keep a log window open while acting on the board.
Before `idf.py`, set `$env:IDF_COMPONENT_CACHE_PATH = "$env:USERPROFILE\.espressif\cm_cache"`: the desktop app's
shell redirects `AppData\Local` for some child processes, and the component manager's git cache breaks there on the
first build that fetches a new tag (L199).

### Claude desktop app / cloud (no USB)

Builds happen in a cloud container; Claude's shell on the PC is a Linux VM with no USB access, and it can't type into
Windows terminals or reach the board's IP.

1. The user starts `tools\devloop\start_flash_helper.bat` once (restart it after editing `flash_helper.ps1`; on a
   Mac `python tools/devloop/flash_helper.py`, docs/MACOS.md).
2. Claude builds in the cloud, copies the parts to the PC folder, runs `python tools/devloop/stage.py` (unique names,
   md5), then `python tools/devloop/devloop.py flash 120` (or writes `.devloop/flash.request` with the log seconds).
3. Claude polls `.devloop/flash.status` and `.devloop/flash.done`, then reads `.devloop/serial_log.txt`. Stop early
   with `devloop.py stop`.
4. No snapshots or harness from here: ask the user for a photo, or switch to Claude Code on the PC.

Cloud build recipe (network allowlists there blocked PyPI, dl.espressif.com, Docker Hub and the component registry;
GitHub worked):

```bash
git clone --depth 1 --branch v5.5.4 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
./install.sh esp32s3            # toolchains download from GitHub; the python-env step fails, which is OK
python3 -m venv --system-site-packages idfenv && . idfenv/bin/activate
pip install --no-deps git+https://github.com/pyserial/pyserial.git@v3.5 \
  git+https://github.com/tomerfiliba-org/reedsolomon.git@v1.7.0 \
  git+https://github.com/python-intelhex/intelhex.git@2.3.0 \
  git+https://github.com/espressif/esptool.git@v4.8.1 \
  git+https://github.com/espressif/esp-idf-kconfig.git@v2.5.0
export IDF_PATH=... IDF_COMPONENT_MANAGER=0 PATH=<xtensa-esp-elf/bin>:$PATH
cmake -S . -B build -G Ninja -DIDF_TARGET=esp32s3 -DPYTHON=$(which python) -DPYTHON_DEPS_CHECKED=1
ninja -C build
```

With the component manager off, every dependency in `main/idf_component.yml` must be vendored under `components/`
at the pinned version (clone from GitHub). Keep `IDF_COMPONENT_MANAGER=0` in the environment: a changed sdkconfig
re-runs CMake (L20). Git on the PC folder from the VM: `core.fileMode false`, `core.autocrlf false` (L150). `.github/`
can't be written from there (L149).

## Talking to the user

- A one-line note before anything over a minute; long silences read as "stuck".
- Ask before taking over their screen; never type into their terminals.
- During a log window, say exactly when to act and what to do.
- Relay `>>> ASK THE USER` from the harness immediately (Easy Connect scans time out).
- Report what the log says, with the line, not an interpretation of a status message.
- Restore any setting a test changed (language, delays, channel).
