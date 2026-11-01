// Local time as the display has it: the POSIX TZ main.c sets (the starter: CONFIG_APP_TZ, "EST5EDT,M3.2.0,M11.1.0").
// Emscripten's localtime_r ignores TZ and uses the browser's time zone, so a visitor in Paris saw Paris times (and
// rtc_quebec asked for tomorrow's buses after 18:00 Québec time). The app's files are built with
// -Dlocaltime_r=emu_localtime_r (emu.mk). Handles "STDoff[DST[off][,Mm.w.d[/h],Mm.w.d[/h]]]" (no rule: no DST); anything else falls back to the
// browser's zone.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

typedef struct { int m, w, d, secs; } rule_t;   // month 1-12, week 1-5 (5 = last), weekday 0 = Sunday, local time

static const char *name(const char *s)          // "EST" or "<-03>"
{
    if (*s == '<') { const char *e = strchr(s, '>'); return e ? e + 1 : NULL; }
    const char *b = s;
    while (isalpha((unsigned char)*s)) s++;
    return s - b >= 3 ? s : NULL;
}

static const char *offset(const char *s, long *out)   // "5", "-9", "3:30": hours west of UTC -> seconds
{
    int sign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*s)) return NULL;
    long h = strtol(s, (char **)&s, 10), m = 0, sec = 0;
    if (*s == ':') { m = strtol(s + 1, (char **)&s, 10); if (*s == ':') sec = strtol(s + 1, (char **)&s, 10); }
    *out = sign * (h * 3600 + m * 60 + sec);
    return s;
}

static const char *rule(const char *s, rule_t *r)
{
    if (*s != 'M') return NULL;
    r->m = (int)strtol(s + 1, (char **)&s, 10);
    if (*s != '.') return NULL;
    r->w = (int)strtol(s + 1, (char **)&s, 10);
    if (*s != '.') return NULL;
    r->d = (int)strtol(s + 1, (char **)&s, 10);
    r->secs = 2 * 3600;
    if (*s == '/') { long t; if (!(s = offset(s + 1, &t))) return NULL; r->secs = (int)t; }
    return r->m >= 1 && r->m <= 12 && r->w >= 1 && r->w <= 5 && r->d >= 0 && r->d <= 6 ? s : NULL;
}

static long days_from_civil(int y, int m, int d)      // days since 1970-01-01 (Howard Hinnant)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// The UTC second the rule's moment falls on in year y, `off` being the offset in force just before it (seconds west)
static long long when(int y, const rule_t *r, long off)
{
    long first = days_from_civil(y, r->m, 1);
    int wd = (int)((first + 4) % 7 + 7) % 7;            // 1970-01-01 was a Thursday
    long day = first + (r->d - wd + 7) % 7 + (r->w - 1) * 7;
    int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int len = mdays[r->m - 1] + (r->m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
    while (day >= first + len) day -= 7;                // week 5 = the last one
    return (long long)day * 86400 + r->secs + off;
}

struct tm *emu_localtime_r(const time_t *t, struct tm *out)
{
    const char *tz = getenv("TZ"), *s;
    long std, dst;
    rule_t on, off;
    if (!tz || !(s = name(tz)) || !(s = offset(s, &std))) return localtime_r(t, out);
    long use = std;
    bool is_dst = false;
    const char *d = *s ? name(s) : NULL;
    if (d) {
        dst = std - 3600;
        if (*d && *d != ',' && !(d = offset(d, &dst))) return localtime_r(t, out);
        if (*d == ',' && (d = rule(d + 1, &on)) && *d == ',' && rule(d + 1, &off)) {
            struct tm u;
            gmtime_r(t, &u);
            int y = u.tm_year + 1900;
            long long a = when(y, &on, std), b = when(y, &off, dst);
            is_dst = a < b ? (*t >= a && *t < b) : (*t >= a || *t < b);   // (southern hemisphere: the year wraps)
            if (is_dst) use = dst;
        }
    }
    time_t local = *t - use;
    gmtime_r(&local, out);
    out->tm_isdst = is_dst;
    return out;
}
