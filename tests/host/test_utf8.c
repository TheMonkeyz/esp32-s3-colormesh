// utf8.h: names and alert texts cut without splitting a character
#include <string.h>
#include "check.h"
#include "utf8.h"

int main(void)
{
    char b[8];
    utf8_copy(b, "Montréal", sizeof(b));                      // "Montr" + 'é' (2 bytes) would end at byte 7
    CHECK(!strcmp(b, "Montré"), "%s", b);
    utf8_copy(b, "ᐃᓄᒃᑎᑐᑦ", sizeof(b));                         // 3-byte syllabics: 2 fit in 7 bytes
    CHECK(!strcmp(b, "ᐃᓄ"), "%s (%zu bytes)", b, strlen(b));
    utf8_copy(b, "short", sizeof(b));
    CHECK(!strcmp(b, "short"), "%s", b);
    char c[32] = "Québec ᐃᓄᒃ";                                 // 17 bytes
    utf8_cut(c, 10);                                           // "Québec " is 8 bytes, then a 3-byte char
    CHECK(!strcmp(c, "Québec "), "[%s]", c);
    char d[4] = "abc";
    utf8_cut(d, 4);
    CHECK(!strcmp(d, "abc"), "fits: %s", d);
    return check_done("utf8");
}
