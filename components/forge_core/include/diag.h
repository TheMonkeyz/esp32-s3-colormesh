#pragma once
#include <stdbool.h>
#include <stdint.h>
// Periodic diagnostics in the serial log, lines starting with "diag:" (docs/PROTOCOL.md §3, tools/diag_summary.py):
// boot info (reset reason, running partition, the last crash from the core dump, then erased), heap (internal / DMA
// / PSRAM with worst largest block), per-task CPU load and stack headroom, and whatever the hooks add (the board's
// display adds frame timing). Every failed heap allocation is counted: the harness wants it at 0.
void diag_start(int period_s);
void diag_mark(const char *stage);     // "diag: mark <stage> ..." with the heap at a boot stage
uint32_t diag_failed_allocs(void);     // heap allocations that failed since boot (any task, any size)

typedef void (*diag_hook_t)(void);     // called from the diag task after each heap line; log one "diag: ..." line
void diag_add_hook(diag_hook_t hook);
