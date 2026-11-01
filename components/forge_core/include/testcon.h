#pragma once
#include <stddef.h>

// Test console on the USB serial port (USB Serial/JTAG): lets tools/harness drive the board without a person.
// One command per line; every answer is a log line with the tag "test": "test: ok ...", "test: <name> k=v ..." or
// "test: error ..." (docs/PROTOCOL.md §2). USB only, never on the network: whoever can type here can already
// reflash the board, so it is in every build and the harness tests exactly what ships.
//
// Components and the app add their own commands with testcon_register(), usually from their init function.
// A command runs in the console task (4 KB stack, internal RAM): heavy work goes to its own task, and a command
// that needs a lock must give up after ~2 s ("error ...: busy, send 'where'") so the console stays free for "where".

typedef void (*testcon_fn_t)(int argc, char **argv);   // argv[0] is the command name
// usage: shown by "help", e.g. "tap X Y". Up to TESTCON_MAX commands; registering a name again replaces it.
void testcon_register(const char *name, const char *usage, testcon_fn_t fn);
// Run one line: 0 ran, -1 unknown command (an error line is logged), -2 empty. The line is split in place.
int testcon_dispatch(char *line);
void testcon_help(char *out, size_t n);                // "ping, heap, tap X Y, ..."

// "where" answers without taking any lock: each provider appends " k=v" breadcrumbs (what a task is doing now), so
// a hang can be located from the log alone. Providers must only read volatile variables.
typedef void (*testcon_where_fn_t)(char *out, size_t n);
void testcon_add_where(testcon_where_fn_t fn);

void testcon_start(void);   // the reader task and the built-in commands (ping, help, heap, where, memspeed, reboot)

#define TESTCON_MAX 48
