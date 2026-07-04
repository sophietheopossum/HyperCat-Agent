/* hc_exec — see hc_exec.h. The bounded, kernel-confined child spawner. Layout: hc_exec_run forks; the CHILD
 * runs child_confine_and_exec (scrub env -> rlimits -> own pgid -> no_new_privs -> Landlock -> seccomp ->
 * execve), reporting any PRE-exec failure to the parent over a CLOEXEC sync pipe (so the parent distinguishes
 * "confinement failed, nothing ran" from a real child exit unambiguously: a byte on the sync pipe = failed at
 * that stage; EOF = execve succeeded). The PARENT drains a bounded capture pipe under a wall-clock deadline,
 * SIGTERM->SIGKILLs the child's whole process group on timeout, then reaps. FAIL-CLOSED: a confinement step
 * that cannot be applied aborts the child before execve — an unconfined binary never runs. Linux-only
 * (Landlock+seccomp); elsewhere hc_exec_run returns HC_EXEC_ERR_UNSUPPORTED.
 * Owns:      see hc_exec.h — nothing process-global; each call's child + pipes are reaped/closed before return.
 * Threading: reentrant, no shared mutable state (see hc_exec.h).
 * Lifetime:  the heap output buffer is owned by the caller (hc_exec_result_free); spec pointers are borrowed. */

#define _GNU_SOURCE /* O_PATH, pipe2, the Landlock/seccomp uapi — must precede every include */

#include "hc_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

/* The confinement stack is Linux-only. If these headers/syscalls are absent we compile a stub that refuses. */
#if defined(__linux__)
#define HC_EXEC_LINUX 1
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
/* Define the Landlock fs rights we govern that a LAGGING userspace header may lack, so the GOVERNED set
 * tracks the running KERNEL (masked by the runtime ABI below), not the build host's headers. Without this a
 * newer right (e.g. IOCTL_DEV, ABI 5) silently compiles out and the kernel never mediates it — a real gap on
 * a kernel that supports it but a header that predates it. The bit values are the stable uapi constants. */
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13) /* ABI 2 */
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14) /* ABI 3 */
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15) /* ABI 5 */
#endif
#endif

const char *hc_exec_strerror(hc_exec_status s)
{
    switch (s) {
    case HC_EXEC_OK:              return "ok";
    case HC_EXEC_ERR_ARGS:        return "invalid exec spec";
    case HC_EXEC_ERR_UNSUPPORTED: return "kernel confinement (Landlock+seccomp) unavailable — exec refused";
    case HC_EXEC_ERR_SPAWN:       return "could not spawn the child";
    case HC_EXEC_ERR_CONFINE:     return "could not confine the child (it was not run)";
    case HC_EXEC_ERR_NOMEM:       return "out of memory";
    }
    return "unknown";
}

void hc_exec_result_free(hc_exec_result *r)
{
    if (!r) return;
    free(r->output);
    r->output = NULL;
    r->output_len = 0;
}

/* ---- defaults (a limit <= 0 in the spec takes these) ------------------------------------------------ */
enum {
    kDefTimeoutMs = 10000,
    kMaxTimeoutMs = 120000,
    kKillGraceMs = 250 /* SIGTERM -> wait -> SIGKILL the group */
};
static const long kDefMaxOutput = 256L * 1024;
static const long kDefCpuSec = 10, kDefMemBytes = 512L * 1024 * 1024, kDefFsize = 64L * 1024 * 1024;
/* RLIMIT_NPROC is OFF by default (0 => not set). It is per-REAL-UID and counts TASKS (threads), so on a normal
 * multi-threaded desktop the user already owns thousands of tasks (a browser/IDE alone is hundreds): any cap
 * low enough to bound a fork bomb also blocks the child's own fork()/exec(), and any cap high enough to not
 * break forking is meaningless as a bound. So a low absolute NPROC is actively broken here, not a safe floor.
 * Fork-bomb containment instead comes from the per-call timeout SIGKILL of the whole process group + RLIMIT_AS
 * /RLIMIT_CPU per task; the robust per-TREE cap is a cgroup pids.max (P07). A caller running agents under a
 * DEDICATED quiet UID may set spec.max_procs to a meaningful value. */
