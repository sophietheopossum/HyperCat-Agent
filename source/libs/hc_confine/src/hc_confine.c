/* hc_confine — see hc_confine.h. Drop the calling process to a kernel-enforced floor: Linux = NO_NEW_PRIVS +
 * Landlock fs ruleset + seccomp-bpf (default-DENY worker allow-set). Modeled on libs/hc_exec's confinement
 * (extract the technique; the shape differs — hc_exec confines a child across execve, this confines the CURRENT
 * process in place, and INVERTS the seccomp posture to default-deny). macOS / other = honestly unsupported.
 *
 * Owns:      nothing process-global. Threading: build + apply on the main thread before any helper thread.
 * Lifetime:  apply() is a one-way door; the spec is caller-owned. The leaf depends on no hc_* lib, only the kernel. */

#define _GNU_SOURCE /* O_PATH + the Landlock/seccomp uapi — must precede every include */

#include "hc_confine.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#define HC_CONFINE_LINUX 1
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
/* Landlock fs rights a LAGGING userspace header may lack — track the running KERNEL (masked by the runtime ABI),
 * not the build host's headers. Stable uapi bit values (see hc_exec.c). */
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

/* 32 dir entries is far more than any single agent workspace spec needs (the workspace + skills + a handful of
 * system RO roots); the fixed capacity keeps the builder grow/copy-free and the spec trivially freeable. */
enum { kMaxDirs = 32, kPathCap = 4096 };

struct hc_confine_spec {
    struct {
        char path[kPathCap];
        int  writable;
    } dirs[kMaxDirs];
    size_t                    n_dirs;
    hc_confine_syscall_policy policy;
    int                       deny_network; /* 1 => stack a network-deny seccomp filter (the audio helper) */
};

const char *hc_confine_strerror(hc_confine_status s)
{
    switch (s) {
    case HC_CONFINE_OK:              return "ok (full profile applied)";
    case HC_CONFINE_ERR_INVALID:     return "invalid confine spec";
    case HC_CONFINE_ERR_UNSUPPORTED: return "kernel confinement unavailable (no Landlock + no seccomp)";
    case HC_CONFINE_ERR_PARTIAL:     return "a weaker-than-requested profile is live";
    case HC_CONFINE_ERR_DENIED:      return "the kernel refused a requested ruleset/filter";
    case HC_CONFINE_ERR_NOMEM:       return "out of memory";
    }
    return "unknown";
}

hc_confine_spec *hc_confine_spec_new(void)
{
    hc_confine_spec *s = (hc_confine_spec *)calloc(1, sizeof *s);
    return s; /* policy defaults to WORKER_DEFAULT (0) */
}
void hc_confine_spec_free(hc_confine_spec *s) { free(s); }

hc_confine_status hc_confine_allow_dir(hc_confine_spec *s, const char *path, int writable)
{
    if (!s || !path || !path[0]) return HC_CONFINE_ERR_INVALID;
    size_t len = strlen(path);
    if (len >= kPathCap) return HC_CONFINE_ERR_INVALID;
    if (s->n_dirs >= kMaxDirs) return HC_CONFINE_ERR_INVALID;
    memcpy(s->dirs[s->n_dirs].path, path, len + 1); /* bounded by the len check above */
    s->dirs[s->n_dirs].writable = writable ? 1 : 0;
    s->n_dirs++;
    return HC_CONFINE_OK;
}
hc_confine_status hc_confine_allow_read(hc_confine_spec *s, const char *path)
{
    return hc_confine_allow_dir(s, path, 0);
}
hc_confine_status hc_confine_set_syscall_policy(hc_confine_spec *s, hc_confine_syscall_policy p)
{
    if (!s) return HC_CONFINE_ERR_INVALID;
    s->policy = p;
    return HC_CONFINE_OK;
}
hc_confine_status hc_confine_deny_network(hc_confine_spec *s)
{
    if (!s) return HC_CONFINE_ERR_INVALID;
    s->deny_network = 1;
    return HC_CONFINE_OK;
}

#ifdef HC_CONFINE_LINUX

/* ---- Landlock (extracted from hc_exec.c; the dirs come from the spec) ------------------------------- */

