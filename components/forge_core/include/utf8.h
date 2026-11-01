#pragma once
#include <string.h>

// Cut s so it fits `size` bytes with its NUL, never in the middle of a UTF-8 character (a byte-wise cut left half a
// syllabic or an "é" at the end of place names and alert texts: a missing glyph box on screen, bad JSON on the page).
static inline void utf8_cut(char *s, size_t size)
{
    if (!size) return;
    size_t n = strlen(s);
    if (n < size) return;
    n = size - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // s[n] continues a character: cut before it
    s[n] = 0;
}

// strlcpy, then the same rule: copy src into dst (size bytes) without splitting a character
static inline void utf8_copy(char *dst, const char *src, size_t size)
{
    if (!size) return;
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
