// IMA ADPCM (adpcm.h): the standard step and index tables (IMA Recommended Practices, 1992)
#include "adpcm.h"

static const int16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};
static const int8_t index_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

void adpcm_init(adpcm_state_t *s)
{
    s->pred = 0;
    s->index = 0;
}

// One nibble applied to the state (shared by both sides, so the encoder tracks what the decoder will hear)
static int16_t step_apply(adpcm_state_t *s, uint8_t code)
{
    int32_t step = steps[s->index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    s->pred += (code & 8) ? -diff : diff;
    if (s->pred > 32767) s->pred = 32767;
    else if (s->pred < -32768) s->pred = -32768;
    int idx = s->index + index_adj[code & 7];
    s->index = (int8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return (int16_t)s->pred;
}

static uint8_t encode_one(adpcm_state_t *s, int16_t x)
{
    int32_t step = steps[s->index];
    int32_t diff = (int32_t)x - s->pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) code |= 1;
    step_apply(s, code);
    return code;
}

void adpcm_encode(adpcm_state_t *s, const int16_t *in, size_t n, uint8_t *out)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t lo = encode_one(s, in[i]);
        uint8_t hi = encode_one(s, in[i + 1]);
        out[i / 2] = (uint8_t)(lo | hi << 4);
    }
}

void adpcm_decode(adpcm_state_t *s, const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = step_apply(s, in[i] & 0x0F);
        out[2 * i + 1] = step_apply(s, in[i] >> 4);
    }
}
