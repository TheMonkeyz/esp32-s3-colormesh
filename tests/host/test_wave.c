// wave.c: the opposite colour, a wave's lifetime, the frame geometry (disc, ring, black corners), each kind's
// direction, the per-pixel merge in linear light (sum, normalise, fade), every fast path against a slow floating
// point reference of the rule in wave.h, band-by-band filling, and the time of a frame (printed, not asserted).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "check.h"
#include "mesh_proto.h"
#include "wave.h"

#define PI 3.14159265358979323846
#define W WAVE_W
#define H WAVE_H
static uint16_t fb[W * H];

typedef struct { int r, g, b; } c565_t;   // 5, 6, 5 bits

static c565_t at(int x, int y)
{
    uint16_t v = fb[y * W + x];
    v = (uint16_t)((v >> 8) | (v << 8));   // the panel's byte order back to RGB565
    c565_t c = { v >> 11, (v >> 5) & 63, v & 31 };
    return c;
}
static c565_t c565(int r, int g, int b)
{
    c565_t c = { r >> 3, g >> 2, b >> 3 };
    return c;
}
static bool near(c565_t a, c565_t b, int tol)
{
    return abs(a.r - b.r) <= tol && abs(a.g - b.g) <= 2 * tol && abs(a.b - b.b) <= tol;
}
static bool eq(c565_t a, c565_t b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static bool full(c565_t c) { return c.r == 31 || c.g == 63 || c.b == 31; }

typedef struct { bool hold; uint8_t hr, hg, hb, rr, rg, rb; } scene_t;
static const scene_t NOHOLD = { false, 0, 0, 0, 0x20, 0x40, 0x60 };

static void render_bands(const wave_t *w, int n, uint32_t now, const scene_t *s, int band)
{
    for (int y = 0; y < H; y += band) {
        int rows = y + band <= H ? band : H - y;
        wave_fill(fb + y * W, y, rows, w, n, now, s->hr, s->hg, s->hb, s->hold, s->rr, s->rg, s->rb);
    }
}
static void render(const wave_t *w, int n, uint32_t now, const scene_t *s) { render_bands(w, n, now, s, 32); }

static wave_t wv(uint8_t kind, uint8_t r, uint8_t g, uint8_t b, uint32_t start)
{
    wave_t w = { kind, r, g, b, start };
    return w;
}

// ---- reference: the rule of wave.h in doubles ----

static double lin(int v) { return pow(v / 255.0, 2.2); }
static int inside(int x, int y, int r)       // pixel centre within radius r of the screen centre
{
    long dx = 2 * x + 1 - 466, dy = 2 * y + 1 - 466;
    return dx * dx + dy * dy <= 4L * r * r;
}
static c565_t reference(int x, int y, const wave_t *w, int n, uint32_t now, const scene_t *s)
{
    if (!inside(x, y, 233)) return c565(0, 0, 0);
    if (!inside(x, y, 233 - WAVE_RING_PX)) return c565(s->rr, s->rg, s->rb);
    double sum[3] = { 0, 0, 0 }, wsum = 0;
    if (s->hold) {
        sum[0] = lin(s->hr);
        sum[1] = lin(s->hg);
        sum[2] = lin(s->hb);
        wsum = 1;
    }
    for (int i = 0; i < n && i < WAVE_MAX; i++) {
        if (!wave_active(&w[i], now)) continue;
        double t = (uint32_t)(now - w[i].start_ms) / 1000.0, d;
        double wt = t * 1000 >= WAVE_DURATION_MS - WAVE_FADE_MS ? (WAVE_DURATION_MS - t * 1000) / WAVE_FADE_MS : 1;
        switch (w[i].kind) {
        case WAVE_CENTRE: d = hypot(x - 233, y - 233); break;
        case WAVE_LEFT: d = x; break;
        case WAVE_RIGHT: d = 465 - x; break;
        case WAVE_TOP: d = y; break;
        default: d = 465 - y; break;
        }
        double ph = fmod(d - WAVE_SPEED_PX_S * t, WAVE_PITCH_PX);
        if (ph < 0) ph += WAVE_PITCH_PX;
        double f = (1 - cos(2 * PI * ph / WAVE_PITCH_PX)) / 2;
        uint8_t br, bg, bb;
        wave_opposite(w[i].r, w[i].g, w[i].b, &br, &bg, &bb);
        sum[0] += wt * (lin(w[i].r) * (1 - f) + lin(br) * f);
        sum[1] += wt * (lin(w[i].g) * (1 - f) + lin(bg) * f);
        sum[2] += wt * (lin(w[i].b) * (1 - f) + lin(bb) * f);
        wsum += wt;
    }
    double m = fmax(sum[0], fmax(sum[1], sum[2]));
    if (m <= 0) return c565(0, 0, 0);
    double k = (wsum < 1 ? wsum : 1) / m;
    int o[3];
    for (int c = 0; c < 3; c++) o[c] = (int)floor(255 * pow(sum[c] * k, 1 / 2.2) + 0.5);
    return c565(o[0], o[1], o[2]);
}

// Every pixel against the reference: geometry exact, inside within tol 5/6-bit levels (green: twice that)
static void compare(const char *name, const wave_t *w, int n, uint32_t now, const scene_t *s, int tol)
{
    render(w, n, now, s);
    int bad = 0, worst = 0, wx = 0, wy = 0;
    double total = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            c565_t got = at(x, y), want = reference(x, y, w, n, now, s);
            int e = abs(got.r - want.r);
            if (abs(got.g - want.g) / 2 > e) e = abs(got.g - want.g) / 2;
            if (abs(got.b - want.b) > e) e = abs(got.b - want.b);
            total += e;
            bool edge = !inside(x, y, 233 - WAVE_RING_PX);
            if (edge ? !eq(got, want) : !near(got, want, tol)) bad++;
            if (e > worst) { worst = e; wx = x; wy = y; }
        }
    CHECK(bad == 0, "%s: %d pixels off the reference (worst %d levels at %d,%d)", name, bad, worst, wx, wy);
    printf("  %-28s worst %d level(s), mean %.3f\n", name, worst, total / (W * H));
}

