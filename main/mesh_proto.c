// ColorMesh protocol core (see mesh_proto.h): payload, version byte, peer table, expiry, mix. Pure C99.
#include "mesh_proto.h"
#include <string.h>

// ---- payload ----

int mp_encode(const mp_state_t *s, uint8_t *buf, int cap)
{
    if (!s || !buf || cap < MP_AD_SIZE) return -1;
    memset(buf, 0, MP_AD_SIZE);                    // reserved byte 29 stays zero
    buf[0] = MP_AD_SIZE - 1;                       // the AD length counts the type and the value
    buf[1] = 0xFF;                                 // manufacturer specific data
    buf[2] = MP_COMPANY & 0xFF;
    buf[3] = MP_COMPANY >> 8;
    buf[4] = MP_PROTO;
    buf[5] = s->origin;
    buf[6] = s->seq;
    buf[7] = s->ttl;
    buf[8] = s->flags;
    buf[9] = s->r;
    buf[10] = s->g;
    buf[11] = s->b;
    memcpy(&buf[12], s->ver, 4);
    buf[16] = s->target;
    buf[17] = s->effect;
    buf[18] = s->effect_seq;
    buf[19] = s->b2r;
    buf[20] = s->b2g;
    buf[21] = s->b2b;
    buf[22] = s->game_seq;
    buf[23] = s->game_starter;
    buf[24] = s->game_left;
    buf[25] = s->game_pick;
    buf[26] = s->clip_id;
    buf[27] = s->clip_ds;
    buf[28] = s->answer_to;
    buf[29] = s->adv_ds;
    return MP_AD_SIZE;
}

bool mp_decode(const uint8_t *mfg, int len, mp_state_t *out)
{
    if (!mfg || !out || len != MP_MFG_LEN) return false;
    if ((mfg[0] | (mfg[1] << 8)) != MP_COMPANY || mfg[2] != MP_PROTO) return false;
    mp_state_t s;
    s.origin = mfg[3];
    s.seq = mfg[4];
    s.ttl = mfg[5];
    s.flags = mfg[6];
    s.r = mfg[7];
    s.g = mfg[8];
    s.b = mfg[9];
    memcpy(s.ver, &mfg[10], 4);
    s.target = mfg[14];
    s.effect = mfg[15];
    s.effect_seq = mfg[16];
    s.b2r = mfg[17];
    s.b2g = mfg[18];
    s.b2b = mfg[19];
    s.game_seq = mfg[20];
    s.game_starter = mfg[21];
    s.game_left = mfg[22];
    s.game_pick = mfg[23];
    s.clip_id = mfg[24];
    s.clip_ds = mfg[25];
    s.answer_to = mfg[26];
    s.adv_ds = mfg[27];
    *out = s;
    return true;
}

bool mp_decode_ad(const uint8_t *ad, int len, mp_state_t *out)
{
    if (!ad || len < 2 || ad[0] + 1 != len || ad[1] != 0xFF) return false;
    return mp_decode(ad + 2, len - 2, out);
}

// ---- version byte ----

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Leading decimal number of *s (advances *s); saturates far above 255 instead of overflowing
static unsigned take_num(const char **s)
{
    unsigned n = 0;
    for (; is_digit(**s); (*s)++) if (n < 100000) n = n * 10 + (unsigned)(**s - '0');
    return n;
}

static uint8_t cap8(unsigned n, unsigned max) { return (uint8_t)(n > max ? max : n); }

