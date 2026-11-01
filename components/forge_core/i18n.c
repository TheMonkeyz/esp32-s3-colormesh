// Display language core (see forge_i18n.h). No ESP-IDF APIs: tests/host compiles it.
#include "forge_i18n.h"
#include <stdio.h>
#include <string.h>

const i18n_lang_t i18n_en = {
    "en", "English",
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
      "November", "December" },
    I18N_DATE_EN,
};

const i18n_lang_t i18n_fr = {
    "fr", "Français",
    { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" },
    { "Dim.", "Lun.", "Mar.", "Mer.", "Jeu.", "Ven.", "Sam." },
    { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre",
      "novembre", "décembre" },                      // lowercase: they're used inside a date
    I18N_DATE_FR,
};

static const i18n_lang_t *const english_only[] = { &i18n_en };
static const i18n_lang_t *const *langs = english_only;
static int nlangs = 1;
static const char *const *texts;
static int ntexts;
static volatile lang_t lang;

void i18n_init(const char *const *t, int count, const i18n_lang_t *const *l, int n)
{
    texts = t;
    ntexts = count;
    if (l && n > 0) { langs = l; nlangs = n; }
    if (lang >= nlangs) lang = 0;
}

static const i18n_lang_t *L(lang_t l) { return langs[l >= 0 && l < nlangs ? l : 0]; }

int i18n_count(void) { return nlangs; }
void i18n_set(lang_t l) { lang = l >= 0 && l < nlangs ? l : 0; }
lang_t i18n_lang(void) { return lang; }
const char *i18n_code(lang_t l) { return L(l)->code; }
const char *i18n_name(lang_t l) { return L(l)->name; }

lang_t i18n_from_code(const char *code)
{
    for (int i = 0; i < nlangs; i++) if (code && !strcmp(code, langs[i]->code)) return i;
    return 0;
}

const char *i18n_text(int id)
{
    if (!texts || id < 0 || id >= ntexts) return "";
    const char *s = texts[id * nlangs + lang];
    return s && s[0] ? s : texts[id * nlangs];      // an empty text showed as nothing (weather_amoled)
}

const char *tr_weekday(int wday, bool full)
{
    wday = ((wday % 7) + 7) % 7;
    return full ? L(lang)->wday[wday] : L(lang)->wday_short[wday];
}

void tr_date_long(const struct tm *tm, char *out, int n)
{
    const i18n_lang_t *l = L(lang);
    int m = ((tm->tm_mon % 12) + 12) % 12, d = tm->tm_mday, wd = ((tm->tm_wday % 7) + 7) % 7;
    if (l->date == I18N_DATE_FR) snprintf(out, n, "%s %d%s %s", l->wday[wd], d, d == 1 ? "er" : "", l->month[m]);
    else snprintf(out, n, "%s, %s %d", l->wday[wd], l->month[m], d);
}

void tr_date_ymd(int y, int m, int d, char *out, int n)
{
    const i18n_lang_t *l = L(lang);
    m = (m - 1) % 12;
    if (m < 0) m = 0;
    if (l->date == I18N_DATE_FR) snprintf(out, n, "%d%s %s %d", d, d == 1 ? "er" : "", l->month[m], y);
    else snprintf(out, n, "%s %d, %d", l->month[m], d, y);
}