// ---- tests ----

static void test_uninitialised(void)
{
    wave_t w = wv(WAVE_CENTRE, 255, 0, 0, 0);
    for (int i = 0; i < 32 * W; i++) fb[i] = 0x1234;
    wave_fill(fb, 0, 32, &w, 1, 10, 255, 255, 255, true, 255, 255, 255);
    bool black = true;
    for (int i = 0; i < 32 * W; i++) black = black && fb[i] == 0;
    CHECK(black, "before wave_init: black");
}

static void test_opposite(void)
{
    uint8_t r, g, b;
    wave_opposite(0xFF, 0x80, 0x00, &r, &g, &b);
    CHECK(r == 0x00 && g >= 0x7E && g <= 0x82 && b == 0xFF, "orange -> blue around #0080ff: %02x%02x%02x", r, g, b);
    wave_opposite(255, 0, 0, &r, &g, &b);
    CHECK(r == 0 && g == 255 && b == 255, "red -> cyan");
    wave_opposite(0, 255, 0, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 255, "green -> magenta");
    wave_opposite(128, 128, 128, &r, &g, &b);
    CHECK(r == 128 && g == 128 && b == 128, "grey stays grey");
    wave_opposite(0, 0, 0, &r, &g, &b);
    CHECK(r == 0 && g == 0 && b == 0, "black stays black");
    wave_opposite(200, 100, 50, &r, &g, &b);
    CHECK(r == 50 && g == 150 && b == 200, "same value and saturation: %d %d %d", r, g, b);
    uint8_t r2, g2, b2;
    wave_opposite(r, g, b, &r2, &g2, &b2);
    CHECK(r2 == 200 && g2 == 100 && b2 == 50, "the opposite of the opposite is the colour");
}

