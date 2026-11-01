// The outgoing call and its answers (calls.h)
#include "calls.h"
#include <string.h>

static bool before(uint32_t now, uint32_t t0, uint32_t ms) { return (uint32_t)(now - t0) < ms; }

void call_begin(call_t *c, uint32_t now)
{
    memset(c, 0, sizeof(*c));
    c->active = true;
    c->start_ms = now;
}

void call_cancel(call_t *c) { memset(c, 0, sizeof(*c)); }

bool call_answer(call_t *c, uint8_t origin, uint8_t r, uint8_t g, uint8_t b, uint32_t now)
{
    if (!c->active || !before(now, c->start_ms, CALL_OUT_MS)) return false;
    for (int i = 0; i < c->n; i++) if (c->ans[i].origin == origin) return false;
    if (c->n == CALL_MAX_ANS) return false;
    c->ans[c->n++] = (call_ans_t){ origin, r, g, b };
    c->last_ms = now;
    return true;
}

call_phase_t call_phase(call_t *c, uint32_t now)
{
    if (!c->active) return CALL_NONE;
    bool ringing = before(now, c->start_ms, CALL_OUT_MS);
    if (c->n && before(now, c->last_ms, ANSWERED_MS)) return CALL_ANSWERED;
    if (!ringing) {
        call_cancel(c);
        return CALL_NONE;
    }
    return c->n ? CALL_NONE : CALL_OUT;      // answered, shown, closed: a later answer opens it again
}
