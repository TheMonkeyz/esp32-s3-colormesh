// png_rows.c: PNGs built here (miniz's deflate, the same library as the ROM's inflate) decoded row by row and compared
// pixel by pixel: grey, RGB, palette (+ tRNS), grey+alpha, RGBA; grey and palette at 1, 2 and 4 bits too; the five row
// filters; data split over two IDAT chunks; a chunk length that wraps around (it read past the buffer on 32-bit before
// v1.12.0); a real OpenStreetMap tile saved with 4-bit palette indices (refused until espforge v0.2.1, so the
// weather display never cached that zoom level's map).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "miniz.h"
#include "png_rows.h"
#include "osm_tile.h"

#define W 7                              // odd: sub-byte rows end with padding bits
#define H 5

static uint8_t want[H][W][4];           // expected RGBA
static uint8_t got[H][W][4];
static int rows_seen;

static bool on_row(unsigned y, const uint8_t *rgba, unsigned w, void *user)
{
    if (y < H && w == W) memcpy(got[y], rgba, W * 4);
    rows_seen++;
    return true;
}

static uint8_t *put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; return p + 4; }

static uint8_t *chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t n)
{
    p = put32(p, n);
    memcpy(p, type, 4);
    if (n) memcpy(p + 4, data, n);
    p += 4 + n;
    return put32(p, 0);                  // CRC: png_rows doesn't check it (the TLS layer does integrity)
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Bytes per pixel for colour type ct at 8 bits (the filters' distance is 1 byte below 8 bits)
static int bpp_of(int ct) { return ct == 6 ? 4 : ct == 2 ? 3 : ct == 4 ? 2 : 1; }

static size_t build(uint8_t *png, int ct, int depth, int filter_mode, bool split)
{
    int bpp = bpp_of(ct), rb = depth < 8 ? (W * depth + 7) / 8 : W * bpp, max = (1 << depth) - 1;
    uint8_t raw[H][W * 4], fil[H * (1 + W * 4)];
    memset(raw, 0, sizeof(raw));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *o = &raw[y][x * bpp], *e = want[y][x];
            uint8_t r = (uint8_t)(x * 37 + y * 11), g = (uint8_t)(y * 53 + 7), b = (uint8_t)(x * y * 19), a = (uint8_t)(255 - x * 30);
            int idx = (x + y) % (depth == 1 ? 2 : 4);
            static const uint8_t pal[4][4] = { {10, 20, 30, 255}, {200, 0, 0, 128}, {0, 255, 0, 0}, {1, 2, 3, 255} };
            if (depth < 8) {                 // packed, the leftmost pixel in the high bits
                int v = ct == 3 ? idx : (x * 3 + y * 5) % (max + 1);
                raw[y][x * depth / 8] |= v << (8 - depth - x * depth % 8);
                if (ct == 3) memcpy(e, pal[idx], 4);
                else { e[0] = e[1] = e[2] = (uint8_t)(v * 255 / max); e[3] = 255; }
                continue;
            }
            switch (ct) {
            case 0: o[0] = r; e[0] = e[1] = e[2] = r; e[3] = 255; break;
            case 2: o[0] = r; o[1] = g; o[2] = b; e[0] = r; e[1] = g; e[2] = b; e[3] = 255; break;
            case 3: o[0] = idx; memcpy(e, pal[idx], 4); break;
            case 4: o[0] = r; o[1] = a; e[0] = e[1] = e[2] = r; e[3] = a; break;
            case 6: o[0] = r; o[1] = g; o[2] = b; o[3] = a; e[0] = r; e[1] = g; e[2] = b; e[3] = a; break;
            }
        }
    if (depth < 8) bpp = 1;
    size_t k = 0;
    for (int y = 0; y < H; y++) {
        int f = filter_mode < 5 ? filter_mode : y % 5;     // 5: every row a different filter
        fil[k++] = f;
        for (int i = 0; i < rb; i++) {
            int cur = raw[y][i], left = i >= bpp ? raw[y][i - bpp] : 0, up = y ? raw[y - 1][i] : 0,
                ul = y && i >= bpp ? raw[y - 1][i - bpp] : 0;
            int v = f == 0 ? cur : f == 1 ? cur - left : f == 2 ? cur - up : f == 3 ? cur - ((left + up) >> 1)
                  : cur - paeth(left, up, ul);
            fil[k++] = (uint8_t)v;
        }
    }
    mz_ulong zn = compressBound(k);
    uint8_t *z = malloc(zn);
    compress(z, &zn, fil, k);
    uint8_t *p = png;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    p += 8;
    uint8_t ihdr[13] = {0};
    put32(ihdr, W);
    put32(ihdr + 4, H);
    ihdr[8] = depth;
    ihdr[9] = ct;
    p = chunk(p, "IHDR", ihdr, 13);
    if (ct == 3) {
        static const uint8_t plte[12] = { 10, 20, 30, 200, 0, 0, 0, 255, 0, 1, 2, 3 }, trns[3] = { 255, 128, 0 };
        p = chunk(p, "PLTE", plte, 12);
        p = chunk(p, "tRNS", trns, 3);
    }
    if (split) { p = chunk(p, "IDAT", z, zn / 2); p = chunk(p, "IDAT", z + zn / 2, zn - zn / 2); }
    else p = chunk(p, "IDAT", z, zn);
    p = chunk(p, "IEND", NULL, 0);
    free(z);
    return p - png;
}

static bool decode(const uint8_t *png, size_t n)
{
    memset(got, 0xEE, sizeof(got));
    rows_seen = 0;
    unsigned w = 0, h = 0;
    bool ok = png_rows(png, n, on_row, NULL, &w, &h);
    return ok && w == W && h == H && rows_seen == H;
}

// The real tile: osm_tile.h

static uint32_t tile_hash;              // FNV-1a over the RGBA rows, row by row
static unsigned tile_rows;

static bool tile_row(unsigned y, const uint8_t *rgba, unsigned w, void *user)
{
    for (unsigned i = 0; i < w * 4; i++) tile_hash = (tile_hash ^ rgba[i]) * 16777619u;
    tile_rows++;
    return true;
}

int main(void)
{
    static uint8_t png[8192];
    static const int types[] = { 0, 2, 3, 4, 6 };
    for (int t = 0; t < 5; t++)
        for (int f = 0; f <= 5; f++) {
            size_t n = build(png, types[t], 8, f, f == 5);
            CHECK(decode(png, n), "colour type %d, filter %d: not decoded", types[t], f);
            CHECK(!memcmp(want, got, sizeof(want)), "colour type %d, filter %d: pixels differ", types[t], f);
        }
    // Grey and palette at 1, 2 and 4 bits: OpenStreetMap saves tiles with few colours that way
    static const int small[] = { 0, 3 }, depths[] = { 1, 2, 4 };
    for (int t = 0; t < 2; t++)
        for (int d = 0; d < 3; d++)
            for (int f = 0; f <= 5; f++) {
                size_t n = build(png, small[t], depths[d], f, f == 5);
                CHECK(decode(png, n), "colour type %d at %d bits, filter %d: not decoded", small[t], depths[d], f);
                CHECK(!memcmp(want, got, sizeof(want)), "colour type %d at %d bits, filter %d: pixels differ",
                      small[t], depths[d], f);
            }
    // The real tile, against a reference decode (Python's zlib, unpacked the same way): 0xae1df9e4
    tile_hash = 2166136261u;
    tile_rows = 0;
    unsigned w = 0, h = 0;
    CHECK(png_rows(osm_4bit, sizeof(osm_4bit), tile_row, NULL, &w, &h) && w == 256 && h == 256 && tile_rows == 256,
          "OpenStreetMap's 4-bit tile not decoded (%ux%u, %u rows)", w, h, tile_rows);
    CHECK(tile_hash == 0xae1df9e4u, "OpenStreetMap's 4-bit tile: pixels differ (hash %08x)", (unsigned)tile_hash);
    // A chunk length that wraps p + 12 + n around: rejected, nothing read past the buffer (ASan watches)
    size_t n = build(png, 2, 8, 0, false);
    uint8_t *bad = malloc(n);
    memcpy(bad, png, n);
    put32(bad + 8 + 25, 0xFFFFFFF8u);                    // the chunk after IHDR (IDAT)
    CHECK(!decode(bad, n), "a wrapped chunk length was accepted");
    free(bad);
    // Not a PNG, too short, 16-bit depth, and bit depths PNG only allows for grey and palette: refused
    CHECK(!png_rows((const uint8_t *)"not a png at all, really not one", 33, on_row, NULL, NULL, NULL), "not a PNG");
    n = build(png, 2, 8, 0, false);
    png[8 + 8 + 8] = 16;
    CHECK(!decode(png, n), "16-bit depth accepted");
    png[8 + 8 + 8] = 4;
    CHECK(!decode(png, n), "4-bit RGB accepted");
    n = build(png, 3, 4, 0, false);
    png[8 + 8 + 8] = 3;
    CHECK(!decode(png, n), "3-bit palette accepted");
    return check_done("png");
}