void mp_ver_from_string(const char *v, uint8_t out[4])
{
    memset(out, 0, 4);
    if (!v) return;
    if (*v == 'v' || *v == 'V') v++;
    unsigned num[3];
    for (int i = 0; i < 3; i++) {
        if (!is_digit(*v)) return;
        num[i] = take_num(&v);
        if (i < 2) {
            if (*v != '.') return;
            v++;
        }
    }
    uint8_t kind;
    if (!*v) {
        kind = MP_VER_RELEASE;
    } else if (*v != '-') {
        return;                                    // "v1.2.3x": not a version
    } else {
        const char *s = v + 1;
        if (!strncmp(s, "rc.", 3)) {               // -rc.N (and -rc.N-M-gHASH, git describe after an rc)
            s += 3;
            kind = (uint8_t)(MP_VER_RC + cap8(take_num(&s), 99));
        } else if (is_digit(*s) && strstr(s, "-g")) {  // -N-gHASH: N commits after the release tag
            kind = (uint8_t)(MP_VER_POST + cap8(take_num(&s), 255 - MP_VER_POST));
        } else {                                   // -anything.N -> N, -anything -> 0
            const char *dot = strrchr(s, '.');
            unsigned n = 0;
            if (dot && is_digit(dot[1])) {
                const char *p = dot + 1;
                n = take_num(&p);
                if (*p) n = 0;                     // "-x.3y": no trailing number
            }
            kind = cap8(n, MP_VER_RC - 1);
        }
    }
    for (int i = 0; i < 3; i++) out[i] = cap8(num[i], 255);
    out[3] = kind;
}

int mp_ver_cmp(const uint8_t a[4], const uint8_t b[4])
{
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

// ---- peer table ----

void mp_table_init(mp_table_t *t, uint32_t hold_expiry_ms, uint32_t peer_expiry_ms)
{
    memset(t, 0, sizeof(*t));
    t->self.target = MP_TARGET_NONE;
    t->hold_expiry_ms = hold_expiry_ms ? hold_expiry_ms : MP_HOLD_EXPIRY_MS;
    t->peer_expiry_ms = peer_expiry_ms ? peer_expiry_ms : MP_PEER_EXPIRY_MS;
}

// Time since last_ms; a last_ms "after" now (a clock read a moment earlier by the caller) counts as just heard
static uint32_t age_of(const mp_peer_t *p, uint32_t now_ms)
{
    uint32_t age = now_ms - p->last_ms;
    return (int32_t)age < 0 ? 0 : age;
}

static bool state_differs(const mp_state_t *a, const mp_state_t *b)
{
    return a->r != b->r || a->g != b->g || a->b != b->b || a->flags != b->flags || a->target != b->target ||
           a->effect != b->effect || a->effect_seq != b->effect_seq || a->b2r != b->b2r || a->b2g != b->b2g ||
           a->b2b != b->b2b || memcmp(a->ver, b->ver, 4) != 0;
}

bool mp_table_update(mp_table_t *t, const uint8_t addr[6], const mp_state_t *st, int8_t rssi, uint32_t now_ms)
{
    if (!t || !addr || !st) return false;
    mp_peer_t *p = NULL, *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        mp_peer_t *q = &t->peers[i];
        if (!q->used) {
            if (!free_slot) free_slot = q;
            continue;
        }
        if (!memcmp(q->addr, addr, 6)) {
            p = q;
            break;
        }
        if (!oldest || age_of(q, now_ms) > age_of(oldest, now_ms)) oldest = q;
    }
    if (!p) {                                      // new peer: a free slot, else the least recently heard
        p = free_slot ? free_slot : oldest;
        memset(p, 0, sizeof(*p));
        memcpy(p->addr, addr, 6);
        p->st = *st;
        p->rssi = rssi;
        p->last_ms = now_ms;
        p->used = true;
        return true;
    }
    p->last_ms = now_ms;
    p->rssi = rssi;
    if ((int8_t)(uint8_t)(st->seq - p->st.seq) < 0) {   // older: stale, unless the peer restarted its seq
        if (++p->stale < MP_SEQ_RESET_REPORTS) return false;
    }
    p->stale = 0;
    bool changed = state_differs(&p->st, st);      // equal seq: the same state, which restores an expired hold
    p->st = *st;
    return changed;
}

uint32_t mp_hold_expiry_ms(uint8_t adv_ds, uint32_t table_ms)
{
    uint32_t ms = 3u * 10u * adv_ds + 100u;
    if (ms < table_ms) ms = table_ms;
    return ms > MP_HOLD_EXPIRY_MAX_MS ? MP_HOLD_EXPIRY_MAX_MS : ms;
}

