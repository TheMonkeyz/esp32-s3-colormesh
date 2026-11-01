# Merging two espforge apps into one firmware

MeteoBus (October 2026) merged weather_amoled and esp32-s3-rtcquebec into one display: a weather app with a radar and
a bus app with a street map, on the same Waveshare ESP32-S3 board. Flash fitted with less headroom; **memory did not fit
as the two apps were**, and the plan that made it fit is worth keeping for the next merge. The numbers are MeteoBus's
(`docs/MERGE-PLAN.md` there); the rules are general.

## What to count first

| Resource | What matters | MeteoBus |
|---|---|---|
| Flash, app slot | both apps share ESP-IDF, Wi-Fi, TLS, LVGL and espforge: the merged image is far below the sum | 2.0 + 1.8 MiB -> 2.3-2.6 MiB, in a 3 MB slot |
| Flash, other | partitions one app had (a map cache) | a 4 MB `mapcache` |
| PSRAM, 8 MB | the **low point** of each, not the average: pictures (434 KB each at 466x466), radar frames, tiles, canvases | 452 KB + 4.8 MB free alone: does not fit as is |
| Internal RAM | the tightest: each TLS download holds 10-15 KB of it; task stacks; DMA buffers | low points 38 KB and 79 KB alone |

## The plan that held

1. **One picture cache.** A single slide.c with its slots, not one per app (2.2-2.6 MB saved). In espforge, forge_lvgl
   allocates the neighbour pictures on demand and `slide_release_unneeded()` frees the pictures of pages not wanted now,
   so a memory-hungry screen on top (a map with its tiles) gets its PSRAM while it is open.
2. **Memory-hungry screens take turns.** The radar's frames and base map (4.2 MB) versus the bus map's tiles and canvas
   (2.2 MB): each is allocated when its screen opens and freed when it closes. Opening such screens by a tap, on top of
   the pager rather than as a neighbour in it, makes this natural: only one is open at a time, and slide.c never keeps a
   picture of it. The cost: the radar reloads after the bus map has been opened.
3. **One download at a time.** Each TLS connection holds 10-15 KB of internal RAM; two at once took the internal low
   point from ~40 to 29 KB. forge_net's `net_dl_begin(who, timeout_ms)` / `net_dl_end(who)` is that slot (the update
   check and install, the map tiles and the service probes take it); an app's own downloads take it too, and a poller
   checks `net_dl_busy()` before each request.
4. **No task per poller.** A task for a poller costs ~6 KB of internal RAM, and a PSRAM stack stopped a TLS task for good
   once. forge_net's background worker (`net_bg_post(fn, arg, who)`: one short-lived task, jobs one at a time inside the
   download slot) runs small network jobs; MeteoBus ran its poller's requests in the radar task's idle time instead.
5. **Smaller additions.** JSON buffers allocated per request and freed after it (128 KB of notices), not static; task
   stacks that don't do TLS in PSRAM.

## How to check

- The harness's `memory.heap_numbers` low points (`internal_min_kb`, `psram_min_kb`) after a run that opens every
  screen, against `baseline.json`; `wifi status` shows who holds the download slot (`dl=`) and the queued jobs (`bg=`).
- `diag: heap` every period, and `diag: mark` at each stage of start-up.
- A screen that allocates on open: open it, close it, read `heap` before and after: the same numbers, or a leak.

Related lessons: L185 (plain statics are internal RAM), L201 (statistics that hold interrupts off), L204 (power save
and request latency), and MeteoBus's `docs/MERGE-PLAN.md` for the whole plan, UI included.
