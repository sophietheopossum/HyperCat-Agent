/* Unit tests for hc_secrets — set/get/replace/delete, env load, and bounds. Offline. Exit
 * non-zero on any failure (CTest reads the exit code). */

#define _DEFAULT_SOURCE 1

#include "hc_secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

int main(void)
{
    hc_secrets *s = hc_secrets_open();
    CHECK(s != NULL, "open");
    if (!s) return 1;

    char out[128];

    CHECK(hc_secrets_set(s, "openrouter", "sk-abc123") == HC_SECRETS_OK, "set");
    CHECK(hc_secrets_has(s, "openrouter"), "has after set");
    CHECK(hc_secrets_get(s, "openrouter", out, sizeof out) == HC_SECRETS_OK
              && strcmp(out, "sk-abc123") == 0,
          "get round-trips");

    CHECK(hc_secrets_get(s, "missing", out, sizeof out) == HC_SECRETS_ERR_NOT_FOUND,
          "get absent -> NOT_FOUND");
    CHECK(!hc_secrets_has(s, "missing"), "has absent -> false");

    /* replace scrubs the old value and stores the new */
    CHECK(hc_secrets_set(s, "openrouter", "sk-newkey") == HC_SECRETS_OK, "replace set");
    CHECK(hc_secrets_get(s, "openrouter", out, sizeof out) == HC_SECRETS_OK
              && strcmp(out, "sk-newkey") == 0,
          "get after replace");

    /* too-small buffer */
    char tiny[4];
    CHECK(hc_secrets_get(s, "openrouter", tiny, sizeof tiny) == HC_SECRETS_ERR_TOO_LONG,
          "get into too-small buffer -> TOO_LONG");

    /* key too long */
    char longkey[80];
    memset(longkey, 'k', sizeof longkey - 1);
    longkey[sizeof longkey - 1] = '\0';
    CHECK(hc_secrets_set(s, longkey, "x") == HC_SECRETS_ERR_TOO_LONG, "over-long key rejected");

    /* delete */
    CHECK(hc_secrets_delete(s, "openrouter") == HC_SECRETS_OK, "delete");
    CHECK(hc_secrets_get(s, "openrouter", out, sizeof out) == HC_SECRETS_ERR_NOT_FOUND,
          "get after delete -> NOT_FOUND");
    CHECK(hc_secrets_delete(s, "openrouter") == HC_SECRETS_ERR_NOT_FOUND,
          "double delete -> NOT_FOUND");

    /* env load */
    setenv("HC_TEST_SECRET_VAR", "from-env", 1);
    CHECK(hc_secrets_load_env(s, "envkey", "HC_TEST_SECRET_VAR") == HC_SECRETS_OK, "load_env");
    CHECK(hc_secrets_get(s, "envkey", out, sizeof out) == HC_SECRETS_OK
              && strcmp(out, "from-env") == 0,
          "get env-loaded secret");
    CHECK(hc_secrets_load_env(s, "k2", "HC_TEST_SECRET_UNSET") == HC_SECRETS_ERR_NOT_FOUND,
          "load_env of unset var -> NOT_FOUND");

    /* the public scrub helper zeroes a buffer */
    {
        char buf[8];
        memset(buf, 'A', sizeof buf);
        hc_secrets_zero(buf, sizeof buf);
        int allzero = 1;
        for (size_t i = 0; i < sizeof buf; i++)
            if (buf[i] != 0) allzero = 0;
        CHECK(allzero, "hc_secrets_zero scrubs the buffer");
    }

    /* many entries to exercise growth, then close (scrubs all) */
    for (int i = 0; i < 20; i++) {
        char k[32];
        snprintf(k, sizeof k, "k%d", i);
        CHECK(hc_secrets_set(s, k, "value") == HC_SECRETS_OK, "bulk set");
    }
    hc_secrets_close(s);

    if (g_fails) {
        fprintf(stderr, "hc_secrets: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_secrets: all checks passed\n");
    return 0;
}
