/* Minimal test helpers: each test binary returns non-zero if any CHECK fails. */
#ifndef MG_TEST_H
#define MG_TEST_H
#include <stdio.h>

static int t_failures;
static int t_checks;

#define CHECK(cond)                                                        \
    do {                                                                   \
        t_checks++;                                                        \
        if (!(cond)) {                                                     \
            t_failures++;                                                  \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define TEST_DONE(name)                                                    \
    (printf("%-14s %s (%d checks, %d failed)\n", name,                     \
            t_failures ? "FAIL" : "ok", t_checks, t_failures),             \
     t_failures ? 1 : 0)
#endif
