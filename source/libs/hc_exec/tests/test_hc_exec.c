/* test_hc_exec — the W4.1 confinement gate. Spawns real children under the Landlock+seccomp jail and asserts:
 * a normal command runs + captures; a non-zero exit is reported; a sleeper is SIGKILLed at the timeout; output
 * is truncated at the cap; the child env carries NO OPENROUTER_API_KEY (the scrub); a write OUTSIDE the
 * workspace is REFUSED by Landlock while a write INSIDE succeeds; an operator read root is readable but
 * never writable or executable, a root reaching /, /proc, /dev or /sys is refused, and a root that no longer
 * resolves to itself (a symlink on its path) is refused rather than followed; bad args are rejected. Skips
 * gracefully if the kernel lacks Landlock (the fail-closed UNSUPPORTED path).
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

    /* Operator read roots: the run jail's read access outside the workspace. An outside dir holds a file and
     * an executable copy of /bin/true. Without a grant it is unreadable; granted, it is readable but still
     * neither writable nor executable, and /proc stays denied. A root that is the whole filesystem or reaches
     * /proc, /dev or /sys, or that resolves anywhere but to itself, refuses the run before anything is spawned;
     * a single file can be granted, a FIFO cannot, and a root inside the workspace is never followed. */
    {
        char outside[64], note[128], tool[128], planted[128], link[128], absent[96], cmd[512];
        char secret[64], secret_file[128], alias[128], other[128], fifo[128], ws_link[128];
        snprintf(outside, sizeof outside, "/tmp/hc_exec_rr_%d", (int)getpid());
        snprintf(note, sizeof note, "%s/note.txt", outside);
        snprintf(tool, sizeof tool, "%s/true", outside);
        snprintf(planted, sizeof planted, "%s/planted.txt", outside);
        snprintf(link, sizeof link, "%s/proclink", outside);
        snprintf(absent, sizeof absent, "/tmp/hc_exec_rr_absent_%d", (int)getpid());
        snprintf(secret, sizeof secret, "/tmp/hc_exec_rr_secret_%d", (int)getpid());
        snprintf(secret_file, sizeof secret_file, "%s/secret.txt", secret);
        snprintf(alias, sizeof alias, "%s/alias", outside);
        snprintf(other, sizeof other, "%s/other.txt", outside);
        snprintf(fifo, sizeof fifo, "%s/fifo", outside);
        snprintf(ws_link, sizeof ws_link, "%s/deps", ws);
        mkdir(outside, 0700);
        mkdir(secret, 0700);
        FILE *sf = fopen(secret_file, "w");
        if (sf) {
            fputs("SECRET_CONTENT\n", sf);
            fclose(sf);
        }
        FILE *of = fopen(other, "w");
        if (of) {
            fputs("OTHER_CONTENT\n", of);
            fclose(of);
        }
        FILE *f = fopen(note, "w");
        if (f) {
            fputs("READ_ROOT_CONTENT\n", f);
            fclose(f);
        }
        FILE *src = fopen("/bin/true", "rb"), *dst = fopen(tool, "wb");
        if (src && dst) {
            char   b[4096];
            size_t k;
            while ((k = fread(b, 1, sizeof b, src)) > 0) fwrite(b, 1, k, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
        chmod(tool, 0755);
        CHECK(symlink("/proc", link) == 0, "test setup: a symlink to /proc");
        CHECK(symlink(secret, alias) == 0, "test setup: a symlink to another folder");
        CHECK(symlink(secret, ws_link) == 0, "test setup: a workspace symlink to another folder");
        CHECK(mkfifo(fifo, 0600) == 0, "test setup: a FIFO");

        const char *roots[] = {outside, NULL};
        hc_exec_result r;

        snprintf(cmd, sizeof cmd, "cat %s", note);
        const char  *rd[] = {"/bin/sh", "-c", cmd, NULL};
        hc_exec_spec s0 = {.argv = rd, .cwd = ws, .timeout_ms = 5000};
        CHECK(hc_exec_run(&s0, &r) == HC_EXEC_OK && !has(&r, "READ_ROOT_CONTENT"),
              "(a) an outside file is unreadable without a read root");
        hc_exec_result_free(&r);
        hc_exec_spec s1 = {.argv = rd, .cwd = ws, .timeout_ms = 5000, .read_roots = roots};
        CHECK(hc_exec_run(&s1, &r) == HC_EXEC_OK && r.exit_code == 0 && has(&r, "READ_ROOT_CONTENT"),
              "(b) a granted read root is readable");
        hc_exec_result_free(&r);

        char wcmd[256];
        snprintf(wcmd, sizeof wcmd, "echo x > %s 2>/dev/null; echo done", planted);
        const char  *wr[] = {"/bin/sh", "-c", wcmd, NULL};
        hc_exec_spec s2 = {.argv = wr, .cwd = ws, .timeout_ms = 5000, .read_roots = roots};
        CHECK(hc_exec_run(&s2, &r) == HC_EXEC_OK, "(c) the write attempt runs");
        hc_exec_result_free(&r);
        struct stat st;
        CHECK(stat(planted, &st) != 0, "(c) a read root is NOT writable (no file landed)");

        char xcmd[256];
        snprintf(xcmd, sizeof xcmd, "%s 2>/dev/null && echo RAN || echo NOEXEC", tool);
        const char  *ex[] = {"/bin/sh", "-c", xcmd, NULL};
        hc_exec_spec s3 = {.argv = ex, .cwd = ws, .timeout_ms = 5000, .read_roots = roots};
        CHECK(hc_exec_run(&s3, &r) == HC_EXEC_OK && has(&r, "NOEXEC") && !has(&r, "RAN"),
              "(d) a binary under a read root is NOT executable");
        hc_exec_result_free(&r);

        const char  *pe[] = {"/bin/sh", "-c",
                             "cat /proc/self/environ >/dev/null 2>&1 && echo PROC_LEAK || echo PROC_DENIED", NULL};
        hc_exec_spec s4 = {.argv = pe, .cwd = ws, .timeout_ms = 5000, .read_roots = roots};
        CHECK(hc_exec_run(&s4, &r) == HC_EXEC_OK && has(&r, "PROC_DENIED") && !has(&r, "PROC_LEAK"),
              "(e) /proc stays denied with a read root granted");
        hc_exec_result_free(&r);

        const char *echo_argv[] = {"/bin/echo", "ok", NULL};
        const char *bad[][2] = {{"/", NULL},     {"/proc", NULL},        {"/dev", NULL},  {"/sys/kernel", NULL},
                                {"/sys", NULL},  {"relative/dir", NULL}, {"/tmp/../proc", NULL}, {"/tmp/./x", NULL}};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            hc_exec_spec sb = {.argv = echo_argv, .cwd = ws, .timeout_ms = 5000, .read_roots = bad[i]};
            CHECK(hc_exec_run(&sb, &r) == HC_EXEC_ERR_READ_ROOT, "(f) a forbidden read root refuses the run");
            CHECK(!hc_exec_read_root_valid(bad[i][0]) && !hc_exec_read_root_canonical(bad[i][0]) &&
                      hc_exec_read_root_problem(bad[i][0], ws),
                  "(f) the read-root checks refuse the same root");
        }
        CHECK(hc_exec_read_root_valid("/tmp/a..b/c.d"), "(f) a `..` INSIDE a name is not a `..` segment");

        const char  *via_link[] = {link, NULL};
        hc_exec_spec sl = {.argv = echo_argv, .cwd = ws, .timeout_ms = 5000, .read_roots = via_link};
        CHECK(hc_exec_run(&sl, &r) == HC_EXEC_ERR_READ_ROOT, "(g) a symlink resolving into /proc refuses the run");
        CHECK(!hc_exec_read_root_canonical(link), "(g) a symlink into /proc cannot be added either");

        const char  *gone[] = {absent, NULL};
        hc_exec_spec sa = {.argv = echo_argv, .cwd = ws, .timeout_ms = 5000, .read_roots = gone};
        CHECK(hc_exec_run(&sa, &r) == HC_EXEC_OK && has(&r, "ok"), "(h) an absent read root is skipped, not fatal");
        hc_exec_result_free(&r);
        CHECK(hc_exec_read_root_valid(absent) && !hc_exec_read_root_problem(absent, ws),
              "(h) an absent root stays valid (settings keep it) and is no problem (runs skip it)");
        CHECK(!hc_exec_read_root_canonical(absent), "(h) an absent root cannot be ADDED");

        char *canon = hc_exec_read_root_canonical(outside);
        CHECK(canon && strcmp(canon, outside) == 0, "(i) a real outside dir canonicalises to itself");
        free(canon);

        /* (j) the swap: a root that resolves elsewhere is refused, never followed. `alias` stands for a stored
         * root that has since become a symlink to a folder nobody granted. Adding it stores the target. */
        char scmd[256];
        snprintf(scmd, sizeof scmd, "cat %s/secret.txt", alias);
        const char  *sw[] = {"/bin/sh", "-c", scmd, NULL};
        const char  *via_alias[] = {alias, NULL};
        hc_exec_spec sj = {.argv = sw, .cwd = ws, .timeout_ms = 5000, .read_roots = via_alias};
        CHECK(hc_exec_run(&sj, &r) == HC_EXEC_ERR_READ_ROOT, "(j) a root that resolves elsewhere refuses the run");
        const char *why = hc_exec_read_root_problem(alias, ws);
        CHECK(why && strstr(why, "symlink"), "(j) and the problem names the symlink");
        canon = hc_exec_read_root_canonical(alias);
        CHECK(canon && strcmp(canon, secret) == 0, "(j) adding a symlink stores the folder it names");
        free(canon);

        /* (k) one regular file can be a root: it is readable, its siblings are not */
        char kcmd[512];
        snprintf(kcmd, sizeof kcmd, "cat %s; cat %s 2>/dev/null || echo SIBLING_DENIED", note, other);
        const char  *kr[] = {"/bin/sh", "-c", kcmd, NULL};
        const char  *file_root[] = {note, NULL};
        hc_exec_spec sk = {.argv = kr, .cwd = ws, .timeout_ms = 5000, .read_roots = file_root};
        CHECK(hc_exec_run(&sk, &r) == HC_EXEC_OK && has(&r, "READ_ROOT_CONTENT") && has(&r, "SIBLING_DENIED") &&
                  !has(&r, "OTHER_CONTENT"),
              "(k) a file root grants that file alone");
        hc_exec_result_free(&r);

        /* (l) a FIFO is neither addable nor grantable (its reader would take another process's stream) */
        const char  *fifo_root[] = {fifo, NULL};
        hc_exec_spec sf2 = {.argv = echo_argv, .cwd = ws, .timeout_ms = 5000, .read_roots = fifo_root};
        CHECK(hc_exec_run(&sf2, &r) == HC_EXEC_ERR_READ_ROOT, "(l) a FIFO root refuses the run");
        CHECK(!hc_exec_read_root_canonical(fifo), "(l) a FIFO cannot be added");

        /* (m) a root inside the workspace is skipped unresolved: the model can re-point anything there, so a
         * symlink it plants (ws/deps -> the secret folder) must grant nothing */
        char mcmd[256];
        snprintf(mcmd, sizeof mcmd, "cat %s/secret.txt 2>/dev/null || echo WS_LINK_DENIED", ws_link);
        const char  *mr[] = {"/bin/sh", "-c", mcmd, NULL};
        const char  *ws_root[] = {ws_link, NULL};
        hc_exec_spec sm = {.argv = mr, .cwd = ws, .timeout_ms = 5000, .read_roots = ws_root};
        CHECK(hc_exec_run(&sm, &r) == HC_EXEC_OK && has(&r, "WS_LINK_DENIED") && !has(&r, "SECRET_CONTENT"),
              "(m) a root inside the workspace is skipped, never followed");
        hc_exec_result_free(&r);

        /* (n) more roots than HC_EXEC_MAX_READ_ROOTS refuse the run */
        const char *many[HC_EXEC_MAX_READ_ROOTS + 2];
        for (int i = 0; i < HC_EXEC_MAX_READ_ROOTS + 1; i++) many[i] = outside;
        many[HC_EXEC_MAX_READ_ROOTS + 1] = NULL;
        hc_exec_spec sn = {.argv = echo_argv, .cwd = ws, .timeout_ms = 5000, .read_roots = many};
        CHECK(hc_exec_run(&sn, &r) == HC_EXEC_ERR_READ_ROOT, "(n) too many read roots refuse the run");

        unlink(ws_link);
        unlink(alias);
        unlink(fifo);
        unlink(other);
        unlink(secret_file);
        rmdir(secret);
        unlink(link);
        unlink(tool);
        unlink(note);
        unlink(planted);
        rmdir(outside);
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
