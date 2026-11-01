// Which kept picture serves a view, and which one a new view replaces (map_priv.h fmap_slots_take). Pure C:
// tests/host/test_map.c.
#include "map_priv.h"

static bool in(int i, const int *list, int n)
{
    for (int k = 0; k < n; k++) if (list[k] == i) return true;
    return false;
}

// An empty slot first, else the least recently used, among those not protected; -1 if none
static int pick(const fmap_slots_t *t, const int *protect, int np)
{
    int best = -1;
    for (int i = 0; i < t->n; i++) {
        if (in(i, protect, np)) continue;
        if (!t->key[i].valid) return i;
        if (best < 0 || t->key[i].used < t->key[best].used) best = i;
    }
    return best;
}

int fmap_slots_take(fmap_slots_t *t, int zoom, double ox, double oy, bool *hit)
{
    int s = -1;
    *hit = false;
    for (int i = 0; i < t->n && s < 0; i++)               // kept (protected or not: returning it again is fine)
        if (t->key[i].valid && t->key[i].zoom == zoom && t->key[i].ox == ox && t->key[i].oy == oy) s = i;
    if (s >= 0) *hit = true;
    else {
        // Protected, the busy slot first in the list (pass 2 keeps only it): the task stops at its next tile, not at
        // once, and would draw the old view's tile into the new picture, which would then be kept wrong. Then the two
        // returned last: an app shows the previous zoom (scaled) until the new one is complete; with fewer than 3
        // slots only as many as leave one to reuse. If they and the busy one are every slot (3 slots, quick moves),
        // the older of them goes: a picture briefly wrong on screen rather than a wrong picture kept.
        int prot[3], np = 0, keep = t->n - 1 < 2 ? t->n - 1 : 2;
        if (t->busy1) prot[np++] = t->busy1 - 1;
        if (keep >= 1 && t->cur1) prot[np++] = t->cur1 - 1;
        if (keep >= 2 && t->prev1) prot[np++] = t->prev1 - 1;
        if ((s = pick(t, prot, np)) < 0) s = pick(t, prot, t->busy1 ? 1 : 0);
        if (s < 0) return -1;
        t->key[s] = (fmap_slot_key_t){ .zoom = zoom, .ox = ox, .oy = oy, .valid = true };
    }
    t->key[s].used = ++t->stamp;
    if (t->cur1 != s + 1) { t->prev1 = t->cur1; t->cur1 = s + 1; }
    return s;
}
