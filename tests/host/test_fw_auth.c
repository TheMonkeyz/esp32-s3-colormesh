// fw_auth.c: the signed pull's message layouts (the bytes the signature covers, the proof, the challenge and verdict
// notifications), the single-use nonce with its 10 s validity, the version and START checks, base64 and the short id.
// The ECDSA itself is mbedTLS on the board (console "admin selftest", harness mesh.admin).
#include <string.h>
#include "check.h"
#include "fw_auth.h"
#include "mesh_proto.h"

static void fill(uint8_t *p, size_t n, uint8_t start)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(start + i);
}

static fwa_proof_t sample(void)
{
    fwa_proof_t p;
    memset(&p, 0, sizeof(p));
    fill(p.nonce, sizeof(p.nonce), 0x10);
    p.image_len = 0x00115A40;                          // 1137216
    fill(p.digest, sizeof(p.digest), 0xA0);
    strcpy(p.version, "v0.3.0-1-gadmin");
    fill(p.sig, sizeof(p.sig), 0x40);
    return p;
}

static void test_message(void)
{
    fwa_proof_t p = sample();
    uint8_t m[FWA_MSG_LEN];
    memset(m, 0xEE, sizeof(m));
    fwa_proof_message(m, &p);
    CHECK(FWA_MSG_LEN == 100, "message is %d bytes", FWA_MSG_LEN);
    CHECK(!memcmp(m, p.nonce, 32), "nonce at 0");
    CHECK(m[32] == 0x40 && m[33] == 0x5A && m[34] == 0x11 && m[35] == 0x00, "image_len LE at 32: %02x %02x %02x %02x",
          m[32], m[33], m[34], m[35]);
    CHECK(!memcmp(m + 36, p.digest, 32), "digest at 36");
    CHECK(!memcmp(m + 68, "v0.3.0-1-gadmin", 15) && m[68 + 15] == 0, "version at 68");
    bool zeros = true;
    for (int i = 68 + 15; i < 100; i++) zeros &= m[i] == 0;
    CHECK(zeros, "version zero padded to 32");
    // a 40-character version is cut to 31 with a NUL inside
    uint8_t m2[FWA_MSG_LEN];
    fwa_message(m2, p.nonce, 7, p.digest, "v1.2.3-0123456789012345678901234567890");
    CHECK(m2[68 + 31] == 0 && m2[68 + 30] != 0, "a long version is cut to 31 characters");
    fwa_message(m2, p.nonce, 7, p.digest, NULL);
    CHECK(m2[68] == 0, "no version: all zero");
}

static void test_proof(void)
{
    fwa_proof_t p = sample(), q;
    uint8_t b[FWA_PROOF_LEN + 1];
    CHECK(FWA_PROOF_LEN == 165, "proof is %d bytes", FWA_PROOF_LEN);
    CHECK(fwa_proof_encode(b, &p) == FWA_PROOF_LEN, "encode length");
    CHECK(b[0] == 0x03, "command byte 03");
    CHECK(!memcmp(b + 1, p.nonce, 32) && b[33] == 0x40 && !memcmp(b + 37, p.digest, 32), "layout after 03");
    CHECK(!memcmp(b + 101, p.sig, 64), "signature at 101");
    memset(&q, 0x55, sizeof(q));
    CHECK(fwa_proof_decode(b, FWA_PROOF_LEN, &q), "decode");
    CHECK(!memcmp(q.nonce, p.nonce, 32) && q.image_len == p.image_len && !memcmp(q.digest, p.digest, 32) &&
          !strcmp(q.version, p.version) && !memcmp(q.sig, p.sig, 64), "round trip");
    uint8_t m1[FWA_MSG_LEN], m2[FWA_MSG_LEN];
    fwa_proof_message(m1, &p);
    fwa_proof_message(m2, &q);
    CHECK(!memcmp(m1, m2, sizeof(m1)) && !memcmp(b + 1, m1, sizeof(m1)), "the proof carries the signed message as is");
    CHECK(!fwa_proof_decode(b, FWA_PROOF_LEN - 1, &q), "short write refused");
    CHECK(!fwa_proof_decode(b, FWA_PROOF_LEN + 1, &q), "long write refused");
    CHECK(!fwa_proof_decode(NULL, FWA_PROOF_LEN, &q), "NULL refused");
    b[0] = 0x01;
    CHECK(!fwa_proof_decode(b, FWA_PROOF_LEN, &q), "START byte refused");
    b[0] = 0x03;
    memset(b + 69, 'x', 32);                           // a version string without its NUL
    CHECK(!fwa_proof_decode(b, FWA_PROOF_LEN, &q), "version without NUL refused");
}

