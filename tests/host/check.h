#pragma once
// Minimal checks for the host tests: CHECK(cond, fmt, ...) counts failures; main returns check_done().
#include <stdio.h>
static int check_fails, check_runs;
#define CHECK(cond, ...) do { check_runs++; if (!(cond)) { check_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static inline int check_done(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_runs, check_fails);
    return check_fails != 0;
}
