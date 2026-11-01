# Diagnostics and performance

<!-- How to measure this firmware again, and the reference numbers to compare with. The diag: lines come from
forge_core (docs/PROTOCOL.md §3); tools/diag_summary.py summarises them. -->

## How to run it again

1. The flash helper is running.
2. `python tools/devloop/devloop.py reboot 300`, wait for `idle`.
3. `python tools/diag_summary.py .devloop/serial_log.txt`.
4. Scenario while it records, ~30 s per step: <!-- the app's typical use: idle, each screen, the settings page open
   on a phone, idle. -->

## What the lines mean

<!-- Only the app's own diag lines; the framework's are in docs/PROTOCOL.md. -->

## Reference numbers (<date>, <version>)

<!-- Internal RAM free / min ever, largest block, DMA, PSRAM min; boot stage times; render times per screen; fps;
per-task CPU and stack spare. Note the bootloader flash mode (QIO/DIO) they were taken with. -->

| Metric | Value | Note |
|---|---|---|

## Findings

<!-- One subsection per investigation: before / after numbers, cause, fix. -->

## Candidate optimisations (only if a symptom appears)
