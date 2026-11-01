---
name: harness
description: Run the device test harness (tools/harness/harness.py) to test the board end to end without a person, read its report, and update the performance baseline. Use before calling a change done, before and after a release, and when adding a test for a fixed bug.
---

# Run the harness

Details: docs/TESTING.md §5. Rules: docs/LESSONS.md L21-L38.

1. **Preconditions:** the flash helper is running (`python tools/devloop/devloop.py status`), nobody else is using
   the board, the firmware has the test console.
2. **Note to the user** with the expected time (all suites: ~10-15 min; one suite: 1-3 min).
3. **Run**, one of:
   - a fresh build: `python tools/harness/harness.py --flash` (forge.json's build_dir; `--flash build/other` for another)
   - what is on the board: `python tools/harness/harness.py`
   - some suites: `python tools/harness/harness.py memory web` (or `--suite memory --suite web`; the names are
     TESTING.md §5's table)
   - a release: `--expect vX.Y.Z-rc.N` (or `--ota vX.Y.Z-rc.N`, see the `release-rc` skill)
   - Easy Connect with a phone: `wifi_setup --phone`
   Run it in the background for long runs and watch its output.
4. **`>>> ASK THE USER`** in the output (or `.devloop/harness.ask`): relay it to the user at once, word for word.
5. **Read the report:** `tools/harness/reports/<date>/report.md`. First the "Testing vX" line (version and flash mode:
   is it the build you meant?), then FAIL / REGRESSION / MISSING / NEW lines, notes (fallback warnings), skipped
   items with their reasons. Failed tests have their log saved next to the report.
6. **A failure is a finding, not noise:** read its log; an unexpected restart has a decoded backtrace (valid only if
   `build_dir` holds the ELF of the build on the board). An outside outage is noted, not failed. A flaky check gets
   several runs before you call it fixed (report n/N).
7. **New test for a fixed bug:** add it to `tools/harness/app_suites.py` (or `core_suites.py` for framework
   behaviour); confirm it fails on the old build, passes on the new. Tests keep their own log positions
   (`at = len(ctx.log.lines())`); a deliberate skip uses `ctx.skip(pattern, reason)`.
8. **Baseline update** (only for a known-good build, and say why): `--update-baseline` writes
   `tools/harness/baseline.proposed.json`; diff it with `baseline.json`, keep margins, copy it over, mention it in the
   commit.
9. **Harness code changed?** `python -m unittest discover -s tools/harness -p "test_*.py"` (ESP-IDF's Python: the
   flash helper's tests need pyserial).
10. **Report** the summary line and anything noted, quoting the report.
