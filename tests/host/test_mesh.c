// mesh_proto.c: the advertisement payload, the version byte, the peer table (seq, expiry, replacement), the
// additive mix in linear light and the advertising cadence.
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "mesh_proto.h"

static mp_state_t state(uint8_t seq, uint8_t flags, uint8_t r, uint8_t g, uint8_t b)
{
    mp_state_t s = { 0 };
    s.origin = 0x11;
    s.seq = seq;
    s.flags = flags;
    s.r = r;
    s.g = g;
    s.b = b;
    s.ver[0] = 0;
    s.ver[1] = 1;
    s.ver[2] = 0;
    s.ver[3] = MP_VER_RC + 1;
    s.target = MP_TARGET_NONE;
    return s;
}

static bool same(const mp_state_t *a, const mp_state_t *b)
{
    return a->origin == b->origin && a->seq == b->seq && a->ttl == b->ttl && a->flags == b->flags && a->r == b->r &&
           a->g == b->g && a->b == b->b && !memcmp(a->ver, b->ver, 4) && a->target == b->target &&
           a->effect == b->effect && a->effect_seq == b->effect_seq && a->b2r == b->b2r && a->b2g == b->b2g &&
           a->b2b == b->b2b && a->game_seq == b->game_seq && a->game_starter == b->game_starter &&
           a->game_left == b->game_left && a->game_pick == b->game_pick && a->clip_id == b->clip_id &&
           a->clip_ds == b->clip_ds && a->answer_to == b->answer_to && a->adv_ds == b->adv_ds;
}

