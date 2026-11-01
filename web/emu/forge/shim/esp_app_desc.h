#pragma once
// Browser emulator: the firmware's version (/api/info, the system page) is the emulator's build (EMU_VERSION, git
// describe) and its project name forge.json's app (EMU_APP): web/emu/forge/emu_web.c
typedef struct { char version[32]; char project_name[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