static void test_notifications(void)
{
    uint8_t n[32], got[32], c[FWA_CHAL_LEN], v[FWA_VERDICT_LEN];
    fill(n, 32, 0x80);
    fwa_challenge_encode(c, n);
    CHECK(FWA_CHAL_LEN == 36, "challenge is %d bytes", FWA_CHAL_LEN);
    CHECK(c[0] == 0xFE && c[1] == 0xFF && c[2] == 3 && c[3] == 0 && !memcmp(c + 4, n, 32), "challenge layout");
    CHECK(fwa_challenge_decode(c, sizeof(c), got) && !memcmp(got, n, 32), "challenge round trip");
    CHECK(!fwa_challenge_decode(c, 20, got), "a 20-byte ACK is not a challenge");
    c[2] = 0;
    CHECK(!fwa_challenge_decode(c, sizeof(c), got), "status 0 is not a challenge");
    uint8_t done[20] = { 0xFF, 0xFF, 3 };              // the end ACK (sector 0xFFFF) is never a challenge
    CHECK(!fwa_challenge_decode(done, 20, got), "sector 0xFFFF");
    fwa_verdict_encode(v, FWA_V_BAD_SIG);
    CHECK(v[0] == 0xFE && v[1] == 0xFF && v[2] == 4 && v[3] == 1 && v[19] == 0, "verdict layout");
    CHECK(fwa_verdict_decode(v, sizeof(v)) == FWA_V_BAD_SIG, "verdict round trip");
    CHECK(fwa_verdict_decode(c, sizeof(c)) == -1, "a challenge is not a verdict");
    CHECK(fwa_verdict_decode(v, 3) == -1, "too short");
}

static void test_nonce(void)
{
    fwa_nonce_t st;
    uint8_t a[32], b[32];
    fill(a, 32, 1);
    fill(b, 32, 2);
    memset(&st, 0, sizeof(st));
    CHECK(fwa_nonce_take(&st, a, 0) == FWA_NO_NONCE, "nothing issued");
    fwa_nonce_issue(&st, a, 1000);
    CHECK(fwa_nonce_take(&st, a, 1500) == FWA_OK, "issued nonce accepted");
    CHECK(fwa_nonce_take(&st, a, 1600) == FWA_NO_NONCE, "single use: replay refused");
    fwa_nonce_issue(&st, a, 1000);
    CHECK(fwa_nonce_take(&st, b, 1500) == FWA_NONCE_MISMATCH, "another nonce");
    CHECK(fwa_nonce_take(&st, a, 1500) == FWA_NO_NONCE, "a wrong guess consumes it too");
    CHECK(!st.live && st.nonce[0] == 0 && st.nonce[31] == 0, "consumed nonce zeroed");
    fwa_nonce_issue(&st, a, 1000);
    CHECK(fwa_nonce_take(&st, a, 1000 + FWA_NONCE_MS) == FWA_OK, "valid for exactly 10 s");
    fwa_nonce_issue(&st, a, 1000);
    CHECK(fwa_nonce_take(&st, a, 1001 + FWA_NONCE_MS) == FWA_NONCE_EXPIRED, "expired after 10 s");
    fwa_nonce_issue(&st, a, 0xFFFFF000u);              // the ms clock wraps (49.7 days)
    CHECK(fwa_nonce_take(&st, a, 0x00000100u) == FWA_OK, "across the wrap: 4.3 s");
    fwa_nonce_issue(&st, a, 0xFFFFF000u);
    CHECK(fwa_nonce_take(&st, a, 0x00003000u) == FWA_NONCE_EXPIRED, "across the wrap: 16.4 s");
    fwa_nonce_issue(&st, a, 5000);
    CHECK(fwa_nonce_take(&st, a, 4990) == FWA_OK, "a clock read just before the issue counts as 0");
    fwa_nonce_issue(&st, a, 1000);
    fwa_nonce_issue(&st, b, 1100);                     // a second challenge replaces the first
    CHECK(fwa_nonce_take(&st, a, 1200) == FWA_NONCE_MISMATCH, "the replaced nonce is refused");
}

static void test_check(void)
{
    fwa_nonce_t st;
    fwa_proof_t p = sample();
    uint8_t own[4];
    mp_ver_from_string("v0.3.0", own);
    memset(&st, 0, sizeof(st));
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10) == FWA_OK, "v0.3.0-1-gadmin over v0.3.0");
    CHECK(fwa_check(&st, &p, own, 20) == FWA_NO_NONCE, "the same proof again");
    strcpy(p.version, "v0.3.0");
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10) == FWA_NOT_NEWER, "equal version");
    strcpy(p.version, "v0.2.9");
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10) == FWA_NOT_NEWER, "older version");
    strcpy(p.version, "v0.3.0-rc.9");
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10) == FWA_NOT_NEWER, "an rc below the release");
    strcpy(p.version, "garbage");
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10) == FWA_BAD_VERSION, "not a version");
    strcpy(p.version, "v0.4.0");
    fwa_nonce_issue(&st, p.nonce, 0);
    CHECK(fwa_check(&st, &p, own, 10001) == FWA_NONCE_EXPIRED, "the nonce is checked before the version");
    CHECK(fwa_result_verdict(FWA_OK) == FWA_V_OK && fwa_result_verdict(FWA_NONCE_EXPIRED) == FWA_V_NONCE &&
          fwa_result_verdict(FWA_NOT_NEWER) == FWA_V_NOT_NEWER && fwa_result_verdict(FWA_BAD_VERSION) == FWA_V_BAD_PROOF,
          "verdicts");
    CHECK(!strcmp(fwa_result_text(FWA_NONCE_MISMATCH), "nonce mismatch"), "texts");

    p = sample();
    CHECK(fwa_start_matches(&p, p.image_len, p.digest), "START with the signed len and digest");
    CHECK(!fwa_start_matches(&p, p.image_len + 1, p.digest), "another length");
    uint8_t d[32];
    memcpy(d, p.digest, 32);
    d[31] ^= 1;
    CHECK(!fwa_start_matches(&p, p.image_len, d), "another digest (last byte)");
    d[31] ^= 1;
    d[0] ^= 0x80;
    CHECK(!fwa_start_matches(&p, p.image_len, d), "another digest (first byte)");
}