static void test_active(void)
{
    wave_t w = wv(WAVE_CENTRE, 255, 0, 0, 1000);
    CHECK(wave_active(&w, 1000), "active at its start");
    CHECK(wave_active(&w, 1000 + WAVE_DURATION_MS - 1), "active until its last ms");
    CHECK(!wave_active(&w, 1000 + WAVE_DURATION_MS), "over after WAVE_DURATION_MS");
    CHECK(!wave_active(&w, 999), "a start in the future is not active");
    CHECK(!wave_active(&w, 1000 + 0x80000000u), "long over");
    w.start_ms = 0xFFFFFF00u;
    CHECK(wave_active(&w, 0x00000100u), "across the 32-bit wrap: 512 ms in");
    CHECK(!wave_active(&w, 0xFFFFFF00u + WAVE_DURATION_MS), "across the wrap: over");
    w.kind = WAVE_NONE;
    CHECK(!wave_active(&w, 0xFFFFFF10u), "kind none");
    w.kind = 6;
    CHECK(!wave_active(&w, 0xFFFFFF10u), "an unknown kind");
    CHECK(!wave_active(NULL, 0), "NULL");
    CHECK(MP_EFFECT_WAVE_CENTRE == WAVE_CENTRE && MP_EFFECT_WAVE_LEFT == WAVE_LEFT && MP_EFFECT_WAVE_RIGHT == WAVE_RIGHT &&
          MP_EFFECT_WAVE_TOP == WAVE_TOP && MP_EFFECT_WAVE_BOTTOM == WAVE_BOTTOM && MP_EFFECT_NONE == WAVE_NONE,
          "the protocol's effect ids are the wave kinds");
}

static void test_geometry(void)
{
    scene_t s = { true, 0, 255, 0, 255, 0, 0 };
    render(NULL, 0, 0, &s);
    CHECK(eq(at(0, 0), c565(0, 0, 0)) && eq(at(465, 0), c565(0, 0, 0)) && eq(at(0, 465), c565(0, 0, 0)) &&
          eq(at(465, 465), c565(0, 0, 0)), "corners black");
    CHECK(eq(at(233, 0), c565(255, 0, 0)) && eq(at(233, 465), c565(255, 0, 0)) && eq(at(0, 233), c565(255, 0, 0)) &&
          eq(at(465, 233), c565(255, 0, 0)), "the disc reaches the four edges, in the ring colour");
    CHECK(eq(at(233, 13), c565(255, 0, 0)) && eq(at(233, 14), c565(0, 255, 0)), "the ring is 14 px at the top");
    CHECK(eq(at(452, 233), c565(255, 0, 0)) && eq(at(451, 233), c565(0, 255, 0)), "and 14 px on the right");
    int black = 0, ring = 0, in = 0, bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            c565_t c = at(x, y);
            if (!inside(x, y, 233)) { black++; bad += !eq(c, c565(0, 0, 0)); }
            else if (!inside(x, y, 233 - WAVE_RING_PX)) { ring++; bad += !eq(c, c565(255, 0, 0)); }
            else { in++; bad += !eq(c, c565(0, 255, 0)); }
        }
    CHECK(bad == 0, "%d pixels in the wrong zone colour", bad);
    CHECK(fabs(in - PI * 219 * 219) < 1000 && fabs(black - (W * H - PI * 233 * 233)) < 1000, "zone areas %d %d %d",
          black, ring, in);
    // symmetric: the frame mirrored left-right and top-bottom is the same
    bool sym = true;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) sym = sym && eq(at(x, y), at(465 - x, y)) && eq(at(x, y), at(x, 465 - y));
    CHECK(sym, "the disc and the ring are symmetric");
}

