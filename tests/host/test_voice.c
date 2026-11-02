// The voice clip's pure parts: ADPCM round trip (signal-to-noise on a tone and on a sweep, clipping at full scale),
// chunk packing and parsing (sizes, the last short chunk, malformed packets), assembling a clip in any order with
// repeats and a foreign clip mixed in, a corrupt chunk caught by the CRC, the id wrap, and the call tone's length.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "adpcm.h"
#include "clip_chunks.h"
#include "ringtone.h"
#include "calls.h"

#define M_PI 3.14159265358979323846

static double snr_db(const int16_t *a, const int16_t *b, size_t n)
{
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        sig += (double)a[i] * a[i];
        err += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
    }
    return err == 0 ? 200 : 10 * log10(sig / err);
}

static void round_trip(const int16_t *in, int16_t *out, uint8_t *code, size_t n)
{
    adpcm_state_t e, d;
    adpcm_init(&e);
    adpcm_init(&d);
    adpcm_encode(&e, in, n, code);
    adpcm_decode(&d, code, n / 2, out);
}

static void test_adpcm(void)
{
    enum { N = 16000 };
    static int16_t in[N], out[N];
    static uint8_t code[N / 2];
    for (int i = 0; i < N; i++) in[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * i / 16000.0));
    round_trip(in, out, code, N);
    double s = snr_db(in + 400, out + 400, N - 400);   // after the step size has settled
    CHECK(s > 20, "440 Hz tone SNR %.1f dB", s);
    for (int i = 0; i < N; i++) {
        double f = 100 + 3900.0 * i / N;                // a sweep up to 4 kHz, speech range
        in[i] = (int16_t)(6000 * sin(2 * M_PI * f * i / 16000.0));
    }
    round_trip(in, out, code, N);
    s = snr_db(in + 400, out + 400, N - 400);
    CHECK(s > 12, "sweep SNR %.1f dB", s);
    for (int i = 0; i < N; i++) in[i] = (i / 50) & 1 ? 32767 : -32768;   // full-scale square: clamps, no wrap
    round_trip(in, out, code, N);
    int bad = 0;
    for (int i = 1000; i < N; i++) if (i % 50 > 25 && (in[i] > 0) != (out[i] > 0)) bad++;
    CHECK(bad == 0, "square wave keeps its sign, %d bad", bad);
    memset(in, 0, sizeof(in));
    round_trip(in, out, code, N);
    int peak = 0;
    for (int i = 0; i < N; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    CHECK(peak < 16, "silence stays silent (peak %d)", peak);
}