static void test_payload(void)
{
    mp_state_t s = { 0xA7, 0xFE, 3, MP_F_HOLDING | MP_F_DEV | MP_F_SERVABLE | MP_F_PENDING, 0x12, 0x34, 0x56,
                     { 1, 2, 3, 104 }, 0x42, MP_EFFECT_WAVE_BOTTOM, 0x9C, 0x00, 0xC8, 0x3B, 0x07, 0x76, 30, 2, 0xD3, 50, 0x9A, 0 };
    uint8_t buf[31];
    memset(buf, 0xEE, sizeof(buf));
    CHECK(mp_encode(&s, buf, sizeof(buf)) == 30, "encode length");
    static const uint8_t want[29] = { 29, 0xFF, 0xFF, 0xFF, 0x43, 0xA7, 0xFE, 3, 0x0F, 0x12, 0x34, 0x56,
                                      1, 2, 3, 104, 0x42, 5, 0x9C, 0x00, 0xC8, 0x3B, 0x07, 0x76, 30, 2, 0xD3, 50, 0x9A };
    CHECK(!memcmp(buf, want, sizeof(want)), "byte map");
    CHECK(buf[29] == 0, "byte 29 (advertising interval) is zero when not set");
    CHECK(buf[30] == 0xEE, "nothing written past 30 bytes");
    CHECK(mp_encode(&s, buf, 29) == -1, "a 29-byte buffer is too small");
    CHECK(mp_encode(&s, buf, 30) == 30, "a 30-byte buffer is enough");

    mp_state_t d;
    memset(&d, 0, sizeof(d));
    CHECK(mp_decode(buf + 2, 28, &d) && same(&d, &s), "round trip through the manufacturer value");
    memset(&d, 0, sizeof(d));
    CHECK(mp_decode_ad(buf, 30, &d) && same(&d, &s), "round trip through the whole AD structure");
    // every field on its own, so a swapped pair would show
    for (int f = 0; f < 25; f++) {         // origin seq ttl flags r g b ver[0..3] target effect effect_seq b2r b2g b2b game_* clip_* answer_to adv_ds
        mp_state_t one = { 0 };
        uint8_t *p = f == 0 ? &one.origin : f == 1 ? &one.seq : f == 2 ? &one.ttl : f == 3 ? &one.flags
                   : f == 4 ? &one.r : f == 5 ? &one.g : f == 6 ? &one.b : f == 11 ? &one.target
                   : f == 12 ? &one.effect : f == 13 ? &one.effect_seq : f == 14 ? &one.b2r : f == 15 ? &one.b2g
                   : f == 16 ? &one.b2b : f == 17 ? &one.game_seq : f == 18 ? &one.game_starter
                   : f == 19 ? &one.game_left : f == 20 ? &one.game_pick : f == 21 ? &one.clip_id
                   : f == 22 ? &one.clip_ds : f == 23 ? &one.answer_to : f == 24 ? &one.adv_ds : &one.ver[f - 7];
        *p = (uint8_t)(0x80 + f);
        uint8_t ad[30];
        mp_state_t back;
        mp_encode(&one, ad, sizeof(ad));
        CHECK(mp_decode_ad(ad, 30, &back) && same(&back, &one), "field %d round trip", f);
    }
    // the effect fields at bytes 17 and 18, every value
    for (int v = 0; v < 256; v++) {
        mp_state_t one = { 0 };
        one.effect = (uint8_t)v;
        one.effect_seq = (uint8_t)(255 - v);
        uint8_t ad[30];
        mp_state_t back;
        memset(&back, 0xEE, sizeof(back));
        mp_encode(&one, ad, sizeof(ad));
        CHECK(ad[17] == v && ad[18] == 255 - v && mp_decode_ad(ad, 30, &back) && back.effect == v &&
              back.effect_seq == 255 - v, "effect %d round trip", v);
    }
    // colour B at bytes 19-21, every value of each channel; automatic (0, 0, 0) stays all zero on air
    for (int v = 0; v < 256; v++) {
        mp_state_t one = { 0 };
        one.b2r = (uint8_t)v;
        one.b2g = (uint8_t)(255 - v);
        one.b2b = (uint8_t)(v ^ 0x5A);
        uint8_t ad[30];
        mp_state_t back;
        memset(&back, 0xEE, sizeof(back));
        CHECK(mp_encode(&one, ad, sizeof(ad)) == 30, "colour B: encode length");
        CHECK(ad[19] == v && ad[20] == 255 - v && ad[21] == (v ^ 0x5A) && mp_decode_ad(ad, 30, &back) &&
              back.b2r == v && back.b2g == 255 - v && back.b2b == (v ^ 0x5A) && ad[17] == 0 && ad[18] == 0,
              "colour B %d round trip", v);
    }
    mp_state_t autob = { 0 };
    autob.r = 0xFF;
    uint8_t ad0[30];
    mp_state_t back0;
    memset(&back0, 0xEE, sizeof(back0));
    mp_encode(&autob, ad0, sizeof(ad0));
    CHECK(ad0[19] == 0 && ad0[20] == 0 && ad0[21] == 0 && mp_decode_ad(ad0, 30, &back0) && back0.b2r == 0 &&
          back0.b2g == 0 && back0.b2b == 0, "automatic colour B: 0, 0, 0 on air and back");
    CHECK(MP_EFFECT_NONE == 0 && MP_EFFECT_WAVE_CENTRE == 1 && MP_EFFECT_WAVE_LEFT == 2 && MP_EFFECT_WAVE_RIGHT == 3 &&
          MP_EFFECT_WAVE_TOP == 4 && MP_EFFECT_WAVE_BOTTOM == 5, "effect ids");

    mp_state_t keep = d;
    CHECK(!mp_decode(buf + 2, 27, &d), "short value");
    CHECK(!mp_decode(buf + 2, 29, &d), "long value");
    CHECK(!mp_decode(buf + 2, 0, &d), "empty value");
    CHECK(!mp_decode_ad(buf, 29, &d), "short AD");
    uint8_t bad[30];
    memcpy(bad, buf, 30);
    bad[2] = 0xE5;
    bad[3] = 0x02;
    CHECK(!mp_decode_ad(bad, 30, &d), "another company id");
    memcpy(bad, buf, 30);
    bad[4] = 0x44;
    CHECK(!mp_decode_ad(bad, 30, &d), "another protocol");
    memcpy(bad, buf, 30);
    bad[1] = 0x16;
    CHECK(!mp_decode_ad(bad, 30, &d), "not manufacturer data");
    memcpy(bad, buf, 30);
    bad[0] = 28;
    CHECK(!mp_decode_ad(bad, 30, &d), "AD length disagrees with the buffer");
    CHECK(same(&d, &keep), "a rejected payload leaves out untouched");
}

static void ver(const char *s, uint8_t *o) { mp_ver_from_string(s, o); }

