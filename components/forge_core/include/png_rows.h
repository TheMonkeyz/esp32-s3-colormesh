#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PNG decoding one row at a time, with the ESP32-S3 ROM's inflate (tinfl): ~50 KB of working memory whatever the image
// size. LVGL's lodepng decodes the whole image at once: in weather_amoled a 466x466 radar frame peaked at ~2-3 MB of
// PSRAM in two big blocks, which failed once other big users had fragmented PSRAM. 8-bit grey, RGB, palette (+tRNS),
// grey+alpha and RGBA, and 1, 2 or 4-bit grey and palette, not interlaced (GeoMet's and OpenStreetMap's images; OSM
// saves tiles with few colours as 4-bit palettes). Not 16-bit samples.
// cb(y, rgba, w, user): row y as RGBA8888 (w pixels, valid only during the call); return false to stop early.
typedef bool (*png_row_cb_t)(unsigned y, const uint8_t *rgba, unsigned w, void *user);
bool png_rows(const uint8_t *png, size_t len, png_row_cb_t cb, void *user, unsigned *w, unsigned *h);
