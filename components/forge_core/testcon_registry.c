// Command table of the test console (see testcon.h). No ESP-IDF APIs besides the log, so tests/host can test it.
#include "testcon.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "test";

typedef struct { const char *name, *usage; testcon_fn_t fn; } cmd_t;
// (EXT_RAM_BSS_ATTR: static buffers are internal RAM otherwise, the scarce kind; weather_amoled lost 2.2 KB of it
// when it took these components, October 5, LESSONS L185)
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR                                  // tests/host
#endif
EXT_RAM_BSS_ATTR static cmd_t cmds[TESTCON_MAX];
static int ncmds;

void testcon_register(const char *name, const char *usage, testcon_fn_t fn)
{
    for (int i = 0; i < ncmds; i++)
        if (!strcmp(cmds[i].name, name)) { cmds[i].usage = usage; cmds[i].fn = fn; return; }
    if (ncmds == TESTCON_MAX) { ESP_LOGE(TAG, "no room for command '%s' (TESTCON_MAX)", name); return; }
    cmds[ncmds++] = (cmd_t){ name, usage ? usage : name, fn };
}

int testcon_dispatch(char *line)
{
    char *argv[10];
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 10; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return -2;
    for (int i = 0; i < ncmds; i++)
        if (!strcmp(cmds[i].name, argv[0])) { cmds[i].fn(argc, argv); return 0; }
    ESP_LOGW(TAG, "error unknown command '%s' (help lists them)", argv[0]);
    return -1;
}

void testcon_help(char *out, size_t n)
{
    size_t len = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < ncmds && len < n; i++)
        len += snprintf(out + len, n - len, "%s%s", i ? ", " : "", cmds[i].usage);
}
