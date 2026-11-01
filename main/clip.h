#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "calls.h"

// Voice clips and calls (docs/ARCHITECTURE.md "Voice"). Record up to CLIP_REC_MAX_MS from the microphones, then send
// the clip to every board in range at once: IMA ADPCM, cut into chunks (clip_chunks.h) and repeated CLIP_ROUNDS times
// on a BLE 5 periodic advertising train (mesh.c instance 1). A board that hears MP_F_CLIP with a new clip id syncs to
// the sender's train, collects the chunks and plays the clip as soon as it is whole. A call (MP_EFFECT_RING, through
// the flood) makes every other board ring with ringtone.c until it is tapped or has rung CLIP_RING_TIMES times.
// The UI and the test console call these from any task; the audio and the radio work run in the clip task.
#define CLIP_REC_MAX_MS 5000
#define CLIP_ROUNDS     3
#define CLIP_RING_TIMES 3

void clip_start(void);                      // after mesh_start(): opens the audio, starts the clip task

void clip_rec_begin(void);                  // the finger went down on the voice page
void clip_rec_end(bool send);               // it went up (send) or the press was lost (!send: the clip is dropped)
bool clip_send_last(void);                  // send the last recorded clip again (console)
bool clip_play_last(void);                  // play the last recorded clip here (console)
void clip_ring(void);                       // call the others ("Calling" here until one answers or they stop ringing)
void clip_call_cancel(void);                // close the outgoing call's screen (a tap on it)
void clip_ring_local(void);                 // ring here (console: try the tone)
void clip_ring_stop(bool answer);           // stop ringing; answer: tell the caller (a tap on the screen)

typedef enum { CLIP_IDLE, CLIP_REC, CLIP_SENDING, CLIP_PLAYING, CLIP_RINGING } clip_phase_t;
typedef enum { CLIP_CALL_NONE, CLIP_CALL_OUT, CLIP_CALL_ANSWERED } clip_call_t;   // this board's outgoing call
typedef struct {
    clip_phase_t phase;
    uint32_t rec_ms;                        // CLIP_REC: recorded so far
    int send_pct;                           // CLIP_SENDING: 0..100
    uint8_t r, g, b;                        // CLIP_PLAYING / CLIP_RINGING: the sender's colour
    bool receiving;                         // a peer's clip is being collected
    clip_call_t call;
    int n_ans;                              // the boards that answered this board's call, in order
    call_ans_t ans[CALL_MAX_ANS];
} clip_state_t;
void clip_get_state(clip_state_t *out);
