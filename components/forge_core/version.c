// Version comparison for updates (see version.h)
#include "version.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool parse_ver(const char *s, ver_t *o)
{
    memset(o, 0, sizeof(*o));
    if (*s == 'v' || *s == 'V') s++;
    char *end;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        o->v[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) { if (*s != '.') return false; s++; }
    }
    if (!*s) { o->kind = 1; return true; }
    if (*s != '-') return false;
    s++;
    if (!strncmp(s, "rc.", 3)) { o->kind = 0; o->n = atoi(s + 3); return true; }
    if (isdigit((unsigned char)*s) && strstr(s, "-g")) { o->kind = 2; o->n = atoi(s); return true; }
    o->kind = 0;                                   // other suffixes (test builds) count as pre-releases
    o->n = -1;
    return true;
}

int cmp_ver(const ver_t *a, const ver_t *b)
{
    for (int i = 0; i < 3; i++) if (a->v[i] != b->v[i]) return a->v[i] - b->v[i];
    if (a->kind != b->kind) return a->kind - b->kind;
    return a->n - b->n;
}
