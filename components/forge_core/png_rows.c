// PNG decoding one row at a time (see png_rows.h)
#include "png_rows.h"
#include <stdlib.h>
#include <string.h>
#include "miniz.h"                       // tinfl, in the ESP32-S3 ROM
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "png";

typedef struct {
    unsigned w, h, y;
    int ct, bpp, depth;                  // colour type, bytes per pixel (1 below 8 bits: the filters' distance), bits
    size_t row_bytes, fill;              // per row, filter byte excluded; bytes in cur so far (filter byte included)
    uint8_t *mem;                        // the allocation holding the rows below (cur and prev swap after each row)
    uint8_t *cur, *prev, *rgba;          // cur[0] = filter type
    uint8_t pal[256][4];
    png_row_cb_t cb;
    void *user;
    bool stop;
} st_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static inline uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void row_done(st_t *s)
{
    uint8_t *r = s->cur + 1, *u = s->prev + 1;
    int n = s->row_bytes, bpp = s->bpp;
    switch (s->cur[0]) {                 // undo the row's filter
    case 1: for (int i = bpp; i < n; i++) r[i] += r[i - bpp]; break;
    case 2: for (int i = 0; i < n; i++) r[i] += u[i]; break;
    case 3:
        for (int i = 0; i < n; i++) r[i] += ((i >= bpp ? r[i - bpp] : 0) + u[i]) >> 1;
        break;
    case 4:
        for (int i = 0; i < n; i++) r[i] += paeth(i >= bpp ? r[i - bpp] : 0, u[i], i >= bpp ? u[i - bpp] : 0);
        break;
    default: break;
    }
    uint8_t *o = s->rgba;
    if (s->depth < 8) {                  // 1, 2 or 4 bits (grey or palette), the leftmost pixel in the high bits
        unsigned max = (1u << s->depth) - 1;
        for (unsigned x = 0; x < s->w; x++, o += 4) {
            unsigned bit = x * s->depth, v = r[bit >> 3] >> (8 - s->depth - (bit & 7)) & max;
            if (s->ct == 3) memcpy(o, s->pal[v], 4);
            else { o[0] = o[1] = o[2] = v * 255 / max; o[3] = 255; }
        }
    }
    for (unsigned x = 0; s->depth == 8 && x < s->w; x++, o += 4) {
        const uint8_t *p = r + x * bpp;
        switch (s->ct) {
        case 6: memcpy(o, p, 4); break;
        case 2: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
        case 3: memcpy(o, s->pal[p[0]], 4); break;
        case 0: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
        case 4: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
        }
    }
    if (!s->cb(s->y, s->rgba, s->w, s->user)) s->stop = true;
    s->y++;
    uint8_t *t = s->prev; s->prev = s->cur; s->cur = t;
    s->fill = 0;
}

static void take(st_t *s, const uint8_t *b, size_t n)
{
    while (n && !s->stop && s->y < s->h) {
        size_t k = 1 + s->row_bytes - s->fill;
        if (k > n) k = n;
        memcpy(s->cur + s->fill, b, k);
        s->fill += k; b += k; n -= k;
        if (s->fill == 1 + s->row_bytes) row_done(s);
    }
}

bool png_rows(const uint8_t *png, size_t len, png_row_cb_t cb, void *user, unsigned *w, unsigned *h)
{
    if (len < 33 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) || memcmp(png + 12, "IHDR", 4)) return false;
    st_t *s = heap_caps_calloc(1, sizeof(st_t), MALLOC_CAP_SPIRAM);
    tinfl_decompressor *d = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
    bool ok = false;
    if (!s || !d || !dict) goto out;
    s->w = be32(png + 16); s->h = be32(png + 20);
    int depth = s->depth = png[24]; s->ct = png[25];
    // 1, 2 and 4 bits only exist for grey and palette: OpenStreetMap saves tiles with few colours as 4-bit palettes
    // (one of the weather display's zoom-4 tiles, refused until v0.2.1: that map level was never complete)
    bool small = (depth == 1 || depth == 2 || depth == 4) && (s->ct == 0 || s->ct == 3);
    if ((depth != 8 && !small) || png[28] != 0 || s->w == 0 || s->w > 4096 || s->h == 0 || s->h > 4096) {
        ESP_LOGW(TAG, "unsupported: depth %d, colour type %d, interlace %d, %ux%u", depth, s->ct, png[28], s->w, s->h);
        goto out;
    }
    s->bpp = s->ct == 6 ? 4 : s->ct == 2 ? 3 : s->ct == 4 ? 2 : (s->ct == 0 || s->ct == 3) ? 1 : 0;
    if (!s->bpp) { ESP_LOGW(TAG, "unsupported colour type %d", s->ct); goto out; }
    s->row_bytes = small ? ((size_t)s->w * depth + 7) / 8 : (size_t)s->w * s->bpp;
    s->mem = s->cur = heap_caps_calloc(2 * (1 + s->row_bytes) + s->w * 4, 1, MALLOC_CAP_SPIRAM);
    if (!s->mem) goto out;
    s->prev = s->cur + 1 + s->row_bytes;              // the row "above" the first one is zeros
    s->rgba = s->prev + 1 + s->row_bytes;
    s->cb = cb; s->user = user;
    for (int i = 0; i < 256; i++) s->pal[i][3] = 255;
    tinfl_init(d);
    size_t dict_ofs = 0;
    bool done = false;
    for (size_t p = 8; p + 12 <= len && !done && !s->stop;) {
        uint32_t n = be32(png + p);
        const uint8_t *type = png + p + 4, *data = png + p + 8;
        if (n > len - p - 12) break;                     // (p + 12 + n wrapped around for a huge length)
        if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < n / 3 && i < 256; i++) memcpy(s->pal[i], data + i * 3, 3);
        } else if (!memcmp(type, "tRNS", 4) && s->ct == 3) {
            for (uint32_t i = 0; i < n && i < 256; i++) s->pal[i][3] = data[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            size_t in_ofs = 0;
            for (;;) {
                size_t in_n = n - in_ofs, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
                tinfl_status r = tinfl_decompress(d, data + in_ofs, &in_n, dict, dict + dict_ofs, &out_n,
                                                  TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                in_ofs += in_n;
                take(s, dict + dict_ofs, out_n);
                dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
                if (r == TINFL_STATUS_DONE) { done = true; break; }
                if (r < 0) { ESP_LOGW(TAG, "inflate error %d", r); goto out; }
                if (r == TINFL_STATUS_NEEDS_MORE_INPUT && in_ofs >= n) break;      // next IDAT
                if (s->stop) break;
            }
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + n;
    }
    ok = s->y == s->h || s->stop;
    if (!ok) ESP_LOGW(TAG, "truncated: %u of %u rows", s->y, s->h);
    if (w) *w = s->w;
    if (h) *h = s->h;
out:
    if (s) free(s->mem);                 // not s->cur: it may point to the second row by now
    free(s);
    free(d);
    free(dict);
    return ok;
}
