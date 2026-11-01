#pragma once
#include <stddef.h>
#include <stdint.h>

// IMA ADPCM (4 bits per sample, low nibble first) for the voice clips (clip.c): 16-bit mono in, 4:1 out. One
// continuous stream per clip, both sides start from adpcm_init(). Pure C99, host-tested (tests/host/test_adpcm.c).
typedef struct {
    int32_t pred;       // predicted sample
    int8_t index;       // step table index 0..88
} adpcm_state_t;

void adpcm_init(adpcm_state_t *s);
// n samples (n even) to n / 2 bytes
void adpcm_encode(adpcm_state_t *s, const int16_t *in, size_t n, uint8_t *out);
// n bytes to 2 * n samples
void adpcm_decode(adpcm_state_t *s, const uint8_t *in, size_t n, int16_t *out);
