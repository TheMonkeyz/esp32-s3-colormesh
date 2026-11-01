# Fix plan from the <date> evaluation

<!-- Work items for a fresh session, derived from docs/EVALUATION-<date>.md. Each item is self-contained: what is
wrong, where, what to change, how to prove it. Finding numbers (H1, M4, ...) refer to the evaluation. -->

## Ground rules for the session

- Read `CLAUDE.md`, `docs/WORKFLOW.md` and `docs/TESTING.md` first. Follow the loop: test build labelled above
  <current release> (`<next>-fix.N` in `version.txt`, touch `CMakeLists.txt`), flash, prove on the device, let the
  user try it, then "document and commit".
- Claude may tag and test rc releases (`harness.py --ota vX`); a stable release needs the user's OK. New behaviour →
  minor bump: the next stable is **<version>**.
- Every bug fix gets a test (harness, Playwright or host test) that fails on the old code. A general lesson goes to
  `docs/LESSONS.md`.
- Questions <n, m> belong to the user; items that depend on them say so. Do everything else first and ask when the
  answer blocks, not up front.

Sizes: S < 1 h, M = a few hours including device proof, L = a session. Groups are ordered by value; one release
candidate per group; items inside a group are independent unless stated.

---

## Group A: <theme> (rc.1)

### A1. <title> (<finding ids>) — S
- **Where:** <file:line>.
- **Change:** <what to do, precisely enough to do it without reading the evaluation>.
- **Test:** <the test that fails on the old code, and the device check>.
- **Changelog:** "<one line for the person holding the device>"

## Group B: <theme> (ask question <n> first; then rc.2)

## Group E: tests and CI (no rc; PC side)

## Group F: documentation and hygiene (one commit, before the next stable)

---

## Suggested order and release grouping

1. **rc.1 (Group A):** <items>. Changelog: <summary>.
2. **Ask the user** questions <n, m>. **rc.2 (Group B):** <items>.
3. **No rc (Groups E, F):** as they are done, before the stable.