static void test_holder(void)
{
    scene_t s = { true, 255, 128, 0, 0, 0, 255 };
    render(NULL, 0, 5000, &s);
    CHECK(eq(at(233, 233), c565(255, 128, 0)) && eq(at(100, 300), c565(255, 128, 0)), "a holder alone: its colour");
    wave_t over = wv(WAVE_CENTRE, 0, 255, 0, 0);
    render(&over, 1, 5000, &s);
    CHECK(eq(at(233, 233), c565(255, 128, 0)), "a finished wave adds nothing");
    render(NULL, 0, 5000, &NOHOLD);
    CHECK(eq(at(233, 233), c565(0, 0, 0)) && eq(at(100, 300), c565(0, 0, 0)), "nobody holding, no wave: black inside");
    CHECK(eq(at(233, 3), c565(0x20, 0x40, 0x60)), "the ring stays");
}

static void test_centre(void)
{
    wave_t w = wv(WAVE_CENTRE, 0xFF, 0x80, 0x00, 7000);
    render(&w, 1, 7000, &NOHOLD);
    CHECK(near(at(233, 233), c565(0xFF, 0x80, 0x00), 0), "t=0: colour A at the centre");
    CHECK(near(at(233 + WAVE_PITCH_PX / 2, 233), c565(0x00, 0x7F, 0xFF), 1), "t=0: colour B at pitch/2");
    CHECK(near(at(233, 233 - WAVE_PITCH_PX / 2), c565(0x00, 0x7F, 0xFF), 1), "t=0: colour B at pitch/2 above");
    CHECK(near(at(233 + WAVE_PITCH_PX, 233), c565(0xFF, 0x80, 0x00), 1), "t=0: colour A again at one pitch");
    // the crest moves outward: 100 ms later it is 16 px out
    render(&w, 1, 7100, &NOHOLD);
    CHECK(near(at(233 + 16, 233), c565(0xFF, 0x80, 0x00), 1), "t=100 ms: the crest 16 px out");
    CHECK(!near(at(233, 233), c565(0xFF, 0x80, 0x00), 2), "t=100 ms: the centre is no longer the crest");
}

// A linear kind: at t=0 a crest at distance 128 from its edge; 100 ms later (16 px) at 144, not at 112
static void test_direction(uint8_t kind, const char *name)
{
    int px[3][2];                          // distance 128, 144, 112 -> pixel
    int dists[3] = { 128, 144, 112 };
    for (int i = 0; i < 3; i++) {
        int d = dists[i];
        int x = kind == WAVE_LEFT ? d : kind == WAVE_RIGHT ? 465 - d : 233;
        int y = kind == WAVE_TOP ? d : kind == WAVE_BOTTOM ? 465 - d : 233;
        px[i][0] = x;
        px[i][1] = y;
    }
    c565_t a = c565(0xFF, 0x80, 0x00);
    wave_t w = wv(kind, 0xFF, 0x80, 0x00, 20000);
    render(&w, 1, 20000, &NOHOLD);
    CHECK(near(at(px[0][0], px[0][1]), a, 0), "%s: t=0 crest at 128", name);
    render(&w, 1, 20100, &NOHOLD);
    CHECK(near(at(px[1][0], px[1][1]), a, 1), "%s: t=100 ms crest at 144 (moved away from its edge)", name);
    CHECK(!near(at(px[2][0], px[2][1]), a, 3), "%s: t=100 ms no crest at 112", name);
    CHECK(!near(at(px[0][0], px[0][1]), a, 3), "%s: t=100 ms the old crest moved", name);
}