int mp_table_expire(mp_table_t *t, uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        mp_peer_t *p = &t->peers[i];
        if (!p->used) continue;
        uint32_t age = age_of(p, now_ms);
        if (age >= t->peer_expiry_ms) {
            p->used = false;
            n++;
        } else if ((p->st.flags & MP_F_HOLDING) && age >= mp_hold_expiry_ms(p->st.adv_ds, t->hold_expiry_ms)) {
            p->st.flags &= (uint8_t)~MP_F_HOLDING;
            n++;
        }
    }
    return n;
}

int mp_table_count(const mp_table_t *t)
{
    int n = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) n += t->peers[i].used;
    return n;
}

void mp_table_set_self(mp_table_t *t, const mp_state_t *self, bool holding)
{
    t->self = *self;
    if (holding) t->self.flags |= MP_F_HOLDING;
    else t->self.flags &= (uint8_t)~MP_F_HOLDING;
    t->self_holding = holding;
}

// ---- mix ----

// sRGB (gamma 2.2) to linear light, 0-65535: round(65535 * (i / 255) ^ 2.2), kept strictly increasing (1 -> 1)
static const uint16_t to_lin[256] = {
        0,     1,     2,     4,     7,    11,    17,    24,    32,    42,    53,    65,
       79,    94,   111,   129,   148,   169,   192,   216,   242,   270,   299,   330,
      362,   396,   432,   469,   508,   549,   591,   635,   681,   729,   779,   830,
      883,   938,   995,  1053,  1113,  1175,  1239,  1305,  1373,  1443,  1514,  1587,
     1663,  1740,  1819,  1900,  1983,  2068,  2155,  2243,  2334,  2427,  2521,  2618,
     2717,  2817,  2920,  3024,  3131,  3240,  3350,  3463,  3578,  3694,  3813,  3934,
     4057,  4182,  4309,  4438,  4570,  4703,  4838,  4976,  5115,  5257,  5401,  5547,
     5695,  5845,  5998,  6152,  6309,  6468,  6629,  6792,  6957,  7124,  7294,  7466,
     7640,  7816,  7994,  8175,  8358,  8543,  8730,  8919,  9111,  9305,  9501,  9699,
     9900, 10102, 10307, 10515, 10724, 10936, 11150, 11366, 11585, 11806, 12029, 12254,
    12482, 12712, 12944, 13179, 13416, 13655, 13896, 14140, 14386, 14635, 14885, 15138,
    15394, 15652, 15912, 16174, 16439, 16706, 16975, 17247, 17521, 17798, 18077, 18358,
    18642, 18928, 19216, 19507, 19800, 20095, 20393, 20694, 20996, 21301, 21609, 21919,
    22231, 22546, 22863, 23182, 23504, 23829, 24156, 24485, 24817, 25151, 25487, 25826,
    26168, 26512, 26858, 27207, 27558, 27912, 28268, 28627, 28988, 29351, 29717, 30086,
    30457, 30830, 31206, 31585, 31966, 32349, 32735, 33124, 33514, 33908, 34304, 34702,
    35103, 35507, 35913, 36321, 36732, 37146, 37562, 37981, 38402, 38825, 39252, 39680,
    40112, 40546, 40982, 41421, 41862, 42306, 42753, 43202, 43654, 44108, 44565, 45025,
    45487, 45951, 46418, 46888, 47360, 47835, 48313, 48793, 49275, 49761, 50249, 50739,
    51232, 51728, 52226, 52727, 53230, 53736, 54245, 54756, 55270, 55787, 56306, 56828,
    57352, 57879, 58409, 58941, 59476, 60014, 60554, 61097, 61642, 62190, 62741, 63295,
    63851, 64410, 64971, 65535,
};
// Linear back to sRGB: thr[i] is the lowest linear value shown as i, ceil(65535 * ((i - 0.5) / 255) ^ 2.2), kept
// strictly increasing and <= to_lin[i], so to_srgb(to_lin[i]) == i for every i
static const uint16_t thr[256] = {
        0,     1,     2,     3,     6,    10,    15,    21,    29,    37,    48,    59,
       72,    87,   103,   120,   139,   159,   181,   205,   230,   256,   285,   314,
      346,   379,   414,   451,   489,   529,   570,   614,   659,   706,   754,   805,
      857,   911,   966,  1024,  1083,  1145,  1208,  1273,  1339,  1408,  1479,  1551,
     1625,  1702,  1780,  1860,  1942,  2026,  2111,  2199,  2289,  2381,  2474,  2570,
     2667,  2767,  2869,  2972,  3078,  3186,  3295,  3407,  3521,  3636,  3754,  3874,
     3996,  4120,  4246,  4374,  4504,  4636,  4771,  4907,  5046,  5187,  5329,  5474,
     5621,  5770,  5922,  6075,  6231,  6388,  6548,  6710,  6875,  7041,  7209,  7380,
     7553,  7728,  7905,  8085,  8267,  8450,  8636,  8825,  9015,  9208,  9403,  9600,
     9800, 10001, 10205, 10411, 10620, 10830, 11043, 11258, 11476, 11696, 11918, 12142,
    12368, 12597, 12828, 13062, 13297, 13535, 13776, 14018, 14263, 14511, 14760, 15012,
    15266, 15523, 15782, 16043, 16307, 16573, 16841, 17111, 17384, 17660, 17937, 18218,
    18500, 18785, 19072, 19361, 19653, 19948, 20244, 20544, 20845, 21149, 21455, 21764,
    22075, 22389, 22704, 23023, 23344, 23667, 23992, 24320, 24651, 24984, 25319, 25657,
    25997, 26340, 26685, 27033, 27383, 27735, 28090, 28447, 28807, 29170, 29534, 29902,
    30271, 30644, 31018, 31396, 31775, 32158, 32542, 32929, 33319, 33711, 34106, 34503,
    34903, 35305, 35710, 36117, 36527, 36939, 37354, 37771, 38191, 38614, 39039, 39466,
    39896, 40329, 40764, 41202, 41642, 42085, 42530, 42978, 43428, 43881, 44337, 44795,
    45256, 45719, 46185, 46653, 47124, 47598, 48074, 48553, 49034, 49518, 50005, 50494,
    50986, 51480, 51977, 52477, 52979, 53483, 53991, 54501, 55013, 55529, 56047, 56567,
    57090, 57616, 58144, 58675, 59209, 59745, 60284, 60826, 61370, 61917, 62466, 63018,
    63573, 64130, 64690, 65253,
};

