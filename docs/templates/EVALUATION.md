# Project evaluation, <date> (<version>, commit <hash>)

<!-- A read-only review: nothing changed, built, flashed or sent to the board. Say who reviewed and how (areas in
parallel; two verifiers per medium/high finding: one tries to refute it, one judges its severity for this project; a
completeness critic), and how many passes. The companion FIX-PLAN-<date>.md turns this into work. -->

**Coverage.** <!-- Every area reviewed (files per area), plus cross-cutting passes: security, robustness sweep,
product/UX, docs accuracy, runtime evidence (harness reports, logs on disk). -->

**Verification.** <!-- N issues found; M rated medium or high were verified: confirmed / refuted; how many the impact
judge moved. Line numbers are as of the commit above. -->

## Verdict

<!-- A paragraph on overall shape, then the weaknesses grouped in a few clusters, two or three sentences each. -->

## Strengths worth keeping

<!-- What the next changes must not break, one bullet per area. -->

## Findings

<!-- High = users hit it or a release risk. Medium = real but narrow, or debt that will bite. -->

### High

| # | Where | Finding |
|---|---|---|

### Medium (confirmed)

| # | Where | Finding |
|---|---|---|

### Confirmed, lowered to low

### Unverified low / info, by area

## The critic's assessment

<!-- Is the picture right? What is it skewed by (e.g. static reads only)? What did the runtime evidence answer? -->

## Open questions for the user

<!-- Decisions only the owner can make, numbered; say which findings depend on each. -->

1.
