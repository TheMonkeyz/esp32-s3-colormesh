// dl_gate.c: one TLS download at a time (forge_net net_dl_begin/end): taken, refused while held, freed only by its
// holder, the holder's name for "wifi status".
#include <string.h>
#include "check.h"
#include "dl_gate.h"

int main(void)
{
    dl_gate_t g = { 0 };
    CHECK(!dl_gate_busy(&g) && !strcmp(dl_gate_holder(&g), "none"), "free at start: %s", dl_gate_holder(&g));
    CHECK(dl_gate_take(&g, "probe", 1000), "the first taker gets it");
    CHECK(dl_gate_busy(&g) && !strcmp(dl_gate_holder(&g), "probe") && g.since_us == 1000, "held by probe since 1000");
    CHECK(!dl_gate_take(&g, "map", 2000), "a second taker is refused");
    CHECK(!dl_gate_take(&g, "probe", 2000), "the holder itself is refused too (no recursion)");
    CHECK(g.taken == 1 && g.refused == 2, "counters: taken %lu refused %lu", (unsigned long)g.taken, (unsigned long)g.refused);
    dl_gate_give(&g, "map");
    CHECK(dl_gate_busy(&g), "another name can't free it");
    dl_gate_give(&g, "probe");
    CHECK(!dl_gate_busy(&g) && !strcmp(dl_gate_holder(&g), "none"), "freed by its holder");
    CHECK(dl_gate_take(&g, "map", 3000) && !strcmp(dl_gate_holder(&g), "map"), "the next taker gets it");
    dl_gate_give(&g, NULL);
    dl_gate_give(NULL, "map");
    CHECK(dl_gate_busy(&g), "NULLs are ignored");
    CHECK(!dl_gate_take(NULL, "x", 0) && !dl_gate_take(&g, NULL, 0), "NULL gate or name: refused");
    return check_done("dlgate");
}
