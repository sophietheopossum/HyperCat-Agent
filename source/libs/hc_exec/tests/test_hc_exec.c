/* test_hc_exec — the W4.1 confinement gate. Spawns real children under the Landlock+seccomp jail and asserts:
 * a normal command runs + captures; a non-zero exit is reported; a sleeper is SIGKILLed at the timeout; output
 * is truncated at the cap; the child env carries NO OPENROUTER_API_KEY (the scrub); a write OUTSIDE the
 * workspace is REFUSED by Landlock while a write INSIDE succeeds; bad args are rejected. Skips gracefully if
 * the kernel lacks Landlock (the fail-closed UNSUPPORTED path).
 * Owns: a mkdtemp workspace it removes at the end. Threading: single-threaded. Lifetime: frees every result
 * via hc_exec_result_free. */

#include "hc_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

static int g_fail = 0;
#define CHECK(c, msg)                                                                                          \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            fprintf(stderr, "FAIL: %s\n", msg);                                                                \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

static int has(const hc_exec_result *r, const char *needle)
{
    return r->output && strstr(r->output, needle) != NULL;
}

int main(void)
{
    /* a private workspace dir = the only writable subtree */
    char ws[] = "/tmp/hc_exec_ws_XXXXXX";
    if (!mkdtemp(ws)) {
        perror("mkdtemp");
        return 2;
    }

    /* probe: if Landlock is unavailable, hc_exec refuses (fail-closed). Skip the jail asserts but verify the
     * refusal is clean, so the suite passes on an old kernel rather than red. */
    {
        const char    *argv[] = {"/bin/echo", "probe", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 5000};
        hc_exec_result r;
        hc_exec_status st = hc_exec_run(&spec, &r);
        if (st == HC_EXEC_ERR_UNSUPPORTED) {
            fprintf(stderr, "test_hc_exec: Landlock unavailable — exec refused (fail-closed); skipping jail asserts\n");
            rmdir(ws);
            return 0;
        }
        CHECK(st == HC_EXEC_OK && r.exit_code == 0 && has(&r, "probe"), "echo runs, exit 0, output captured");
        hc_exec_result_free(&r);
    }

    /* a non-zero exit is reported */
    {
        const char    *argv[] = {"/bin/false", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 5000};
        hc_exec_result r;
        CHECK(hc_exec_run(&spec, &r) == HC_EXEC_OK && r.exit_code == 1 && r.term_signal == 0,
              "/bin/false exits 1");
        hc_exec_result_free(&r);
    }

    /* a sleeper is killed at the timeout (not allowed to run for its full duration) */
    {
        const char    *argv[] = {"/bin/sleep", "30", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 400};
        hc_exec_result r;
        hc_exec_status st = hc_exec_run(&spec, &r);
        CHECK(st == HC_EXEC_OK && r.timed_out && r.term_signal != 0, "a 30s sleeper is SIGKILLed at the 400ms timeout");
        hc_exec_result_free(&r);
    }

    /* output is truncated at the cap (the child cannot flood host memory) */
    {
        /* pure shell builtins (no external command) so the flood is deterministic regardless of /usr/bin */
        const char    *argv[] = {"/bin/sh", "-c",
                                 "i=0; while [ $i -lt 5000 ]; do echo aaaaaaaaaaaaaaaa; i=$((i+1)); done", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 8000, .max_output = 4096};
        hc_exec_result r;
        CHECK(hc_exec_run(&spec, &r) == HC_EXEC_OK && r.truncated && r.output_len <= 4096,
              "a flood of output is truncated at max_output");
        hc_exec_result_free(&r);
    }

    /* the scrub: the child env carries NO OPENROUTER_API_KEY even though the parent set it */
    {
        setenv("OPENROUTER_API_KEY", "sk-SECRET-must-not-leak-into-child", 1);
        const char    *argv[] = {"/bin/sh", "-c", "echo \"key=[$OPENROUTER_API_KEY]\"", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 5000};
        hc_exec_result r;
        CHECK(hc_exec_run(&spec, &r) == HC_EXEC_OK && has(&r, "key=[]") && !has(&r, "SECRET"),
              "the API key is NOT in the scrubbed child env");
        hc_exec_result_free(&r);
        unsetenv("OPENROUTER_API_KEY");
    }

    /* Landlock: a write INSIDE the workspace succeeds; a write OUTSIDE (e.g. /tmp) is refused (no file lands) */
    {
        char escape[64];
        snprintf(escape, sizeof escape, "/tmp/hc_exec_escape_%d", (int)getpid());
        unlink(escape);
        char cmd[256];
        snprintf(cmd, sizeof cmd,
                 "echo inside > inside.txt; echo escaped > %s 2>/dev/null; echo done", escape);
        const char    *argv[] = {"/bin/sh", "-c", cmd, NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 5000};
        hc_exec_result r;
        CHECK(hc_exec_run(&spec, &r) == HC_EXEC_OK, "the landlock write test runs");
        hc_exec_result_free(&r);

        char inside[300];
        snprintf(inside, sizeof inside, "%s/inside.txt", ws);
        struct stat st;
        CHECK(stat(inside, &st) == 0 && st.st_size > 0, "a write INSIDE the workspace succeeded");
        CHECK(stat(escape, &st) != 0, "a write OUTSIDE the workspace was REFUSED by Landlock (no file)");
        unlink(inside);
        unlink(escape); /* belt + suspenders if the jail somehow let it through */
    }

    /* Landlock READ confinement: /proc is NOT granted, so the child cannot read /proc/self/environ — the path
     * by which a host env (the API key) could otherwise leak. Locks in that a sysdir grant never widens to /proc. */
    {
        const char    *argv[] = {"/bin/sh", "-c",
                              "cat /proc/self/environ >/dev/null 2>&1 && echo PROC_LEAK || echo PROC_DENIED", NULL};
        hc_exec_spec   spec = {.argv = argv, .cwd = ws, .timeout_ms = 5000};
        hc_exec_result r;
        CHECK(hc_exec_run(&spec, &r) == HC_EXEC_OK && has(&r, "PROC_DENIED") && !has(&r, "PROC_LEAK"),
              "Landlock denies reading /proc (the host env stays unreachable)");
        hc_exec_result_free(&r);
    }

    /* bad args are rejected without spawning */
    {
        hc_exec_result    r;
        const char       *rel[] = {"echo", "x", NULL}; /* argv[0] not absolute */
        hc_exec_spec      s1 = {.argv = rel, .cwd = ws, .timeout_ms = 1000};
        CHECK(hc_exec_run(&s1, &r) == HC_EXEC_ERR_ARGS, "a non-absolute argv[0] is rejected");
        hc_exec_spec      s2 = {.argv = NULL, .cwd = ws, .timeout_ms = 1000};
        CHECK(hc_exec_run(&s2, &r) == HC_EXEC_ERR_ARGS, "a null argv is rejected");
    }

    rmdir(ws);
    if (g_fail) {
        fprintf(stderr, "test_hc_exec: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_exec: all checks passed\n");
    return 0;
}