static void test_pack(void)
{
    enum { LEN = 1000 };
    uint8_t clip[LEN], pkt[CLIP_PKT_MAX];
    for (int i = 0; i < LEN; i++) clip[i] = (uint8_t)(i * 7);
    uint32_t crc = clip_crc32(clip, LEN);
    CHECK(clip_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(clip_chunks(LEN) == 5 && clip_chunks(224) == 1 && clip_chunks(225) == 2, "chunk counts");
    CHECK(clip_chunks(CLIP_MAX_BYTES) == CLIP_MAX_CHUNKS, "the bitmap fits the largest clip");
    int n = clip_pack(0x9a, 7, clip, LEN, crc, 0, pkt);
    CHECK(n == CLIP_PKT_MAX && n <= 247, "a full chunk is %d bytes", n);
    clip_pkt_t p;
    CHECK(clip_parse(pkt, n, &p) && p.origin == 0x9a && p.id == 7 && p.chunk == 0 && p.total == 5 && p.len == LEN &&
          p.crc == crc && p.data_len == CLIP_CHUNK_DATA && !memcmp(p.data, clip, CLIP_CHUNK_DATA), "parse chunk 0");
    n = clip_pack(0x9a, 7, clip, LEN, crc, 4, pkt);
    CHECK(n == CLIP_PKT_HDR + LEN - 4 * CLIP_CHUNK_DATA, "the last chunk is short (%d)", n);
    CHECK(clip_parse(pkt, n, &p) && p.data_len == LEN - 4 * CLIP_CHUNK_DATA, "parse the last chunk");
    CHECK(clip_pack(0x9a, 7, clip, LEN, crc, 5, pkt) == 0, "no chunk past the end");
    CHECK(clip_pack(0x9a, 7, clip, CLIP_MAX_BYTES + 1, crc, 0, pkt) == 0, "too long");
    n = clip_pack(0x9a, 7, clip, LEN, crc, 1, pkt);
    CHECK(!clip_parse(pkt, n - 1, &p), "truncated");
    pkt[5] = 'X';
    CHECK(!clip_parse(pkt, n, &p), "another kind");
    pkt[5] = 'V';
    pkt[10] = 9;
    CHECK(!clip_parse(pkt, n, &p), "total does not match len");
}

static void test_assemble(void)
{
    enum { LEN = 40000 };                           // a 5 s clip
    static uint8_t clip[LEN], buf[CLIP_MAX_BYTES];
    uint8_t pkt[CLIP_PKT_MAX], other[CLIP_PKT_MAX];
    for (int i = 0; i < LEN; i++) clip[i] = (uint8_t)rand();
    uint32_t crc = clip_crc32(clip, LEN);
    uint16_t total = clip_chunks(LEN);
    CHECK(total == 179, "a 5 s clip is %u chunks", total);
    static clip_rx_t r;
    clip_rx_begin(&r, 0x9a, 3, buf);
    CHECK(clip_rx_missing(&r) == 0xFFFF, "unknown size before the first packet");
    clip_pkt_t p;
    bool done = false;
    int completions = 0;
    for (int round = 0; round < 3; round++) {        // the carousel: odd chunks lost in round 0, a foreign clip mixed in
        for (uint16_t c = 0; c < total; c++) {
            uint16_t k = (uint16_t)((c * 37 + round) % total);   // any order
            if (round == 0 && (k & 1)) continue;
            int n = clip_pack(0x9a, 3, clip, LEN, crc, k, pkt);
            CHECK(clip_parse(pkt, n, &p), "parse %u", k);
            if (clip_rx_add(&r, &p)) done = true, completions++;
            n = clip_pack(0x76, 3, clip, 500, 1, 0, other);
            if (clip_parse(other, n, &p)) CHECK(!clip_rx_add(&r, &p), "another origin is ignored");
        }
        if (round == 0) CHECK(clip_rx_missing(&r) == total / 2, "missing after round 0: %u", clip_rx_missing(&r));
    }
    CHECK(done && completions == 1 && !memcmp(buf, clip, LEN), "whole once, same bytes");

    clip_rx_begin(&r, 0x9a, 4, buf);
    for (uint16_t k = 0; k < total; k++) {
        int n = clip_pack(0x9a, 4, clip, LEN, crc, k, pkt);
        if (k == 17) pkt[CLIP_PKT_HDR + 3] ^= 1;     // one bit flipped on air
        clip_parse(pkt, n, &p);
        done = clip_rx_add(&r, &p);
    }
    CHECK(!done && !r.active, "a corrupt chunk fails the CRC");
}

static void test_ids(void)
{
    CHECK(clip_id_newer(1, 0) && clip_id_newer(0, 255) && !clip_id_newer(5, 5) && !clip_id_newer(0, 1), "wrap");
}

// The amplitude of the tone at hz in x (Goertzel), relative to full scale 1.0 = a sine of peak 1
static double tone_level(const int16_t *x, size_t n, double hz)
{
    double w = 2 * M_PI * hz / RINGTONE_RATE, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) * 2 / n;
}

