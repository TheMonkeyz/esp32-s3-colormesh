// A path drawn into a map picture (forge_map.h fmap_path_draw): pure C, host-tested (tests/host/test_map.c). From
// esp32-s3-rtcquebec's map_compose (its route over the map), without its second 434 KB canvas: the pixels under the
// path are recorded so the path can be undone.
#include "map_priv.h"
#include <math.h>
#include <string.h>

uint16_t fmap_blend565(uint16_t dst, uint16_t src, uint8_t opa)
{
    if (opa >= 255) return src;
    if (!opa) return dst;
    int a = opa, b = 255 - opa;
    int r = ((dst >> 11) * b + (src >> 11) * a) / 255;
    int g = (((dst >> 5) & 63) * b + ((src >> 5) & 63) * a) / 255;
    int bl = ((dst & 31) * b + (src & 31) * a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

typedef struct {
    uint16_t *px; int w, h; uint16_t c; uint8_t opa; uint8_t *mask;
    fmap_undo_t *undo; int undo_cap, undo_n; bool full; int changed;
} painter_t;

static void dot(painter_t *p, int x, int y)
{
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return;
    uint32_t i = (uint32_t)y * p->w + x;
    if (p->mask[i >> 3] & (1u << (i & 7))) return;        // blended once
    p->mask[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (p->undo && p->undo_n < p->undo_cap) p->undo[p->undo_n++] = (fmap_undo_t){ i, p->px[i] };
    else if (p->undo) p->full = true;
    p->px[i] = fmap_blend565(p->px[i], p->c, p->opa);
    p->changed++;
}

static void disc(painter_t *p, double cx, double cy, double r)
{
    int ri = (int)ceil(r);
    for (int dy = -ri; dy <= ri; dy++)
        for (int dx = -ri; dx <= ri; dx++)
            if (dx * dx + dy * dy <= r * r + 0.25) dot(p, (int)lround(cx) + dx, (int)lround(cy) + dy);
}

int fmap_path_draw(uint16_t *px, int w, int h, int z, double ox, double oy, const fmap_place_t *pts, int n,
                   uint16_t c565, int width_px, uint8_t opa, uint8_t *mask, fmap_undo_t *undo, int undo_cap,
                   bool *undo_full)
{
    if (undo_full) *undo_full = false;
    if (!px || !mask || w <= 0 || h <= 0 || !pts || n <= 0) return 0;
    memset(mask, 0, ((size_t)w * h + 7) / 8);
    painter_t p = { px, w, h, c565, opa, mask, undo, undo ? undo_cap : 0, 0, false, 0 };
    double r = (width_px < 1 ? 1 : width_px) / 2.0;
    double x0, y0;
    geo_to_view(pts[0].lat, pts[0].lon, z, ox, oy, &x0, &y0);
    if (n == 1) disc(&p, x0, y0, r);
    for (int i = 1; i < n; i++) {
        double x1, y1;
        geo_to_view(pts[i].lat, pts[i].lon, z, ox, oy, &x1, &y1);
        // Skip a segment wholly outside the picture (with the line's width as margin)
        double lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
        if (hx >= -r && lx <= w + r && hy >= -r && ly <= h + r) {
            double len = hypot(x1 - x0, y1 - y0);
            int steps = (int)ceil(len * 2) + 1;           // a disc every half pixel: no gaps at any angle
            for (int k = 0; k <= steps; k++) disc(&p, x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps, r);
        }
        x0 = x1;
        y0 = y1;
    }
    if (undo_full) *undo_full = p.full;
    return p.changed;
}

void fmap_path_undo(uint16_t *px, const fmap_undo_t *undo, int n)
{
    if (!px || !undo) return;
    for (int i = n - 1; i >= 0; i--) px[undo[i].idx] = undo[i].old;
}
