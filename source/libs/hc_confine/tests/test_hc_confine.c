/* test_hc_confine — the under-confinement SMOKE (the P07 gate). In forked children, apply hc_confine, then
 * PROVE: (a) the worker's ops still work (write IN the workspace, read a system file, mmap/clock/file I/O),
 * (b) the forbidden is denied (a write OUTSIDE the workspace -> EACCES; /etc write -> denied), and (c) the
 * never-legitimate is KILLED (execve, ptrace -> SIGSYS), and (d) with the deny_network option, socket() is
 * KILLED too (the audio-helper network floor). Skipped with a clear message on a kernel that lacks
 * Landlock+seccomp (so CI on an old kernel is green, not red). This converts "I think the allow-set is
 * complete" into a tested property: a missing ALLOW entry makes child_works fail a needed op. */

#define _GNU_SOURCE

#include "hc_confine.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int g_fail = 0;
#define CHECK(c, m)                                                                                          \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "FAIL: %s\n", (m));                                                              \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

/* child exit codes (so the parent can pinpoint a failure stage); 0 = full pass. */
enum {
    OK = 0,
    E_APPLY = 10, /* hc_confine_apply did not return OK (no full confinement) -> the parent SKIPS */
    E_WS_OPEN = 11,
    E_WS_WRITE = 12,
    E_MMAP = 13,
    E_CLOCK = 14,
    E_GETDENTS = 15,
    E_OUTSIDE_ALLOWED = 16, /* a write OUTSIDE the workspace was NOT denied (a hole) */
    E_OUTSIDE_WRONGERR = 17,
    E_ETC_WRITE_ALLOWED = 18,  /* /etc write was NOT denied                          */
    E_SYSREAD_DENIED = 19,     /* a read of a granted system file was wrongly denied  */
    E_SOCKET = 20,             /* socket() was blocked by seccomp (a network hole)    */
    E_CONNECT = 21,            /* connect() was EPERM'd by seccomp (a network hole)   */
    E_RENAME = 22              /* rename() in the workspace failed (hc_store commit path) */
};

static hc_confine_status confine_worker(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) return HC_CONFINE_ERR_NOMEM;
    hc_confine_allow_dir(sp, ws, 1); /* workspace: read+write+create */
    hc_confine_allow_read(sp, "/usr");
    hc_confine_allow_read(sp, "/lib");
    hc_confine_allow_read(sp, "/lib64");
    hc_confine_allow_read(sp, "/etc");
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_WORKER_DEFAULT);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    return st;
}

/* (a)+(b): the worker's ops succeed; the forbidden is Landlock-denied. _exit(0) iff every assertion holds. */
static void child_works(const char *ws)
{
    if (confine_worker(ws) != HC_CONFINE_OK) _exit(E_APPLY);

    /* ALLOWED: create + write a file IN the workspace */
    char p[4096];
    snprintf(p, sizeof p, "%s/out.txt", ws);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) _exit(E_WS_OPEN);
    if (write(fd, "hi", 2) != 2) _exit(E_WS_WRITE);
    close(fd);

    /* ALLOWED: rename WITHIN the workspace (the hc_store atomic-write commit — glibc may emit the legacy
     * `rename` nr, which must be in the allow-set, and Landlock must permit a same-subtree rename) */
    char p2[4096];
    snprintf(p2, sizeof p2, "%s/out.renamed", ws);
    if (rename(p, p2) != 0) _exit(E_RENAME);
    unlink(p2);

    /* ALLOWED: mmap (allocator), clock_gettime, a directory read of the workspace (getdents64) */
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) _exit(E_MMAP);
    munmap(m, 4096);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) _exit(E_CLOCK);
    { /* read the workspace dir (getdents64) */
        int dfd = open(ws, O_RDONLY | O_DIRECTORY);
        if (dfd < 0) _exit(E_GETDENTS);
        char dbuf[1024];
        long n = syscall(SYS_getdents64, dfd, dbuf, sizeof dbuf);
        close(dfd);
        if (n < 0) _exit(E_GETDENTS);
    }

    /* ALLOWED: read a granted system file (/etc/hosts almost always exists). Absent => skip; DENIED => a hole. */
    int rfd = open("/etc/hosts", O_RDONLY);
    if (rfd < 0 && errno == EACCES) _exit(E_SYSREAD_DENIED);
    if (rfd >= 0) close(rfd);

    /* ALLOWED: the network path — socket() + a non-blocking connect() must be PERMITTED (an EPERM here means the
     * seccomp blocked them, the exact hole that would break the worker's LLM call). The connect itself may fail
     * with a network error (refused / in-progress); only an EPERM/EACCES indicates a seccomp/Landlock block. */
    int sk = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sk < 0) _exit(E_SOCKET);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(1);                  /* a port nothing listens on */
    sa.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1 */
    int cr = connect(sk, (struct sockaddr *)&sa, sizeof sa);
    if (cr < 0 && errno == EPERM) {
        close(sk);
        _exit(E_CONNECT);
    }
    close(sk);

    /* FORBIDDEN: create a file OUTSIDE the workspace (/tmp is not granted) -> Landlock EACCES */
    int bad = open("/tmp/hc_confine_should_not_exist", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (bad >= 0) {
        close(bad);
        unlink("/tmp/hc_confine_should_not_exist");
        _exit(E_OUTSIDE_ALLOWED);
    }
    if (errno != EACCES && errno != EPERM) _exit(E_OUTSIDE_WRONGERR);

    /* FORBIDDEN: open /etc (read-only granted) for WRITE -> denied */
    int bad2 = open("/etc/hostname", O_WRONLY);
    if (bad2 >= 0) {
        close(bad2);
        _exit(E_ETC_WRITE_ALLOWED);
    }

    _exit(OK);
}

