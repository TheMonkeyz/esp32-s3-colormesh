#pragma once

// The app's screens: "pick" (a ring of colours at boot), "main" (own colour, the mix, the peers), "system"
// (version, address, memory, peers) and "message". Any task may call these: they take the display lock.
void ui_init(void);
void ui_show_pick(void);
void ui_show_main(void);
void ui_message(const char *title, const char *body);
void ui_texts_changed(void);                        // the language changed: every label again
