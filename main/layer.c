// A page's pixels over the mesh background (layer.h)
#include "layer.h"

void layer_over(uint16_t *dst, const uint8_t *argb, int n)
{
    for (int i = 0; i < n; i++, argb += 4) {
        unsigned a = argb[3];
        if (!a) continue;                            // most of a page is transparent: one test per pixel
        unsigned b = argb[0], g = argb[1], r = argb[2];
        if (a < 255) {
            unsigned px = (uint16_t)(dst[i] << 8 | dst[i] >> 8);   // un-swap
            unsigned br = (px >> 11) << 3 | (px >> 13), bg = ((px >> 5) & 63) << 2 | ((px >> 9) & 3);
            unsigned bb = (px & 31) << 3 | ((px >> 2) & 7);
            r = (r * a + br * (255 - a) + 127) / 255;
            g = (g * a + bg * (255 - a) + 127) / 255;
            b = (b * a + bb * (255 - a) + 127) / 255;
        }
        unsigned px = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
        dst[i] = (uint16_t)(px << 8 | px >> 8);      // swap back
    }
}
