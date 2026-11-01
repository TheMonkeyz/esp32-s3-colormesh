// The flash cache's layout and files over an I/O table (forge_map.h): pure C, host-tested with a RAM-backed table
// (tests/host/test_map_cache.c). weather_amoled's "MAP7" layout (radar.c): one slot per zoom, the header written last so
// a slot is valid only when complete, sectors erased and written one at a time with a pause between them, reads in
// 4 KB pieces. The binding to esp_partition is map_cache_flash.c (not built for the host or the emulator).
#include "map_priv.h"
#include <string.h>

#define SECT 4096u
typedef struct { uint32_t magic; int32_t zoom, ox, oy; } hdr_t;

size_t fmap_cache_slot_bytes(int w, int h)
{
    size_t bytes = sizeof(hdr_t) + (size_t)w * h * 2;
    return (bytes + SECT - 1) & ~(size_t)(SECT - 1);
}

bool fmap_cache_slot(const fmap_cache_io_t *io, int w, int h, int zoom, int zoom_min, size_t *off)
{
    if (!io || zoom < zoom_min) return false;
    size_t slot = fmap_cache_slot_bytes(w, h);
    *off = (size_t)(zoom - zoom_min) * slot;
    return *off + slot <= io->size;
}

bool fmap_cache_hit(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy)
{
    hdr_t hd;
    if (!io || !io->read(io->ctx, off, &hd, sizeof(hd))) return false;
    return hd.magic == FMAP_CACHE_MAGIC && hd.zoom == zoom && hd.ox == ox && hd.oy == oy;
}

bool fmap_cache_load(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy, uint16_t *px, int w, int h)
{
    if (!px || !fmap_cache_hit(io, off, zoom, ox, oy)) return false;
    size_t total = (size_t)w * h * 2;
    for (size_t o = 0; o < total; o += SECT)              // 4 KB pieces: a bigger read into PSRAM borrows internal RAM
        if (!io->read(io->ctx, off + sizeof(hdr_t) + o, (uint8_t *)px + o, total - o < SECT ? total - o : SECT)) return false;
    return true;
}

bool fmap_cache_save(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy, const uint16_t *px, int w, int h)
{
    if (!io || !px) return false;
    size_t total = (size_t)w * h * 2, len = fmap_cache_slot_bytes(w, h);
    if (off + len > io->size) return false;
    for (size_t o = 0; o < len; o += SECT) {              // erase one sector at a time: a whole slot blocks the core
        if (!io->erase(io->ctx, off + o, SECT)) return false;
        if (io->pause) io->pause(io->ctx);
    }
    for (size_t o = 0; o < total; o += SECT) {
        size_t n = total - o < SECT ? total - o : SECT;
        if (!io->write(io->ctx, off + sizeof(hdr_t) + o, (const uint8_t *)px + o, n)) return false;
        if (io->pause) io->pause(io->ctx);
    }
    hdr_t hd = { FMAP_CACHE_MAGIC, zoom, ox, oy };        // header last = valid only when complete
    return io->write(io->ctx, off, &hd, sizeof(hd));
}