static const long kDefNproc = 0, kDefNofile = 256;

/* The 1-byte stage codes the child writes to the sync pipe on a PRE-exec failure (EOF instead == execve ran). */
enum { kStageRlimit = 1, kStageChdir = 2, kStageNoNewPrivs = 3, kStageLandlock = 4, kStageSeccomp = 5,
       kStageExec = 6 };

#ifdef HC_EXEC_LINUX

/* resolved rlimits the child applies (defined before any use so it is one file-scope type, not a
 * prototype-scoped one) */
struct rlimit_set {
    long cpu, mem, fsize, nproc, nofile;
};

/* ---- Landlock: confine fs access to the workspace (rw) + the system dirs the loader needs (r-x) ------ */

static int ll_create_ruleset(const struct landlock_ruleset_attr *attr, size_t size, __u32 flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
static int ll_add_rule(int fd, enum landlock_rule_type t, const void *attr, __u32 flags)
{
    return (int)syscall(SYS_landlock_add_rule, fd, t, attr, flags);
}
static int ll_restrict_self(int fd, __u32 flags) { return (int)syscall(SYS_landlock_restrict_self, fd, flags); }

/* Add a PATH_BENEATH rule granting `access` on the subtree at `path`. A missing path is skipped (returns 0 —
 * e.g. no /sbin); a real failure on an existing path returns -1 (the caller fails closed). */
static int ll_allow(int ruleset_fd, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1; /* absent dir: nothing to grant; other error: fail */
    struct landlock_path_beneath_attr pb = {.allowed_access = access, .parent_fd = fd};
    int rc = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

/* Build + enforce the Landlock ruleset. Returns 0 on success, -1 if Landlock is unavailable or any step on an
 * existing path fails (fail-closed). Best-effort ABI masking so it works across kernels >= the ABI we need. */
static int apply_landlock(const char *cwd)
{
    long abi = syscall(SYS_landlock_create_ruleset, (void *)NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1; /* no Landlock -> fail closed */

    /* Every fs right we want to GOVERN (anything not granted on a path below is then denied). Start from the
     * full set this build knows and mask off bits newer than the running ABI. */
    uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                       LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                       LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                       LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                       LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                       LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                       LANDLOCK_ACCESS_FS_MAKE_SYM;
    /* Govern the newer rights too (defined above if the header lacked them), masking each to the running
     * ABI so create_ruleset never rejects a bit the kernel doesn't know. IOCTL_DEV (ABI 5) matters here:
     * without governing it, device ioctls go unmediated. */
    handled |= LANDLOCK_ACCESS_FS_REFER;    /* ABI 2 */
    handled |= LANDLOCK_ACCESS_FS_TRUNCATE; /* ABI 3 */
    handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV; /* ABI 5 */
    if (abi < 2) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_REFER;
    if (abi < 3) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi < 5) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_IOCTL_DEV;

    struct landlock_ruleset_attr attr = {.handled_access_fs = handled};
    int rs = ll_create_ruleset(&attr, sizeof attr, 0);
    if (rs < 0) return -1;

    /* System dirs the loader needs read + EXECUTE (the binary, its shared libs, ld.so) — NO write/create. */
    const uint64_t sys_rx = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                             LANDLOCK_ACCESS_FS_EXECUTE) & handled;
    /* /etc is granted READ only (no execute — nothing here is run): the loader needs ld.so.cache + NSS
     * configs. ACCEPTED RESIDUAL: this makes all of world-readable /etc visible to the child (host-identifying
     * files like machine-id) — info disclosure within the same-uid trust boundary, not an escape; a tighter
     * per-file allowlist is deferred (it risks breaking NSS-using binaries). */
    const uint64_t sys_r = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR) & handled;
    static const char *const sysdirs[] = {"/usr", "/lib", "/lib64", "/bin", "/sbin", NULL};
    /* Workspace: read + write + create/remove — but deliberately NO EXECUTE, so a binary the child writes
     * into the workspace cannot then be run (a re-exec from here is denied at the Landlock layer; this is the
     * actual mechanism, since seccomp does NOT block execve). Rights are masked to `handled`. */
    const uint64_t ws_rw = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                            LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
                            LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                            LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_SYM) &
                           handled;

    int rc = 0;
    for (const char *const *d = sysdirs; *d && rc == 0; d++) rc = ll_allow(rs, *d, sys_rx);
    if (rc == 0) rc = ll_allow(rs, "/etc", sys_r); /* config: read only */
    if (rc == 0)
        rc = ll_allow(rs, "/dev/null",
                      (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) & handled);
    if (rc == 0) rc = ll_allow(rs, cwd, ws_rw); /* the workspace is the ONLY writable subtree */
    if (rc != 0) {
        close(rs);
        return -1;
    }
    if (ll_restrict_self(rs, 0) != 0) {
        close(rs);
        return -1;
    }
    close(rs);
    return 0;
}

