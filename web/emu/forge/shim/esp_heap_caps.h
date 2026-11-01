#pragma once
// Host shim: every capability is plain malloc
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_8BIT 8
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, s, caps) calloc(n, s)
#define heap_caps_realloc(p, n, caps) realloc(p, n)
#define heap_caps_get_free_size(caps) ((size_t)8 << 20)            // (browser: plenty, or slide.c keeps no pictures)
#define heap_caps_get_largest_free_block(caps) ((size_t)4 << 20)
