/* test_hc_util — hc_getenv_int: unset/empty/non-numeric -> default, valid parse, negative, and saturation
 * (a huge value must NOT wrap to a negative like atoi would). */

#include "hc_util.h"

#include <stdio.h>
#include <stdlib.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            fprintf(stderr, "FAIL: %s\n", (m));                                                        \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

int main(void)
{
    unsetenv("HC_TEST_X");
    CHECK(hc_getenv_int("HC_TEST_X", 42) == 42, "unset -> default");
    CHECK(hc_getenv_int(NULL, 7) == 7, "null name -> default");

    setenv("HC_TEST_X", "100", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == 100, "valid number parses");
    setenv("HC_TEST_X", "-5", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == -5, "negative parses");

    setenv("HC_TEST_X", "", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == 42, "empty -> default");
    setenv("HC_TEST_X", "abc", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == 42, "non-numeric -> default");

    setenv("HC_TEST_X", "99999999999999", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == 2147483647, "huge -> saturates to INT_MAX (no atoi wrap)");
    setenv("HC_TEST_X", "-99999999999999", 1);
    CHECK(hc_getenv_int("HC_TEST_X", 42) == (-2147483647 - 1), "huge negative -> saturates to INT_MIN");

    unsetenv("HC_TEST_X");
    if (g_fails == 0) printf("test_hc_util: OK\n");
    return g_fails ? 1 : 0;
}