static int ll_create_ruleset(const struct landlock_ruleset_attr *attr, size_t size, __u32 flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
static int ll_add_rule(int fd, enum landlock_rule_type t, const void *attr, __u32 flags)
{
    return (int)syscall(SYS_landlock_add_rule, fd, t, attr, flags);
}
static int ll_restrict_self(int fd, __u32 flags) { return (int)syscall(SYS_landlock_restrict_self, fd, flags); }

static int ll_allow(int ruleset_fd, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1; /* absent dir: nothing to grant; other error: fail */
    struct landlock_path_beneath_attr pb = {.allowed_access = access, .parent_fd = fd};
    int                               rc = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    int                               e = errno;
    close(fd);
    errno = e;
    return rc;
}

/* Build + enforce the Landlock ruleset from the spec dirs. Returns the ABI level (>=1) on success, 0 if
 * Landlock is unavailable, -1 if a rule on an EXISTING path failed (fail-closed within Landlock). */
static int apply_landlock(const hc_confine_spec *s)
{
    long abi = syscall(SYS_landlock_create_ruleset, (void *)NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return 0; /* no Landlock on this kernel */

    uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                       LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                       LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                       LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                       LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                       LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                       LANDLOCK_ACCESS_FS_MAKE_SYM;
    handled |= LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_IOCTL_DEV;
    if (abi < 2) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_REFER;
    if (abi < 3) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi < 5) handled &= ~(uint64_t)LANDLOCK_ACCESS_FS_IOCTL_DEV;

    struct landlock_ruleset_attr attr = {.handled_access_fs = handled};
    int                          rs = ll_create_ruleset(&attr, sizeof attr, 0);
    if (rs < 0) return -1;

    /* read-only subtree (system dirs + skills): READ + READ_DIR + EXECUTE. EXECUTE governs execve (which seccomp
     * KILLs anyway) — granting it matches hc_exec's proven posture + covers any loader edge; the worker never
     * writes here. TRUNCATE deliberately withheld. */
    const uint64_t ro = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                         LANDLOCK_ACCESS_FS_EXECUTE) &
                        handled;
    /* writable subtree (the workspace): read + write + create + remove — but NO EXECUTE, so a binary written
     * into it can never be run (Landlock denies a re-exec from here; seccomp also KILLs execve). MAKE_SYM is
     * granted (file tools legitimately create symlinks); the worker's own access is O_NOFOLLOW-jailed by
     * hc_sandbox, and the HOST must not follow an unvalidated worker-written symlink (an accepted residual). */
    const uint64_t rw = (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                         LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
                         LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                         LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_TRUNCATE) &
                        handled;

    int rc = 0;
    for (size_t i = 0; i < s->n_dirs && rc == 0; i++)
        rc = ll_allow(rs, s->dirs[i].path, s->dirs[i].writable ? rw : ro);
    /* /dev/null is needed read+write by libc/curl regardless of the spec dirs */
    if (rc == 0)
        rc = ll_allow(rs, "/dev/null", (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) & handled);
    if (rc != 0) {
        close(rs);
        return -1;
    }
    if (ll_restrict_self(rs, 0) != 0) {
        close(rs);
        return -1;
    }
    close(rs);
    return (int)abi;
}

/* ---- seccomp: default-DENY the worker's footprint (INVERTED vs hc_exec) ----------------------------- */

#if defined(__x86_64__)
#define HC_CONFINE_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define HC_CONFINE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define HC_CONFINE_NO_SECCOMP_ARCH 1
#endif

#ifndef HC_CONFINE_NO_SECCOMP_ARCH
/* one ALLOW jump for an allowed nr; one KILL jump for a never-legitimate nr; one ERRNO(EPERM) jump for a denied-
 * but-survivable nr (the call fails instead of killing the process — used where a legitimate startup path probes a
 * denied syscall, e.g. glibc NSS opening an nscd socket: we want it to get EPERM and fall back, not die). */
#define ALLOW(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define KILLNR(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
                   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
#define ERRNONR(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
#endif