static void test_version(void)
{
    static const struct { const char *s; uint8_t v[4]; } table[] = {
        { "v0.1.0", { 0, 1, 0, 200 } },
        { "v0.1.0-rc.1", { 0, 1, 0, 101 } },
        { "v0.1.0-rc.2", { 0, 1, 0, 102 } },
        { "v0.1.0-fw.3", { 0, 1, 0, 3 } },
        { "v0.1.0-3-gabc123", { 0, 1, 0, 204 } },
        { "v0.2.0", { 0, 2, 0, 200 } },
        { "0.1.0", { 0, 1, 0, 200 } },              // the v is optional
        { "v1.12.3-dirty", { 1, 12, 3, 0 } },       // a suffix without a number
        { "v0.1.0-rc.1-2-gdeadbee", { 0, 1, 0, 101 } }, // git describe after an rc: that rc
        { "v0.1.0-rc.250", { 0, 1, 0, 199 } },      // capped
        { "v0.1.0-99-gabc", { 0, 1, 0, 255 } },     // capped
        { "v0.1.0-fw.150", { 0, 1, 0, 99 } },       // capped
        { "v0.1.0-x.3y", { 0, 1, 0, 0 } },          // no trailing number
        { "v300.2.1", { 255, 2, 1, 200 } },         // numbers saturate
        { "1a2b3c", { 0, 0, 0, 0 } },               // not a version
        { "v1.2", { 0, 0, 0, 0 } },
        { "v1.2.3x", { 0, 0, 0, 0 } },
        { "", { 0, 0, 0, 0 } },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint8_t o[4];
        memset(o, 0xAA, 4);
        ver(table[i].s, o);
        CHECK(!memcmp(o, table[i].v, 4), "%s -> %u.%u.%u kind %u", table[i].s, o[0], o[1], o[2], o[3]);
    }
    uint8_t o[4] = { 9, 9, 9, 9 };
    mp_ver_from_string(NULL, o);
    CHECK(!o[0] && !o[1] && !o[2] && !o[3], "NULL -> zeros");

    static const char *const order[] = { "v0.1.0-fw.3", "v0.1.0-fw.4", "v0.1.0-rc.1", "v0.1.0-rc.2", "v0.1.0",
                                         "v0.1.0-3-gabc123", "v0.1.0-4-gabc124", "v0.1.1-x.1", "v0.2.0-rc.1",
                                         "v0.2.0", "v1.0.0" };
    for (unsigned i = 0; i + 1 < sizeof(order) / sizeof(order[0]); i++) {
        uint8_t a[4], b[4];
        ver(order[i], a);
        ver(order[i + 1], b);
        CHECK(mp_ver_cmp(a, b) < 0, "%s < %s", order[i], order[i + 1]);
        CHECK(mp_ver_cmp(b, a) > 0, "%s > %s", order[i + 1], order[i]);
        CHECK(mp_ver_cmp(a, a) == 0, "%s == itself", order[i]);
    }
}

static const uint8_t A[6] = { 1, 2, 3, 4, 5, 0xA1 }, B[6] = { 1, 2, 3, 4, 5, 0xB2 };

