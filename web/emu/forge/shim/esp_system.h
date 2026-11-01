#pragma once
#include <stdlib.h>
#include <emscripten.h>
static inline void esp_restart(void) { EM_ASM({ location.reload(); }); }
static inline void esp_system_abort(const char *why) { emscripten_log(EM_LOG_ERROR, "abort: %s", why); abort(); }
