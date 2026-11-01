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

int main(void)
{
    test_adpcm();
    test_pack();
    test_assemble();
    test_ids();
    test_ringtone();
    return check_done("voice");
}