static void test_table(void)
{
    mp_table_t t;
    mp_table_init(&t, 0, 0);
    CHECK(t.hold_expiry_ms == 250 && t.peer_expiry_ms == 10000, "defaults");
    CHECK(mp_table_count(&t) == 0, "empty");

    mp_state_t s = state(10, MP_F_HOLDING, 255, 0, 0);
    CHECK(mp_table_update(&t, A, &s, -40, 1000), "new peer is a change");
    CHECK(mp_table_count(&t) == 1 && t.peers[0].rssi == -40 && t.peers[0].last_ms == 1000, "stored");
    CHECK(!mp_table_update(&t, A, &s, -41, 1020), "the same state again is no change");
    CHECK(t.peers[0].last_ms == 1020 && t.peers[0].rssi == -41, "but refreshes last heard and rssi");
    mp_state_t s2 = state(11, MP_F_HOLDING, 0, 0, 255);
    CHECK(mp_table_update(&t, A, &s2, -40, 1040), "a new colour is a change");
    CHECK(t.peers[0].st.b == 255 && t.peers[0].st.seq == 11, "the new state is stored");
    mp_state_t old = state(10, 0, 0, 255, 0);
    CHECK(!mp_table_update(&t, A, &old, -40, 1060), "an older seq is ignored");
    CHECK(t.peers[0].st.b == 255 && t.peers[0].st.seq == 11 && t.peers[0].last_ms == 1060,
          "state kept, last heard refreshed");
    mp_state_t nohold = state(12, 0, 0, 0, 255);
    CHECK(mp_table_update(&t, A, &nohold, -40, 1080), "release is a change");
    mp_state_t dev = state(13, MP_F_DEV, 0, 0, 255);
    CHECK(mp_table_update(&t, A, &dev, -40, 1100), "dev mode is a change");
    {                                      // the admin bit (bit 4) on, then off again: both are changes
        mp_table_t ta;
        mp_table_init(&ta, 0, 0);
        mp_state_t adm = state(40, MP_F_DEV, 0, 0, 255);
        mp_table_update(&ta, A, &adm, -40, 1000);
        adm.seq = 41;
        adm.flags |= MP_F_ADMIN;
        CHECK(mp_table_update(&ta, A, &adm, -40, 1001), "admin on is a change");
        CHECK(ta.peers[0].st.flags == (MP_F_DEV | MP_F_ADMIN), "the admin bit is stored");
        CHECK(!mp_table_update(&ta, A, &adm, -40, 1002), "admin again is no change");
        adm.seq = 42;
        adm.flags &= (uint8_t)~MP_F_ADMIN;
        CHECK(mp_table_update(&ta, A, &adm, -40, 1003), "admin off is a change");
        CHECK(MP_F_ADMIN == 0x10 && !(MP_F_ADMIN & (MP_F_HOLDING | MP_F_DEV | MP_F_SERVABLE | MP_F_PENDING)),
              "MP_F_ADMIN is bit 4, apart from the others");
    }
    mp_state_t newer_ver = dev;
    newer_ver.seq = 14;
    newer_ver.ver[3] = MP_VER_RELEASE;
    CHECK(mp_table_update(&t, A, &newer_ver, -40, 1120), "a new version is a change");
    mp_state_t bumped = newer_ver;
    bumped.seq = 15;
    CHECK(!mp_table_update(&t, A, &bumped, -40, 1140), "a seq bump alone is no change");
    mp_state_t wave = bumped;
    wave.seq = 16;
    wave.effect = MP_EFFECT_WAVE_LEFT;
    wave.effect_seq = 1;
    CHECK(mp_table_update(&t, A, &wave, -40, 1160), "a new effect is a change");
    CHECK(t.peers[0].st.effect == MP_EFFECT_WAVE_LEFT && t.peers[0].st.effect_seq == 1, "the effect is stored");
    CHECK(!mp_table_update(&t, A, &wave, -40, 1180), "the same effect again is no change");
    mp_state_t again = wave;
    again.seq = 17;
    again.effect_seq = 2;
    CHECK(mp_table_update(&t, A, &again, -40, 1200), "the same effect restarted (effect seq) is a change");
    mp_state_t stop = again;
    stop.seq = 18;
    stop.effect = MP_EFFECT_NONE;
    CHECK(mp_table_update(&t, A, &stop, -40, 1220), "an effect ending is a change");
    mp_state_t b2 = stop;
    b2.seq = 19;
    b2.b2g = 0xFF;
    CHECK(mp_table_update(&t, A, &b2, -40, 1240), "a chosen colour B is a change");
    CHECK(t.peers[0].st.b2g == 0xFF && t.peers[0].st.b2r == 0 && t.peers[0].st.b2b == 0, "colour B is stored");
    mp_state_t b2same = b2;
    b2same.seq = 20;
    CHECK(!mp_table_update(&t, A, &b2same, -40, 1260), "the same colour B again is no change");
    mp_state_t b2r = b2same;
    b2r.seq = 21;
    b2r.b2r = 1;
    CHECK(mp_table_update(&t, A, &b2r, -40, 1280), "colour B red channel alone is a change");
    mp_state_t b2b = b2r;
    b2b.seq = 22;
    b2b.b2b = 1;
    CHECK(mp_table_update(&t, A, &b2b, -40, 1300), "colour B blue channel alone is a change");
    mp_state_t b2auto = b2b;
    b2auto.seq = 23;
    b2auto.b2r = b2auto.b2g = b2auto.b2b = 0;
    CHECK(mp_table_update(&t, A, &b2auto, -40, 1320), "back to automatic is a change");

    // seq wraps: 250 -> 3 is newer
    mp_table_init(&t, 0, 0);
    mp_state_t w = state(250, 0, 10, 10, 10);
    mp_table_update(&t, A, &w, 0, 0);
    mp_state_t w2 = state(3, 0, 20, 20, 20);
    CHECK(mp_table_update(&t, A, &w2, 0, 20) && t.peers[0].st.seq == 3, "250 -> 3 is newer across the wrap");
    mp_state_t w3 = state(250, 0, 30, 30, 30);
    CHECK(!mp_table_update(&t, A, &w3, 0, 40) && t.peers[0].st.r == 20, "3 -> 250 is older");

    // a peer that rebooted restarts its seq: adopted after MP_SEQ_RESET_REPORTS reports in a row
    mp_table_init(&t, 0, 0);
    mp_state_t before = state(100, 0, 255, 0, 0);
    mp_table_update(&t, A, &before, 0, 0);
    mp_state_t after = state(0, MP_F_HOLDING, 0, 255, 0);
    CHECK(!mp_table_update(&t, A, &after, 0, 20), "reboot: first older report ignored");
    CHECK(!mp_table_update(&t, A, &after, 0, 40), "reboot: second older report ignored");
    CHECK(mp_table_update(&t, A, &after, 0, 60), "reboot: third adopted");
    CHECK(t.peers[0].st.seq == 0 && t.peers[0].st.g == 255 && t.peers[0].stale == 0, "reboot: new seq and state");
    mp_state_t next = state(1, MP_F_HOLDING, 0, 0, 255);
    CHECK(mp_table_update(&t, A, &next, 0, 80), "reboot: the following seq is newer");
    // an older report between newer ones does not count toward a reset
    mp_state_t o1 = state(0, 0, 1, 1, 1), n2 = state(2, MP_F_HOLDING, 0, 0, 255);
    mp_table_update(&t, A, &o1, 0, 100);
    mp_table_update(&t, A, &o1, 0, 120);
    mp_table_update(&t, A, &n2, 0, 140);
    CHECK(!mp_table_update(&t, A, &o1, 0, 160) && t.peers[0].st.seq == 2, "older reports must come in a row");

    // expiry: a holder not heard for 250 ms stops holding; a peer not heard for 10 s leaves
    mp_table_init(&t, 0, 0);
    mp_state_t h = state(5, MP_F_HOLDING, 255, 0, 0);
    mp_table_update(&t, A, &h, 0, 1000);
    mp_state_t idle = state(1, 0, 0, 255, 0);
    mp_table_update(&t, B, &idle, 0, 1000);
    CHECK(mp_table_expire(&t, 1249) == 0 && mp_holders(&t) == 1, "still holding at 249 ms");
    CHECK(mp_table_expire(&t, 1250) == 1 && mp_holders(&t) == 0, "holding cleared at 250 ms");
    CHECK(mp_table_count(&t) == 2, "both peers still in the table");
    CHECK(mp_table_expire(&t, 1300) == 0, "nothing more to clear");
    CHECK(mp_table_update(&t, A, &h, 0, 1400) && mp_holders(&t) == 1,
          "heard again with the same seq: holding again (a change)");
    CHECK(mp_table_expire(&t, 10999) == 1 && mp_table_count(&t) == 2, "B still there at 9999 ms (A's hold expired)");
    CHECK(mp_table_expire(&t, 11000) == 1 && mp_table_count(&t) == 1, "B removed at 10 s");
    CHECK(mp_table_expire(&t, 11400) == 1 && mp_table_count(&t) == 0, "A removed 10 s after it was last heard");
    // a clock read a moment before the last report does not expire anything
    mp_table_update(&t, A, &h, 0, 5000);
    CHECK(mp_table_expire(&t, 4990) == 0 && mp_holders(&t) == 1, "now slightly before last_ms");
    // the millisecond counter wraps
    mp_table_init(&t, 0, 0);
    mp_table_update(&t, A, &h, 0, 0xFFFFFF80u);
    CHECK(mp_table_expire(&t, 0x00000010u) == 0 && mp_holders(&t) == 1, "144 ms across the wrap: still holding");
    mp_table_init(&t, 0, 0);
    mp_table_update(&t, A, &h, 0, 0xFFFFFFF0u);
    CHECK(mp_table_expire(&t, 0x00000100u) == 1 && mp_holders(&t) == 0, "272 ms across the wrap: cleared");

    // full table: a 17th peer replaces the least recently heard
    mp_table_init(&t, 0, 0);
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        uint8_t a[6] = { 9, 9, 9, 9, 9, (uint8_t)i };
        mp_state_t p = state(1, 0, (uint8_t)i, 0, 0);
        mp_table_update(&t, a, &p, 0, 1000 + (uint32_t)i * 10);
    }
    uint8_t a3[6] = { 9, 9, 9, 9, 9, 3 }, a0[6] = { 9, 9, 9, 9, 9, 0 }, a1[6] = { 9, 9, 9, 9, 9, 1 };
    mp_state_t p3 = state(2, 0, 3, 0, 0);
    mp_table_update(&t, a3, &p3, 0, 2000);
    mp_state_t p0 = state(2, 0, 0, 0, 0);
    mp_table_update(&t, a0, &p0, 0, 2010);             // 0 and 3 refreshed: peer 1 is now the oldest
    CHECK(mp_table_count(&t) == MP_MAX_PEERS, "16 peers");
    uint8_t x[6] = { 7, 7, 7, 7, 7, 7 };
    mp_state_t px = state(1, MP_F_HOLDING, 1, 2, 3);
    CHECK(mp_table_update(&t, x, &px, -70, 2100), "17th peer is new");
    CHECK(mp_table_count(&t) == MP_MAX_PEERS, "still 16");
    bool has1 = false, hasx = false, has0 = false, has2 = false;
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        has1 |= !memcmp(t.peers[i].addr, a1, 6);
        hasx |= !memcmp(t.peers[i].addr, x, 6) && t.peers[i].st.b == 3 && t.peers[i].rssi == -70;
        has0 |= !memcmp(t.peers[i].addr, a0, 6);
        uint8_t a2[6] = { 9, 9, 9, 9, 9, 2 };
        has2 |= !memcmp(t.peers[i].addr, a2, 6);
    }
    CHECK(!has1 && hasx && has0 && has2, "the least recently heard (peer 1) was replaced");

    // self
    mp_state_t me = state(1, 0, 0, 255, 0);
    mp_table_set_self(&t, &me, true);
    CHECK(t.self_holding && (t.self.flags & MP_F_HOLDING) && t.self.g == 255, "self holding, flag mirrored");
    mp_table_set_self(&t, &me, false);
    CHECK(!t.self_holding && !(t.self.flags & MP_F_HOLDING), "self released, flag mirrored");
}

