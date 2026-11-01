// Bicolour waves (see wave.h). Pure C99. Every wave of one group shares the same phase source, so the per-frame work
// folds them together before the pixel loop: all centre waves into one 256-entry linear LUT indexed by the distance
// table's phase, all left/right waves into one per-column sum, all top/bottom waves (and the holder) into one
// per-row sum. The column and row sums repeat every WAVE_PITCH_PX, so whenever at most two groups are present the
// per-pixel normalisation is done once per frame into a final pixel table and the pixel loop is a table read, a LUT
// read and a store:
//   centre only -> 256 pixels by phase (M_FLUT); left/right only -> one row of pixels (M_XPIX);
//   top/bottom only -> one pixel per row (M_ROW); centre + left/right -> [x mod pitch][phase / 2] (M_CX);
//   centre + top/bottom -> [y mod pitch][phase / 2] (M_CY); left/right + top/bottom -> [x mod pitch][y mod pitch]
//   (M_XY). Only all three groups at once normalise per pixel (M_GENERAL).
#include "wave.h"
#include <math.h>
#include <string.h>

#define QN      234                       // the quarter distance table is QN x QN: |x - 233|, |y - 233| 0..233
#define LIN_MAX 4095                      // the normalised output, 12-bit linear light
#define SRGB_N  4128                      // srgb[] also covers the normaliser's small overshoot (<= 4111)
#define CURVE_ONE 4096                    // curve[]: 0 at the crest (colour A) .. CURVE_ONE at the trough (colour B)

typedef struct { uint16_t r, g, b; } lin3_t;

// Tables in the caller's buffer (wave_table_bytes); the order keeps every array aligned to its type
static struct {
    lin3_t *lut;                          // [256] one wave's colour by phase (scratch)
    lin3_t *csum;                         // [256] the centre waves' sum, by the distance table's phase
    lin3_t *xsum;                         // [WAVE_W] the left/right waves' sum, by column
    lin3_t *ysum;                         // [WAVE_H] the top/bottom waves' sum (plus the holder), by row
    uint16_t *flut;                       // [256] final pixels by phase (centre waves only)
    uint16_t *xpix;                       // [WAVE_W] final pixels by column (left/right waves only)
    uint16_t *l2;                         // [WAVE_PITCH_PX * 128] final pixels of the two-group modes
    uint16_t *colbase;                    // [WAVE_W] (x mod pitch) * 128: a column's row in l2
    int16_t *disc_l, *in_l;               // [WAVE_H] first column inside the disc / inside the ring (233: none)
    uint8_t *srgb;                        // [SRGB_N] linear 12-bit -> sRGB 8-bit
    uint8_t *dist;                        // [QN * QN] phase (0-255 over one pitch) of the distance to 233,233
} T;
#define SZ_LUT   (256 * sizeof(lin3_t))
#define SZ_COL   (WAVE_W * sizeof(lin3_t))
#define SZ_ROW   (WAVE_H * sizeof(lin3_t))
#define SZ_FLUT  (256 * sizeof(uint16_t))
#define SZ_XPIX  (WAVE_W * sizeof(uint16_t))
#define SZ_L2    (WAVE_PITCH_PX * 128 * sizeof(uint16_t))
#define SZ_SPAN  (WAVE_H * sizeof(int16_t))
// the column and row sums repeat every pitch only if the pitch divides the 256 phase steps; l2 holds 128 per row
typedef char wave_pitch_check[(256 % WAVE_PITCH_PX == 0 && WAVE_PITCH_PX <= 128) ? 1 : -1];
#define SZ_SRGB  SRGB_N
#define SZ_DIST  (QN * QN)

static bool ready;
static uint16_t lin16[256];               // sRGB 8-bit -> linear 16-bit (gamma 2.2, as mp_mix)
static uint16_t curve[256];               // the crest-to-trough blend: (1 - cos(2 pi k / 256)) / 2
static uint16_t rcp_base[256];            // normaliser: ceil(LIN_MAX * 2^16 / ((i + 256) << 7))
static uint16_t rcp[256];                 // rcp_base scaled by this frame's brightness

