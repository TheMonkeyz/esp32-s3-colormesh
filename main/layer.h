#pragma once
#include <stdint.h>

// A page's own pixels over the mesh background (ui.c: the waves behind the pick, voice and game pages). The page is
// a picture with alpha (LVGL ARGB8888: bytes B, G, R, A; its background transparent); the background is a band of
// the raw frame (RGB565 byte-swapped, as wave_fill writes it and display_raw_frame sends it). Each pixel of the band
// becomes the picture's pixel blended over it by its alpha (0 = background kept, 255 = picture). Pure C99,
// host-tested (tests/host/test_wave.c).
void layer_over(uint16_t *dst, const uint8_t *argb, int n);
