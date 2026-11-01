// The call tone (ringtone.h). Timing and timbre measured from a recording of the old phone (2026-11-01): an eighth
// is 117 ms, a quarter 242, the last note 813 ms; the notes run into each other (legato), and the last one stops
// sharply (only the 2 ms anti-click fade: the user's choice over the recording's fade, 2026-11-01). The buzzer passes almost nothing of a note's own pitch: each partial's level depends only on its
// frequency, with two resonances near 2.9 and 5.2 kHz. So every partial k * f below PARTIAL_MAX_HZ is a sine at the
// level response() gives for k * f, in any note.
#include "ringtone.h"
#include <math.h>

#define E8 117
#define Q4 242
#define LAST 813
#define FADE 32                             // samples of fade in and out per note (2 ms): no click between notes
#define PARTIAL_MAX_HZ 7800                 // under the 8 kHz Nyquist limit: nothing folds back as a rasp
#define RMS 0.3f                            // each note's level, of full scale (the recording's is flat at ~0.3)

const ringtone_note_t ringtone_notes[] = {
    { 1319, E8 }, { 1175, E8 }, { 740, Q4 }, { 831, Q4 },     // E6 D6 F#5 G#5
    { 1109, E8 }, { 988, E8 }, { 587, Q4 }, { 659, Q4 },      // C#6 B5 D5 E5
    { 988, E8 }, { 880, E8 }, { 554, Q4 }, { 659, Q4 },       // B5 A5 C#5 E5
    { 880, LAST },                                            // A5
};
const int ringtone_n_notes = sizeof(ringtone_notes) / sizeof(ringtone_notes[0]);

const ringtone_note_t ringtone_answered[] = { { 880, 90 }, { 1109, 90 }, { 1319, 140 } };   // A5 C#6 E6
const int ringtone_n_answered = sizeof(ringtone_answered) / sizeof(ringtone_answered[0]);

// The buzzer's response: a partial's amplitude in the recording, every 250 Hz from 625 Hz (measured, the 3125 Hz
// bin filled between its neighbours)
static const float resp[] = {
    0.0014f, 0.0044f, 0.0092f, 0.0161f, 0.0272f, 0.0526f, 0.0981f, 0.1342f, 0.1907f, 0.2097f, 0.1680f, 0.1256f,
    0.1312f, 0.1171f, 0.1205f, 0.1684f, 0.1722f, 0.3128f, 0.2602f, 0.3009f, 0.2159f, 0.0837f, 0.0457f, 0.0179f,
    0.0267f, 0.0269f, 0.0225f, 0.0110f, 0.0070f, 0.0071f,
};
#define RESP_N  (int)(sizeof(resp) / sizeof(resp[0]))
#define RESP_F0 625.0f
#define RESP_DF 250.0f

static float response(float hz)
{
    float x = (hz - RESP_F0) / RESP_DF;
    if (x <= 0) return resp[0];
    if (x >= RESP_N - 1) return resp[RESP_N - 1];
    int i = (int)x;
    return resp[i] + (resp[i + 1] - resp[i]) * (x - i);
}

static uint32_t len_of(const ringtone_note_t *n) { return (uint32_t)n->ms * RINGTONE_RATE / 1000; }

uint32_t ringtone_seq_samples(const ringtone_note_t *seq, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += len_of(&seq[i]);
    return s;
}

uint32_t ringtone_samples(void) { return ringtone_seq_samples(ringtone_notes, ringtone_n_notes); }

void ringtone_init_seq(ringtone_t *t, const ringtone_note_t *seq, int n)
{
    t->seq = seq;
    t->n_seq = n;
    t->pos = 0;
    t->phase = 0;
    t->note = -1;
}

void ringtone_init(ringtone_t *t) { ringtone_init_seq(t, ringtone_notes, ringtone_n_notes); }

// A new note: its partials' levels, scaled so that the note's RMS is RMS
static void note_setup(ringtone_t *t, int i)
{
    float f = t->seq[i].freq_hz, sq = 0;
    t->note = i;
    t->np = 0;
    for (int k = 1; k <= RINGTONE_PARTIALS && k * f < PARTIAL_MAX_HZ; k++) {
        t->w[t->np] = response(k * f);
        sq += t->w[t->np] * t->w[t->np] / 2;
        t->np++;
    }
    float g = sq > 0 ? RMS / sqrtf(sq) : 0;
    for (int k = 0; k < t->np; k++) t->w[k] *= g;
}

size_t ringtone_render(ringtone_t *t, int16_t *out, size_t n, int16_t amp)
{
    const float two_pi = 6.2831853f;
    size_t done = 0;
    while (done < n) {
        uint32_t start = 0;
        int i = 0;
        while (i < t->n_seq && t->pos >= start + len_of(&t->seq[i])) start += len_of(&t->seq[i++]);
        if (i == t->n_seq) break;                   // the sequence is over
        if (i != t->note) note_setup(t, i);
        uint32_t len = len_of(&t->seq[i]), at = t->pos - start;
        float g = 1.0f;
        if (at < FADE) g = (float)at / FADE;
        else if (len - at <= FADE) g = (float)(len - at) / FADE;
        float s = 0.0f;
        for (int k = 0; k < t->np; k++) s += t->w[k] * sinf((k + 1) * t->phase);
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        out[done++] = (int16_t)(s * g * amp);
        t->phase += two_pi * t->seq[i].freq_hz / RINGTONE_RATE;
        if (t->phase >= two_pi) t->phase -= two_pi;
        t->pos++;
    }
    return done;
}
