// The signed pull's pure part (see fw_auth.h). Pure C99: no clock, no randomness, no cryptography here.
#include "fw_auth.h"
#include <string.h>
#include "mesh_proto.h"

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool fwa_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

void fwa_zero(void *p, size_t n)
{
    volatile uint8_t *q = p;
    while (n--) *q++ = 0;
}

// ---- messages ----

void fwa_message(uint8_t out[FWA_MSG_LEN], const uint8_t nonce[FWA_NONCE_LEN], uint32_t image_len,
                 const uint8_t digest[FWA_DIGEST_LEN], const char *version)
{
    uint8_t *o = out;
    memcpy(o, nonce, FWA_NONCE_LEN);
    o += FWA_NONCE_LEN;
    put32(o, image_len);
    o += 4;
    memcpy(o, digest, FWA_DIGEST_LEN);
    o += FWA_DIGEST_LEN;
    memset(o, 0, FWA_VER_LEN);
    if (version) {
        size_t n = strlen(version);
        memcpy(o, version, n < FWA_VER_LEN - 1 ? n : FWA_VER_LEN - 1);   // always a NUL inside
    }
}

void fwa_proof_message(uint8_t out[FWA_MSG_LEN], const fwa_proof_t *p)
{
    fwa_message(out, p->nonce, p->image_len, p->digest, p->version);
}

int fwa_proof_encode(uint8_t out[FWA_PROOF_LEN], const fwa_proof_t *p)
{
    out[0] = FWA_CTRL_PROOF;
    fwa_proof_message(out + 1, p);
    memcpy(out + 1 + FWA_MSG_LEN, p->sig, FWA_SIG_LEN);
    return FWA_PROOF_LEN;
}

bool fwa_proof_decode(const uint8_t *in, size_t n, fwa_proof_t *out)
{
    if (!in || !out || n != FWA_PROOF_LEN || in[0] != FWA_CTRL_PROOF) return false;
    const uint8_t *v = in + 1 + FWA_NONCE_LEN + 4 + FWA_DIGEST_LEN;
    if (!memchr(v, 0, FWA_VER_LEN)) return false;    // the version string must end inside its 32 bytes
    memcpy(out->nonce, in + 1, FWA_NONCE_LEN);
    out->image_len = get32(in + 1 + FWA_NONCE_LEN);
    memcpy(out->digest, in + 1 + FWA_NONCE_LEN + 4, FWA_DIGEST_LEN);
    memcpy(out->version, v, FWA_VER_LEN);
    memcpy(out->sig, in + 1 + FWA_MSG_LEN, FWA_SIG_LEN);
    return true;
}

void fwa_challenge_encode(uint8_t out[FWA_CHAL_LEN], const uint8_t nonce[FWA_NONCE_LEN])
{
    out[0] = (uint8_t)FWA_SECTOR_AUTH;
    out[1] = (uint8_t)(FWA_SECTOR_AUTH >> 8);
    out[2] = FWA_ST_CHALLENGE;
    out[3] = 0;
    memcpy(out + 4, nonce, FWA_NONCE_LEN);
}

bool fwa_challenge_decode(const uint8_t *in, size_t n, uint8_t nonce[FWA_NONCE_LEN])
{
    if (!in || n < FWA_CHAL_LEN) return false;
    if ((in[0] | in[1] << 8) != FWA_SECTOR_AUTH || in[2] != FWA_ST_CHALLENGE) return false;
    memcpy(nonce, in + 4, FWA_NONCE_LEN);
    return true;
}

void fwa_verdict_encode(uint8_t out[FWA_VERDICT_LEN], uint8_t verdict)
{
    memset(out, 0, FWA_VERDICT_LEN);
    out[0] = (uint8_t)FWA_SECTOR_AUTH;
    out[1] = (uint8_t)(FWA_SECTOR_AUTH >> 8);
    out[2] = FWA_ST_VERDICT;
    out[3] = verdict;
}

int fwa_verdict_decode(const uint8_t *in, size_t n)
{
    if (!in || n < 4) return -1;
    if ((in[0] | in[1] << 8) != FWA_SECTOR_AUTH || in[2] != FWA_ST_VERDICT) return -1;
    return in[3];
}

// ---- nonce ----