/* ---- seccomp: block ptrace + cross-process memory peeks + socket creation + io_uring (no debugger, no
 * reading another process's memory, no network — including the io_uring async bypass of socket()) + a set of
 * CVE-rich kernel surfaces (keyring, userfaultfd, open_by_handle_at, bpf) as attack-surface reduction. NOTE we
 * do NOT block execve: the
 * filter (with no_new_privs) and the Landlock ruleset are BOTH inherited across execve, so a re-exec'd process
 * is confined identically — and blocking execve here would block the child's OWN execve(argv[0]), so nothing
 * would ever run. no_new_privs additionally neuters any setuid bit on a re-exec'd binary, so re-exec cannot
 * escalate. Network isolation comes from refusing socket() (stronger + simpler than an unprivileged netns). -- */

#if defined(__x86_64__)
#define HC_EXEC_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define HC_EXEC_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define HC_EXEC_NO_SECCOMP_ARCH 1 /* unknown arch: refuse rather than apply a wrong-arch filter */
#endif

static int apply_seccomp(void)
{
#ifdef HC_EXEC_NO_SECCOMP_ARCH
    return -1; /* fail closed on an arch we can't build a correct filter for */
#else
    /* Validate the arch first (a wrong-arch syscall-number reinterpretation is a classic seccomp bypass), then
     * deny the dangerous syscalls with EPERM, else allow. KILL_PROCESS on an arch mismatch. */
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HC_EXEC_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#define DENY(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
                 BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
        DENY(__NR_ptrace),
        DENY(__NR_socket),
#ifdef __NR_socketcall
        DENY(__NR_socketcall), /* the i386/compat multiplexed socket entry */
#endif
        /* process_vm_readv/writev: a cross-process memory peek that does NOT go through ptrace() — on a
         * permissive yama ptrace_scope a same-uid child could otherwise read the HOST's memory (the API key).
         * Block both (the child has nothing legitimate to peek). */
#ifdef __NR_process_vm_readv
        DENY(__NR_process_vm_readv),
#endif
#ifdef __NR_process_vm_writev
        DENY(__NR_process_vm_writev),
#endif
        /* io_uring: an async submission ring that can perform socket/connect/openat WITHOUT issuing those as
         * individual syscalls — a classic seccomp bypass of the socket() block. Deny the whole io_uring API so
         * the network/fs denials can't be smuggled past the filter. */
#ifdef __NR_io_uring_setup
        DENY(__NR_io_uring_setup),
#endif
#ifdef __NR_io_uring_enter
        DENY(__NR_io_uring_enter),
#endif
#ifdef __NR_io_uring_register
        DENY(__NR_io_uring_register),
#endif
        /* attack-surface reduction (CVE-rich kernel surfaces a test runner never legitimately needs; each is
         * an unconditional nr deny, so no risk of breaking threads/normal syscalls). NOT load-bearing for the
         * escape analysis — Landlock + non-root + no_new_privs already bound these — but shrinking the surface
         * is the right posture for an adversarial-agent model. */
#ifdef __NR_keyctl
        DENY(__NR_keyctl), /* the kernel keyring — a recurring LPE surface */
#endif
#ifdef __NR_add_key
        DENY(__NR_add_key),
#endif
#ifdef __NR_request_key
        DENY(__NR_request_key),
#endif
#ifdef __NR_userfaultfd
        DENY(__NR_userfaultfd), /* a kernel-exploit TOCTOU-widening primitive */
#endif
#ifdef __NR_open_by_handle_at
        DENY(__NR_open_by_handle_at), /* opens by inode handle — bypasses PATH-based Landlock mediation */
#endif
#ifdef __NR_bpf
        DENY(__NR_bpf), /* the BPF verifier — a large historically-CVE-rich surface */
#endif
#undef DENY
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {.len = (unsigned short)(sizeof filter / sizeof filter[0]), .filter = filter};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) == 0 ? 0 : -1;
#endif
}