enum { M_ROW, M_XPIX, M_FLUT, M_CX, M_CY, M_XY, M_GENERAL };
static struct {                           // this frame's state, rebuilt when the inputs change
    bool valid;
    uint32_t now;
    int n;
    wave_t w[WAVE_MAX];
    bool holding;
    uint8_t hr, hg, hb;
    int mode;
    bool has_c, has_x, has_y;
    int shift;                            // every source's linear values >> shift, so that any sum fits 16 bits
} F;

size_t wave_table_bytes(void)
{
    return 2 * SZ_LUT + SZ_COL + SZ_ROW + SZ_FLUT + SZ_XPIX + SZ_L2 + SZ_XPIX + 2 * SZ_SPAN + SZ_SRGB + SZ_DIST;
}

static double round_half(double v) { return floor(v + 0.5); }

void wave_init(void *table_mem)
{
    ready = false;
    F.valid = false;
    if (!table_mem) return;
    uint8_t *p = table_mem;
    T.lut = (lin3_t *)(void *)p;            p += SZ_LUT;
    T.csum = (lin3_t *)(void *)p;           p += SZ_LUT;
    T.xsum = (lin3_t *)(void *)p;           p += SZ_COL;
    T.ysum = (lin3_t *)(void *)p;           p += SZ_ROW;
    T.flut = (uint16_t *)(void *)p;         p += SZ_FLUT;
    T.xpix = (uint16_t *)(void *)p;         p += SZ_XPIX;
    T.l2 = (uint16_t *)(void *)p;           p += SZ_L2;
    T.colbase = (uint16_t *)(void *)p;      p += SZ_XPIX;
    T.disc_l = (int16_t *)(void *)p;        p += SZ_SPAN;
    T.in_l = (int16_t *)(void *)p;          p += SZ_SPAN;
    T.srgb = p;                             p += SZ_SRGB;
    T.dist = p;

    for (int i = 0; i < 256; i++) {
        lin16[i] = (uint16_t)round_half(65535.0 * pow(i / 255.0, 2.2));
        curve[i] = (uint16_t)round_half(CURVE_ONE * (1.0 - cos(2.0 * 3.14159265358979323846 * i / 256.0)) / 2.0);
        uint32_t den = (uint32_t)(i + 256) << 7;
        rcp_base[i] = (uint16_t)((((uint32_t)LIN_MAX << 16) + den - 1) / den);
    }
    for (int x = 0; x < WAVE_W; x++) T.colbase[x] = (uint16_t)((x % WAVE_PITCH_PX) * 128);
    for (int v = 0; v < SRGB_N; v++)
        T.srgb[v] = v >= LIN_MAX ? 255 : (uint8_t)round_half(255.0 * pow(v / (double)LIN_MAX, 1.0 / 2.2));
    for (int qy = 0; qy < QN; qy++)
        for (int qx = 0; qx < QN; qx++) {
            double d = sqrt((double)(qx * qx + qy * qy));
            T.dist[qy * QN + qx] = (uint8_t)((long)round_half(d * 256.0 / WAVE_PITCH_PX) & 255);
        }
    // Spans in doubled coordinates: pixel x's centre is 2x + 1, the disc's centre 466 (= 2 x 233), radius 2 x 233
    const long r_out = 2 * 233, r_in = 2 * (233 - WAVE_RING_PX);
    for (int y = 0; y < WAVE_H; y++) {
        long dy = 2 * y + 1 - 466;
        int dl = 233, il = 233;
        for (int x = 0; x < 233; x++) {
            long dx = 2 * x + 1 - 466;
            if (dl == 233 && dx * dx + dy * dy <= r_out * r_out) dl = x;
            if (il == 233 && dx * dx + dy * dy <= r_in * r_in) il = x;
        }
        T.disc_l[y] = (int16_t)dl;
        T.in_l[y] = (int16_t)il;
    }
    ready = true;
}

void wave_opposite(uint8_t r, uint8_t g, uint8_t b, uint8_t *or_, uint8_t *og, uint8_t *ob)
{
    int mx = r > g ? r : g, mn = r < g ? r : g;
    if (b > mx) mx = b;
    if (b < mn) mn = b;
    *or_ = (uint8_t)(mx + mn - r);
    *og = (uint8_t)(mx + mn - g);
    *ob = (uint8_t)(mx + mn - b);
}

