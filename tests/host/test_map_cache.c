// map_cache.c: the flash cache over a RAM-backed I/O table: the slot layout, a save then a load round trip, the header
// written last (a save that stops early leaves no valid slot), a mismatching header refused, writes only onto erased
// bytes, a pause between sectors, no room past the partition.
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "forge_map.h"

enum { W = 64, H = 48 };
static uint8_t flash[3 * 4096 * 2];                      // room for two 8 KB slots... (W x H x 2 = 6144 + 16 -> 8192)
static int reads, writes, erases, pauses, fail_write_at, hdr_write_order;
static bool wrote_non_erased;

static bool f_read(void *c, size_t off, void *dst, size_t n) { (void)c; reads++; if (off + n > sizeof(flash)) return false; memcpy(dst, flash + off, n); return true; }
static bool f_write(void *c, size_t off, const void *src, size_t n)
{
    (void)c;
    if (fail_write_at && ++writes == fail_write_at) return false;
    if (!fail_write_at) writes++;
    if (off + n > sizeof(flash)) return false;
    for (size_t i = 0; i < n; i++) if (flash[off + i] != 0xFF) wrote_non_erased = true;
    if (n == 16 && hdr_write_order == 0) hdr_write_order = writes;    // the header: a 16-byte write
    memcpy(flash + off, src, n);
    return true;
}
static bool f_erase(void *c, size_t off, size_t n) { (void)c; erases++; if (off + n > sizeof(flash)) return false; memset(flash + off, 0xFF, n); return true; }
static void f_pause(void *c) { (void)c; pauses++; }

int main(void)
{
    fmap_cache_io_t io = { NULL, sizeof(flash), f_read, f_write, f_erase, f_pause };
    static uint16_t pic[W * H], back[W * H];
    for (int i = 0; i < W * H; i++) pic[i] = (uint16_t)(i * 7);
    size_t slot = fmap_cache_slot_bytes(W, H), off = 0;
    CHECK(slot == 8192, "a %dx%d picture + header rounds up to 8192, not %zu", W, H, slot);
    CHECK(fmap_cache_slot(&io, W, H, 5, 4, &off) && off == 8192, "zoom 5 with zoom_min 4: the second slot (%zu)", off);
    CHECK(fmap_cache_slot(&io, W, H, 6, 4, &off) && off == 16384, "zoom 6: the third slot, the last that fits");
    CHECK(!fmap_cache_slot(&io, W, H, 7, 4, &off), "zoom 7: no room in a 24 KB partition");
    CHECK(!fmap_cache_slot(&io, W, H, 3, 4, &off), "below zoom_min: none");

    memset(flash, 0, sizeof(flash));                          // a partition full of old data
    fmap_cache_slot(&io, W, H, 5, 4, &off);
    CHECK(!fmap_cache_hit(&io, off, 5, 100, 200), "no hit on old data");
    CHECK(fmap_cache_save(&io, off, 5, 100, 200, pic, W, H), "saved");
    CHECK(erases == 2 && !wrote_non_erased, "every sector erased first (%d erases), writes only onto erased bytes", erases);
    CHECK(pauses == erases + (W * H * 2 + 4095) / 4096, "a pause after each sector erased or written (%d)", pauses);
    CHECK(hdr_write_order == writes, "the header was the last write (%d of %d)", hdr_write_order, writes);
    CHECK(fmap_cache_hit(&io, off, 5, 100, 200), "a hit on the saved view");
    CHECK(!fmap_cache_hit(&io, off, 5, 100, 201) && !fmap_cache_hit(&io, off, 6, 100, 200), "another view or zoom: no hit");
    CHECK(fmap_cache_load(&io, off, 5, 100, 200, back, W, H) && !memcmp(pic, back, sizeof(pic)), "loaded, the same pixels");
    CHECK(!fmap_cache_load(&io, off, 5, 101, 200, back, W, H), "a load of another view is refused");

    // A save that stops before the header (power lost, a write error) leaves no valid slot: the old one is gone too
    fail_write_at = 2; writes = 0;
    CHECK(!fmap_cache_save(&io, off, 5, 100, 200, pic, W, H), "the interrupted save reports failure");
    CHECK(!fmap_cache_hit(&io, off, 5, 100, 200), "no valid slot after an interrupted save");
    fail_write_at = 0;
    CHECK(!fmap_cache_save(&io, 16384 + 8192, 5, 1, 1, pic, W, H), "a slot past the partition is refused");
    return check_done("map_cache");
}