/* ---- the child: confine, then exec. Never returns on success (execve); on ANY failure writes a stage byte
 * to `errfd` and _exit()s WITHOUT execve (fail-closed). ---- */
static void child_fail(int errfd, unsigned char stage) __attribute__((noreturn));
static void child_fail(int errfd, unsigned char stage)
{
    /* best-effort report; the parent reads this byte to classify. A short write is irrelevant (we _exit). */
    ssize_t w = write(errfd, &stage, 1);
    (void)w;
    _exit(120 + stage);
}

static void set_rlimit(int res, long v)
{
    if (v <= 0) return;
    struct rlimit rl = {(rlim_t)v, (rlim_t)v};
    setrlimit(res, &rl); /* best-effort; a failure to lower a limit is not itself an escape */
}

static void child_confine_and_exec(const hc_exec_spec *spec, const struct rlimit_set *rl,
                                   int errfd) __attribute__((noreturn));
static void child_confine_and_exec(const hc_exec_spec *spec, const struct rlimit_set *rl, int errfd)
{
    /* own process group so a timeout can SIGKILL the whole tree, not just argv[0] */
    setpgid(0, 0);

    /* rlimits (CORE=0 always; the rest from the spec) */
    struct rlimit zero = {0, 0};
    setrlimit(RLIMIT_CORE, &zero);
    set_rlimit(RLIMIT_CPU, rl->cpu);
    set_rlimit(RLIMIT_AS, rl->mem);
    set_rlimit(RLIMIT_FSIZE, rl->fsize);
    set_rlimit(RLIMIT_NPROC, rl->nproc);
    set_rlimit(RLIMIT_NOFILE, rl->nofile);

    if (chdir(spec->cwd) != 0) child_fail(errfd, kStageChdir);

    /* no_new_privs is REQUIRED before seccomp and hardens Landlock (no setuid escalation) */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) child_fail(errfd, kStageNoNewPrivs);
    if (apply_landlock(spec->cwd) != 0) child_fail(errfd, kStageLandlock);
    if (apply_seccomp() != 0) child_fail(errfd, kStageSeccomp);

    /* the scrubbed environment — the host's env (incl. any API key) is NOT inherited. HOME points at the
     * workspace (the only writable subtree) so a tool's $HOME cache writes land inside the jail rather than
     * EPERM'ing on an un-granted /tmp. Built with memcpy (async-signal-safe post-fork), never snprintf. */
    char         home[4096];
    const char   pfx[] = "HOME=";
    size_t       cl = strlen(spec->cwd);
    if (cl > sizeof home - sizeof pfx) cl = sizeof home - sizeof pfx; /* truncate a pathological cwd */
    memcpy(home, pfx, sizeof pfx - 1);
    memcpy(home + sizeof pfx - 1, spec->cwd, cl);
    home[sizeof pfx - 1 + cl] = '\0';
    char *const envp[] = {home, (char *)"PATH=/usr/bin:/bin", (char *)"TERM=dumb", NULL};
    execve(spec->argv[0], (char *const *)spec->argv, envp);
    child_fail(errfd, kStageExec); /* execve only returns on failure */
}