bool wave_active(const wave_t *w, uint32_t now_ms)
{
    return w && w->kind >= WAVE_CENTRE && w->kind <= WAVE_BOTTOM && (uint32_t)(now_ms - w->start_ms) < WAVE_DURATION_MS;
}

// RGB565, byte-swapped (the panel's order: high byte first in memory)
static inline uint16_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r & 0xF8) | (g >> 5) | ((((g << 3) & 0xE0) | (b >> 3)) << 8));
}

#if defined(__GNUC__)
#define CLZ16(m) (__builtin_clz(m) - 16)
#else
static int CLZ16(uint32_t m)
{
    int s = 0;
    while (!(m & 0x8000u)) { m <<= 1; s++; }
    return s;
}
#endif

// Linear sums (each < 65536, see F.shift) -> the pixel, normalised so the largest channel is full, times this frame's brightness
static inline uint16_t norm(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t m = r > g ? r : g;
    if (b > m) m = b;
    if (!m) return 0;
    int s = CLZ16(m);                       // m << s in [32768, 65535]
    uint32_t k = rcp[((m << s) >> 7) - 256];
    return pack(T.srgb[((r << s) * k) >> 16], T.srgb[((g << s) * k) >> 16], T.srgb[((b << s) * k) >> 16]);
}

static inline void add3(lin3_t *a, const lin3_t *b)
{
    a->r = (uint16_t)(a->r + b->r);
    a->g = (uint16_t)(a->g + b->g);
    a->b = (uint16_t)(a->b + b->b);
}

// One wave's weighted colour by phase into T.lut, rotated so that T.lut[phase of d] is its colour at distance d.
// Returns its weight (0-256).
static unsigned wave_lut(const wave_t *w, uint32_t now_ms)
{
    uint32_t t = now_ms - w->start_ms;
    unsigned wt = t >= WAVE_DURATION_MS - WAVE_FADE_MS ? (unsigned)((WAVE_DURATION_MS - t) * 256u / WAVE_FADE_MS) : 256u;
    unsigned off = (unsigned)(((uint64_t)t * WAVE_SPEED_PX_S * 256u / ((uint64_t)WAVE_PITCH_PX * 1000u)) & 255u);
    uint8_t br, bg, bb;
    wave_opposite(w->r, w->g, w->b, &br, &bg, &bb);
    uint32_t ar = lin16[w->r], ag = lin16[w->g], ab = lin16[w->b], cr = lin16[br], cg = lin16[bg], cb = lin16[bb];
    int sh = 8 + F.shift;
    for (unsigned k = 0; k < 256; k++) {
        uint32_t c = curve[(k - off) & 255u], a = CURVE_ONE - c;
        T.lut[k].r = (uint16_t)((((ar * a + cr * c) >> 12) * wt) >> sh);
        T.lut[k].g = (uint16_t)((((ag * a + cg * c) >> 12) * wt) >> sh);
        T.lut[k].b = (uint16_t)((((ab * a + cb * c) >> 12) * wt) >> sh);
    }
    return wt;
}

static bool same_inputs(const wave_t *waves, int n, uint32_t now_ms, uint8_t hr, uint8_t hg, uint8_t hb, bool holding)
{
    if (!F.valid || F.now != now_ms || F.n != n || F.holding != holding) return false;
    if (holding && (F.hr != hr || F.hg != hg || F.hb != hb)) return false;
    for (int i = 0; i < n; i++)
        if (F.w[i].kind != waves[i].kind || F.w[i].r != waves[i].r || F.w[i].g != waves[i].g ||
            F.w[i].b != waves[i].b || F.w[i].start_ms != waves[i].start_ms) return false;
    return true;
}

