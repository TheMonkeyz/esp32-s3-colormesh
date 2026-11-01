// slide_slots.c: which neighbour slot a picture goes in (forge_lvgl's slide.c, MeteoBus's rules): empty first, then
// an unwanted page (the least recently used), a wanted one only when worth less than the new picture or when forced;
// with no room for another buffer, a less useful picture's buffer.
#include "check.h"
#include "slide_slots.h"

int main(void)
{
    const char *A = "A", *B = "B", *C = "C", *D = "D", *X = "X";
    const void *want[] = { A, B, C };                                  // worth 0, 1, 2; others 3; empty 4
    slide_slot_t s[4] = { { true, A, 10 }, { true, X, 20 }, { false, NULL, 0 }, { true, B, 30 } };
    CHECK(slide_slot_worth(&s[0], want, 3) == 0 && slide_slot_worth(&s[3], want, 3) == 1, "worth: the place in want");
    CHECK(slide_slot_worth(&s[1], want, 3) == 3 && slide_slot_worth(&s[2], want, 3) == 4, "unwanted: n; empty: n + 1");

    CHECK(slide_slot_pick(s, 4, want, 3, false, 2) == 2, "an empty slot first");
    s[2] = (slide_slot_t){ true, D, 5 };                               // D unwanted, oldest
    CHECK(slide_slot_pick(s, 4, want, 3, false, 2) == 2, "then an unwanted page, the least recently used (D@5 before X@20)");
    s[2].used = 25;
    CHECK(slide_slot_pick(s, 4, want, 3, false, 2) == 1, "ties among unwanted: the least recently used (X@20)");
    CHECK(slide_slot_pick(s, 4, want, 3, false, 3) == 1, "an unwanted new picture still takes an unwanted slot");

    slide_slot_t t[2] = { { true, A, 10 }, { true, C, 20 } };          // all wanted: A worth 0, C worth 2
    CHECK(slide_slot_pick(t, 2, want, 3, false, 1) == 1, "B (worth 1) takes C's slot: C is worth less");
    slide_slot_t u[2] = { { true, A, 10 }, { true, B, 20 } };
    CHECK(slide_slot_pick(u, 2, want, 3, false, 2) == -1, "C (worth 2) takes nobody's: A and B are worth more");
    CHECK(slide_slot_pick(u, 2, want, 3, false, 1) == -1, "the same worth is not less: B's twin takes nothing");
    CHECK(slide_slot_pick(u, 2, want, 3, true, 2) == 1, "forced (a drag needs it now): the one worth least, B");
    const void *all[] = { A, B, C, D };
    slide_slot_t v[4] = { { true, A, 1 }, { true, B, 2 }, { true, C, 3 }, { true, D, 4 } };
    CHECK(slide_slot_pick(v, 4, all, 4, false, 4) == -1, "an unwanted picture never evicts a wanted one");

    slide_slot_t r[3] = { { true, A, 10 }, { true, X, 20 }, { false, B, 30 } };
    CHECK(slide_slot_reuse(r, 3, want, 3, 1) == 1, "no room: the unwanted X's buffer (B's slot has none)");
    slide_slot_t q[2] = { { true, A, 10 }, { true, B, 20 } };
    CHECK(slide_slot_reuse(q, 2, want, 3, 2) == -1, "no room, all worth more: nothing reused");
    CHECK(slide_slot_reuse(q, 2, want, 3, 0) == 1, "no room, the new one worth most: B's buffer (worth 1 > 0)");
    slide_slot_t e[2] = { { true, NULL, 10 }, { true, X, 5 } };
    CHECK(slide_slot_reuse(e, 2, want, 3, 2) == 0, "an empty slot with a buffer before an unwanted page");
    CHECK(slide_slot_pick(NULL, 0, want, 3, true, 0) == -1 && slide_slot_reuse(NULL, 0, want, 3, 0) == -1, "no slots");
    return check_done("slide_slots");
}