/* (c): a KILL-set syscall terminates the process with SIGSYS. */
static void child_execve(const char *ws)
{
    if (confine_worker(ws) != HC_CONFINE_OK) _exit(E_APPLY);
    char *const argv[] = {(char *)"/bin/true", NULL};
    char *const envp[] = {NULL};
    execve("/bin/true", argv, envp); /* seccomp KILLs execve -> SIGSYS; only returns on a (denied) error */
    _exit(1);                        /* if we get here, execve was NOT killed (a hole) */
}
static void child_ptrace(const char *ws)
{
    if (confine_worker(ws) != HC_CONFINE_OK) _exit(E_APPLY);
    syscall(SYS_ptrace, 0L, 0L, 0L, 0L); /* seccomp KILLs ptrace -> SIGSYS */
    _exit(1);
}

/* (d): with deny_network set, socket() is KILLed by the stacked filter (the audio decode/probe helper's floor).
 * Uses the SAME spec shape as confine_worker plus hc_confine_deny_network, so a regression in the stacked filter
 * (or the worker filter accidentally allowing socket past it) is caught. */
static void child_socket_denied(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) _exit(E_APPLY);
    hc_confine_allow_dir(sp, ws, 1);
    hc_confine_deny_network(sp);
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_WORKER_DEFAULT);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    if (st != HC_CONFINE_OK) _exit(E_APPLY);
    int sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); /* deny_network KILLs socket() -> SIGSYS */
    if (sk >= 0) close(sk);
    _exit(1); /* reached only if socket() was NOT killed (a hole in the network-deny filter) */
}

/* (d'): execve must STILL be KILLed when deny_network is set — i.e. the stacked deny filter's default-ALLOW does
 * NOT downgrade the MAIN filter's KILL set (a KILL in either stacked filter is the severest action and wins). */
static void child_denynet_execve(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) _exit(E_APPLY);
    hc_confine_allow_dir(sp, ws, 1);
    hc_confine_allow_read(sp, "/usr");
    hc_confine_deny_network(sp);
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_WORKER_DEFAULT);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    if (st != HC_CONFINE_OK) _exit(E_APPLY);
    char *const argv[] = {(char *)"/bin/true", NULL};
    char *const envp[] = {NULL};
    execve("/bin/true", argv, envp); /* still KILLed by the main filter -> SIGSYS */
    _exit(1);                        /* reached only if the stacked deny filter downgraded the execve KILL */
}

/* MANAGED_RUNTIME profile (the host tool launcher's floor for a non-C interpreter): same allow-set + RO system
 * dirs, but execve/clone are additionally ALLOWED. /bin is granted RO too so the interpreter binary is executable. */