static void test_merge(void)
{
    // red and green from the centre, both at their crest there: yellow, the brightest channel full
    wave_t two[2] = { wv(WAVE_CENTRE, 255, 0, 0, 100), wv(WAVE_CENTRE, 0, 255, 0, 100) };
    render(two, 2, 100, &NOHOLD);
    CHECK(eq(at(233, 233), c565(255, 255, 0)), "red + green crests: yellow");
    // at pitch/2 the troughs: cyan + magenta = (1, 1, 2) in linear light -> blue full, red and green at half
    c565_t t = at(233 + WAVE_PITCH_PX / 2, 233);
    CHECK(t.b == 31 && abs(t.r - (int)(255 * pow(0.5, 1 / 2.2)) / 8) <= 1 && t.r == t.g / 2, "cyan + magenta troughs");
    int notfull = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (inside(x, y, 233 - WAVE_RING_PX)) notfull += !full(at(x, y));
    CHECK(notfull == 0, "two waves: the brightest channel is full everywhere inside (%d pixels not)", notfull);
    // a centre wave and a left wave of different colours (the general path)
    wave_t mixed[2] = { wv(WAVE_CENTRE, 255, 0, 0, 100), wv(WAVE_LEFT, 0, 0, 255, 600) };
    render(mixed, 2, 1300, &NOHOLD);
    notfull = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (inside(x, y, 233 - WAVE_RING_PX)) notfull += !full(at(x, y));
    CHECK(notfull == 0, "centre + left: the brightest channel is full everywhere inside (%d pixels not)", notfull);
    // a holder plus a wave: the brightest channel full too, and the holder's colour shows in the mix
    scene_t s = { true, 0, 0, 255, 0, 0, 0 };
    wave_t red = wv(WAVE_CENTRE, 255, 0, 0, 100);
    render(&red, 1, 100, &s);
    CHECK(eq(at(233, 233), c565(255, 0, 255)), "red crest over a blue holder: magenta");
}

static void test_fade(void)
{
    wave_t w = wv(WAVE_TOP, 255, 0, 0, 0);
    uint32_t t_half = WAVE_DURATION_MS - WAVE_FADE_MS / 2;          // weight 1/2
    render(&w, 1, 0, &NOHOLD);
    CHECK(at(233, 128).r == 31, "before the fade: full red at a crest");
    // at t_half the crest is at d = 160 * 3.75 s = 600 -> 600 mod 64 = 24: rows 24 + 64 k
    render(&w, 1, t_half, &NOHOLD);
    c565_t c = at(233, 24 + 64 * 3);
    CHECK(c.g == 0 && c.b == 0 && abs(c.r - (int)(255 * pow(0.5, 1 / 2.2)) / 8) <= 1, "half faded: half linear red (%d)",
          c.r);
    render(&w, 1, WAVE_DURATION_MS - 1, &NOHOLD);
    int lit = 0;
    for (int y = 30; y < 436; y++) lit += at(233, y).r > 2 || at(233, y).g > 4 || at(233, y).b > 2;
    CHECK(lit == 0, "the last ms: almost black (%d rows lit)", lit);
    // over a holder, the wave fades into the holder's colour
    scene_t s = { true, 0, 255, 0, 0, 0, 0 };
    render(&w, 1, WAVE_DURATION_MS - 1, &s);
    CHECK(near(at(233, 200), c565(0, 255, 0), 1), "over a holder: fades into the holder's colour");
}

