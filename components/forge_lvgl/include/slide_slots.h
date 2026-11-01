#pragma once
#include <stdbool.h>
#include <stdint.h>

// Which neighbour slot a picture goes in (slide.c): pure bookkeeping, host-tested (tests/host/test_slide_slots.c).
// A slot holds the picture of one page (page NULL = empty; has_buf = a 434 KB buffer is allocated). `want` lists the
// pages worth keeping now, best first (the neighbours of the pagers on view); a slot's worth is its page's place in
// want, n_want when its page isn't wanted, n_want + 1 when empty. MeteoBus's slide.c rules (v0.2.0-v0.2.1): a slot
// still needed is taken only when the new picture is worth more, or when a drag needs its picture now (force); with
// no room in PSRAM for another buffer, a less useful picture's buffer is reused.
typedef struct { bool has_buf; const void *page; uint32_t used; } slide_slot_t;

int slide_slot_worth(const slide_slot_t *s, const void *const *want, int n_want);
// The slot for a new picture of worth want_p: the least worth keeping (empty first, then unwanted pages, then wanted
// ones worth less than want_p; `force` takes a wanted one however worth). Ties: the least recently used. -1 = none.
int slide_slot_pick(const slide_slot_t *s, int n, const void *const *want, int n_want, bool force, int want_p);
// No room in PSRAM for another buffer: a slot whose buffer a picture of worth want_p may take over, the one worth
// least if worth less than want_p (an empty or unwanted one first). -1 = none.
int slide_slot_reuse(const slide_slot_t *s, int n, const void *const *want, int n_want, int want_p);