static hc_confine_status confine_managed(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) return HC_CONFINE_ERR_NOMEM;
    hc_confine_allow_dir(sp, ws, 1);
    hc_confine_allow_read(sp, "/usr");
    hc_confine_allow_read(sp, "/lib");
    hc_confine_allow_read(sp, "/lib64");
    hc_confine_allow_read(sp, "/bin");
    hc_confine_allow_read(sp, "/etc");
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_MANAGED_RUNTIME);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    return st;
}
/* MANAGED: execve is ALLOWED (the launcher must exec the interpreter) — /bin/true execs + exits 0 (not SIGSYS). */
static void child_managed_execve(const char *ws)
{
    if (confine_managed(ws) != HC_CONFINE_OK) _exit(E_APPLY);
    char *const argv[] = {(char *)"/bin/true", NULL};
    char *const envp[] = {NULL};
    execve("/bin/true", argv, envp);
    _exit(70); /* reached only if execve was DENIED (Landlock) — not killed, but a hole vs the managed contract */
}
/* MANAGED: ptrace is STILL KILLed — only exec + thread creation were loosened; the escape/escalation floor holds. */
static void child_managed_ptrace(const char *ws)
{
    if (confine_managed(ws) != HC_CONFINE_OK) _exit(E_APPLY);
    syscall(SYS_ptrace, 0L, 0L, 0L, 0L);
    _exit(1);
}
/* MANAGED + deny_network (the launcher's no-egress default): socket() creation is DENIED for EVERY family — even
 * the local AF_UNIX bus — but with EPERM, not a KILL. No socket is ever created (so a no-egress tool reaches
 * neither the network NOR any local AF_UNIX service), yet a REAL interpreter survives its startup socket probe
 * (glibc NSS opening an nscd socket → EPERM → fall back to files) instead of being SIGSYS-killed before it runs.
 * The tool still speaks the bus, but over a fd the host launcher pre-opens BEFORE this jail and hands in. So the
 * expected outcome here is socket() == -1 with errno EPERM (a clean exit OK), NOT a signal death. */
static void child_managed_unix_socket(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) _exit(E_APPLY);
    hc_confine_allow_dir(sp, ws, 1);
    hc_confine_allow_read(sp, "/usr");
    hc_confine_deny_network(sp);
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_MANAGED_RUNTIME);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    if (st != HC_CONFINE_OK) _exit(E_APPLY);
    int sk = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); /* DENIED with EPERM (not killed); no socket created */
    if (sk >= 0) {
        close(sk);
        _exit(E_SOCKET); /* a socket was actually created -> a containment hole */
    }
    _exit(errno == EPERM ? OK : E_SOCKET); /* EPERM = denied gracefully; any other errno (or a kill) is a failure */
}
/* MANAGED + deny_network: a NETWORK socket is denied too (EPERM) -> no egress whatsoever from a no-egress tool. */
static void child_managed_inet_socket(const char *ws)
{
    hc_confine_spec *sp = hc_confine_spec_new();
    if (!sp) _exit(E_APPLY);
    hc_confine_allow_dir(sp, ws, 1);
    hc_confine_allow_read(sp, "/usr");
    hc_confine_deny_network(sp);
    hc_confine_set_syscall_policy(sp, HC_CONFINE_SYSCALL_MANAGED_RUNTIME);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(sp, &ap);
    hc_confine_spec_free(sp);
    if (st != HC_CONFINE_OK) _exit(E_APPLY);
    int sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); /* network family: DENIED with EPERM (not killed) */
    if (sk >= 0) {
        close(sk);
        _exit(E_SOCKET); /* a network socket was created -> an egress hole */
    }
    _exit(errno == EPERM ? OK : E_SOCKET);
}

static int run(void (*child)(const char *), const char *ws, int *exited, int *exitcode, int *sig)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        child(ws);
        _exit(99); /* unreachable */
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    *exited = WIFEXITED(st);
    *exitcode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    *sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    return 0;
}

