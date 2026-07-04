/* Tests for the OS keychain persistence verbs (hc_secrets_persist / _load_keychain / _forget_keychain).
 *
 * SELF-SKIPPING: a host with no reachable Secret Service (headless / CI / bare SSH), OR a non-keychain BUILD,
 * reports the backend unavailable -> we assert the graceful UNSUPPORTED fallback and PASS. So this one binary
 * validates BOTH the keychain build (UNSUPPORTED at runtime when no keyring) AND the in-memory stub build. On a
 * host WITH a reachable keyring it runs a full set -> persist -> reload-in-a-FRESH-store -> forget round-trip
 * under a dedicated HC_TEST_* name, so a developer's real OPENROUTER_API_KEY / keyring is never touched and no
 * residue is left. Offline; exits non-zero on any failure (CTest reads the exit code). */

#include "hc_secrets.h"

#include <stdio.h>
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
    if (!hc_secrets_keychain_available()) {
        /* No Secret Service reachable, OR a non-keychain build -> every persistence verb must degrade to
         * UNSUPPORTED, never crash, never block. This is the dominant CI / headless / container path. */
        hc_secrets *s = hc_secrets_open();
        CHECK(s != NULL, "open");
        if (s) {
            hc_secrets_set(s, "k", "v");
            CHECK(hc_secrets_persist(s, "k") == HC_SECRETS_ERR_UNSUPPORTED, "persist -> UNSUPPORTED");
            CHECK(hc_secrets_load_keychain(s, "k") == HC_SECRETS_ERR_UNSUPPORTED, "load_keychain -> UNSUPPORTED");
            CHECK(hc_secrets_forget_keychain(s, "k") == HC_SECRETS_ERR_UNSUPPORTED, "forget -> UNSUPPORTED");
            hc_secrets_close(s);
        }
        if (g_fails) return 1;
        printf("hc_secrets_keychain: skipped (no Secret Service) -- UNSUPPORTED fallback verified\n");
        return 0;
    }

    /* A reachable keyring: a full persistence round-trip under a dedicated test name (no residue afterward). */
    const char *K = "HC_TEST_KEYCHAIN_RT";
    const char *V = "sk-roundtrip-xyz-0123456789";
    char        out[128];

    hc_secrets *a = hc_secrets_open();
    CHECK(a != NULL, "open a");
    if (!a) return 1;
    CHECK(hc_secrets_set(a, K, V) == HC_SECRETS_OK, "set in memory");
    CHECK(hc_secrets_persist(a, K) == HC_SECRETS_OK, "persist to keychain");

    /* A FRESH store == "after a restart": the value must come back from the keychain. */
    hc_secrets *b = hc_secrets_open();
    CHECK(b != NULL, "open b");
    CHECK(hc_secrets_load_keychain(b, K) == HC_SECRETS_OK, "load_keychain into a fresh store");
    CHECK(hc_secrets_get(b, K, out, sizeof out) == HC_SECRETS_OK && strcmp(out, V) == 0,
          "loaded value round-trips");

    /* persist of a key NOT in memory -> NOT_FOUND (the in-memory precondition). */
    CHECK(hc_secrets_persist(b, "HC_TEST_NOT_IN_MEMORY") == HC_SECRETS_ERR_NOT_FOUND,
          "persist of absent-in-memory -> NOT_FOUND");

    /* forget removes it; a later load misses -> NOT_FOUND (no residue). */
    CHECK(hc_secrets_forget_keychain(b, K) == HC_SECRETS_OK, "forget from keychain");
    CHECK(hc_secrets_load_keychain(b, K) == HC_SECRETS_ERR_NOT_FOUND, "load after forget -> NOT_FOUND");
    CHECK(hc_secrets_forget_keychain(b, K) == HC_SECRETS_ERR_NOT_FOUND, "double forget -> NOT_FOUND");

    hc_secrets_zero(out, sizeof out);
    hc_secrets_close(a);
    hc_secrets_close(b);

    if (g_fails) {
        fprintf(stderr, "hc_secrets_keychain: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_secrets_keychain: round-trip passed\n");
    return 0;
}
