#pragma once
// Rock paper scissors over the mesh (the "game" page): one round at a time, identified by (starter origin, seq).
// A tap starts a round on this board: the reveal is RPS_COUNT_MS later. The advert carries the round (bytes 22-25,
// mesh_proto.h) with the time left to the reveal, so a board that hears it joins with the same reveal time (within
// one RPS_LEFT_UNIT_MS step plus the radio delay). Each player picks before the reveal (and may change it until
// then); RPS_COLLECT_MS after the reveal the picks heard decide the result, shown for RPS_RESULT_MS. Two rounds
// started at once: the one of the lower starter origin wins on every board. Pure C99, no ESP-IDF, time always an
// argument (tests/host/test_rps.c).
#include <stdbool.h>
#include <stdint.h>

typedef enum { RPS_NONE = 0, RPS_ROCK = 1, RPS_PAPER = 2, RPS_SCISSORS = 3 } rps_pick_t;
typedef enum { RPS_IDLE, RPS_COUNT, RPS_COLLECT, RPS_RESULT } rps_phase_t;
typedef enum { RPS_OUT_NONE, RPS_WON, RPS_LOST, RPS_DRAW, RPS_LATE } rps_outcome_t;

#define RPS_COUNT_MS     3000     // tap to reveal
#define RPS_COLLECT_MS   600      // after the reveal: the last picks still on their way (adverts every 20 ms)
#define RPS_RESULT_MS    4000     // the result on screen
#define RPS_ADVERT_MS    5000     // the round stays in the advert this long after the reveal (left 0, the pick)
#define RPS_LEFT_UNIT_MS 100      // byte 24's unit

typedef struct {
    bool used;                    // a round id is known (running or over): its reports are not joined again
    bool done;                    // its result phase is over (rps_tick)
    uint8_t starter, seq;
    uint8_t own;                  // this board's pick (rps_pick_t)
    uint32_t reveal_ms;
} rps_round_t;

void rps_init(rps_round_t *r);
// This board starts round (own_origin, seq; seq != 0). The pick is cleared. False (nothing changes) while a round
// counts down or collects: a tap then is a pick, not a new round.
bool rps_start(rps_round_t *r, uint8_t own_origin, uint8_t seq, uint32_t now_ms);
// A peer's round bytes. True when this board takes that round: a round id not known yet with left > 0, unless this
// board counts down a round of a lower starter. A switch during a countdown keeps the pick; otherwise it is cleared.
bool rps_join(rps_round_t *r, uint8_t starter, uint8_t seq, uint8_t left, uint32_t now_ms);
// The own pick (RPS_ROCK..RPS_SCISSORS), only during the countdown. False otherwise.
bool rps_pick(rps_round_t *r, uint8_t pick, uint32_t now_ms);
rps_phase_t rps_phase(const rps_round_t *r, uint32_t now_ms);
// Marks the round done once its result phase is over (so a clock that wraps 49 days later never revives it).
// Call it at least once a second. True when it just became done.
bool rps_tick(rps_round_t *r, uint32_t now_ms);
// The advert bytes for now: false (send zeros) when there is no round or its RPS_ADVERT_MS are over. left: time to the
// reveal in RPS_LEFT_UNIT_MS steps rounded up (1..255) during the countdown, 0 after.
bool rps_advert(const rps_round_t *r, uint32_t now_ms, uint8_t *starter, uint8_t *seq, uint8_t *left, uint8_t *pick);
uint32_t rps_ms_left(const rps_round_t *r, uint32_t now_ms);   // to the reveal, 0 once it is past

bool rps_beats(uint8_t a, uint8_t b);                          // rock > scissors > paper > rock
// own: this board's pick; picks: the peers' (RPS_NONE entries are ignored). No own pick: RPS_LATE. One kind, or all
// three: RPS_DRAW. Two kinds: RPS_WON for the one that beats the other.
rps_outcome_t rps_outcome(uint8_t own, const uint8_t *picks, int n);
const char *rps_pick_name(uint8_t pick);                       // "none", "rock", "paper", "scissors", "?"
uint8_t rps_pick_from_name(const char *name);                  // the reverse; RPS_NONE for anything else
const char *rps_outcome_name(rps_outcome_t o);                 // "none", "won", "lost", "draw", "late"
