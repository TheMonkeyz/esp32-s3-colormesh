// Display texts: one line per text, one column per language (forge_i18n.h: English, French). Included several
// times with different X() definitions (app_text.h, app_text.c, tests/host/test_i18n.c): no include guard.
// French is Canadian French (Québec), standard written: no anglicisms, no slang.
// Every text needs every language: an empty "" shows English (tests/host/test_i18n.c checks), and printf
// conversions (%s, %d) must be the same, in the same order. Check fit with snapshots: French is longer and the
// round screen wraps it badly; give long lines explicit \n breaks.

// Start-up (main.c)
X(T_STARTING,       "Starting...",                 "Démarrage...")

// Pick screen
X(T_PICK_COLOUR,    "Pick your colour",            "Choisissez votre couleur")
X(T_TAP_ONE,        "Tap one",                     "Touchez-en une")

// Main screen
X(T_HOLD_HINT,      "Hold to send\nyour colour",   "Gardez le doigt\npour envoyer votre couleur")
X(T_NEAR,           "%d near  ·  v%s",             "%d à proximité  ·  v%s")
X(T_DEV,            "DEV",                         "DEV")