/* A STACKED filter that DENIES the network-CREATING syscalls, layered on top of the worker allow-set, for a
 * process that must never reach the network: the audio decode/probe helper AND a MANAGED-runtime tool. seccomp
 * filters combine most-restrictive-wins under NO_NEW_PRIVS, so this only ADDS denials — the main filter is
 * installed UNCHANGED (the deny_network flag defaults off, so the worker's BPF is byte-identical to before this
 * option existed). Its OWN arch + x32 guard is mandatory: an x32 `socket | 0x40000000` would otherwise slip past
 * a bare KILLNR/ERRNONR constant (the same H1 bypass the main filter guards). Default ALLOW: this filter is silent
 * on every NON-network syscall, deferring the verdict to the main filter.
 *
 * socket() is the CHOKEPOINT: with no socket creatable, neither the network NOR any local AF_UNIX service is
 * reachable — the strongest "no egress" posture. A managed tool can still speak the bus because the host LAUNCHER
 * (hc_tool_launch) opens the bus socket BEFORE applying this jail and hands it to the interpreter as an inherited
 * fd, exactly as the native C SDK connects before self-confining; the interpreter then runs the protocol over that
 * pre-opened fd and never calls socket() itself. (An earlier design instead carved out socket(AF_UNIX) here — but
 * seccomp cannot bound connect() to a path, so that admitted arbitrary same-uid local IPC: D-Bus, X11, the session
 * bus, etc. The pre-opened-fd approach is strictly tighter, so it is the one kept.)
 *
 * `errno_mode` picks the denial ACTION (the denied SET is identical either way):
 *   0 (the audio helper): KILL — a socket() attempt in a pure pipe-fed decoder is a compromise; die on it.
 *   1 (a managed tool): ERRNO(EPERM) — a real interpreter's STARTUP legitimately probes a socket (glibc NSS
 *     opens an nscd AF_UNIX socket); KILL would abort the interpreter before it ran a line of the tool. EPERM
 *     denies the socket (none is ever created — same containment) while letting NSS fall back to files. The tool
 *     still cannot reach the network or any local service: there is no socket fd to connect with.
 * Returns 0 on install, -1 if seccomp / the arch is unavailable. */
static int apply_seccomp_deny_network(int errno_mode)
{
#ifdef HC_CONFINE_NO_SECCOMP_ARCH
    (void)errno_mode;
    return -1;
#else
    /* KILL variant (audio helper): a socket() here is never legitimate -> die. */
    const struct sock_filter deny_kill[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HC_CONFINE_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1), /* x32 range guard (H1), as in the main filter */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        KILLNR(__NR_socket), KILLNR(__NR_connect), KILLNR(__NR_bind), KILLNR(__NR_listen), KILLNR(__NR_accept),
#ifdef __NR_accept4
        KILLNR(__NR_accept4),
#endif
#ifdef __NR_socketpair
        KILLNR(__NR_socketpair),
#endif
#ifdef __NR_socketcall
        KILLNR(__NR_socketcall), /* i386 socket multiplexer (absent on x86_64/aarch64 — harmless to guard-list) */
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW), /* defer all non-network syscalls to the main filter */
    };
    /* EPERM variant (managed tool): same denied set, but the call fails instead of killing — so a real
     * interpreter survives its startup socket probe (glibc NSS/nscd) while still creating no socket. */
    const struct sock_filter deny_errno[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HC_CONFINE_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), /* a WRONG-arch nr is still a hard KILL (anti-bypass) */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), /* x32-range nr likewise KILLed (anti-bypass) */
        ERRNONR(__NR_socket), ERRNONR(__NR_connect), ERRNONR(__NR_bind), ERRNONR(__NR_listen), ERRNONR(__NR_accept),
#ifdef __NR_accept4
        ERRNONR(__NR_accept4),
#endif
#ifdef __NR_socketpair
        ERRNONR(__NR_socketpair),
