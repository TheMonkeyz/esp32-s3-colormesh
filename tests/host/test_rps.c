// rps.c: who beats whom, the outcome with several players, a round's phases and advert bytes, joining a peer's round
// (same reveal time within one step), the clash rule (lower starter wins), picks only during the countdown, a known
// round never joined twice, and a clock that wraps.
#include <string.h>
#include "check.h"
#include "rps.h"

#define R RPS_ROCK
#define P RPS_PAPER
#define S RPS_SCISSORS

static void test_beats(void)
{
    CHECK(rps_beats(R, S) && rps_beats(P, R) && rps_beats(S, P), "the three wins");
    CHECK(!rps_beats(S, R) && !rps_beats(R, P) && !rps_beats(P, S), "the three losses");
    CHECK(!rps_beats(R, R) && !rps_beats(RPS_NONE, R) && !rps_beats(R, 9), "same or invalid");
}

static void test_outcome(void)
{
    uint8_t one[] = { S };
    CHECK(rps_outcome(R, one, 1) == RPS_WON, "rock vs scissors");
    CHECK(rps_outcome(S, one, 1) == RPS_DRAW, "same pick");
    uint8_t two[] = { P, P };
    CHECK(rps_outcome(R, two, 2) == RPS_LOST, "rock vs two papers");
    uint8_t mixed[] = { R, P };
    CHECK(rps_outcome(R, mixed, 2) == RPS_LOST, "rock, rock, paper: paper wins");
    CHECK(rps_outcome(P, mixed, 2) == RPS_WON, "paper vs rock, rock");
    uint8_t all[] = { P, S };
    CHECK(rps_outcome(R, all, 2) == RPS_DRAW, "all three kinds");
    uint8_t late[] = { RPS_NONE, S };
    CHECK(rps_outcome(R, late, 2) == RPS_WON, "a peer with no pick does not count");
    CHECK(rps_outcome(RPS_NONE, one, 1) == RPS_LATE, "no own pick");
    CHECK(rps_outcome(R, NULL, 0) == RPS_DRAW, "alone");
}

static void test_round(void)
{
    rps_round_t r;
    rps_init(&r);
    uint32_t t = 1000;
    CHECK(rps_phase(&r, t) == RPS_IDLE, "idle at first");
    uint8_t st, sq, left, pk;
    CHECK(!rps_advert(&r, t, &st, &sq, &left, &pk) && !st && !sq && !left && !pk, "zeros with no round");
    CHECK(!rps_pick(&r, R, t), "no pick while idle");
    CHECK(!rps_start(&r, 0x9a, 0, t), "seq 0 is not a round");
    CHECK(rps_start(&r, 0x9a, 1, t), "start");
    CHECK(rps_phase(&r, t) == RPS_COUNT && rps_ms_left(&r, t) == RPS_COUNT_MS, "countdown 3 s");
    CHECK(rps_advert(&r, t, &st, &sq, &left, &pk) && st == 0x9a && sq == 1 && left == 30 && pk == 0, "advert %u %u %u",
          st, sq, left);
    CHECK(rps_advert(&r, t + 2950, &st, &sq, &left, &pk) && left == 1, "50 ms left rounds up to 1 step: %u", left);
    CHECK(!rps_start(&r, 0x9a, 2, t + 100), "no new round during the countdown");
    CHECK(rps_pick(&r, P, t + 500) && rps_pick(&r, R, t + 2999) && r.own == R, "pick, then change it");
    CHECK(!rps_pick(&r, 7, t + 600) && r.own == R, "invalid pick ignored");
    CHECK(rps_phase(&r, t + 3000) == RPS_COLLECT && !rps_pick(&r, S, t + 3000) && r.own == R, "no pick at 0");
    CHECK(rps_advert(&r, t + 3000, &st, &sq, &left, &pk) && left == 0 && pk == R, "after 0: left 0, the pick stays");
    CHECK(!rps_start(&r, 0x9a, 2, t + 3100), "no new round while collecting");
    CHECK(rps_phase(&r, t + 3000 + RPS_COLLECT_MS) == RPS_RESULT, "result");
    CHECK(rps_phase(&r, t + 3000 + RPS_COLLECT_MS + RPS_RESULT_MS) == RPS_IDLE, "back to idle");
    CHECK(rps_advert(&r, t + 3000 + RPS_ADVERT_MS - 1, &st, &sq, &left, &pk), "advert still on");
    CHECK(!rps_advert(&r, t + 3000 + RPS_ADVERT_MS, &st, &sq, &left, &pk) && !sq, "advert off");
    CHECK(!rps_tick(&r, t + 3000 + RPS_ADVERT_MS - 1) && rps_tick(&r, t + 3000 + RPS_ADVERT_MS) && r.done, "done");
    CHECK(!rps_tick(&r, t + 9000), "done once");
    CHECK(rps_phase(&r, t + 3000 + 0x80000000u) == RPS_IDLE, "a wrapped clock never revives a done round");
    CHECK(rps_start(&r, 0x9a, 2, t + 4000) && r.own == RPS_NONE, "a tap during the result starts the next round");
}

static void test_join(void)
{
    rps_round_t a, b;
    rps_init(&a);
    rps_init(&b);
    uint32_t ta = 5000, tb = 70000;               // two boards' clocks
    CHECK(rps_start(&a, 0x76, 1, ta), "B starts");
    uint8_t st, sq, left, pk;
    rps_advert(&a, ta + 420, &st, &sq, &left, &pk);   // heard 420 ms later
    CHECK(rps_join(&b, st, sq, left, tb), "A joins");
    CHECK(rps_ms_left(&b, tb) == 2600 && rps_ms_left(&a, ta + 420) == 2580, "same reveal within a step: %u",
          (unsigned)rps_ms_left(&b, tb));
    CHECK(!rps_join(&b, st, sq, left - 3, tb + 300), "the same round is not joined twice");
    CHECK(rps_ms_left(&b, tb + 300) == 2300, "reveal unchanged");
    CHECK(!rps_join(&b, 0x76, 2, 0, tb), "left 0: nothing to join");
    CHECK(!rps_join(&b, 0x76, 0, 5, tb), "seq 0: no round");

    // A clash: two boards start at once; both end on the round of the lower origin, a pick made meanwhile stays
    rps_round_t x, y;
    rps_init(&x);
    rps_init(&y);
    rps_start(&x, 0x1a, 4, 100);
    rps_start(&y, 0x9a, 9, 100);
    rps_pick(&y, S, 120);
    CHECK(!rps_join(&x, 0x9a, 9, 30, 150), "the lower starter keeps its round");
    CHECK(rps_join(&y, 0x1a, 4, 30, 150) && y.starter == 0x1a && y.seq == 4 && y.own == S, "the higher one switches");
    CHECK(!rps_join(&y, 0x9a, 9, 29, 200), "its own old round does not come back");

    // A round heard after the own one is over: joined, and the old pick cleared
    rps_round_t z;
    rps_init(&z);
    rps_start(&z, 0x9a, 3, 0);
    rps_pick(&z, P, 10);
    CHECK(rps_join(&z, 0xff, 1, 30, 3700) && z.own == RPS_NONE, "a new round during the result");
    CHECK(rps_phase(&z, 3700) == RPS_COUNT, "counting again");
}

int main(void)
{
    test_beats();
    test_outcome();
    test_round();
    test_join();
    CHECK(rps_pick_from_name("paper") == P && rps_pick_from_name("lizard") == RPS_NONE, "names");
    CHECK(!strcmp(rps_pick_name(S), "scissors") && !strcmp(rps_outcome_name(RPS_LATE), "late"), "names back");
    return check_done("rps");
}
