#pragma once
#include <stdbool.h>
#include <stdint.h>

// One TLS download at a time (net.h: net_dl_begin / net_dl_end): the bookkeeping alone, pure C and host-tested
// (tests/host/test_dlgate.c); the waiting and the locking are net.c's. Each TLS connection holds 10-15 KB of internal
// RAM, and two at once took an app's internal low point from ~40 to 29 KB (MeteoBus's bus poller beside its radar).
typedef struct {
    const char *who;            // the holder's name ("probe", "update", "map", an app's); NULL = free
    int64_t since_us;           // when it was taken
    uint32_t taken, refused;    // counters for the log
} dl_gate_t;

bool dl_gate_take(dl_gate_t *g, const char *who, int64_t now_us);   // true: g is who's now; false: held by another
void dl_gate_give(dl_gate_t *g, const char *who);                   // only the holder frees it
bool dl_gate_busy(const dl_gate_t *g);
const char *dl_gate_holder(const dl_gate_t *g);                     // "none" when free
