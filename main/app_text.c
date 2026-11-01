// The app's text table (see app_text.h)
#include "app_text.h"

static const char *const texts[T_COUNT][LANG_COUNT] = {
#define X(id, ...) [id] = { __VA_ARGS__ },
#include "i18n_strings.h"
#undef X
};

static const i18n_lang_t *const langs[LANG_COUNT] = { &i18n_en, &i18n_fr };

void app_text_init(void)
{
    i18n_init(&texts[0][0], T_COUNT, langs, LANG_COUNT);
    i18n_load();
}