static void mix_of(mp_table_t *t, uint8_t *o)
{
    mp_mix(t, &o[0], &o[1], &o[2]);
}

#define RGB_IS(o, R, G, B) ((o)[0] == (R) && (o)[1] == (G) && (o)[2] == (B))

static void holders(mp_table_t *t, int n, const uint8_t (*cols)[3])
{
    mp_table_init(t, 0, 0);
    for (int i = 0; i < n; i++) {
        uint8_t a[6] = { 5, 5, 5, 5, 5, (uint8_t)i };
        mp_state_t s = state(1, MP_F_HOLDING, cols[i][0], cols[i][1], cols[i][2]);
        mp_table_update(t, a, &s, 0, 0);
    }
}

static void test_mix(void)
{
    mp_table_t t;
    uint8_t o[3];
    static const uint8_t red_blue[][3] = { { 255, 0, 0 }, { 0, 0, 255 } };
    holders(&t, 2, red_blue);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 0, 255), "red + blue = %u,%u,%u", o[0], o[1], o[2]);
    static const uint8_t red_green[][3] = { { 255, 0, 0 }, { 0, 255, 0 } };
    holders(&t, 2, red_green);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 255, 0), "red + green = %u,%u,%u", o[0], o[1], o[2]);
    static const uint8_t rgb[][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 } };
    holders(&t, 3, rgb);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 255, 255) && mp_holders(&t) == 3, "red + green + blue = %u,%u,%u", o[0], o[1], o[2]);
    static const uint8_t reds[][3] = { { 255, 0, 0 }, { 255, 0, 0 } };
    holders(&t, 2, reds);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 0, 0), "two reds = %u,%u,%u", o[0], o[1], o[2]);
    static const uint8_t black[][3] = { { 0, 0, 0 } };
    holders(&t, 1, black);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 0, 0, 0) && mp_holders(&t) == 1, "a black holder = black");

    // one holder: its own colour unchanged, for every value of the other channels
    int bad = 0;
    for (int c = 0; c < 256; c++) {
        uint8_t col[1][3] = { { 255, (uint8_t)c, (uint8_t)(255 - c) } };
        holders(&t, 1, (const uint8_t (*)[3])col);
        mix_of(&t, o);
        if (!RGB_IS(o, 255, c, 255 - c)) {
            if (!bad++) CHECK(0, "one holder 255,%d,%d -> %u,%u,%u", c, 255 - c, o[0], o[1], o[2]);
        }
    }
    CHECK(bad == 0, "%d single-holder colours changed", bad);
    static const uint8_t orange[][3] = { { 255, 128, 0 } };
    holders(&t, 1, orange);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 128, 0), "orange alone = %u,%u,%u", o[0], o[1], o[2]);

    // nobody holds: black; peers that do not hold do not count
    mp_table_init(&t, 0, 0);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 0, 0, 0) && mp_holders(&t) == 0, "empty table = black");
    mp_state_t idle = state(1, 0, 255, 0, 0);
    mp_table_update(&t, A, &idle, 0, 0);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 0, 0, 0) && mp_holders(&t) == 0, "a peer not holding = black");

    // self is mixed like a peer while self_holding, and only then
    mp_state_t blue = state(1, MP_F_HOLDING, 0, 0, 255);
    mp_table_update(&t, B, &blue, 0, 0);
    mp_state_t me = state(1, 0, 255, 0, 0);
    mp_table_set_self(&t, &me, true);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 0, 255) && mp_holders(&t) == 2, "self red + peer blue = %u,%u,%u", o[0], o[1], o[2]);
    mp_table_set_self(&t, &me, false);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 0, 0, 255) && mp_holders(&t) == 1, "self not holding: peer blue only = %u,%u,%u", o[0], o[1],
          o[2]);
    mp_table_init(&t, 0, 0);
    mp_table_set_self(&t, &me, true);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 0, 0) && mp_holders(&t) == 1, "self alone = its own colour");
    // an expired holder drops out of the mix
    mp_table_update(&t, B, &blue, 0, 0);
    mp_table_expire(&t, 250);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 0, 0), "expired holder no longer mixed = %u,%u,%u", o[0], o[1], o[2]);

    // seventeen holders (16 peers + self) at full white do not overflow
    mp_table_init(&t, 0, 0);
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        uint8_t a[6] = { 6, 6, 6, 6, 6, (uint8_t)i };
        mp_state_t s = state(1, MP_F_HOLDING, 255, 255, 255);
        mp_table_update(&t, a, &s, 0, 0);
    }
    mp_state_t white = state(1, 0, 255, 255, 255);
    mp_table_set_self(&t, &white, true);
    mix_of(&t, o);
    CHECK(RGB_IS(o, 255, 255, 255) && mp_holders(&t) == 17, "17 whites = white");
}

