#pragma once
// The signed firmware pull (admin board, mesh_fw.c): the message layouts, the single-use nonce and the checks that
// need no cryptography. Pure C99, no ESP-IDF and no mbedTLS header, so tests/host/test_fw_auth.c builds it as is;
// mesh_fw.c does the ECDSA P-256 signing and verifying and the SHA-256 around these functions.
//
// The exchange, on the firmware service's characteristics (mesh_fw.c):
//   1. sender -> CTRL  04 (FWA_CTRL_CHALLENGE, 1 byte): "a challenge, please" (an admin sender, after discovery)
//   2. recv   -> ACK   notify, 36 bytes (FWA_CHAL_LEN): sector u16 LE 0xFFFE (FWA_SECTOR_AUTH) · status 3
//                (FWA_ST_CHALLENGE) · 0 · nonce[32] (esp_fill_random; valid FWA_NONCE_MS, one use)
//   3. sender -> CTRL  03 (FWA_CTRL_PROOF), 165 bytes (FWA_PROOF_LEN): 03 · nonce[32] · image_len u32 LE ·
//                image SHA-256[32] (the digest appended to the image: SHA-256 of its first image_len - 32 bytes) ·
//                version[32] (the app description's string, zero padded, NUL inside) · signature[64] (raw r || s,
//                big-endian, ECDSA P-256 with the admin's private key over SHA-256 of the 100-byte message
//                nonce || image_len || digest || version, FWA_MSG_LEN)
//   4. recv   -> ACK   notify, 20 bytes: sector 0xFFFE · status 4 (FWA_ST_VERDICT) · verdict (FWA_V_*) · zeros
//   5. sender -> CTRL  START (01 · len · digest · ver[4]) as before: on a board out of dev mode it is accepted only
//                after verdict 0, and only with the signed length and digest; at the end the receiver's SHA-256 of
//                the bytes it wrote must equal the signed digest.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FWA_NONCE_LEN       32
#define FWA_DIGEST_LEN      32
#define FWA_VER_LEN         32
#define FWA_SIG_LEN         64
#define FWA_PUB_LEN         64      // uncompressed point without the 0x04 prefix: X || Y, big-endian
#define FWA_PRIV_LEN        32
#define FWA_MSG_LEN         (FWA_NONCE_LEN + 4 + FWA_DIGEST_LEN + FWA_VER_LEN)          // 100
#define FWA_PROOF_LEN       (1 + FWA_MSG_LEN + FWA_SIG_LEN)                            // 165
#define FWA_CHAL_LEN        (4 + FWA_NONCE_LEN)                                        // 36
#define FWA_VERDICT_LEN     20
#define FWA_CTRL_PROOF      0x03
#define FWA_CTRL_CHALLENGE  0x04
#define FWA_SECTOR_AUTH     0xFFFE
#define FWA_ST_CHALLENGE    3
#define FWA_ST_VERDICT      4
#define FWA_NONCE_MS        10000
#define FWA_B64_PUB_LEN     88      // base64 of FWA_PUB_LEN bytes, with padding
#define FWA_SHORT_LEN       8       // the short id: the first 8 base64 characters of the public key

// Verdicts (byte 3 of the status 4 notification)
#define FWA_V_OK            0       // signature, nonce and version good: START must carry the signed len and digest
#define FWA_V_BAD_SIG       1
#define FWA_V_NONCE         2       // no nonce issued, another nonce, expired or already used
#define FWA_V_NOT_NEWER     3
#define FWA_V_BAD_PROOF     4       // malformed (length, version string)
#define FWA_V_UNCHECKED     5       // dev mode: proof not verified (no trusted key, or it failed): the pull goes on unsigned

typedef struct {
    uint8_t nonce[FWA_NONCE_LEN];
    uint32_t image_len;
    uint8_t digest[FWA_DIGEST_LEN];
    char version[FWA_VER_LEN];      // NUL-terminated inside the 32 bytes
    uint8_t sig[FWA_SIG_LEN];
} fwa_proof_t;

