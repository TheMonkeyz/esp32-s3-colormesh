// testcon_registry.c: the test console's command table (docs/PROTOCOL.md §2): registration, splitting a line into
// arguments, unknown and empty lines, replacing a command, the "help" list.
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "testcon.h"

static int calls, last_argc;
static char last[10][32];

static void record(int argc, char **argv)
{
    calls++;
    last_argc = argc;
    for (int i = 0; i < argc && i < 10; i++) snprintf(last[i], sizeof(last[i]), "%s", argv[i]);
}
static int other_calls;
static void other(int argc, char **argv) { other_calls++; }

static int run(const char *s)
{
    char line[128];
    snprintf(line, sizeof(line), "%s", s);          // dispatch splits the line in place
    return testcon_dispatch(line);
}

int main(void)
{
    testcon_register("ping", NULL, record);
    testcon_register("tap", "tap X Y", record);

    CHECK(run("ping") == 0 && calls == 1 && last_argc == 1 && !strcmp(last[0], "ping"), "ping: argc %d", last_argc);
    CHECK(run("tap 233 120") == 0 && last_argc == 3 && !strcmp(last[1], "233") && !strcmp(last[2], "120"),
          "tap: argc %d [%s] [%s]", last_argc, last[1], last[2]);
    CHECK(run("  tap\t10   20  ") == 0 && last_argc == 3 && !strcmp(last[0], "tap") && !strcmp(last[2], "20"),
          "extra spaces and tabs: argc %d", last_argc);
    int before = calls;
    CHECK(run("nope 1 2") == -1, "unknown command");
    CHECK(run("") == -2, "empty line");
    CHECK(run("   ") == -2, "blank line");
    CHECK(run("pin") == -1 && run("pingx") == -1, "names match whole");
    CHECK(calls == before, "nothing ran for unknown or empty lines");

    testcon_register("ping", "ping", other);        // registering again replaces it
    CHECK(run("ping") == 0 && other_calls == 1 && calls == before, "replaced: %d %d", other_calls, calls);

    char help[256];
    testcon_help(help, sizeof(help));
    CHECK(strstr(help, "ping") && strstr(help, "tap X Y"), "help: %s", help);
    char tiny[8];
    testcon_help(tiny, sizeof(tiny));               // a short buffer is cut, never overrun (ASan checks)
    CHECK(strlen(tiny) < sizeof(tiny), "tiny help: %s", tiny);
    return check_done("testcon");
}