static void test_cadence(void)
{
    CHECK(mp_adv_interval_ms(0, false) == 20, "alone: 20 ms");
    CHECK(mp_adv_interval_ms(2, false) == 20, "three boards: 20 ms");
    CHECK(mp_adv_interval_ms(5, false) == 20, "5 peers: 20 ms");
    CHECK(mp_adv_interval_ms(6, false) == 100, "6 peers, steady: 100 ms");
    CHECK(mp_adv_interval_ms(6, true) == 20, "6 peers after a change: 20 ms");
    CHECK(mp_adv_interval_ms(15, true) == 20, "15 peers after a change: 20 ms");
}

// Byte 29: the sender's advertising interval in 10 ms steps (0 = old firmware, 20 ms). A slow sender (the Android
// app advertises every 100 ms at best) keeps its hold for 3 intervals + 100 ms, so one or two missed adverts do not
// drop its colour out of the mix; 0 and 2 keep the table's 250 ms; the expiry is capped at MP_HOLD_EXPIRY_MAX_MS.
static void test_slow_sender(void)
{
    static const uint8_t A[6] = { 1, 2, 3, 4, 5, 6 }, B[6] = { 9, 9, 9, 9, 9, 9 }, C[6] = { 7, 7, 7, 7, 7, 7 };
    mp_state_t one = { 0 };
    one.adv_ds = 10;
    uint8_t ad[30];
    mp_state_t back;
    CHECK(mp_encode(&one, ad, sizeof(ad)) == 30 && ad[29] == 10, "byte 29 carries the interval");
    CHECK(mp_decode_ad(ad, 30, &back) && back.adv_ds == 10, "byte 29 round trip");

    CHECK(mp_hold_expiry_ms(0, 250) == 250 && mp_hold_expiry_ms(2, 250) == 250, "20 ms or unknown: 250 ms");
    CHECK(mp_hold_expiry_ms(10, 250) == 400, "100 ms: 400 ms");
    CHECK(mp_hold_expiry_ms(255, 250) == MP_HOLD_EXPIRY_MAX_MS, "capped");
    CHECK(mp_hold_expiry_ms(0, 600) == 600, "a longer table expiry is kept");

    mp_table_t t;
    mp_table_init(&t, 0, 0);
    mp_state_t slow = state(1, MP_F_HOLDING, 0, 0, 255), fast = state(1, MP_F_HOLDING, 255, 0, 0),
               old = state(1, MP_F_HOLDING, 0, 255, 0);
    slow.adv_ds = 10;
    fast.adv_ds = 2;
    mp_table_update(&t, A, &slow, 0, 1000);
    mp_table_update(&t, B, &fast, 0, 1000);
    mp_table_update(&t, C, &old, 0, 1000);
    CHECK(mp_table_expire(&t, 1300) == 2 && mp_holders(&t) == 1, "at 300 ms only the 100 ms sender still holds");
    CHECK(t.peers[0].st.flags & MP_F_HOLDING, "and it is the slow one");
    CHECK(mp_table_expire(&t, 1399) == 0 && mp_holders(&t) == 1, "still holding at 399 ms");
    CHECK(mp_table_expire(&t, 1400) == 1 && mp_holders(&t) == 0, "cleared at 400 ms");
}

