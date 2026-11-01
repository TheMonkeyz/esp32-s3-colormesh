// The flash cache's I/O table over the "mapcache" partition (esp_partition): firmware only. map.c declares a weak
// fmap_cache_flash_io() returning NULL, so a build without this file (the host tests, the emulator) has no cache.
#include "map_priv.h"
#include "esp_partition.h"
#include "esp_log.h"

static const esp_partition_t *part;
static bool p_read(void *ctx, size_t off, void *dst, size_t n) { return esp_partition_read(ctx, off, dst, n) == ESP_OK; }
static bool p_write(void *ctx, size_t off, const void *src, size_t n) { return esp_partition_write(ctx, off, src, n) == ESP_OK; }
static bool p_erase(void *ctx, size_t off, size_t n) { return esp_partition_erase_range(ctx, off, n) == ESP_OK; }
static fmap_cache_io_t io;

const fmap_cache_io_t *fmap_cache_flash_io(void (*pause)(void *ctx))
{
    if (!part) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "mapcache");
        if (!part) { ESP_LOGI("fmap", "no mapcache partition: pictures are not kept in flash"); return NULL; }
        io = (fmap_cache_io_t){ (void *)part, part->size, p_read, p_write, p_erase, pause };
    }
    return &io;
}