#endif /* HC_EXEC_LINUX */

/* ---- parent: bounded capture under a deadline, then reap ---- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#ifdef HC_EXEC_LINUX
/* Drain the child's capture pipe (stdout+stderr, bounded into `buf` up to `cap`) AND the CLOEXEC confinement
 * sync pipe, under a wall-clock deadline that SIGTERM->SIGKILLs the child's whole process GROUP on timeout.
 * Always drains both to EOF so a child writing a full pipe never blocks. Fills *out_len / *out_trunc /
 * *out_timed_out and, iff the child aborted before execve, *out_fail_stage (the non-zero stage byte). */
static void parent_capture(int cap_fd, int err_fd, char *buf, long cap, long timeout_ms, pid_t pid,
                           size_t *out_len, int *out_trunc, int *out_timed_out, unsigned char *out_fail_stage)
{
    size_t        len = 0;
    int           truncated = 0, timed_out = 0;
    unsigned char fail_stage = 0;
    long          deadline = now_ms() + timeout_ms;
    int           killed = 0, sigkilled = 0; /* SIGTERM sent / SIGKILL sent (each once) */
    int           cap_open = 1, err_open = 1;
    while (cap_open || err_open) {
        long remaining = deadline - now_ms();
        if (remaining <= 0 && !killed) {
            kill(-pid, SIGTERM); /* the whole group */
            timed_out = 1;
            killed = 1;
            deadline = now_ms() + kKillGraceMs; /* brief grace, then SIGKILL below */
            continue;
        }
        if (remaining <= 0 && killed && !sigkilled) {
            kill(-pid, SIGKILL); /* once — then keep draining to EOF so the parent never deadlocks */
            sigkilled = 1;
            deadline = now_ms() + kKillGraceMs;
        }
        struct pollfd pfds[2];
        int           n = 0, cap_idx = -1, err_idx = -1;
        if (cap_open) {
            pfds[n].fd = cap_fd;
            pfds[n].events = POLLIN;
            cap_idx = n++;
        }
        if (err_open) {
            pfds[n].fd = err_fd;
            pfds[n].events = POLLIN;
            err_idx = n++;
        }
        long wait = deadline - now_ms();
        if (wait < 0) wait = 0;
        if (wait > 100) wait = 100; /* cap the slice so the deadline is re-checked promptly */
        int pr = poll(pfds, (nfds_t)n, (int)wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue; /* slice elapsed — re-evaluate the deadline */
        if (cap_idx >= 0 && (pfds[cap_idx].revents & (POLLIN | POLLHUP))) {
            char    tmp[8192];
            ssize_t r = read(cap_fd, tmp, sizeof tmp);
            if (r > 0) {
                size_t room = len < (size_t)cap ? (size_t)cap - len : 0;
                size_t take = (size_t)r < room ? (size_t)r : room;
                memcpy(buf + len, tmp, take);
                len += take;
                if (take < (size_t)r) truncated = 1; /* over cap — keep draining, just stop storing */
            } else if (r == 0 || errno != EINTR) {
                cap_open = 0;
            }
        }
        if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP))) {
            unsigned char b;
            ssize_t       r = read(err_fd, &b, 1);
            if (r > 0) fail_stage = b; /* a pre-exec failure stage; EOF (r==0) => execve ran */
            err_open = 0;
        }
    }
    *out_len = len;
    *out_trunc = truncated;
    *out_timed_out = timed_out;
    *out_fail_stage = fail_stage;
}
#endif /* HC_EXEC_LINUX */

hc_exec_status hc_exec_run(const hc_exec_spec *spec, hc_exec_result *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!spec || !out || !spec->argv || !spec->argv[0] || spec->argv[0][0] != '/' || !spec->cwd)
        return HC_EXEC_ERR_ARGS;