static uint8_t to_srgb(uint32_t lin)
{
    int lo = 0, hi = 255;                          // the largest i with thr[i] <= lin
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (thr[mid] <= lin) lo = mid;
        else hi = mid - 1;
    }
    return (uint8_t)lo;
}

static void add_lin(uint32_t sum[3], const mp_state_t *s)
{
    sum[0] += to_lin[s->r];
    sum[1] += to_lin[s->g];
    sum[2] += to_lin[s->b];
}

void mp_mix(const mp_table_t *t, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t sum[3] = { 0, 0, 0 };                 // at most 17 x 65535
    for (int i = 0; i < MP_MAX_PEERS; i++)
        if (t->peers[i].used && (t->peers[i].st.flags & MP_F_HOLDING)) add_lin(sum, &t->peers[i].st);
    if (t->self_holding) add_lin(sum, &t->self);
    uint32_t max = sum[0];
    if (sum[1] > max) max = sum[1];
    if (sum[2] > max) max = sum[2];
    uint8_t out[3] = { 0, 0, 0 };
    if (max)
        for (int c = 0; c < 3; c++) out[c] = to_srgb((uint32_t)((uint64_t)sum[c] * 65535u / max));
    *r = out[0];
    *g = out[1];
    *b = out[2];
}

int mp_holders(const mp_table_t *t)
{
    int n = t->self_holding ? 1 : 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) n += t->peers[i].used && (t->peers[i].st.flags & MP_F_HOLDING);
    return n;
}

// ---- cadence ----

uint16_t mp_adv_interval_ms(int peers, bool recent_change)
{
    return (peers < MP_ADV_FAST_BELOW_PEERS || recent_change) ? MP_ADV_FAST_MS : MP_ADV_SLOW_MS;
}
