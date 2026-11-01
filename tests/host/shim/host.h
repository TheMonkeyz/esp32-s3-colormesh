#pragma once
// Host shim, included before every file (-include): what newlib has and glibc doesn't
#include <stddef.h>
size_t strlcpy(char *dst, const char *src, size_t size);
