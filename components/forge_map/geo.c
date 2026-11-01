// Web Mercator maths (see forge_geo.h). Pure C: tests/host/test_map.c.
#include "forge_geo.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The world's width (and height) in pixels at zoom z
static double world(int z) { return GEO_TILE * (double)(1L << z); }

void geo_world_px(double lat, double lon, int z, double *x, double *y)
{
    double n = world(z), r = lat * M_PI / 180;
    *x = (lon + 180) / 360 * n;
    *y = (1 - log(tan(r) + 1 / cos(r)) / M_PI) / 2 * n;
}

void geo_origin(double lat, double lon, int z, int w, int h, double *ox, double *oy)
{
    double x, y;
    geo_world_px(lat, lon, z, &x, &y);
    // floor first, then whole pixels off: the same value whichever way an app computed it before (esp32-s3-rtcquebec
    // floor(x - 466 / 2), weather_amoled floor(x) - 466 / 2), so their kept and cached maps keep matching
    *ox = floor(x) - w / 2;
    *oy = floor(y) - h / 2;
}

void geo_to_view(double lat, double lon, int z, double ox, double oy, double *sx, double *sy)
{
    double x, y, n = world(z);
    geo_world_px(lat, lon, z, &x, &y);
    *sx = x - ox;
    // the copy of the world within half a world of the window's left edge: [-n/2, n/2)
    *sx -= n * floor((*sx + n / 2) / n);
    *sy = y - oy;
}

void geo_tiles(double ox, double oy, int w, int h, int *tx0, int *ty0, int *tx1, int *ty1)
{
    *tx0 = (int)floor(ox / GEO_TILE);
    *ty0 = (int)floor(oy / GEO_TILE);
    *tx1 = (int)floor((ox + w - 1) / GEO_TILE);
    *ty1 = (int)floor((oy + h - 1) / GEO_TILE);
}

void geo_cover(double ox, double oy, int w, int h, int z, geo_cover_t *c)
{
    geo_tiles(ox, oy, w, h, &c->tx0, &c->ty0, &c->tx1, &c->ty1);
    int last = (1 << z) - 1;
    if (c->ty0 < 0) c->ty0 = 0;                 // north of 85.05 N: no tiles there
    if (c->ty1 > last) c->ty1 = last;           // south of 85.05 S
    c->count = c->ty1 < c->ty0 || c->tx1 < c->tx0 ? 0 : (c->tx1 - c->tx0 + 1) * (c->ty1 - c->ty0 + 1);
}

int geo_wrap_x(int tx, int z)
{
    int n = 1 << z, m = tx % n;                 // C's % keeps the sign of tx: -1 % 16 = -1
    return m < 0 ? m + n : m;
}

double geo_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
    double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 6371000 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

bool geo_clamp_circle(double *px, double *py, double r)
{
    double d = sqrt(*px * *px + *py * *py);
    if (d <= r) return false;
    *px = *px * r / d;
    *py = *py * r / d;
    return true;
}
