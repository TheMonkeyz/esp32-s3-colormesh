#pragma once
#include <stdbool.h>
#include <stdint.h>

// This board's outgoing call and everyone who answered it (clip.c). Answers count while the callees can still ring
// (CALL_OUT_MS from the start); each answerer once; "Answered" shows until ANSWERED_MS after the latest new answer,
// so a late answer opens it again. Time is an argument (ms, wrap-safe). Pure C99, host-tested (test_voice.c).
#define CALL_MAX_ANS  8
#define CALL_OUT_MS   (3 * 2970 + 2 * 3200 + 2000)   // the callees' ring (3 phrases, 2 gaps) + a margin
#define ANSWERED_MS   4000

typedef enum { CALL_NONE, CALL_OUT, CALL_ANSWERED } call_phase_t;
typedef struct { uint8_t origin, r, g, b; } call_ans_t;
typedef struct {
    bool active;                            // a call is out (until cancelled or CALL_OUT_MS)
    uint32_t start_ms, last_ms;             // the call; the latest new answer
    int n;
    call_ans_t ans[CALL_MAX_ANS];
} call_t;

void call_begin(call_t *c, uint32_t now);
void call_cancel(call_t *c);
// An answer from origin: true if it is new for this call (the list is full: still false)
bool call_answer(call_t *c, uint8_t origin, uint8_t r, uint8_t g, uint8_t b, uint32_t now);
call_phase_t call_phase(call_t *c, uint32_t now);   // also ends the call once CALL_OUT_MS has passed
