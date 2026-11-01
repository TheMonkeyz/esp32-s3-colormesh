#pragma once
#include "forge_i18n.h"

// The app's languages, in the order of the columns of i18n_strings.h and of app_text.c's list
enum { LANG_EN, LANG_FR, LANG_COUNT };

// The app's texts (i18n_strings.h) as an enum, and tr(): the text in the display language. Never put a bare string
// literal on screen: add a line to i18n_strings.h with every language.
typedef enum {
#define X(id, ...) id,
#include "i18n_strings.h"
#undef X
    T_COUNT
} tid_t;

#define tr(id) i18n_text(id)
void app_text_init(void);    // hands the table to forge_i18n and loads the saved language
