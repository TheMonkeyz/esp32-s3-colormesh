# The starter app in the browser (README.md): what this app builds; the rules are the framework's (forge/emu.mk).
#   make -j8        # in WSL, after `source ~/emsdk/emsdk_env.sh`: build/, served over HTTP
# A new app changes these lists, emu_main.c and index.html (docs/NEW-PROJECT.md).

# main/'s sources, built unchanged (main/CMakeLists.txt SRCS)
APP_SRC := main.c ui.c app_text.c
# This app's own emulator files: main() and the browser's demo data (and stand-ins for hardware of its own)
APP_EMU := emu_main.c
# What main/CMakeLists.txt embeds that the app reads (EMBED_FILES): the font, as ttf_start / ttf_end (ui.c)
EMBED := ttf=../../main/montserrat.ttf
# The Settings screen a long press opens (forge_settings)
USE_SETTINGS := 1

include forge/emu.mk