#ifndef HC_EXEC_LINUX
    (void)now_ms;
    return HC_EXEC_ERR_UNSUPPORTED; /* no Landlock/seccomp on this platform -> refuse (fail-closed) */
#else
    /* refuse early if Landlock is missing on THIS kernel, so the caller gets UNSUPPORTED (not CONFINE) */
    if (syscall(SYS_landlock_create_ruleset, (void *)NULL, 0, LANDLOCK_CREATE_RULESET_VERSION) < 1)
        return HC_EXEC_ERR_UNSUPPORTED;

    long timeout = spec->timeout_ms > 0 ? spec->timeout_ms : kDefTimeoutMs;
    if (timeout > kMaxTimeoutMs) timeout = kMaxTimeoutMs;
    long cap = spec->max_output > 0 ? spec->max_output : kDefMaxOutput;
    struct rlimit_set rl = {spec->cpu_seconds > 0 ? spec->cpu_seconds : kDefCpuSec,
                            spec->mem_bytes > 0 ? spec->mem_bytes : kDefMemBytes,
                            spec->fsize_bytes > 0 ? spec->fsize_bytes : kDefFsize,
                            spec->max_procs > 0 ? spec->max_procs : kDefNproc,
                            spec->max_files > 0 ? spec->max_files : kDefNofile};

    int cap_pipe[2], err_pipe[2]; /* capture (stdout+stderr) + the CLOEXEC confinement-status channel */
    if (pipe2(cap_pipe, O_CLOEXEC) != 0) return HC_EXEC_ERR_SPAWN;
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(cap_pipe[0]);
        close(cap_pipe[1]);
        return HC_EXEC_ERR_SPAWN;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(cap_pipe[0]);
        close(cap_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return HC_EXEC_ERR_SPAWN;
    }
    if (pid == 0) {
        /* CHILD: wire stdout+stderr to the capture pipe; the err pipe write-end stays CLOEXEC so a successful
         * execve closes it (parent sees EOF == "ran"); stdin from /dev/null. */
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(cap_pipe[1], STDOUT_FILENO);
        dup2(cap_pipe[1], STDERR_FILENO);
        close(cap_pipe[0]);
        close(cap_pipe[1]);
        close(err_pipe[0]);
        /* err_pipe[1] is CLOEXEC + inherited; child_fail writes to it, execve closes it */
        child_confine_and_exec(spec, &rl, err_pipe[1]);
        /* unreachable */
    }

    /* PARENT */
    close(cap_pipe[1]);
    close(err_pipe[1]);

    char  *buf = (char *)malloc((size_t)cap + 1);
    if (!buf) {
        close(cap_pipe[0]);
        close(err_pipe[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return HC_EXEC_ERR_NOMEM;
    }
    size_t        len = 0;
    int           truncated = 0, timed_out = 0;
    unsigned char fail_stage = 0; /* a byte from err_pipe => confinement failed at that stage */
    parent_capture(cap_pipe[0], err_pipe[0], buf, cap, timeout, pid, &len, &truncated, &timed_out, &fail_stage);
    close(cap_pipe[0]);
    close(err_pipe[0]);

    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
    }

    if (fail_stage != 0) { /* the child aborted before execve — nothing ran */
        free(buf);
        return fail_stage == kStageExec ? HC_EXEC_ERR_SPAWN : HC_EXEC_ERR_CONFINE;
    }

    buf[len] = '\0';
    out->output = buf;
    out->output_len = len;
    out->truncated = truncated;
    out->timed_out = timed_out;
    if (WIFEXITED(wstatus)) {
        out->exit_code = WEXITSTATUS(wstatus);
        out->term_signal = 0;
    } else if (WIFSIGNALED(wstatus)) {
        out->exit_code = -1;
        out->term_signal = WTERMSIG(wstatus);
    }
    return HC_EXEC_OK;
#endif /* HC_EXEC_LINUX */
}
