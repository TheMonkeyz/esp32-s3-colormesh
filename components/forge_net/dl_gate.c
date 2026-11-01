// One TLS download at a time: the bookkeeping (include/dl_gate.h). No locking here: net.c wraps the calls.
#include "dl_gate.h"
#include <string.h>

bool dl_gate_take(dl_gate_t *g, const char *who, int64_t now_us)
{
    if (!g || !who) return false;
    if (g->who) { g->refused++; return false; }
    g->who = who;
    g->since_us = now_us;
    g->taken++;
    return true;
}

void dl_gate_give(dl_gate_t *g, const char *who)
{
    if (g && g->who && who && !strcmp(g->who, who)) g->who = NULL;
}

bool dl_gate_busy(const dl_gate_t *g) { return g && g->who; }

const char *dl_gate_holder(const dl_gate_t *g) { return g && g->who ? g->who : "none"; }