typedef enum {
    FWA_OK = 0,
    FWA_NO_NONCE,           // nothing issued (or already used)
    FWA_NONCE_MISMATCH,
    FWA_NONCE_EXPIRED,
    FWA_NOT_NEWER,          // the signed version is not strictly newer than the receiver's
    FWA_BAD_VERSION,        // the signed version string is not a version (mp_ver_from_string gives 0.0.0)
} fwa_result_t;

// The message the signature covers (FWA_MSG_LEN bytes); version is copied zero padded (cut to 31 characters)
void fwa_message(uint8_t out[FWA_MSG_LEN], const uint8_t nonce[FWA_NONCE_LEN], uint32_t image_len,
                 const uint8_t digest[FWA_DIGEST_LEN], const char *version);
// The same from a decoded proof
void fwa_proof_message(uint8_t out[FWA_MSG_LEN], const fwa_proof_t *p);

// CTRL 03 into out (FWA_PROOF_LEN bytes); returns FWA_PROOF_LEN
int  fwa_proof_encode(uint8_t out[FWA_PROOF_LEN], const fwa_proof_t *p);
// A CTRL 03 write: false on a wrong command byte, a wrong length, or a version without a NUL in its 32 bytes
bool fwa_proof_decode(const uint8_t *in, size_t n, fwa_proof_t *out);

// The status 3 notification (FWA_CHAL_LEN bytes) and its parsing (false for anything else)
void fwa_challenge_encode(uint8_t out[FWA_CHAL_LEN], const uint8_t nonce[FWA_NONCE_LEN]);
bool fwa_challenge_decode(const uint8_t *in, size_t n, uint8_t nonce[FWA_NONCE_LEN]);
// The status 4 notification (FWA_VERDICT_LEN bytes) and its parsing: the verdict, or -1 for anything else
void fwa_verdict_encode(uint8_t out[FWA_VERDICT_LEN], uint8_t verdict);
int  fwa_verdict_decode(const uint8_t *in, size_t n);

// ---- the receiver's nonce: one live at a time, single use, FWA_NONCE_MS ----
typedef struct {
    uint8_t nonce[FWA_NONCE_LEN];
    uint32_t issued_ms;
    bool live;
} fwa_nonce_t;
void fwa_nonce_issue(fwa_nonce_t *st, const uint8_t nonce[FWA_NONCE_LEN], uint32_t now_ms);   // replaces any other
void fwa_nonce_clear(fwa_nonce_t *st);                                                         // zeroed
// Consumes the live nonce whatever the outcome (a second proof with it gets FWA_NO_NONCE); constant-time compare
fwa_result_t fwa_nonce_take(fwa_nonce_t *st, const uint8_t nonce[FWA_NONCE_LEN], uint32_t now_ms);
// Every check but the signature: the nonce (taken), then the version strictly newer than own_ver (mesh_proto.h's
// 4-byte form)
fwa_result_t fwa_check(fwa_nonce_t *st, const fwa_proof_t *p, const uint8_t own_ver[4], uint32_t now_ms);
// START's length and digest against the signed ones (constant-time digest compare)
bool fwa_start_matches(const fwa_proof_t *p, uint32_t image_len, const uint8_t digest[FWA_DIGEST_LEN]);
const char *fwa_result_text(fwa_result_t r);    // "ok", "no nonce", "nonce mismatch", "nonce expired", ...
uint8_t fwa_result_verdict(fwa_result_t r);     // FWA_V_*

// ---- helpers ----
bool fwa_ct_equal(const void *a, const void *b, size_t n);   // constant time in n
void fwa_zero(void *p, size_t n);                            // not optimised away
// Standard base64 with padding; returns the length written (NUL added), 0 when cap is too small
size_t fwa_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
// Strict: the alphabet, padding only at the end, length a multiple of 4; returns the bytes written, -1 on bad input
// or when out is too small
int  fwa_b64_decode(const char *in, uint8_t *out, size_t cap);
// The short id of a public key: the first FWA_SHORT_LEN base64 characters (out: FWA_SHORT_LEN + 1)
void fwa_short_id(const uint8_t pub[FWA_PUB_LEN], char out[FWA_SHORT_LEN + 1]);
