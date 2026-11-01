// Display texts: one line per text, one column per language (forge_i18n.h: English, French). Included several
// times with different X() definitions (app_text.h, app_text.c, tests/host/test_i18n.c): no include guard.
// French is Canadian French (Québec), standard written: "appuyez longuement", "balayez le code QR", "1er".
// Every text needs every language: an empty "" shows English (tests/host/test_i18n.c checks), and printf
// conversions (%s, %d) must be the same, in the same order. Check fit with snapshots: French is longer and the
// round screen wraps it badly; give long lines explicit \n breaks.

// Screens
X(T_HELLO,          "Hello",                       "Bonjour")
X(T_HELLO_SUB,      "espforge starter",            "Application de départ espforge")
X(T_SWIPE_HINT,     "Swipe left: system\nLong-press: settings",
                    "Glissez à gauche : système\nAppuyez longuement : réglages")
X(T_SYSTEM,         "System",                      "Système")
X(T_SYS_VERSION,    "Version %s",                  "Version %s")
X(T_SYS_WIFI,       "Wi-Fi %s  ·  %d dBm",         "Wi-Fi %s  ·  %d dBm")
X(T_SYS_OFFLINE,    "Wi-Fi offline",               "Wi-Fi hors ligne")
X(T_SYS_IP,         "Address %s",                  "Adresse %s")
X(T_SYS_MEMORY,     "Memory %u KB  ·  PSRAM %u KB","Mémoire %u Ko  ·  PSRAM %u Ko")
X(T_SYS_UPTIME,     "Up %s",                       "En marche depuis %s")
X(T_SYS_SCAN,       "Scan for the settings page",  "Balayez pour les réglages")

// Updates (forge_ota states and reasons)
X(T_UPD_IDLE,       "Updates: not checked yet",    "Mises à jour : pas encore vérifiées")
X(T_UPD_CHECKING,   "Checking for updates...",     "Vérification des mises à jour...")
X(T_UPD_UP_TO_DATE, "Up to date",                  "À jour")
X(T_UPD_AVAILABLE,  "Update %s available",         "Mise à jour %s offerte")
X(T_UPD_DOWNLOADING,"Updating... %d %%",           "Mise à jour... %d %%")
X(T_UPD_DONE,       "Updated: restarting",         "Mise à jour faite : redémarrage")
X(T_UPD_FAILED,     "Update failed: %s",           "Échec de la mise à jour : %s")
X(T_OTA_NO_SITE,    "Can't reach the update site", "Site des mises à jour injoignable")
X(T_OTA_BAD_SITE,   "Unexpected reply from the update site", "Réponse inattendue du site des mises à jour")
X(T_OTA_NO_IMAGE,   "No app image in the manifest","Aucune image dans le manifeste")
X(T_OTA_NO_START,   "Download failed to start",    "Le téléchargement n'a pas démarré")
X(T_OTA_WRONG,      "Wrong firmware image",        "Mauvaise image de micrologiciel")
X(T_OTA_INTERRUPTED,"Download interrupted",        "Téléchargement interrompu")
X(T_OTA_INVALID,    "Downloaded image is invalid", "Image téléchargée invalide")
X(T_OTA_ROLLED_BACK,"%s was undone: the device restarted before it was confirmed",
                    "%s annulée : l'appareil a redémarré avant de la confirmer")

// Wi-Fi setup screen
X(T_WIFI_SETUP,     "Wi-Fi setup",                 "Configuration Wi-Fi")
X(T_WIFI_JOIN,      "Scan to join %s\n(password %s).\nAndroid? Swipe left to skip\nthe password",
                    "Balayez pour joindre %s\n(mot de passe %s).\nAndroid? Glissez à gauche\npour éviter le mot de passe")
X(T_WIFI_DPP_TITLE, "Easy Connect",                "Easy Connect")      // short: the circle is ~260 px wide up there
X(T_WIFI_DPP_HOW,   "Android phone on your Wi-Fi?\nScan this (camera or any QR app):\nit sends its network.\nSwipe right for other phones",
                    "Téléphone Android sur votre Wi-Fi?\nBalayez ce code (caméra, appli QR) :\nil envoie son réseau.\nGlissez à droite : autres téléphones")
X(T_WIFI_DPP_NONE,  "Easy Connect isn't available.\nSwipe right for other phones.",
                    "Easy Connect n'est pas offert.\nGlissez à droite : autres téléphones.")
