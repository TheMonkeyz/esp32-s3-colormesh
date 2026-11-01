// textfit.c: characters the display's font (main/montserrat.ttf) can't draw are left out (emoji showed as boxes)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "textfit.h"

static char out[64];
static const char *fit(const char *s) { textfit(s, out, sizeof(out)); return out; }

int main(void)
{
    FILE *f = fopen("../../main/montserrat.ttf", "rb");
    CHECK(f != NULL, "main/montserrat.ttf not found");
    if (!f) return check_done("textfit");
    static unsigned char ttf[400 * 1024];
    size_t len = fread(ttf, 1, sizeof(ttf), f);
    fclose(f);
    CHECK(!strcmp(fit("Maison \xF0\x9F\x8F\xA0 net"), "Maison \xF0\x9F\x8F\xA0 net"), "before init: a copy (%s)", out);
    textfit_init(ttf, len);
    CHECK(textfit_has('A') && textfit_has(0xE9) && textfit_has(0xC7), "Latin-1 letters");   // é, Ç
    CHECK(!textfit_has(0x1F3E0) && !textfit_has(0x2764), "emoji are not in Montserrat");
    CHECK(!strcmp(fit("Montréal Québec"), "Montréal Québec"), "French kept: %s", out);
    CHECK(!strcmp(fit("Maison \xF0\x9F\x8F\xA0 net"), "Maison net"), "emoji in the middle: [%s]", out);
    CHECK(!strcmp(fit("\xF0\x9F\x8F\xA0 Home"), "Home"), "emoji first: [%s]", out);
    CHECK(!strcmp(fit("Cafe \xE2\x98\x95"), "Cafe"), "emoji last: [%s]", out);
    // a ZWJ family emoji (man, ZWJ, woman) and a heart with its variation selector: nothing left of them
    CHECK(!strcmp(fit("A\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9" "B \xE2\x9D\xA4\xEF\xB8\x8F C"), "AB C"), "[%s]", out);
    CHECK(!strcmp(fit("\xF0\x9F\x92\xA9\xF0\x9F\x92\xA9"), "\xE2\x80\xA6"), "all emoji: an ellipsis, not nothing: [%s]", out);
    CHECK(!strcmp(fit("two  spaces"), "two  spaces"), "spaces kept when nothing was removed: [%s]", out);
    CHECK(!strcmp(fit("bad \xC3"), "bad"), "a cut UTF-8 sequence dropped: [%s]", out);
    // Several fonts (a fallback chain): a character counts if any has it; garbage isn't a font; the table has a limit
    static const unsigned char junk[64] = { 0 };
    CHECK(!textfit_add(junk, sizeof(junk)), "a file with no cmap is not added");
    CHECK(!textfit_has(0x1400 + 0x90), "Montserrat has no syllabics (U+1490)");
    CHECK(textfit_add(ttf, len), "a second font is added");
    CHECK(textfit_has(0xE9) && !textfit_has(0x1F3E0), "two fonts: their union, still no emoji");
    for (int i = 0; i < TEXTFIT_MAX_FONTS; i++) textfit_add(ttf, len);
    CHECK(!textfit_add(ttf, len), "no more than TEXTFIT_MAX_FONTS");
    textfit_init(ttf, len);                                   // init forgets the others
    CHECK(textfit_add(ttf, len), "after init there is room again");
    char small[6];
    textfit("Québec", small, sizeof(small));                  // "Qu" + é (2) + "b" = 5 bytes: then 'e' doesn't fit
    CHECK(!strcmp(small, "Québ"), "whole characters only: [%s]", small);
    return check_done("textfit");
}
