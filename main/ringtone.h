#pragma once
#include <stddef.h>
#include <stdint.h>

// The call tone: the short phrase of Tárrega's Gran Vals (1902) known as a phone ring, in the old phones' buzzer
// sound (timing and timbre measured from a recording, ringtone.c): legato notes whose partials follow the buzzer's
// response, with a 2 ms fade in and out (no clicks). Rendered in blocks so the player streams it. The same synth
// plays the caller's "answered" sound (a rising doo-di-dit, the user's pick of ten, 2026-11-01). Pure C99 with libm,
// host-tested.
#define RINGTONE_RATE 16000
#define RINGTONE_PARTIALS 16                // the most partials of one note (C#5: 14 below 7.8 kHz)

typedef struct {
    uint16_t freq_hz;                       // 0 = a rest
    uint16_t ms;
} ringtone_note_t;

extern const ringtone_note_t ringtone_notes[];   // the ring phrase
extern const int ringtone_n_notes;
extern const ringtone_note_t ringtone_answered[];   // A5 C#6 E6: a call was answered
extern const int ringtone_n_answered;
uint32_t ringtone_samples(void);            // one ring phrase, at RINGTONE_RATE

typedef struct {
    const ringtone_note_t *seq;             // the notes played
    int n_seq;
    uint32_t pos;                           // sample within the sequence
    float phase;                            // of the note's fundamental
    int note, np;                           // the note set up, its partial count
    float w[RINGTONE_PARTIALS];             // its partials' levels
} ringtone_t;

void ringtone_init(ringtone_t *t);          // the ring phrase
void ringtone_init_seq(ringtone_t *t, const ringtone_note_t *seq, int n);
uint32_t ringtone_seq_samples(const ringtone_note_t *seq, int n);
// The next n mono samples at RINGTONE_RATE, amplitude up to `amp`; returns how many were written (< n at the end)
size_t ringtone_render(ringtone_t *t, int16_t *out, size_t n, int16_t amp);
