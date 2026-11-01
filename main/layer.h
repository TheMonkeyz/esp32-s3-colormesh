#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A page's own pixels over the mesh background (ui.c: the waves behind the pick, voice and game pages). The page is
// a picture with alpha (LVGL ARGB8888: bytes B, G, R, A; its background transparent); the background is a band of
// the raw frame (RGB565 byte-swapped, as wave_fill writes it and display_raw_frame sends it). Each pixel of the band
// becomes the picture's pixel blended over it by its alpha (0 = background kept, 255 = picture). Pure C99,
// host-tested (tests/host/test_wave.c).
void layer_over(uint16_t *dst, const uint8_t *argb, int n);

/* The same picture compiled once (when it is taken) for a fast paste in every frame: its opaque pixels as segments of
 * ready RGB565 byte-swapped pixels (one memcpy each), its partly transparent ones (the anti-aliased edges) as a list
 * blended one by one (in RGB565, alpha in 64 steps: within a step of layer_over). Per frame, a page then costs a few
 * block copies a row instead of a test and a conversion per pixel (pick and voice: 37-39 fps with layer_over over
 * each row's runs, v0.7.1-smooth.3). The caller gives the
 * arrays (PSRAM is fine) and their sizes; layer_build returns false when one is too small (then nothing is drawn). */
typedef struct { uint16_t x0, n; uint32_t off; } layer_seg_t;
typedef struct { uint16_t x; uint8_t a, r, g, b; } layer_edge_t;   // a: 0..64; r, b: 5 bits; g: 6 bits
#define LAYER_MAX_H 480
typedef struct {
    layer_seg_t *seg;
    layer_edge_t *edge;
    uint16_t *pix;
    uint32_t max_seg, max_edge, max_pix;
    uint32_t nseg, nedge, npix;
    int h;
    uint32_t seg_first[LAYER_MAX_H + 1], edge_first[LAYER_MAX_H + 1];   // row y: [first[y], first[y + 1])
} layer_t;

bool layer_build(layer_t *l, const uint8_t *argb, int w, int h, size_t stride);
// Rows [y0, y0 + n) of the frame, `w` pixels each, from dst
void layer_draw(const layer_t *l, uint16_t *dst, int y0, int n, int w);