static void test_ringtone(void)
{
    uint32_t n = ringtone_samples();
    CHECK(n > RINGTONE_RATE * 2.9 && n < RINGTONE_RATE * 3.05, "the phrase lasts %.2f s (the recording: 2.96)",
          n / (double)RINGTONE_RATE);
    static int16_t out[RINGTONE_RATE * 4];
    ringtone_t t;
    ringtone_init(&t);
    size_t got = 0, k;
    while ((k = ringtone_render(&t, out + got, 1000, 12000)) > 0) got += k;
    CHECK(got == n, "rendered %zu of %u", got, (unsigned)n);
    int peak = 0;
    for (size_t i = 0; i < got; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    CHECK(peak > 6000 && peak <= 12000, "peak %d", peak);
    CHECK(abs(out[0]) < 1000 && abs(out[got - 1]) < 100, "fades in, fades out");
    // the first note (E6, 1319 Hz, 117 ms) as the recording: its own pitch 26 dB under its 4th partial (5276 Hz, the
    // 5.2 kHz resonance), the 2nd (2637 Hz) above the 1st, and nothing at an alias (the 7th, 9233 Hz, would fold to
    // 6767 Hz)
    uint32_t a = RINGTONE_RATE / 100, b = a + 1600;   // 100 ms from 10 ms in
    double f1 = tone_level(out + a, b - a, 1319), f2 = tone_level(out + a, b - a, 2637);
    double f4 = tone_level(out + a, b - a, 5276), alias = tone_level(out + a, b - a, 16000 - 9233);
    CHECK(f4 > 10 * f1 && f4 < 40 * f1, "4th partial %.1f x the fundamental", f4 / f1);
    CHECK(f2 > f1, "2nd partial %.1f x the fundamental", f2 / f1);
    CHECK(alias < f4 * 0.01, "no alias (%.4f of the 4th)", alias / f4);
    double sq = 0;
    for (uint32_t i = a; i < b; i++) sq += (double)out[i] * out[i];
    double rms = sqrt(sq / (b - a)) / 12000;
    CHECK(rms > 0.25 && rms < 0.35, "note level %.2f of full scale (the recording: ~0.3)", rms);
    // legato and a sharp end: no 5 ms window of the phrase is quiet, up to its last 5 ms
    int quiet = 0;
    for (uint32_t w = 0; w + 80 <= n - 80; w += 80) {
        double s = 0;
        for (uint32_t i = w; i < w + 80; i++) s += (double)out[i] * out[i];
        if (sqrt(s / 80) < 0.1 * 0.3 * 12000) quiet++;
    }
    CHECK(quiet == 0, "%d quiet 5 ms windows inside the phrase", quiet);
}

// The caller's "answered" sound: 320 ms, three notes rising A5 C#6 E6, each note's window matching its own pitch
static void test_answered(void)
{
    uint32_t n = ringtone_seq_samples(ringtone_answered, ringtone_n_answered);
    CHECK(n == RINGTONE_RATE * 320 / 1000, "answered sound %u samples", (unsigned)n);
    static int16_t out[RINGTONE_RATE];
    ringtone_t t;
    ringtone_init_seq(&t, ringtone_answered, ringtone_n_answered);
    size_t got = 0, k;
    while ((k = ringtone_render(&t, out + got, 512, 12000)) > 0) got += k;
    CHECK(got == n, "rendered %zu of %u", got, (unsigned)n);
    uint32_t start = 0;
    for (int i = 0; i < ringtone_n_answered; i++) {
        uint32_t len = ringtone_answered[i].ms * RINGTONE_RATE / 1000, a = start + len / 5, w = len * 3 / 5;
        int best = -1;
        double best_lv = 0;
        for (int j = 0; j < ringtone_n_answered; j++) {   // the pitch whose partials carry the most
            double lv = 0;
            for (int p = 1; p * ringtone_answered[j].freq_hz < 7800; p++)
                lv += tone_level(out + a, w, p * ringtone_answered[j].freq_hz);
            if (lv > best_lv) best_lv = lv, best = j;
        }
        CHECK(best == i, "note %d sounds as note %d", i, best);
        if (i) CHECK(ringtone_answered[i].freq_hz > ringtone_answered[i - 1].freq_hz, "rising");
        start += len;
    }
}

// The outgoing call: every answerer counted once, the "Answered" screen open 4 s after each new answer (a late one
// opens it again), answers after the ring ignored, a cancelled call takes none, the clock wrapping
static void test_calls(void)
{
    call_t c;
    uint32_t t0 = 0xFFFFF000u;                  // wraps during the call
    call_begin(&c, t0);
    CHECK(call_phase(&c, t0 + 10) == CALL_OUT, "calling");
    CHECK(call_answer(&c, 0x1a, 0x80, 0xff, 0, t0 + 3000), "first answer");
    CHECK(!call_answer(&c, 0x1a, 0x80, 0xff, 0, t0 + 3100), "the same board twice");
    CHECK(call_answer(&c, 0x76, 0, 0, 0xff, t0 + 5000), "second answer");
    CHECK(c.n == 2 && c.ans[0].origin == 0x1a && c.ans[1].origin == 0x76 && c.ans[1].b == 0xff, "both listed");
    CHECK(call_phase(&c, t0 + 8900) == CALL_ANSWERED, "open 4 s after the latest answer");
    CHECK(call_phase(&c, t0 + 9100) == CALL_NONE && c.active, "closed, still listening");
    CHECK(call_answer(&c, 0x33, 1, 2, 3, t0 + 12000) && call_phase(&c, t0 + 12001) == CALL_ANSWERED,
          "a late answer opens it again");
    CHECK(c.n == 3, "three answerers");
    CHECK(call_phase(&c, t0 + 16100) == CALL_NONE && c.active, "closed again");
    CHECK(!call_answer(&c, 0x44, 1, 2, 3, t0 + CALL_OUT_MS + 1), "after the ring: ignored");
    CHECK(call_phase(&c, t0 + CALL_OUT_MS + 2) == CALL_NONE && !c.active, "the call ends");
    call_begin(&c, 100);
    call_cancel(&c);
    CHECK(!call_answer(&c, 0x1a, 1, 1, 1, 200) && call_phase(&c, 201) == CALL_NONE, "cancelled: no answer");
    call_begin(&c, 100);
    for (int i = 0; i < CALL_MAX_ANS; i++) call_answer(&c, (uint8_t)i, 1, 1, 1, 200);
    CHECK(c.n == CALL_MAX_ANS && !call_answer(&c, 0xEE, 1, 1, 1, 300), "a full list");
    CHECK(call_phase(&c, 15000) == CALL_NONE && c.active, "no answers yet expired: listening, nothing shown");
}

// Clips from the Android app (android/.../Clip.kt): Android sends one manufacturer AD per company id, so the phone
// puts several chunks in one periodic packet with company ids 0xFFFF, 0xFFFE...; the packet can come in several
// HCI fragments (data status 1 = more to come, 2 = truncated); the phone's clip set is found by its beacon AD.
static void test_phone_clip(void)
{
    enum { LEN = 1000 };
    uint8_t clip[LEN], one[CLIP_PKT_MAX];
    for (int i = 0; i < LEN; i++) clip[i] = (uint8_t)(i * 13);
    uint32_t crc = clip_crc32(clip, LEN);
    // three chunks in one packet, company ids 0xFFFF, 0xFFFE, 0xFFFD
    static uint8_t pkt[3 * CLIP_PKT_MAX];
    size_t n = 0;
    for (int c = 0; c < 3; c++) {
        int k = clip_pack(0x3e, 9, clip, LEN, crc, (uint16_t)c, one);
        one[2] = (uint8_t)(0xFF - c);
        memcpy(pkt + n, one, (size_t)k);
        n += (size_t)k;
    }
    clip_pkt_t p;
    memcpy(one, pkt, CLIP_PKT_MAX);
    one[2] = 0xFE;
    CHECK(clip_parse(one, CLIP_PKT_MAX, &p) && p.chunk == 0, "company 0xFFFE is a clip chunk too");
    one[2] = 0xFF;
    one[3] = 0xFE;
    CHECK(!clip_parse(one, CLIP_PKT_MAX, &p), "company 0xFEFF is not");

    // the packet in fragments of 247, 247, then the rest
    static clip_frag_t f;
    clip_frag_reset(&f);
    CHECK(clip_frag_add(&f, pkt, 247, 1) == 0, "first fragment: more to come");
    CHECK(clip_frag_add(&f, pkt + 247, 247, 1) == 0, "second fragment");
    CHECK(clip_frag_add(&f, pkt + 494, n - 494, 0) == (int)n, "complete: the whole packet");
    CHECK(!memcmp(f.buf, pkt, n), "the bytes in order");
    size_t off = 0, ad_len;
    const uint8_t *ad;
    int chunks = 0;
    while (clip_next_ad(f.buf, n, &off, &ad, &ad_len)) {
        CHECK(clip_parse(ad, ad_len, &p) && p.chunk == chunks && p.origin == 0x3e && p.id == 9, "AD %d", chunks);
        chunks++;
    }
    CHECK(chunks == 3 && off == n, "three chunks, the whole packet read");
    // a one-fragment packet (a board's own train) still works after a split one
    clip_frag_reset(&f);
    CHECK(clip_frag_add(&f, pkt, CLIP_PKT_MAX, 0) == CLIP_PKT_MAX, "a packet in one fragment");
    // truncated: dropped, and the next packet starts clean
    CHECK(clip_frag_add(&f, pkt, 247, 1) == 0 && clip_frag_add(&f, pkt + 247, 100, 2) == 0, "truncated: dropped");
    CHECK(clip_frag_add(&f, pkt, CLIP_PKT_MAX, 0) == CLIP_PKT_MAX, "the next packet is whole");
    // too long for the buffer: dropped
    static uint8_t big[CLIP_FRAG_MAX];
    memset(big, 0, sizeof(big));
    CHECK(clip_frag_add(&f, big, sizeof(big), 1) == 0 && clip_frag_add(&f, big, 10, 0) == 0, "too long: dropped");
    CHECK(clip_frag_add(&f, pkt, CLIP_PKT_MAX, 0) == CLIP_PKT_MAX, "and the next one is whole again");
    // a malformed AD list stops the walk
    uint8_t bad[4] = { 9, 0xFF, 0xFF, 0xFF };
    off = 0;
    CHECK(!clip_next_ad(bad, sizeof(bad), &off, &ad, &ad_len), "an AD longer than the packet");
    uint8_t zero[2] = { 0, 0 };
    off = 0;
    CHECK(!clip_next_ad(zero, sizeof(zero), &off, &ad, &ad_len), "a zero length ends the list");

    // the beacon, alone or after another AD
    uint8_t beacon[13] = { 2, 0x01, 0x06, 9, 0xFF, 0xFF, 0xFF, 0x43, 'B', 0x3e, 9, 31, 8 };
    uint8_t o = 0, id = 0, ds = 0, khz = 0;
    CHECK(clip_beacon_parse(beacon, sizeof(beacon), &o, &id, &ds, &khz) && o == 0x3e && id == 9 && ds == 31 &&
          khz == 8, "beacon, 8 kHz");
    CHECK(clip_beacon_parse(beacon + 3, 10, &o, &id, &ds, &khz) && khz == 8, "beacon alone");
    uint8_t old[9] = { 8, 0xFF, 0xFF, 0xFF, 0x43, 'B', 0x3e, 9, 31 };
    CHECK(clip_beacon_parse(old, sizeof(old), &o, &id, &ds, &khz) && khz == 16, "a beacon without the rate: 16 kHz");
    beacon[12] = 44;
    CHECK(clip_beacon_parse(beacon, sizeof(beacon), &o, &id, &ds, &khz) && khz == 16, "an unknown rate: 16 kHz");
    beacon[8] = 'V';
    CHECK(!clip_beacon_parse(beacon, sizeof(beacon), &o, &id, &ds, &khz), "not a beacon");
    CHECK(!clip_beacon_parse(pkt, n, &o, &id, &ds, &khz), "clip chunks are not a beacon");

    // a board's train beacon (mesh.c instance 1): the phone finds the train by it; a board never takes it for a
    // phone's beacon
    uint8_t train[CLIP_BEACON_LEN];
    CHECK(clip_train_beacon(0x9a, 7, 50, train) == CLIP_BEACON_LEN, "train beacon length");
    static const uint8_t want_train[CLIP_BEACON_LEN] = { 9, 0xFF, 0xFF, 0xFF, 0x43, 'T', 0x9a, 7, 50, 16 };
    CHECK(!memcmp(train, want_train, CLIP_BEACON_LEN), "train beacon bytes");
    CHECK(!clip_beacon_parse(train, CLIP_BEACON_LEN, &o, &id, &ds, &khz), "a train beacon is not a phone's beacon");

    // 8 kHz to 16 kHz in place
    int16_t up[8] = { 100, -100, 32767, 32767 };
    clip_upsample2(up, 4);
    static const int16_t want[8] = { 100, 0, -100, 16333, 32767, 32767, 32767, 32767 };
    CHECK(!memcmp(up, want, sizeof(want)), "upsample: %d %d %d %d %d %d %d %d", up[0], up[1], up[2], up[3], up[4], up[5],
          up[6], up[7]);
}

int main(void)
{
    test_adpcm();
    test_pack();
    test_assemble();
    test_ids();
    test_ringtone();
    test_answered();
    test_calls();
    test_phone_clip();
    return check_done("voice");
}