void fwa_nonce_issue(fwa_nonce_t *st, const uint8_t nonce[FWA_NONCE_LEN], uint32_t now_ms)
{
    memcpy(st->nonce, nonce, FWA_NONCE_LEN);
    st->issued_ms = now_ms;
    st->live = true;
}

void fwa_nonce_clear(fwa_nonce_t *st)
{
    fwa_zero(st, sizeof(*st));
}

fwa_result_t fwa_nonce_take(fwa_nonce_t *st, const uint8_t nonce[FWA_NONCE_LEN], uint32_t now_ms)
{
    if (!st->live) return FWA_NO_NONCE;
    bool same = fwa_ct_equal(st->nonce, nonce, FWA_NONCE_LEN);
    uint32_t age = now_ms - st->issued_ms;            // wraps correctly; a clock read "before" issue counts as 0
    if ((int32_t)age < 0) age = 0;
    fwa_nonce_clear(st);                              // single use: whatever the outcome
    if (!same) return FWA_NONCE_MISMATCH;
    if (age > FWA_NONCE_MS) return FWA_NONCE_EXPIRED;
    return FWA_OK;
}

fwa_result_t fwa_check(fwa_nonce_t *st, const fwa_proof_t *p, const uint8_t own_ver[4], uint32_t now_ms)
{
    fwa_result_t r = fwa_nonce_take(st, p->nonce, now_ms);
    if (r != FWA_OK) return r;
    uint8_t v[4], zero[4] = { 0, 0, 0, 0 };
    mp_ver_from_string(p->version, v);
    if (!memcmp(v, zero, 4)) return FWA_BAD_VERSION;
    if (mp_ver_cmp(v, own_ver) <= 0) return FWA_NOT_NEWER;
    return FWA_OK;
}

bool fwa_start_matches(const fwa_proof_t *p, uint32_t image_len, const uint8_t digest[FWA_DIGEST_LEN])
{
    bool d = fwa_ct_equal(p->digest, digest, FWA_DIGEST_LEN);
    return d && p->image_len == image_len;
}

const char *fwa_result_text(fwa_result_t r)
{
    switch (r) {
    case FWA_OK: return "ok";
    case FWA_NO_NONCE: return "no nonce";
    case FWA_NONCE_MISMATCH: return "nonce mismatch";
    case FWA_NONCE_EXPIRED: return "nonce expired";
    case FWA_NOT_NEWER: return "signed version is not newer";
    case FWA_BAD_VERSION: return "signed version is not a version";
    }
    return "?";
}

uint8_t fwa_result_verdict(fwa_result_t r)
{
    switch (r) {
    case FWA_OK: return FWA_V_OK;
    case FWA_NO_NONCE: case FWA_NONCE_MISMATCH: case FWA_NONCE_EXPIRED: return FWA_V_NONCE;
    case FWA_NOT_NEWER: return FWA_V_NOT_NEWER;
    case FWA_BAD_VERSION: return FWA_V_BAD_PROOF;
    }
    return FWA_V_BAD_PROOF;
}

// ---- base64 ----

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t fwa_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = (n + 2) / 3 * 4;
    if (!out || cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int fwa_b64_decode(const char *in, uint8_t *out, size_t cap)
{
    if (!in || !out) return -1;
    size_t n = strlen(in);
    if (n == 0 || n % 4) return -1;
    size_t pad = in[n - 1] == '=' ? (in[n - 2] == '=' ? 2 : 1) : 0;
    size_t bytes = n / 4 * 3 - pad;
    if (bytes > cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            bool tail = i + 4 == n && k >= 4 - (int)pad;   // the padding positions of the last quad
            v[k] = tail ? (c == '=' ? 0 : -1) : b64_val(c);
            if (v[k] < 0) return -1;
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        if (i + 4 == n && (w & (pad == 2 ? 0xFFFFu : pad == 1 ? 0xFFu : 0u))) return -1;   // canonical only
        uint8_t b[3] = { (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
        for (int k = 0; k < 3 && o < bytes; k++) out[o++] = b[k];
    }
    return (int)bytes;
}

void fwa_short_id(const uint8_t pub[FWA_PUB_LEN], char out[FWA_SHORT_LEN + 1])
{
    char b[13];
    fwa_b64_encode(pub, 9, b, sizeof(b));              // 9 bytes -> 12 characters, no padding
    memcpy(out, b, FWA_SHORT_LEN);
    out[FWA_SHORT_LEN] = 0;
}
