// Rock paper scissors rounds (rps.h). Pure C99.
#include "rps.h"
#include <stddef.h>
#include <string.h>

static const char *const pick_names[] = { "none", "rock", "paper", "scissors" };
static const char *const outcome_names[] = { "none", "won", "lost", "draw", "late" };

void rps_init(rps_round_t *r)
{
    if (r) memset(r, 0, sizeof(*r));
}

// now - reveal, signed: < 0 during the countdown
static int32_t since_reveal(const rps_round_t *r, uint32_t now_ms) { return (int32_t)(now_ms - r->reveal_ms); }

rps_phase_t rps_phase(const rps_round_t *r, uint32_t now_ms)
{
    if (!r || !r->used || r->done) return RPS_IDLE;
    int32_t d = since_reveal(r, now_ms);
    if (d < 0) return RPS_COUNT;
    if (d < RPS_COLLECT_MS) return RPS_COLLECT;
    if (d < RPS_COLLECT_MS + RPS_RESULT_MS) return RPS_RESULT;
    return RPS_IDLE;
}

bool rps_tick(rps_round_t *r, uint32_t now_ms)
{
    if (!r || !r->used || r->done) return false;
    int32_t d = since_reveal(r, now_ms);
    int32_t end = RPS_COLLECT_MS + RPS_RESULT_MS > RPS_ADVERT_MS ? RPS_COLLECT_MS + RPS_RESULT_MS : RPS_ADVERT_MS;
    if (d < end) return false;
    r->done = true;
    return true;
}

bool rps_start(rps_round_t *r, uint8_t own_origin, uint8_t seq, uint32_t now_ms)
{
    if (!r || !seq) return false;
    rps_phase_t ph = rps_phase(r, now_ms);
    if (ph == RPS_COUNT || ph == RPS_COLLECT) return false;
    r->used = true;
    r->done = false;
    r->starter = own_origin;
    r->seq = seq;
    r->own = RPS_NONE;
    r->reveal_ms = now_ms + RPS_COUNT_MS;
    return true;
}

bool rps_join(rps_round_t *r, uint8_t starter, uint8_t seq, uint8_t left, uint32_t now_ms)
{
    if (!r || !seq || !left) return false;
    if (r->used && r->starter == starter && r->seq == seq) return false;    // this round, running or over
    rps_phase_t ph = rps_phase(r, now_ms);
    if (ph == RPS_COUNT && starter > r->starter) return false;              // a clash: the lower starter wins
    if (ph != RPS_COUNT) r->own = RPS_NONE;
    r->used = true;
    r->done = false;
    r->starter = starter;
    r->seq = seq;
    r->reveal_ms = now_ms + (uint32_t)left * RPS_LEFT_UNIT_MS;
    return true;
}

bool rps_pick(rps_round_t *r, uint8_t pick, uint32_t now_ms)
{
    if (!r || pick < RPS_ROCK || pick > RPS_SCISSORS || rps_phase(r, now_ms) != RPS_COUNT) return false;
    r->own = pick;
    return true;
}

uint32_t rps_ms_left(const rps_round_t *r, uint32_t now_ms)
{
    if (rps_phase(r, now_ms) != RPS_COUNT) return 0;
    return (uint32_t)-since_reveal(r, now_ms);
}

bool rps_advert(const rps_round_t *r, uint32_t now_ms, uint8_t *starter, uint8_t *seq, uint8_t *left, uint8_t *pick)
{
    bool on = r && r->used && !r->done && since_reveal(r, now_ms) < RPS_ADVERT_MS;
    uint32_t ms = on ? rps_ms_left(r, now_ms) : 0;
    uint32_t steps = (ms + RPS_LEFT_UNIT_MS - 1) / RPS_LEFT_UNIT_MS;
    if (starter) *starter = on ? r->starter : 0;
    if (seq) *seq = on ? r->seq : 0;
    if (left) *left = (uint8_t)(steps > 255 ? 255 : steps);
    if (pick) *pick = on ? r->own : RPS_NONE;
    return on;
}

bool rps_beats(uint8_t a, uint8_t b)
{
    if (a < RPS_ROCK || a > RPS_SCISSORS || b < RPS_ROCK || b > RPS_SCISSORS) return false;
    return (a - b + 3) % 3 == 1;
}

rps_outcome_t rps_outcome(uint8_t own, const uint8_t *picks, int n)
{
    if (own < RPS_ROCK || own > RPS_SCISSORS) return RPS_LATE;
    bool seen[4] = { false };
    seen[own] = true;
    for (int i = 0; picks && i < n; i++)
        if (picks[i] >= RPS_ROCK && picks[i] <= RPS_SCISSORS) seen[picks[i]] = true;
    int kinds = seen[1] + seen[2] + seen[3];
    if (kinds != 2) return RPS_DRAW;
    for (uint8_t k = RPS_ROCK; k <= RPS_SCISSORS; k++)
        if (k != own && seen[k]) return rps_beats(own, k) ? RPS_WON : RPS_LOST;
    return RPS_DRAW;
}

const char *rps_pick_name(uint8_t pick) { return pick <= RPS_SCISSORS ? pick_names[pick] : "?"; }

uint8_t rps_pick_from_name(const char *name)
{
    for (uint8_t k = RPS_ROCK; name && k <= RPS_SCISSORS; k++) if (!strcmp(name, pick_names[k])) return k;
    return RPS_NONE;
}

const char *rps_outcome_name(rps_outcome_t o) { return (unsigned)o <= RPS_LATE ? outcome_names[o] : "?"; }
