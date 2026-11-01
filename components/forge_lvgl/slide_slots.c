// Which neighbour slot a picture goes in (include/slide_slots.h): pure bookkeeping for slide.c, host-tested
#include "slide_slots.h"

int slide_slot_worth(const slide_slot_t *s, const void *const *want, int n_want)
{
    if (!s->page) return n_want + 1;
    for (int i = 0; i < n_want; i++) if (want[i] == s->page) return i;
    return n_want;
}

static bool older(const slide_slot_t *a, const slide_slot_t *b) { return (int32_t)(a->used - b->used) < 0; }

int slide_slot_pick(const slide_slot_t *s, int n, const void *const *want, int n_want, bool force, int want_p)
{
    int best = -1, best_w = -1;
    for (int i = 0; i < n; i++) {
        int w = slide_slot_worth(&s[i], want, n_want);
        if (w > best_w || (w == best_w && older(&s[i], &s[best]))) { best = i; best_w = w; }
    }
    if (best < 0) return -1;
    if (best_w < n_want && best_w <= want_p && !force) return -1;   // still wanted, worth as much or more
    return best;
}

int slide_slot_reuse(const slide_slot_t *s, int n, const void *const *want, int n_want, int want_p)
{
    int best = -1, best_w = want_p;
    for (int i = 0; i < n; i++) {
        if (!s[i].has_buf) continue;
        int w = slide_slot_worth(&s[i], want, n_want);
        if (w > best_w || (w == best_w && best >= 0 && older(&s[i], &s[best]))) { best = i; best_w = w; }
    }
    return best;
}