// tests/fixtures/mesh_vectors.txt: adverts, mixes and hold expiries the Android app's tests check too (MeshProtoTest.kt),
// so the app and the boards keep the same rules. Run from tests/host (make does).
static void test_vectors(void)
{
    FILE *f = fopen("../fixtures/mesh_vectors.txt", "r");
    CHECK(f != NULL, "open ../fixtures/mesh_vectors.txt");
    if (!f) return;
    char line[512];
    int n_ad = 0, n_mix = 0, n_hold = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "ad ", 3)) {
            uint8_t ad[30], back[30];
            unsigned v[25];
            const char *p = line + 3;
            for (int i = 0; i < 30; i++, p += 2) { unsigned x; sscanf(p, "%2x", &x); ad[i] = (uint8_t)x; }
            int used = 0, k = 0;
            for (; k < 25 && sscanf(p, " %u%n", &v[k], &used) == 1; k++) p += used;
            mp_state_t st;
            CHECK(k == 25 && mp_decode_ad(ad, 30, &st), "vector ad %d decodes", n_ad);
            const uint8_t got[25] = { st.origin, st.seq, st.ttl, st.flags, st.r, st.g, st.b, st.ver[0], st.ver[1],
                                      st.ver[2], st.ver[3], st.target, st.effect, st.effect_seq, st.b2r, st.b2g, st.b2b,
                                      st.game_seq, st.game_starter, st.game_left, st.game_pick, st.clip_id, st.clip_ds,
                                      st.answer_to, st.adv_ds };
            bool ok = true;
            for (int i = 0; i < 25; i++) ok = ok && got[i] == v[i];
            CHECK(ok, "vector ad %d fields", n_ad);
            CHECK(mp_encode(&st, back, 30) == 30 && !memcmp(back, ad, 30), "vector ad %d encodes back", n_ad);
            n_ad++;
        } else if (!strncmp(line, "mix ", 4)) {
            const char *p = line + 4;
            int k = 0, used = 0;
            sscanf(p, "%d%n", &k, &used);
            p += used;
            mp_table_t t;
            mp_table_init(&t, 0, 0);
            for (int i = 0; i < k; i++) {
                unsigned r, g, b;
                sscanf(p, " %u %u %u%n", &r, &g, &b, &used);
                p += used;
                uint8_t a[6] = { (uint8_t)i, 1, 2, 3, 4, 5 };
                mp_state_t s = { 0 };
                s.flags = MP_F_HOLDING;
                s.r = (uint8_t)r;
                s.g = (uint8_t)g;
                s.b = (uint8_t)b;
                mp_table_update(&t, a, &s, 0, 0);
            }
            unsigned wr, wg, wb;
            sscanf(p, " = %u %u %u", &wr, &wg, &wb);
            uint8_t o[3];
            mix_of(&t, o);
            CHECK(o[0] == wr && o[1] == wg && o[2] == wb, "vector mix %d", n_mix);
            n_mix++;
        } else if (!strncmp(line, "hold ", 5)) {
            unsigned ds, tbl, want;
            sscanf(line + 5, "%u %u = %u", &ds, &tbl, &want);
            CHECK(mp_hold_expiry_ms((uint8_t)ds, tbl) == want, "vector hold %u %u", ds, tbl);
            n_hold++;
        }
    }
    fclose(f);
    CHECK(n_ad >= 8 && n_mix >= 20 && n_hold >= 8, "vectors read: %d ads, %d mixes, %d holds", n_ad, n_mix, n_hold);
}

int main(void)
{
    test_payload();
    test_version();
    test_table();
    test_mix();
    test_cadence();
    test_slow_sender();
    test_vectors();
    return check_done("mesh");
}