static void prepare(const wave_t *waves, int n, uint32_t now_ms, uint8_t hr, uint8_t hg, uint8_t hb, bool holding)
{
    F.now = now_ms;
    F.n = n;
    for (int i = 0; i < n; i++) F.w[i] = waves[i];
    F.holding = holding;
    F.hr = hr;
    F.hg = hg;
    F.hb = hb;
    F.valid = true;

    int sources = holding ? 1 : 0;
    for (int i = 0; i < n; i++) sources += wave_active(&waves[i], now_ms);
    F.shift = 0;                          // N sources of at most 65535 >> shift each: N <= 2^shift
    while ((1 << F.shift) < sources) F.shift++;
    lin3_t hold = { 0, 0, 0 };
    if (holding) {
        hold.r = (uint16_t)(lin16[hr] >> F.shift);
        hold.g = (uint16_t)(lin16[hg] >> F.shift);
        hold.b = (uint16_t)(lin16[hb] >> F.shift);
    }
    memset(T.csum, 0, SZ_LUT);
    memset(T.xsum, 0, SZ_COL);
    for (int y = 0; y < WAVE_H; y++) T.ysum[y] = hold;
    unsigned wsum = holding ? 256u : 0u;
    bool has_c = false, has_x = false, has_y = false;
    for (int i = 0; i < n; i++) {
        const wave_t *w = &waves[i];
        if (!wave_active(w, now_ms)) continue;
        wsum += wave_lut(w, now_ms);
        switch (w->kind) {
        case WAVE_CENTRE:
            for (int k = 0; k < 256; k++) add3(&T.csum[k], &T.lut[k]);
            has_c = true;
            break;
        case WAVE_LEFT:
        case WAVE_RIGHT:
            for (int x = 0; x < WAVE_W; x++) {
                int d = w->kind == WAVE_LEFT ? x : WAVE_W - 1 - x;
                add3(&T.xsum[x], &T.lut[(d * 256 / WAVE_PITCH_PX) & 255]);
            }
            has_x = true;
            break;
        default:                            // WAVE_TOP, WAVE_BOTTOM
            for (int y = 0; y < WAVE_H; y++) {
                int d = w->kind == WAVE_TOP ? y : WAVE_H - 1 - y;
                add3(&T.ysum[y], &T.lut[(d * 256 / WAVE_PITCH_PX) & 255]);
            }
            has_y = true;
            break;
        }
    }
    unsigned bright = wsum < 256u ? wsum : 256u;
    for (int i = 0; i < 256; i++) rcp[i] = (uint16_t)((rcp_base[i] * bright) >> 8);
    F.has_c = has_c;
    F.has_x = has_x;
    F.has_y = has_y;
    const lin3_t *cs = T.csum, *xs = T.xsum, *ys = T.ysum;   // ys includes the holder
    if (!has_c && !has_x) {
        F.mode = M_ROW;
    } else if (!has_c && !has_y) {
        F.mode = M_XPIX;
        for (int x = 0; x < WAVE_W; x++) T.xpix[x] = norm(hold.r + xs[x].r, hold.g + xs[x].g, hold.b + xs[x].b);
    } else if (!has_x && !has_y) {
        F.mode = M_FLUT;
        for (int k = 0; k < 256; k++) T.flut[k] = norm(hold.r + cs[k].r, hold.g + cs[k].g, hold.b + cs[k].b);
    } else if (!has_y || !has_x) {         // centre + one linear group: [x or y mod pitch][phase / 2]
        F.mode = has_x ? M_CX : M_CY;
        const lin3_t *ls = has_x ? xs : ys;
        const lin3_t zero = { 0, 0, 0 }, base = has_x ? hold : zero;
        for (int m = 0; m < WAVE_PITCH_PX; m++) {
            uint16_t *row = T.l2 + m * 128;
            uint32_t br = (uint32_t)base.r + ls[m].r, bg = (uint32_t)base.g + ls[m].g, bb = (uint32_t)base.b + ls[m].b;
            for (int j = 0; j < 128; j++) {
                const lin3_t *c = &cs[2 * j + 1];          // the middle of the two phases j stands for
                row[j] = norm(br + c->r, bg + c->g, bb + c->b);
            }
        }
    } else if (!has_c) {                   // left/right + top/bottom: [x mod pitch][y mod pitch]
        F.mode = M_XY;
        for (int m = 0; m < WAVE_PITCH_PX; m++)
            for (int k = 0; k < WAVE_PITCH_PX; k++)
                T.l2[m * 128 + k] = norm((uint32_t)xs[m].r + ys[k].r, (uint32_t)xs[m].g + ys[k].g,
                                         (uint32_t)xs[m].b + ys[k].b);
    } else {
        F.mode = M_GENERAL;
    }
}

