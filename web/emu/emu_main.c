// The starter app in the browser: main.c's app_main() runs unchanged as a task (forge/emu_loop.c). The "Wi-Fi" is up
// at once (forge/emu_stubs.c), so it goes from its start-up straight to the hello page, as a display that joined its
// network. Screen dimming (forge_presence) starts from app_main too, with main.c's own hooks: the board's microphones
// and motion sensor answer "not there" (forge/emu_stubs.c), the finger and the brightness are the emulator's. An app
// whose main() doesn't run main.c passes its own presence_hooks_t to presence_start() here (README.md, Notes).
// This file is the place for what only the browser needs: demo data a new visitor sees, a link's ?parameters
// (emu_param), stand-ins for the app's own hardware.
#include <ctype.h>
#include "nvs.h"
#include "emu.h"

// ?lang=fr in the page's address: the display starts in that language and keeps it, as if chosen on the settings
// page. Saved where forge_core's i18n_load() reads it (NVS "i18n"/"lang"), before app_main: an unknown code is English.
static void lang_from_address(void)
{
    char code[8];
    nvs_handle_t h;
    if (!emu_param("lang", code, sizeof(code)) || !isalpha((unsigned char)code[0])) return;
    if (nvs_open("i18n", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "lang", code);
    nvs_commit(h);
    nvs_close(h);
}

int main(void)
{
    lang_from_address();
    emu_start_app_main();
    emu_loop();
}