#endif
#ifdef __NR_socketcall
        ERRNONR(__NR_socketcall),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW), /* defer all non-network syscalls to the main filter */
    };
    const struct sock_filter *filter = errno_mode ? deny_errno : deny_kill;
    size_t n = errno_mode ? sizeof deny_errno / sizeof deny_errno[0] : sizeof deny_kill / sizeof deny_kill[0];
    /* sock_fprog.filter is a non-const ptr in the (legacy) uapi though the kernel never writes it; cast away the
     * const on our compile-time-literal filter, matching how the main filter's prog is built. */
    struct sock_fprog prog = {.len = (unsigned short)n, .filter = (struct sock_filter *)filter};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) == 0 ? 0 : -1;
#endif
}

/* Install the worker seccomp filter. Returns 0 on success, -1 if seccomp is unavailable / the arch is unknown.
 * `strict` => the DEFAULT action is KILL; else ERRNO(EPERM) (a missed allow-set entry degrades a tool, never
 * crashes the worker). The KILL set (execve/ptrace/io_uring/namespace/priv/module/...) is KILL in BOTH modes.
 * The body is long because it is a DECLARATIVE BPF allow/kill TABLE — extracting it into a loop would require
 * building the filter at runtime, losing the compile-time-constant safety of an inline cBPF literal; the length
 * is irreducible data, not branching complexity. */