static void test_reference(void)
{
    printf("against the reference:\n");
    scene_t hold = { true, 255, 200, 40, 9, 99, 199 };
    wave_t c1 = wv(WAVE_CENTRE, 255, 128, 0, 50);
    compare("centre", &c1, 1, 1234, &NOHOLD, 1);
    compare("centre + holder", &c1, 1, 1234, &hold, 1);
    wave_t cc[3] = { wv(WAVE_CENTRE, 255, 128, 0, 50), wv(WAVE_CENTRE, 40, 0, 255, 900), wv(WAVE_NONE, 1, 2, 3, 0) };
    compare("2 centres (+ a none)", cc, 3, 1500, &NOHOLD, 1);
    wave_t l1 = wv(WAVE_LEFT, 0, 255, 128, 300);
    compare("left", &l1, 1, 2000, &NOHOLD, 1);
    wave_t r1 = wv(WAVE_RIGHT, 0, 255, 128, 300);
    compare("right + holder", &r1, 1, 2000, &hold, 1);
    wave_t t1 = wv(WAVE_TOP, 200, 0, 255, 0);
    compare("top + holder", &t1, 1, 777, &hold, 1);
    wave_t b1 = wv(WAVE_BOTTOM, 200, 0, 255, 0);
    compare("bottom", &b1, 1, 777, &NOHOLD, 1);
    wave_t cl[2] = { wv(WAVE_CENTRE, 255, 0, 0, 0), wv(WAVE_LEFT, 0, 0, 255, 400) };
    compare("centre + left", cl, 2, 1111, &NOHOLD, 1);
    wave_t ct[2] = { wv(WAVE_CENTRE, 255, 0, 0, 0), wv(WAVE_BOTTOM, 0, 200, 255, 400) };
    compare("centre + bottom", ct, 2, 1111, &NOHOLD, 1);
    wave_t lt[2] = { wv(WAVE_RIGHT, 255, 0, 0, 0), wv(WAVE_TOP, 0, 200, 255, 400) };
    compare("right + top", lt, 2, 1111, &NOHOLD, 1);
    wave_t all[6] = { wv(WAVE_CENTRE, 255, 0, 0, 0), wv(WAVE_LEFT, 0, 255, 0, 500), wv(WAVE_RIGHT, 0, 0, 255, 1000),
                      wv(WAVE_TOP, 255, 255, 0, 1500), wv(WAVE_BOTTOM, 255, 0, 255, 2000), wv(WAVE_CENTRE, 90, 30, 0, 300) };
    compare("all kinds + holder, fading", all, 6, 3800, &hold, 1);
    compare("all kinds, fading", all, 6, 4100, &NOHOLD, 1);
    wave_t fading = wv(WAVE_CENTRE, 255, 128, 0, 0);
    compare("centre, fading", &fading, 1, 3700, &NOHOLD, 1);
    // dim colours: the sum is normalised, so a dim sender's wave shows at full brightness
    wave_t dim = wv(WAVE_CENTRE, 40, 20, 0, 0);
    compare("dim centre", &dim, 1, 600, &NOHOLD, 1);
}

static void test_bands(void)
{
    static uint16_t ref[W * H];
    wave_t w[3] = { wv(WAVE_CENTRE, 255, 0, 0, 0), wv(WAVE_LEFT, 0, 0, 255, 400), wv(WAVE_TOP, 0, 255, 0, 800) };
    scene_t s = { true, 10, 20, 30, 1, 2, 3 };
    render_bands(w, 3, 2000, &s, H);
    memcpy(ref, fb, sizeof(fb));
    render_bands(w, 3, 2500, &s, 32);       // another frame in between
    render_bands(w, 3, 2000, &s, 7);
    CHECK(!memcmp(ref, fb, sizeof(fb)), "bands of 7 rows = one band of 466");
    render_bands(w, 3, 2000, &s, 32);
    CHECK(!memcmp(ref, fb, sizeof(fb)), "bands of 32 rows = one band of 466");
    scene_t s2 = s;
    s2.hr = 200;
    render_bands(w, 3, 2000, &s2, 32);
    CHECK(memcmp(ref, fb, sizeof(fb)) != 0, "a new holder colour at the same time is redrawn");
    wave_t w2[3];
    memcpy(w2, w, sizeof(w));
    w2[1].start_ms = 401;
    render_bands(w2, 3, 2000, &s, 32);
    CHECK(memcmp(ref, fb, sizeof(fb)) != 0, "a new wave start at the same time is redrawn");
    // rows outside the frame are black, rows inside unaffected by the offset
    static uint16_t two[2 * W];
    wave_fill(two, -1, 2, w, 3, 2000, s.hr, s.hg, s.hb, s.hold, s.rr, s.rg, s.rb);
    bool ok = true;
    for (int x = 0; x < W; x++) ok = ok && two[x] == 0 && two[W + x] == ref[x];
    CHECK(ok, "row -1 is black, row 0 drawn");
    // more than WAVE_MAX waves: the extra ones are ignored
    wave_t many[WAVE_MAX + 2];
    for (int i = 0; i < WAVE_MAX + 2; i++) many[i] = wv(WAVE_CENTRE, 255, 255, 255, (uint32_t)i * 10);
    many[WAVE_MAX] = wv(WAVE_CENTRE, 0, 0, 255, 0);
    many[WAVE_MAX + 1] = wv(WAVE_CENTRE, 255, 0, 0, 0);
    render(many, WAVE_MAX + 2, 200, &NOHOLD);
    c565_t c = at(233, 233);
    CHECK(eq(c, c565(255, 255, 255)), "waves past WAVE_MAX are ignored (white only): %d %d %d", c.r, c.g, c.b);
}

