// LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC).
// With the default C library malloc, LVGL's many small allocations (objects, styles, label text,
// glyph cache) landed in internal RAM (anything under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL goes
// there first) and filled it up. Internal RAM is kept for Wi-Fi, DMA and task stacks.
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
// PSRAM full: small blocks may fall back to internal RAM (counted, logged once); a big one would take the RAM Wi-Fi
// and the task stacks need (internal RAM's low point is under 10 KB)
#define FALLBACK_MAX 4096

static const char *TAG = "lvgl_mem";
static volatile uint32_t fallbacks;

uint32_t lvgl_mem_fallbacks(void) { return fallbacks; }

// Nothing left: restart. LVGL's LV_ASSERT_MALLOC would otherwise halt its task in while(1) holding the display lock:
// a frozen screen until someone unplugs it (the owner chose a restart, October 2026).
static void *out_of_memory(size_t size)
{
    ESP_LOGE(TAG, "LVGL out of memory (%u bytes): restarting", (unsigned)size);
    esp_system_abort("LVGL out of memory");
    return NULL;
}

static void *fallback(void *p, size_t size, bool re)
{
    if (size > FALLBACK_MAX) return out_of_memory(size);
    void *q = re ? realloc(p, size) : malloc(size);
    if (!q) return out_of_memory(size);
    if (fallbacks++ == 0) ESP_LOGW(TAG, "PSRAM full: LVGL uses internal RAM (%u bytes); counted in 'heap'", (unsigned)size);
    return q;
}

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : fallback(NULL, size, false);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, LV_CAPS);
    return q || !new_size ? q : fallback(p, new_size, true);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, LV_CAPS);
    mon->total_size = i.total_free_bytes + i.total_allocated_bytes;
    mon->free_size = i.total_free_bytes;
    mon->free_biggest_size = i.largest_free_block;
    mon->used_cnt = i.allocated_blocks;
    mon->free_cnt = i.free_blocks;
    mon->used_pct = mon->total_size ? 100 - (uint8_t)(100ULL * i.total_free_bytes / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