// The inside of row y, columns [il, 465 - il] (il <= 232)
static void inner_row(uint16_t *d, int y, int il)
{
    int ir = WAVE_W - 1 - il;
    const uint8_t *trow = T.dist + (y >= 233 ? y - 233 : 233 - y) * QN;
    switch (F.mode) {
    case M_ROW: {
        uint16_t px = norm(T.ysum[y].r, T.ysum[y].g, T.ysum[y].b);
        for (int x = il; x <= ir; x++) d[x] = px;
        break;
    }
    case M_XPIX:
        memcpy(d + il, T.xpix + il, (size_t)(ir - il + 1) * sizeof(uint16_t));
        break;
    case M_FLUT: {
        const uint16_t *lut = T.flut;
        const uint8_t *tp = trow + (233 - il);
        for (int x = il; x <= 232; x++) d[x] = lut[*tp--];      // left half: the table read backwards
        tp = trow;
        for (int x = 233; x <= ir; x++) d[x] = lut[*tp++];
        break;
    }
    case M_CX: {
        const uint16_t *lut = T.l2, *cb = T.colbase;
        const uint8_t *tp = trow + (233 - il);
        for (int x = il; x <= 232; x++) d[x] = lut[cb[x] + (*tp-- >> 1)];
        tp = trow;
        for (int x = 233; x <= ir; x++) d[x] = lut[cb[x] + (*tp++ >> 1)];
        break;
    }
    case M_CY: {
        const uint16_t *lut = T.l2 + (y % WAVE_PITCH_PX) * 128;
        const uint8_t *tp = trow + (233 - il);
        for (int x = il; x <= 232; x++) d[x] = lut[*tp-- >> 1];
        tp = trow;
        for (int x = 233; x <= ir; x++) d[x] = lut[*tp++ >> 1];
        break;
    }
    case M_XY: {
        const uint16_t *lut = T.l2 + y % WAVE_PITCH_PX, *cb = T.colbase;
        for (int x = il; x <= ir; x++) d[x] = lut[cb[x]];
        break;
    }
    default: {                             // all three groups: the sums and the normalisation per pixel
        const lin3_t *xs = T.xsum, *cs = T.csum;
        const uint32_t br = T.ysum[y].r, bg = T.ysum[y].g, bb = T.ysum[y].b;
        const uint8_t *tp = trow + (233 - il);
        for (int x = il; x <= 232; x++) {
            const lin3_t *c = &cs[*tp--], *s = &xs[x];
            d[x] = norm(br + s->r + c->r, bg + s->g + c->g, bb + s->b + c->b);
        }
        tp = trow;
        for (int x = 233; x <= ir; x++) {
            const lin3_t *c = &cs[*tp++], *s = &xs[x];
            d[x] = norm(br + s->r + c->r, bg + s->g + c->g, bb + s->b + c->b);
        }
        break;
    }
    }
}

void wave_fill(uint16_t *dst, int y0, int n, const wave_t *waves, int nwaves, uint32_t now_ms,
               uint8_t hr, uint8_t hg, uint8_t hb, bool holding, uint8_t ring_r, uint8_t ring_g, uint8_t ring_b)
{
    if (!dst || n <= 0) return;
    if (!ready) {
        memset(dst, 0, (size_t)n * WAVE_W * sizeof(uint16_t));
        return;
    }
    if (!waves || nwaves < 0) nwaves = 0;
    if (nwaves > WAVE_MAX) nwaves = WAVE_MAX;
    if (!same_inputs(waves, nwaves, now_ms, hr, hg, hb, holding)) prepare(waves, nwaves, now_ms, hr, hg, hb, holding);
    uint16_t ring = pack(ring_r, ring_g, ring_b);
    for (int i = 0; i < n; i++) {
        uint16_t *d = dst + (size_t)i * WAVE_W;
        int y = y0 + i;
        if (y < 0 || y >= WAVE_H) {
            memset(d, 0, WAVE_W * sizeof(uint16_t));
            continue;
        }
        int dl = T.disc_l[y], il = T.in_l[y];
        for (int x = 0; x < dl; x++) d[x] = 0;
        for (int x = dl; x < il; x++) d[x] = ring;
        if (il <= 232) inner_row(d, y, il);
        for (int x = WAVE_W - il; x < WAVE_W - dl; x++) d[x] = ring;
        for (int x = WAVE_W - dl; x < WAVE_W; x++) d[x] = 0;
    }
}
