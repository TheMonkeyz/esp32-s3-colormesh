#pragma once
#include <stdbool.h>

// Web Mercator ("slippy map") maths: OpenStreetMap's tiles are 256 px squares, 2^z of them across the world at zoom z,
// tile (0, 0) at the north-west corner (85.0511 N, 180 W). A place is a point in "world pixels" at a zoom: x to the
// east, 0..256 x 2^z; y to the south, same range. A picture of the map is a window of world pixels, given by its
// top-left corner (ox, oy). Pure C, no ESP-IDF: tests/host/test_map.c (esp32-s3-rtcquebec's geo.c, through v0.3.1,
// plus the origin, the view and the tile cover that its map.c and weather_amoled's radar.c each computed themselves).
//
// The world's edges, decided once for every user of these functions:
//   - x wraps: the world repeats to the east and the west, so a window that crosses the 180th meridian shows the
//     tiles of the other side (tile x is taken mod 2^z, geo_wrap_x). A window wider than the world at zoom 0..1 shows
//     the same tiles more than once, as a map application would.
//   - y doesn't: north of 85.05 N and south of 85.05 S there is no map. The tile cover leaves those rows out (not
//     requested, not counted), and the picture keeps its empty colour there.

#define GEO_TILE 256
#define GEO_ZOOM_MAX 22                // 2^22 tiles across: 1L << z and 256 x 2^z stay exact in a double

// World pixel coordinates of a place at zoom z
void geo_world_px(double lat, double lon, int z, double *x, double *y);

// The top-left corner of a w x h window centred on a place at zoom z, in whole world pixels (floor(x) - w / 2):
// the key a kept picture is found by, so the same place at the same zoom always gives the same window
void geo_origin(double lat, double lon, int z, int w, int h, double *ox, double *oy);

// A place's position in a window whose top-left corner is (ox, oy), in the window's own pixels (it may lie outside:
// negative, or past the window's size). x is taken from the copy of the world nearest to the window's left edge
// (within half a world), so a place just east of the 180th meridian lands in a window that crosses it from the west.
// That choice is unambiguous for windows up to half the world wide (466 px from zoom 2 on); y never wraps.
void geo_to_view(double lat, double lon, int z, double ox, double oy, double *sx, double *sy);

// The tiles touching a w x h window at (ox, oy), as they are: tx0..tx1, ty0..ty1, may be negative or past 2^z - 1
void geo_tiles(double ox, double oy, int w, int h, int *tx0, int *ty0, int *tx1, int *ty1);

// The tiles to fetch for a w x h window at zoom z: x as geo_tiles (unwrapped: the tile lands at tx * 256 - ox in the
// window, its URL takes geo_wrap_x(tx, z)), y limited to the world's rows 0..2^z - 1. count = tiles to fetch
// ((tx1 - tx0 + 1) x (ty1 - ty0 + 1)), 0 when the window lies wholly north or south of the world (ty1 < ty0 then).
typedef struct { int tx0, tx1, ty0, ty1, count; } geo_cover_t;
void geo_cover(double ox, double oy, int w, int h, int z, geo_cover_t *c);

// Tile column tx at zoom z within the world, 0..2^z - 1 (mod 2^z, also for negative tx)
int geo_wrap_x(int tx, int z);

// Distance in metres between two places (haversine)
double geo_distance_m(double lat1, double lon1, double lat2, double lon2);

// A point (px, py) relative to a circle's centre, pulled in to radius r when it lies outside: true if it was outside
// (a marker kept on a round screen's edge)
bool geo_clamp_circle(double *px, double *py, double r);
