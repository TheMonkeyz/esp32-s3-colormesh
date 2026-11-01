# Architecture

<!-- How the firmware works and why it works that way. For each part: what it does, the rules that came from bugs
(with the numbers), and what to check after touching it. Framework components are described in docs/COMPONENTS.md;
describe here only how this app uses them. -->

## Hardware

<!-- Board, chip, flash and PSRAM sizes; a table of parts with I2C addresses and pins; panel quirks. -->

| Part | Details | Pins |
|---|---|---|

## Tasks

<!-- Every task: core / priority, stack size (from measured high-water marks: docs/LESSONS.md L63), its job. The
locking rule for LVGL calls from outside the LVGL task. -->

| Task | Core / priority, stack | Job |
|---|---|---|

## Display pipeline

<!-- Buffers, flush, bus speed, anything that draws outside LVGL and the rules that keep it safe. -->

## Screens

<!-- One subsection per screen in forge.json "screens": what it shows, where the data comes from, gestures, what
marks it out of date. -->

## Data sources

<!-- Each outside service: URL shape, refresh period, timeouts and backoff, what the screen shows when it fails. -->

## Settings and web API

<!-- The app's routes on top of docs/PROTOCOL.md §4, NVS namespaces and keys (typed keys, not blobs: L68). -->

## Languages

<!-- Languages offered, rules for each (French = Québec, "1er"), where texts live. -->

## Updates

<!-- Partition layout, OTA site, channels, anything app-specific about rollback. -->

## Memory budget

<!-- Table of big users (where, size), measured internal RAM free / min ever, PSRAM min. -->

| Item | Where | Size |
|---|---|---|

## Known issues / TODO

<!-- One bullet each, with the version it was seen in. -->