static void test_b64(void)
{
    char out[100];
    uint8_t b[80];
    CHECK(fwa_b64_encode((const uint8_t *)"foobar", 6, out, sizeof(out)) == 8 && !strcmp(out, "Zm9vYmFy"), "foobar");
    CHECK(fwa_b64_encode((const uint8_t *)"fo", 2, out, sizeof(out)) == 4 && !strcmp(out, "Zm8="), "fo");
    CHECK(fwa_b64_encode((const uint8_t *)"f", 1, out, sizeof(out)) == 4 && !strcmp(out, "Zg=="), "f");
    CHECK(fwa_b64_encode((const uint8_t *)"f", 1, out, 4) == 0, "no room for the NUL");
    CHECK(fwa_b64_decode("Zm9vYmFy", b, sizeof(b)) == 6 && !memcmp(b, "foobar", 6), "decode foobar");
    CHECK(fwa_b64_decode("Zm8=", b, sizeof(b)) == 2 && !memcmp(b, "fo", 2), "decode fo");
    CHECK(fwa_b64_decode("Zg==", b, sizeof(b)) == 1 && b[0] == 'f', "decode f");
    CHECK(fwa_b64_decode("Zh==", b, sizeof(b)) == -1, "non-canonical padding bits refused");
    CHECK(fwa_b64_decode("Zm9", b, sizeof(b)) == -1, "length not a multiple of 4");
    CHECK(fwa_b64_decode("Zm9v!mFy", b, sizeof(b)) == -1, "bad character");
    CHECK(fwa_b64_decode("Zg==Zg==", b, sizeof(b)) == -1, "padding in the middle");
    CHECK(fwa_b64_decode("====", b, sizeof(b)) == -1, "only padding");
    CHECK(fwa_b64_decode("", b, sizeof(b)) == -1, "empty");
    CHECK(fwa_b64_decode("Zm9vYmFy", b, 5) == -1, "too small an output");

    uint8_t pub[FWA_PUB_LEN], back[FWA_PUB_LEN + 4];
    fill(pub, sizeof(pub), 0xC3);
    char s[FWA_B64_PUB_LEN + 1];
    CHECK(fwa_b64_encode(pub, sizeof(pub), s, sizeof(s)) == FWA_B64_PUB_LEN, "a public key is 88 characters");
    CHECK(s[87] == '=' && s[86] == '=' && s[85] != '=', "two padding characters (64 = 21 x 3 + 1)");
    CHECK(fwa_b64_decode(s, back, sizeof(back)) == FWA_PUB_LEN && !memcmp(back, pub, FWA_PUB_LEN), "key round trip");
    char id[FWA_SHORT_LEN + 1];
    fwa_short_id(pub, id);
    CHECK(strlen(id) == 8 && !strncmp(id, s, 8), "short id = the first 8 characters (%s / %.8s)", id, s);
    // every byte value survives
    uint8_t all[256], all2[256];
    char big[400];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    CHECK(fwa_b64_encode(all, 256, big, sizeof(big)) == 344, "256 bytes");
    CHECK(fwa_b64_decode(big, all2, sizeof(all2)) == 256 && !memcmp(all, all2, 256), "256 bytes back");
}

static void test_helpers(void)
{
    uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8];
    memcpy(b, a, 8);
    CHECK(fwa_ct_equal(a, b, 8), "equal");
    b[7] = 9;
    CHECK(!fwa_ct_equal(a, b, 8), "last byte differs");
    CHECK(fwa_ct_equal(a, b, 7), "prefix equal");
    CHECK(fwa_ct_equal(a, b, 0), "empty");
    fwa_zero(a, sizeof(a));
    bool z = true;
    for (int i = 0; i < 8; i++) z &= a[i] == 0;
    CHECK(z, "zeroed");
}

int main(void)
{
    test_message();
    test_proof();
    test_notifications();
    test_nonce();
    test_check();
    test_b64();
    test_helpers();
    return check_done("fw_auth");
}
