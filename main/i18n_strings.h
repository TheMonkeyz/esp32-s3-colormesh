// Display texts: one line per text, one column per language (forge_i18n.h: English, French). Included several
// times with different X() definitions (app_text.h, app_text.c, tests/host/test_i18n.c): no include guard.
// French is Canadian French (Québec), standard written: no anglicisms, no slang.
// Every text needs every language: an empty "" shows English (tests/host/test_i18n.c checks), and printf
// conversions (%s, %d) must be the same, in the same order. Check fit with snapshots: French is longer and the
// round screen wraps it badly; give long lines explicit \n breaks.

// Start-up (main.c)
X(T_STARTING,       "Starting...",                 "Démarrage...")

// Pick screen (the pager's first page, left of main)
X(T_PICK_COLOUR,    "Pick your colour",            "Choisissez votre couleur")
// A tap on a swatch sets the colour, a hold (400 ms, finger still) the waves' colour B (again: back to automatic)
X(T_PICK_HINT,      "Tap: your colour\nHold: wave colour", "Touchez : votre couleur\nMaintenez : couleur des vagues")
X(T_SWIPE_BACK,     "Swipe left to go back",       "Glissez vers la gauche\npour revenir")

// Main screen (the version string already starts with "v")
X(T_HOLD_HINT,      "Touch the screen to send your colour", "Toucher l'écran pour envoyer votre couleur")
X(T_NEAR,           "%d near  ·  %s",              "%d à portée  ·  %s")
X(T_DEV,            "DEV",                         "DEV")
X(T_ADMIN,          "ADMIN",                       "ADMIN")      // this board signs updates (secure updates)

// System page
X(T_SYSTEM,         "System",                      "Système")
X(T_SYS_VERSION,    "Version %s",                  "Version %s")
X(T_SYS_BT,         "Bluetooth %s",                "Bluetooth %s")
X(T_SYS_NAME,       "Name: %s",                    "Nom : %s")
X(T_SYS_NO_NAME,    "(none)",                      "(aucun)")
X(T_SYS_DEV,        "Dev mode: %s",                "Mode développeur : %s")
X(T_ON,             "on",                          "activé")
X(T_OFF,            "off",                         "désactivé")
X(T_SYS_MEMORY,     "Memory %u KB · PSRAM %u KB",  "Mémoire %u Ko · PSRAM %u Ko")
X(T_SYS_UPTIME,     "Running for %s",              "En marche depuis %s")
X(T_UP_MIN,         "%d min",                      "%d min")
X(T_UP_H,           "%d h %02d",                   "%d h %02d")
X(T_UP_D,           "%d d %d h",                   "%d j %d h")
X(T_SYS_PEERS,      "Boards near: %d",             "Appareils à portée : %d")
X(T_SYS_NO_PEERS,   "No board heard yet",          "Aucun appareil entendu")
X(T_PEER_HOLD,      "sending",                     "envoie")
X(T_PEER_IDLE,      "idle",                        "au repos")