int main(void)
{
    char ws[256];
    snprintf(ws, sizeof ws, "/tmp/hc_confine_ws_%ld", (long)getpid());
    if (mkdir(ws, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "test_hc_confine: cannot make a workspace dir\n");
        return 1;
    }

    int exited, code, sig;

    /* (a)+(b) */
    if (run(child_works, ws, &exited, &code, &sig) != 0) {
        fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (exited && code == E_APPLY) {
        printf("test_hc_confine: SKIP — kernel lacks full Landlock+seccomp confinement\n");
        rmdir(ws);
        return 0; /* green on an old kernel */
    }
    CHECK(exited && code == OK,
          "under confinement: workspace write + system read + mmap/clock/getdents WORK, and an outside-workspace "
          "write + an /etc write are DENIED");
    if (exited && code != OK && code != E_APPLY)
        fprintf(stderr, "  (child_works failed at stage %d)\n", code);

    /* (c) execve is KILLed */
    if (run(child_execve, ws, &exited, &code, &sig) == 0)
        CHECK(sig == SIGSYS, "execve under confinement is KILLed with SIGSYS (the worker never execs)");

    /* (c) ptrace is KILLed */
    if (run(child_ptrace, ws, &exited, &code, &sig) == 0)
        CHECK(sig == SIGSYS, "ptrace under confinement is KILLed with SIGSYS");

    /* (d) deny_network: socket() is KILLed (only reached when confinement is active — child_works skipped above
     * otherwise, so this never runs on a kernel without seccomp). */
    if (run(child_socket_denied, ws, &exited, &code, &sig) == 0)
        CHECK(sig == SIGSYS, "with deny_network, socket() is KILLed with SIGSYS (the audio-helper network floor)");

    /* (d') deny_network does NOT downgrade the main KILL set: execve is still SIGSYS-KILLed when stacked. */
    if (run(child_denynet_execve, ws, &exited, &code, &sig) == 0)
        CHECK(sig == SIGSYS, "with deny_network, execve is STILL KILLed (the stacked filter does not downgrade KILL)");

    /* (e) MANAGED_RUNTIME (the non-C tool launcher's floor): execve is ALLOWED so the interpreter can start... */
    if (run(child_managed_execve, ws, &exited, &code, &sig) == 0) {
        CHECK(exited && code == OK,
              "MANAGED_RUNTIME ALLOWS execve (the launcher execs the interpreter; /bin/true runs + exits 0)");
        if (exited && code == 70) fprintf(stderr, "  (managed execve was Landlock-denied — /bin not executable?)\n");
    }
    /* ...but the escape/escalation floor is UNCHANGED: ptrace is still SIGSYS-KILLed under MANAGED_RUNTIME. */
    if (run(child_managed_ptrace, ws, &exited, &code, &sig) == 0)
        CHECK(sig == SIGSYS, "MANAGED_RUNTIME still KILLs ptrace (only exec + thread creation were loosened)");

    /* (e') MANAGED_RUNTIME + deny_network: socket() is DENIED for EVERY family (AF_UNIX + AF_INET) with EPERM, not
     * a KILL — no socket is ever created (so a no-egress managed tool reaches neither the network nor any local
     * IPC), but a real interpreter survives its startup socket probe (glibc NSS/nscd) rather than dying. Its bus
     * fd is pre-opened by the host launcher and inherited, so it needs no socket() of its own. A signal death here
     * (code != OK) would be the regression that aborts a real Python tool at startup. */
    if (run(child_managed_unix_socket, ws, &exited, &code, &sig) == 0)
        CHECK(exited && code == OK,
              "MANAGED_RUNTIME + deny_network EPERMs socket(AF_UNIX) gracefully (no socket, no kill — interpreter survives)");
    if (run(child_managed_inet_socket, ws, &exited, &code, &sig) == 0)
        CHECK(exited && code == OK,
              "MANAGED_RUNTIME + deny_network EPERMs socket(AF_INET) gracefully (no network egress, no kill)");

    {
        char p[4096];
        snprintf(p, sizeof p, "%s/out.txt", ws);
        unlink(p);
    }
    rmdir(ws);

    if (g_fail) {
        fprintf(stderr, "test_hc_confine: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_confine: all checks passed (full confinement applied + verified)\n");
    return 0;
}
