#pragma once
#include <stdbool.h>

// Firmware versions as the updater compares them: vMAJOR.MINOR.PATCH[-rc.N | -N-gHASH (git describe) | -anything].
// Same X.Y.Z: a pre-release < the release < a dev build after it (git describe); among pre-releases -rc.N by N, and
// any other suffix (a test build, "-fix.3") counts below every rc. tests/host/test_version.c pins the rules.
typedef struct { int v[3]; int kind; int n; } ver_t;   // kind: 0 pre-release, 1 release, 2 dev build after it

bool parse_ver(const char *s, ver_t *o);              // false if s isn't vX.Y.Z[...]
int cmp_ver(const ver_t *a, const ver_t *b);          // <0, 0, >0
