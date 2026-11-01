#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Text from outside (a network name, a place, anything a phone typed) may hold characters the display's fonts can't
// draw: emoji showed as boxes ("tofu"). TinyTTF can't tell (it always returns a glyph: the box), so each font's own
// character map is read once (the TTF's cmap, format 4: the Basic Multilingual Plane; emoji above it are never there).
// Several fonts: an app that chains fallback fonts (weather_amoled: syllabics behind Montserrat) adds each one; a
// character is kept if any of them draws it. No ESP-IDF or LVGL calls: tests/host checks it with real TTF files.
// (Twin file: weather_amoled main/textfit.h.)

#define TEXTFIT_MAX_FONTS 4

void textfit_init(const uint8_t *ttf, size_t len);   // forget all fonts, then add this one
bool textfit_add(const uint8_t *ttf, size_t len);    // one more (a fallback font); false if its cmap can't be read
bool textfit_has(uint32_t cp);                       // one of the fonts draws this character (no font read: true)
// in -> out without the characters the fonts lack (emoji, their joiners and variation selectors), spaces left
// doubled by a removal collapsed, trimmed; nothing left at all (a name all in emoji): "…". Returns out's length.
// Without a font: a plain copy.
size_t textfit(const char *in, char *out, size_t n);