static double frame_ms(const wave_t *w, int n, const scene_t *s, int frames)
{
    clock_t t0 = clock();
    for (int f = 0; f < frames; f++) render(w, n, 1000 + (uint32_t)f * 33, s);   // a new time each frame: no reuse
    return (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;
}

static void test_timing(void)
{
    const int frames = 30;
    wave_t c1 = wv(WAVE_CENTRE, 255, 128, 0, 900);
    wave_t cc[2] = { wv(WAVE_CENTRE, 255, 128, 0, 900), wv(WAVE_CENTRE, 0, 255, 0, 950) };
    wave_t cl[2] = { wv(WAVE_CENTRE, 255, 128, 0, 900), wv(WAVE_LEFT, 0, 255, 0, 950) };
    wave_t ct[2] = { wv(WAVE_CENTRE, 255, 128, 0, 900), wv(WAVE_TOP, 0, 255, 0, 950) };
    wave_t lr[2] = { wv(WAVE_LEFT, 255, 128, 0, 900), wv(WAVE_RIGHT, 0, 255, 0, 950) };
    wave_t xy[2] = { wv(WAVE_RIGHT, 255, 128, 0, 900), wv(WAVE_TOP, 0, 255, 0, 950) };
    wave_t cxy[3] = { wv(WAVE_CENTRE, 255, 128, 0, 900), wv(WAVE_LEFT, 0, 255, 0, 950), wv(WAVE_TOP, 0, 0, 255, 990) };
    scene_t hold = { true, 255, 0, 0, 0, 0, 0 };
    printf("frame time on this host (466 rows in bands of 32, %d frames each):\n", frames);
    printf("  1 centre wave                %.2f ms\n", frame_ms(&c1, 1, &NOHOLD, frames));
    printf("  2 centre waves + holder      %.2f ms\n", frame_ms(cc, 2, &hold, frames));
    printf("  left + right                 %.2f ms\n", frame_ms(lr, 2, &NOHOLD, frames));
    printf("  centre + top                 %.2f ms\n", frame_ms(ct, 2, &NOHOLD, frames));
    printf("  centre + left                %.2f ms\n", frame_ms(cl, 2, &NOHOLD, frames));
    printf("  centre + left + holder       %.2f ms\n", frame_ms(cl, 2, &hold, frames));
    printf("  right + top                  %.2f ms\n", frame_ms(xy, 2, &NOHOLD, frames));
    printf("  centre + left + top          %.2f ms (3 groups: normalised per pixel)\n", frame_ms(cxy, 3, &NOHOLD, frames));
}

int main(void)
{
    test_uninitialised();
    void *mem = malloc(wave_table_bytes());
    CHECK(mem != NULL, "table memory");
    if (!mem) return check_done("wave");
    printf("wave_table_bytes: %zu\n", wave_table_bytes());
    wave_init(mem);
    test_opposite();
    test_active();
    test_geometry();
    test_holder();
    test_centre();
    test_direction(WAVE_LEFT, "left");
    test_direction(WAVE_RIGHT, "right");
    test_direction(WAVE_TOP, "top");
    test_direction(WAVE_BOTTOM, "bottom");
    test_merge();
    test_fade();
    test_reference();
    test_bands();
    test_timing();
    free(mem);
    return check_done("wave");
}