X(T_WIFI_RECEIVED,  "Wi-Fi received",              "Wi-Fi reçu")
X(T_WIFI_GOT,       "Got \"%s\" from your phone.\nRestarting...", "« %s » reçu du téléphone.\nRedémarrage...")
X(T_WIFI_DPP_FAIL,  "That didn't work. Swipe right\nand join the setup network.",
                    "Échec. Glissez à droite et\njoignez le réseau de configuration.")
X(T_TAP_RETRY,      "Tap to try again",            "Touchez pour réessayer")
X(T_TAP_CANCEL,     "Tap to cancel",               "Touchez pour annuler")

// Settings screen (forge_settings: the app's word for each settings_text_t code, ui.c set_texts; weather_amoled's
// French). Sections in capitals; the values right of a row's name are short (a 300 px row holds both).
X(T_SET_DONE,       "Done",                        "OK")
X(T_SET_SEC_SCREEN, "SCREEN",                      "ÉCRAN")
X(T_SET_DIM_QUIET,  "Dim when quiet",              "Tamiser si calme")
X(T_SET_WAKE_PICKUP,"Wake on pick-up",             "Réveil si soulevé")
X(T_SET_TIMING,     "Timing",                      "Délais")
X(T_SET_SHORT,      "Short",                       "Courts")     // agrees with "Délais" (plural)
X(T_SET_NORMAL,     "Normal",                      "Normaux")
X(T_SET_LONG,       "Long",                        "Longs")
X(T_SET_CUSTOM,     "Custom",                      "Autres")
X(T_SET_BRIGHTNESS, "Brightness %d%%",             "Luminosité %d %%")
X(T_SET_LANGUAGE,   "Language",                    "Langue")
X(T_SET_SEC_MORE,   "MORE",                        "PLUS")
X(T_SET_PHONE,      "Settings on phone",           "Réglages (téléphone)")
X(T_SET_PHONE_SCAN, "Scan with your phone's\ncamera to open\nthe settings page",
                    "Balayez avec l'appareil\nphoto du téléphone pour\nouvrir la page des réglages")
X(T_SET_PHONE_NONE, "Connect to Wi-Fi first",      "Connectez d'abord le Wi-Fi")
X(T_SET_WIFI,       "Wi-Fi network",               "Réseau Wi-Fi")
X(T_SET_UPDATES,    "Updates",                     "Mises à jour")
X(T_SET_CHECK_NOW,  "Check now",                   "Vérifier")
X(T_SET_CHECKING,   "Checking...",                 "Vérification...")
X(T_SET_UP_TO_DATE, "Up to date",                  "À jour")
X(T_SET_FAILED,     "Failed",                      "Échec")
X(T_SET_INSTALL,    "Install %s",                  "Installer %s")
X(T_SET_TAP_AGAIN,  "Tap again",                   "Touchez encore")
X(T_SET_RESTART,    "Restart",                     "Redémarrer")
X(T_SET_RESTARTING, "Restarting...",               "Redémarrage...")
X(T_SET_SEC_ABOUT,  "ABOUT",                       "À PROPOS")
X(T_SET_VERSION,    "Version",                     "Version")
X(T_SET_NETWORK,    "Network",                     "Réseau")
X(T_SET_OFFLINE,    "Offline",                     "Hors ligne")
X(T_SET_IP,         "IP address",                  "Adresse IP")
X(T_SET_MEMORY,     "Memory",                      "Mémoire")
X(T_SET_MEMORY_KB,  "%u / %u KB",                  "%u / %u Ko")
X(T_SET_UPTIME,     "Running for",                 "En marche depuis")
X(T_SET_UPTIME_MIN, "%d min",                      "%d min")
X(T_SET_UPTIME_H,   "%d h %02d",                   "%d h %02d")
X(T_SET_UPTIME_D,   "%d d %d h",                   "%d j %d h")

// Start-up messages (main.c)
X(T_STARTING,       "Starting...",                 "Démarrage...")
X(T_CONNECTING,     "Connecting to\n%s\n\nLong-press for Wi-Fi setup", "Connexion à\n%s\n\nAppuyez longuement pour le Wi-Fi")
X(T_CONNECTED,      "Connected",                   "Connecté")
X(T_CANT_REACH,     "Can't reach %s\nTap to try again", "%s injoignable\nTouchez pour réessayer")
X(T_STILL_TRYING,   "Can't reach %s\nStill trying\n\nLong-press for Wi-Fi setup",
                    "%s injoignable\nNouvel essai en cours\n\nAppuyez longuement pour le Wi-Fi")
X(T_FIRST_SETUP,    "First-time setup",            "Première configuration")
