---
name: evaluate
description: Run a read-only review of the whole project (firmware, tools, tests, docs, CI) and write docs/EVALUATION-<date>.md, then turn it into docs/FIX-PLAN-<date>.md with self-contained work items grouped one release candidate per group. Use when the user asks for an evaluation, an audit or a review of the project, or every few releases.
---

# Evaluate, then plan

**Read-only:** change no code, build nothing, flash nothing, send nothing to the board, push nothing. The only files
written are the two documents. Templates: `docs/templates/EVALUATION.md`, `docs/templates/FIX-PLAN.md`.

1. **Scope:** note the version and commit (`git describe --tags`, `git rev-parse --short HEAD`). List the areas:
   each component, the app's screens and data paths, the web page and API, OTA and release pipeline, tools and tests,
   docs, build configuration; plus cross-cutting passes: security, a mechanical robustness sweep (unchecked returns,
   unbounded loops, array indexes, stack use), product/UX, docs accuracy.
2. **Runtime evidence:** read the latest harness reports (`tools/harness/reports/`), logs on disk
   (`.devloop/serial_log.txt`), `docs/DIAGNOSTICS.md`. Static reads miss memory floors and stack margins.
3. **Review in parallel:** one agent per area, each returning findings with file:line, severity (high = users hit it
   or a release risk; medium = real but narrow, or debt that will bite; low/info) and evidence.
4. **Verify every medium/high finding twice:** one agent re-reads the code and tries to refute it; another judges the
   severity for *this* project. Keep the verdicts and any severity change.
5. **Critic:** one agent reads the whole set: is the picture right, what is missing, what is skewed. Cover the gaps it
   names with more readers.
6. **Write `docs/EVALUATION-<YYYY-MM-DD>.md`:** coverage and verification counts, verdict with clustered weaknesses,
   strengths worth keeping, findings tables (H1.., M1..), lowered and unverified items by area, the critic's
   assessment, **open questions for the user** (decisions only the owner can make).
7. **Write `docs/FIX-PLAN-<YYYY-MM-DD>.md`:** ground rules (the loop, labels, rc vs stable, tests for every fix), then
   work items grouped by value, **one release candidate per group**. Each item: where (file:line), the change, the test
   that fails on the old code plus the device check, a changelog line, size S/M/L, the finding ids. Items that depend
   on a user decision say which question; questions are asked when they block, not up front.
8. **Report** to the user: the verdict in a few lines, the high findings, the questions, and the two file paths. Ask
   whether to commit the documents.