static int apply_seccomp(int strict, int deny_network, int managed)
{
#ifdef HC_CONFINE_NO_SECCOMP_ARCH
    (void)strict;
    (void)deny_network;
    (void)managed;
    return -1; /* refuse on an arch we can't build a correct filter for */
#else
    /* Install the network-deny filter FIRST when requested. The main worker filter below does NOT allow the
     * seccomp() syscall (the worker never installs filters), so a second SET_SECCOMP layered AFTER it would itself
     * be denied (EPERM) and the whole confinement would report PARTIAL. Installing deny while seccomp is still
     * permitted, then the main filter, leaves BOTH active; per-syscall precedence is over both regardless of
     * install order (a KILL in either wins — it is the most-severe action, signed-ordered above ALLOW). */
    /* deny_network stacks the network-deny filter — KILL socket creation for a pure helper (audio), but ERRNO
     * (EPERM) it for a MANAGED tool so a real interpreter survives its startup socket probe (glibc NSS/nscd) while
     * still creating no socket. The managed tool's bus fd is pre-opened by the launcher before this jail. */
    if (deny_network && apply_seccomp_deny_network(/*errno_mode=*/managed) != 0) return -1;
    /* The filter is assembled from position-independent pieces (each ALLOW/KILLNR is a self-contained
     * compare-then-RET unit, so order only decides FIRST-MATCH precedence): the arch/x32 GUARD head, then —
     * for MANAGED_RUNTIME only — a small ALLOW prefix for exec + thread creation (it matches BEFORE the body's
     * KILL execve, overriding it), then the shared BODY (the KILL set + worker allow-set, byte-identical to the
     * worker/strict filter), then the per-policy default RET. The pieces stay compile-time-constant literals; only
     * the concatenation is at runtime (memcpy below), so the kernel still verifies each piece's BPF as before. */
    static const struct sock_filter head[] = {
        /* validate the arch first (a wrong-arch nr reinterpretation is the classic seccomp bypass) */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HC_CONFINE_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* x32-ABI bypass guard (H1): x32 reuses AUDIT_ARCH_X86_64 but sets bit 0x40000000 on the syscall
         * number, so an x32 `execve|0x40000000` would match no bare KILLNR/ALLOW constant and fall to the
         * default (ERRNO in worker mode) — defeating the KILL set. KILL the entire x32 number range. (Harmless
         * on arches without x32; their syscall numbers are all below this bit.) */
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };
    /* MANAGED_RUNTIME only (a managed interpreter must exec + spawn threads to start): ALLOW exec + clone, placed
     * BEFORE the body so first-match wins over the body's KILL execve. nr is already in the accumulator (loaded by
     * the head). Not emitted for worker/strict — their filter is byte-identical to before. */
    static const struct sock_filter managed_exec[] = {
        ALLOW(__NR_execve), ALLOW(__NR_execveat),
#ifdef __NR_clone
        ALLOW(__NR_clone),
#endif
#ifdef __NR_clone3
        ALLOW(__NR_clone3),
#endif
    };
    static const struct sock_filter body[] = {
        /* ---- KILL: never legitimate for the worker (invocation == compromise) ---- */
        KILLNR(__NR_execve), KILLNR(__NR_execveat), KILLNR(__NR_ptrace),
#ifdef __NR_process_vm_readv
        KILLNR(__NR_process_vm_readv),
#endif
#ifdef __NR_process_vm_writev
        KILLNR(__NR_process_vm_writev),
#endif
#ifdef __NR_io_uring_setup
        KILLNR(__NR_io_uring_setup),
#endif
#ifdef __NR_io_uring_enter
        KILLNR(__NR_io_uring_enter),
#endif
#ifdef __NR_io_uring_register
        KILLNR(__NR_io_uring_register),
#endif
#ifdef __NR_mount
        KILLNR(__NR_mount),
#endif
#ifdef __NR_umount2
        KILLNR(__NR_umount2),
#endif
#ifdef __NR_pivot_root
        KILLNR(__NR_pivot_root),
#endif
#ifdef __NR_chroot
        KILLNR(__NR_chroot),
#endif
#ifdef __NR_setns
        KILLNR(__NR_setns),
#endif
#ifdef __NR_unshare
        KILLNR(__NR_unshare),
#endif
#ifdef __NR_setuid
        KILLNR(__NR_setuid),
#endif
#ifdef __NR_setgid
        KILLNR(__NR_setgid),
#endif
#ifdef __NR_setreuid
        KILLNR(__NR_setreuid),
#endif
#ifdef __NR_setresuid
        KILLNR(__NR_setresuid),
#endif
#ifdef __NR_kexec_load
        KILLNR(__NR_kexec_load),
#endif
#ifdef __NR_init_module
        KILLNR(__NR_init_module),
#endif
#ifdef __NR_finit_module
        KILLNR(__NR_finit_module),
#endif
#ifdef __NR_bpf
        KILLNR(__NR_bpf),
#endif
#ifdef __NR_keyctl
        KILLNR(__NR_keyctl),
#endif
#ifdef __NR_add_key
        KILLNR(__NR_add_key),
#endif
#ifdef __NR_request_key
        KILLNR(__NR_request_key),
#endif
#ifdef __NR_open_by_handle_at
        KILLNR(__NR_open_by_handle_at),
#endif
#ifdef __NR_name_to_handle_at
        KILLNR(__NR_name_to_handle_at), /* the PRODUCER of the inode handle open_by_handle_at consumes — KILL
                                         * both so the path-Landlock bypass pair is symmetric (worker never uses it) */
#endif
#ifdef __NR_userfaultfd
        KILLNR(__NR_userfaultfd),
#endif
#ifdef __NR_perf_event_open
        KILLNR(__NR_perf_event_open),
#endif

        /* ---- ALLOW: the worker's post-seam footprint (file I/O via hc_sandbox, libcurl net + DNS, memory,
         * sync, time, poll, signals, process info, exit). Comprehensive on purpose: the security comes from
         * Landlock-fs + non-root + the KILL set + the egress guard; a syscall NOT listed here hits the default
         * (ERRNO in worker mode) and degrades a tool rather than crashing the worker. ---- */
        /* file I/O */
        ALLOW(__NR_read), ALLOW(__NR_write), ALLOW(__NR_openat), ALLOW(__NR_close), ALLOW(__NR_lseek),
#ifdef __NR_openat2
        ALLOW(__NR_openat2),
#endif
#ifdef __NR_close_range
        ALLOW(__NR_close_range),
#endif
#ifdef __NR_pread64
        ALLOW(__NR_pread64),
#endif
#ifdef __NR_pwrite64
        ALLOW(__NR_pwrite64),
#endif
#ifdef __NR_preadv
        ALLOW(__NR_preadv),
#endif
#ifdef __NR_pwritev
        ALLOW(__NR_pwritev),
#endif
#ifdef __NR_fstat
        ALLOW(__NR_fstat),
#endif
#ifdef __NR_newfstatat
        ALLOW(__NR_newfstatat),
#endif
#ifdef __NR_statx
        ALLOW(__NR_statx),
#endif
#ifdef __NR_fstatfs
        ALLOW(__NR_fstatfs),
#endif
#ifdef __NR_statfs
        ALLOW(__NR_statfs),
#endif
        ALLOW(__NR_getdents64),
#ifdef __NR_getdents
        ALLOW(__NR_getdents),
#endif
        ALLOW(__NR_mkdirat), ALLOW(__NR_unlinkat), ALLOW(__NR_renameat2),
#ifdef __NR_renameat
        ALLOW(__NR_renameat),
#endif
#ifdef __NR_linkat
        ALLOW(__NR_linkat),
#endif
#ifdef __NR_symlinkat
        ALLOW(__NR_symlinkat),
#endif
        ALLOW(__NR_readlinkat),
        /* legacy NON-*at file ops (x86_64): modern glibc routes the POSIX wrappers to the *at syscalls, but some
         * versions still emit these bare numbers — notably rename() (the COMMIT step of hc_fs_atomic_write, used
         * by hc_store), so omitting them would silently EPERM every store/meta write. They are no weaker than the
         * *at forms (Landlock mediates the resolved path identically). #ifdef-guarded (absent on *at-only arches). */
#ifdef __NR_rename
        ALLOW(__NR_rename),
#endif
#ifdef __NR_unlink
        ALLOW(__NR_unlink),
#endif
#ifdef __NR_mkdir
        ALLOW(__NR_mkdir),
#endif
#ifdef __NR_rmdir
        ALLOW(__NR_rmdir),
#endif
#ifdef __NR_stat
        ALLOW(__NR_stat),
#endif
#ifdef __NR_lstat
        ALLOW(__NR_lstat),
#endif
#ifdef __NR_access
        ALLOW(__NR_access),
#endif
#ifdef __NR_readlink
        ALLOW(__NR_readlink),
#endif
#ifdef __NR_chmod
        ALLOW(__NR_chmod),
#endif
#ifdef __NR_fchmod
        ALLOW(__NR_fchmod),
#endif
#ifdef __NR_fchmodat
        ALLOW(__NR_fchmodat),
#endif
#ifdef __NR_dup2
        ALLOW(__NR_dup2),
#endif
#ifdef __NR_pipe
        ALLOW(__NR_pipe),
#endif
        ALLOW(__NR_fsync),
#ifdef __NR_fdatasync
        ALLOW(__NR_fdatasync),
#endif
        ALLOW(__NR_ftruncate),
#ifdef __NR_fallocate
        ALLOW(__NR_fallocate),
#endif
        ALLOW(__NR_fcntl),
#ifdef __NR_flock
        ALLOW(__NR_flock),
#endif
#ifdef __NR_dup
        ALLOW(__NR_dup),
#endif
#ifdef __NR_dup3
        ALLOW(__NR_dup3),
#endif
        ALLOW(__NR_faccessat),
#ifdef __NR_faccessat2
        ALLOW(__NR_faccessat2),
#endif
#ifdef __NR_getcwd
        ALLOW(__NR_getcwd),
#endif
        /* ioctl is allowed unconditionally (curl needs FIONBIO/FIONREAD on sockets). Its safety rests on the
         * REACHABLE fd set, not the filter: Landlock grants only the workspace + RO system dirs + /dev/null, so
         * the worker can't open a device node (no tty -> no TIOCSTI injection); Landlock IOCTL_DEV (ABI>=5) also
         * mediates device ioctls. The targets are thus sockets / pipes / regular files only. */
        ALLOW(__NR_ioctl),
        /* network (libcurl) + DNS. socket() is allowed for ALL domains (no arg-filter). ACCEPTED RESIDUALS:
         *  - AF_PACKET/raw sockets need CAP_NET_RAW, so a NON-ROOT worker cannot create them. PRECONDITION: the
         *    host is not run as root / with CAP_NET_RAW (the supervisor does not drop privilege; this rests on the
         *    operator not launching HyperCat as root). A future domain arg-filter to {UNIX,INET,INET6} would make
         *    this independent of uid + close the AF_NETLINK host-topology info-leak (also reachable here).
         *  - SSRF/egress is NOT enforced at THIS floor: a raw connect() to a private/metadata IP is allowed here;
         *    egress is gated ONE LAYER UP by hc_policy's guard inside hc_http. The worker has no code path that
         *    does a raw connect outside libcurl, and the model cannot write+run new code (execve is KILLed). */
        ALLOW(__NR_socket), ALLOW(__NR_connect), ALLOW(__NR_getsockopt), ALLOW(__NR_setsockopt),
        ALLOW(__NR_getsockname), ALLOW(__NR_getpeername), ALLOW(__NR_sendto), ALLOW(__NR_recvfrom),
        ALLOW(__NR_sendmsg), ALLOW(__NR_recvmsg),
#ifdef __NR_sendmmsg
        ALLOW(__NR_sendmmsg),
#endif
#ifdef __NR_recvmmsg
        ALLOW(__NR_recvmmsg),
#endif
        ALLOW(__NR_shutdown),
        /* memory */
        ALLOW(__NR_mmap), ALLOW(__NR_mremap), ALLOW(__NR_munmap), ALLOW(__NR_mprotect), ALLOW(__NR_brk),
        ALLOW(__NR_madvise),
#ifdef __NR_msync
        ALLOW(__NR_msync),
#endif
#ifdef __NR_membarrier
        ALLOW(__NR_membarrier), /* C5: recent glibc/runtime startup may issue a private-expedited membarrier */
#endif
        /* sync / TLS / thread-local bootstrap (no NEW thread — clone is denied by default) */
        ALLOW(__NR_futex),
#ifdef __NR_futex_waitv
        ALLOW(__NR_futex_waitv),
#endif
        ALLOW(__NR_set_robust_list), ALLOW(__NR_get_robust_list),
#ifdef __NR_rseq
        ALLOW(__NR_rseq),
#endif
        ALLOW(__NR_set_tid_address),
#ifdef __NR_arch_prctl
        ALLOW(__NR_arch_prctl),
#endif
        /* prctl is a multiplexer, but it cannot WIDEN this floor: seccomp filters STACK additively under
         * NO_NEW_PRIVS (a second SET_SECCOMP filter can only further restrict), and NO_NEW_PRIVS is already set
         * + cannot be unset. So a later prctl (PR_SET_NAME, a tighter SET_SECCOMP, etc.) is non-escalating. */
        ALLOW(__NR_prctl),
        /* time */
        ALLOW(__NR_clock_gettime), ALLOW(__NR_clock_nanosleep), ALLOW(__NR_nanosleep), ALLOW(__NR_gettimeofday),
        ALLOW(__NR_clock_getres),
#ifdef __NR_time
        ALLOW(__NR_time),
#endif
        /* poll / epoll / pipe (the bus recv loop + curl) */
#ifdef __NR_poll
        ALLOW(__NR_poll),
#endif
        ALLOW(__NR_ppoll),
#ifdef __NR_select
        ALLOW(__NR_select),
#endif
        ALLOW(__NR_pselect6), ALLOW(__NR_epoll_create1), ALLOW(__NR_epoll_ctl), ALLOW(__NR_epoll_pwait),
#ifdef __NR_epoll_wait
        ALLOW(__NR_epoll_wait),
#endif
        ALLOW(__NR_eventfd2), ALLOW(__NR_pipe2),
        /* process / identity / entropy / limits / info */
        ALLOW(__NR_getpid), ALLOW(__NR_gettid), ALLOW(__NR_getppid), ALLOW(__NR_getuid), ALLOW(__NR_geteuid),
        ALLOW(__NR_getgid), ALLOW(__NR_getegid), ALLOW(__NR_getgroups), ALLOW(__NR_getrandom),
        ALLOW(__NR_prlimit64), ALLOW(__NR_getrlimit), ALLOW(__NR_uname), ALLOW(__NR_sysinfo),
        ALLOW(__NR_sched_getaffinity), ALLOW(__NR_sched_yield),
#ifdef __NR_getcpu
        ALLOW(__NR_getcpu),
#endif
        /* signals */
        ALLOW(__NR_rt_sigaction), ALLOW(__NR_rt_sigprocmask), ALLOW(__NR_rt_sigreturn),
#ifdef __NR_rt_sigtimedwait
        ALLOW(__NR_rt_sigtimedwait),
#endif
        ALLOW(__NR_sigaltstack), ALLOW(__NR_restart_syscall),
#ifdef __NR_tgkill
        ALLOW(__NR_tgkill), /* C4: glibc abort()/assert()/__stack_chk_fail -> raise() -> tgkill (self-directed);
                             * without it a fatal error path can't deliver SIGABRT + the process limps on */
#endif
        /* exit */
        ALLOW(__NR_exit), ALLOW(__NR_exit_group),
    };
    /* Assemble head ++ (managed ? managed_exec) ++ body ++ default-RET into one program. The buffer is sized to
     * the MAXIMUM (managed included); `n` is the actual instruction count installed. Default action: ERRNO
     * (degrade a missing allow-set entry) unless STRICT (KILL). */
    struct sock_filter prog_filter[(sizeof head + sizeof managed_exec + sizeof body) / sizeof(struct sock_filter) + 1];
    size_t n = 0;
    memcpy(prog_filter, head, sizeof head);
    n += sizeof head / sizeof head[0];
    if (managed) {
        memcpy(prog_filter + n, managed_exec, sizeof managed_exec);
        n += sizeof managed_exec / sizeof managed_exec[0];
    }
    memcpy(prog_filter + n, body, sizeof body);
    n += sizeof body / sizeof body[0];
    prog_filter[n++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K,
        strict ? SECCOMP_RET_KILL_PROCESS : (SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
    struct sock_fprog prog = {.len = (unsigned short)n, .filter = prog_filter};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) == 0 ? 0 : -1;
#endif
}

hc_confine_status hc_confine_apply(const hc_confine_spec *s, hc_confine_applied *out)
{
    hc_confine_applied zero = {0};
    if (out) *out = zero;
    if (!s) return HC_CONFINE_ERR_INVALID;

    /* NO_NEW_PRIVS first: required for an unprivileged seccomp filter + neuters any setuid bit. */
    int nnp = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;

    int abi = apply_landlock(s); /* >=1 = installed, 0 = no Landlock on this kernel, -1 = a rule was REFUSED */
    int ll_denied = abi < 0;     /* the kernel HAS Landlock but refused a ruleset/rule we requested */
    int ll_ok = abi > 0;
    if (out) {
        out->landlock_fs = ll_ok;
        out->landlock_abi = abi > 0 ? abi : 0;
    }

    int strict = s->policy == HC_CONFINE_SYSCALL_STRICT;
    int managed = s->policy == HC_CONFINE_SYSCALL_MANAGED_RUNTIME;
    int sc_ok = nnp && apply_seccomp(strict, s->deny_network, managed) == 0; /* seccomp needs NO_NEW_PRIVS */
    if (out) {
        out->seccomp_active = sc_ok;
        out->default_kill = sc_ok && strict;
    }

    /* A REQUESTED layer the kernel REFUSED (not merely absent) is DENIED — distinct from PARTIAL ("a weaker
     * subset is live because a facility is unavailable") so the caller can tell "old kernel" from "kernel
     * rejected our ruleset" (the honest-posture governance rule). The seccomp side cannot return -1-vs-absent
     * here (an unsupported arch is the only non-install, reported via seccomp_active=0). */
    if (ll_denied) return HC_CONFINE_ERR_DENIED;
    if (ll_ok && sc_ok) return HC_CONFINE_OK;
    if (ll_ok || sc_ok) return HC_CONFINE_ERR_PARTIAL; /* one layer live, the other simply unavailable */
    return HC_CONFINE_ERR_UNSUPPORTED;                 /* neither facility available */
}

#else /* not Linux */

hc_confine_status hc_confine_apply(const hc_confine_spec *s, hc_confine_applied *out)
{
    hc_confine_applied zero = {0};
    if (out) *out = zero;
    if (!s) return HC_CONFINE_ERR_INVALID;
    return HC_CONFINE_ERR_UNSUPPORTED; /* macOS sandbox_init arm is a deferred slice (P07 §7) */
}

#endif /* HC_CONFINE_LINUX */
