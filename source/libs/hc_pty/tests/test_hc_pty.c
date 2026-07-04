/* Tests for hc_pty. The load-bearing test is the ENV SCRUB: a secret set in the parent's environment
 * (standing in for OPENROUTER_API_KEY) must NOT be readable from the spawned shell. Also: basic output
 * capture and input write. Offline. Exit non-zero on any failure. */

#define _DEFAULT_SOURCE 1

#include "hc_pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            fprintf(stderr, "FAIL: %s\n", (msg));                                                   \
            g_fails++;                                                                              \
        }                                                                                           \
    } while (0)

/* Poll-read up to `ms` collecting output into out[cap] (NUL-terminated). Stops early when the pty ends. */
static size_t drain(hc_pty *p, char *out, size_t cap, int ms)
{
    size_t total = 0;
    for (int i = 0; i < ms / 10 && total + 1 < cap; i++) {
        long n = hc_pty_read(p, out + total, cap - 1 - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0) {
            break; /* the child closed the pty */
        } else {
            struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
            nanosleep(&ts, NULL);
        }
    }
    out[total] = '\0';
    return total;
}

int main(void)
{
    char out[8192];

    /* --- basic output capture --- */
    {
        const char *argv[] = {"/bin/sh", "-c", "echo hello-pty", NULL};
        hc_pty     *p = hc_pty_spawn(argv, NULL);
        CHECK(p != NULL, "spawn echo");
        if (p) {
            drain(p, out, sizeof out, 3000);
            CHECK(strstr(out, "hello-pty") != NULL, "captured the command's output");
            hc_pty_close(p);
        }
    }

    /* --- THE ENV SCRUB: a parent secret must not reach the shell --- */
    {
        setenv("OPENROUTER_API_KEY", "SECRET-LEAK-CANARY", 1);
        const char *argv[] = {"/bin/sh", "-c", "echo \"[$OPENROUTER_API_KEY]\"", NULL};
        hc_pty     *p = hc_pty_spawn(argv, NULL);
        CHECK(p != NULL, "spawn env-probe");
        if (p) {
            drain(p, out, sizeof out, 3000);
            CHECK(strstr(out, "SECRET-LEAK-CANARY") == NULL,
                  "the API key is SCRUBBED — not readable from the shell");
            CHECK(strstr(out, "[]") != NULL, "the env var is empty in the shell (proves the scrub ran)");
            hc_pty_close(p);
        }
        unsetenv("OPENROUTER_API_KEY");
    }

    /* --- input write (cat echoes what we send) --- */
    {
        const char *argv[] = {"/bin/cat", NULL};
        hc_pty     *p = hc_pty_spawn(argv, NULL);
        CHECK(p != NULL, "spawn cat");
        if (p) {
            const char *line = "ping-through-pty\n";
            CHECK(hc_pty_write(p, line, strlen(line)) > 0, "write input to the pty");
            drain(p, out, sizeof out, 2000);
            CHECK(strstr(out, "ping-through-pty") != NULL, "input round-trips through the child");
            CHECK(hc_pty_alive(p), "cat is still alive (reads stdin)");
            hc_pty_close(p); /* terminates + reaps + frees p (no use-after-close) */
        }
    }

    if (g_fails == 0) printf("test_hc_pty: OK\n");
    return g_fails ? 1 : 0;
}
