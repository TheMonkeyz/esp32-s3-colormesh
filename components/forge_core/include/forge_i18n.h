#pragma once
#include <stdbool.h>
#include <time.h>

// Display language core. The texts are the app's: main/i18n_strings.h has one line per text, one column per language
// (X(T_ID, "English", "Français")), and main/app_text.h turns it into an enum and tr(). The languages are the app's
// too: it hands over its list (i18n_en, i18n_fr below, or its own i18n_lang_t: weather_amoled adds Inuktitut) and
// defines its LANG_ enum in that order. Framework components never show text themselves: they return codes the app
// translates. tests/host/test_i18n.c checks every text in every language.

typedef int lang_t;                       // an index into the app's language list; 0 is the fallback (English)

typedef enum {
    I18N_DATE_EN,                         // "Wednesday, September 30" / "September 30, 2026"
    I18N_DATE_FR,                         // "Mercredi 1er octobre" / "1er octobre 2026" (Québec: "1er" for the 1st)
} i18n_date_t;

typedef struct {
    const char *code, *name;              // "fr", "Français" (its name in its own language)
    const char *wday[7], *wday_short[7];  // 0 = Sunday
    const char *month[12];                // as used inside a date (French: lowercase)
    i18n_date_t date;
} i18n_lang_t;

extern const i18n_lang_t i18n_en, i18n_fr;

// texts[id * nlangs + lang], in the order of langs; until this is called: English only, no texts
void i18n_init(const char *const *texts, int count, const i18n_lang_t *const *langs, int nlangs);
int i18n_count(void);                     // languages
const char *i18n_text(int id);            // in the current language; the first language's when missing *or empty*
lang_t i18n_lang(void);
void i18n_set(lang_t l);                  // out of range: 0
const char *i18n_code(lang_t l);          // "en", "fr"
const char *i18n_name(lang_t l);          // "English", "Français"
lang_t i18n_from_code(const char *code);  // unknown -> 0

// Saved language (NVS "i18n"/"lang", a code): i18n_load() at start-up after NVS is up, i18n_save() after i18n_set().
// Optional: an app that keeps the language with its own settings doesn't call them.
void i18n_load(void);
bool i18n_save(void);

const char *tr_weekday(int wday, bool full);   // 0 = Sunday; "Wednesday" / "Wed", "Mercredi" / "Mer."
void tr_date_long(const struct tm *tm, char *out, int n);   // see i18n_date_t
void tr_date_ymd(int y, int m, int d, char *out, int n);    // m 1..12
