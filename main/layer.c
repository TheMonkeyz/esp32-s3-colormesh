// A page's pixels over the mesh background (layer.h)
#include "layer.h"
#include <string.h>

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

static uint16_t swap565(unsigned r, unsigned g, unsigned b)
{
    unsigned px = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    return (uint16_t)(px << 8 | px >> 8);
}

bool layer_build(layer_t *l, const uint8_t *argb, int w, int h, size_t stride)
{
    l->nseg = l->nedge = l->npix = 0;
    l->h = 0;
    if (h > LAYER_MAX_H) return false;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = argb + (size_t)y * stride;
        l->seg_first[y] = l->nseg;
        l->edge_first[y] = l->nedge;
        for (int x = 0; x < w;) {
            uint8_t a = row[x * 4 + 3];
            if (!a) { x++; continue; }
            if (a < 255) {
                if (l->nedge == l->max_edge) return false;
                // stored for a blend in RGB565 itself: 5/6/5-bit colour, alpha in 0..64
                l->edge[l->nedge++] = (layer_edge_t){ (uint16_t)x, (uint8_t)((a * 64 + 127) / 255), row[x * 4 + 2] >> 3,
                                                      row[x * 4 + 1] >> 2, row[x * 4] >> 3 };
                x++;
                continue;
            }
            int x0 = x;
            while (x < w && row[x * 4 + 3] == 255) x++;
            if (l->nseg == l->max_seg || l->npix + (uint32_t)(x - x0) > l->max_pix) return false;
            l->seg[l->nseg++] = (layer_seg_t){ (uint16_t)x0, (uint16_t)(x - x0), l->npix };
            for (int i = x0; i < x; i++) l->pix[l->npix++] = swap565(row[i * 4 + 2], row[i * 4 + 1], row[i * 4]);
        }
    }
    l->seg_first[h] = l->nseg;
    l->edge_first[h] = l->nedge;
    l->h = h;
    return true;
}

void layer_draw(const layer_t *l, uint16_t *dst, int y0, int n, int w)
{
    for (int r = 0; r < n; r++) {
        int y = y0 + r;
        if (y < 0 || y >= l->h) continue;
        uint16_t *d = dst + (size_t)r * w;
        for (uint32_t k = l->seg_first[y]; k < l->seg_first[y + 1]; k++)
            memcpy(d + l->seg[k].x0, l->pix + l->seg[k].off, (size_t)l->seg[k].n * 2);
        for (uint32_t k = l->edge_first[y]; k < l->edge_first[y + 1]; k++) {   // an edge: blended in RGB565
            const layer_edge_t *e = &l->edge[k];
            unsigned px = (uint16_t)(d[e->x] << 8 | d[e->x] >> 8), a = e->a, ia = 64 - a;
            unsigned r = (e->r * a + (px >> 11) * ia + 32) >> 6, g = (e->g * a + ((px >> 5) & 63) * ia + 32) >> 6;
            unsigned b = (e->b * a + (px & 31) * ia + 32) >> 6;
            px = r << 11 | g << 5 | b;
            d[e->x] = (uint16_t)(px << 8 | px >> 8);
        }
    }
}