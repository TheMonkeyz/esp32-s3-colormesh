#pragma once
// Bicolour waves over the mesh (docs/IDEAS.md, "Animations over the mesh"): each wave is a stateless function of
// (now - start), drawn into rows of a 466x466 RGB565 frame for display_raw_frame. Waves from several boards and the
// holders' solid mix merge per pixel like the colours do: summed in linear light, normalised so the largest channel
// is full (mp_mix's rule, per pixel). Pure C99, no ESP-IDF: the same file builds in the host tests (test_wave.c).
//
// Geometry: the disc is the screen's circle, radius 233 around the screen centre measured to pixel centres (so it is
// symmetric like LVGL's 466 px circle: pixel (x, y) is inside when (2x+1-466)^2 + (2y+1-466)^2 <= 466^2); the edge
// ring is its outer WAVE_RING_PX, as the main screen draws it. A centre wave's distance is from pixel (233, 233).
// A wave's crest colour is the sender's colour A, its trough colour B: the sender's chosen one (b2_set), else
// wave_opposite(A); its phase at a pixel is
// (d - WAVE_SPEED_PX_S * t) mod WAVE_PITCH_PX, d the distance from its origin: centre: radial; left: x;
// right: 465 - x; top: y; bottom: 465 - y. Crests move away from the origin. It fades out over its last 500 ms.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { WAVE_NONE = 0, WAVE_CENTRE = 1, WAVE_LEFT = 2, WAVE_RIGHT = 3, WAVE_TOP = 4, WAVE_BOTTOM = 5 } wave_kind_t;
// colour A = the sender's colour; colour B = b2r, b2g, b2b when b2_set, else wave_opposite(A). The fields after
// start_ms may be left out of an initialiser (zero: automatic).
typedef struct { uint8_t kind; uint8_t r, g, b; uint32_t start_ms; uint8_t b2r, b2g, b2b; bool b2_set; } wave_t;
#define WAVE_MAX 8                 // wave_fill draws at most this many waves (more are ignored)
#define WAVE_DURATION_MS 4000      // a wave runs this long after its start
#define WAVE_FADE_MS 500           // ...and fades out over its last WAVE_FADE_MS
#define WAVE_PITCH_PX 64           // distance between two crests
#define WAVE_SPEED_PX_S 160        // crest speed
#define WAVE_W 466
#define WAVE_H 466
#define WAVE_RING_PX 14            // the own-colour edge ring, as the main screen draws it

// Bytes the caller must give wave_init (88172, about 86 KB): the quarter centre-distance table (234 x 234) plus the
// colour tables and per-frame scratch wave_fill uses. PSRAM is fine (heap_caps_malloc(..., MALLOC_CAP_SPIRAM));
// internal RAM is faster if there is room. Any alignment malloc gives (4 bytes) is enough.
size_t wave_table_bytes(void);
// Builds the tables once into table_mem (wave_table_bytes() bytes, kept for as long as wave_fill is used).
// Calling it again with another buffer moves to that buffer. NULL: wave_fill draws black until a real init.
void wave_init(void *table_mem);
// Hue + 180 degrees with the same saturation and value (HSV): each channel c -> max + min - c. Grey stays grey.
void wave_opposite(uint8_t r, uint8_t g, uint8_t b, uint8_t *or_, uint8_t *og, uint8_t *ob);
// A known kind (WAVE_CENTRE..WAVE_BOTTOM) and now - start < WAVE_DURATION_MS (unsigned, so wrap-safe; a start in
// the future is not active). NULL: false.
bool wave_active(const wave_t *w, uint32_t now_ms);
/* Fill rows [y0, y0+n) of the frame into dst (n*WAVE_W pixels, RGB565 byte-swapped, the order display_raw_frame
 * wants): black outside the disc, the edge ring in ring colour (as given, no mixing), inside: the holders' solid mix
 * (hr, hg, hb while holding) plus every active wave, summed in linear light and normalised so the largest channel is
 * full. A fading wave counts with its weight w (1 -> 0 over its last WAVE_FADE_MS): the normalised sum is scaled by
 * min(1, holding + sum of the waves' weights), so a lone wave fades to black and a wave over a holder fades into the
 * holder's colour. Integer math; the per-frame tables are rebuilt only when now_ms, the waves or the holder change,
 * so call it band by band with the same arguments for one frame (the first band of a frame pays for them).
 * Speed: waves of at most two groups (centre / left+right / top+bottom), any number of each, with or without a
 * holder: a table read, a LUT read and a store per pixel. All three groups at once: a normalisation per pixel,
 * several times slower (about 0.75 ms vs 0.14 ms a frame on the PC; the ESP32-S3 has not been measured).
 * Not reentrant (one caller task: the display's fill callback). Rows outside [0, WAVE_H) are filled black. */
void wave_fill(uint16_t *dst, int y0, int n, const wave_t *waves, int nwaves, uint32_t now_ms,
               uint8_t hr, uint8_t hg, uint8_t hb, bool holding, uint8_t ring_r, uint8_t ring_g, uint8_t ring_b);
