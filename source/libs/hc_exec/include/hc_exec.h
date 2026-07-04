#ifndef HC_EXEC_H
#define HC_EXEC_H

/* hc_exec — run ONE allowlisted binary as a bounded, KERNEL-CONFINED child process, capture its output, and
 * return its exit. The execution primitive behind the host's brokered `run` tool (Worker Revamp W4): the
 * worker never execve's; the host validates an argv against a default-deny allowlist, operator-gates it, then
 * calls this to spawn the child at the agent's workspace.
 *
 * CONFINEMENT (the W4.0 owner decision — "kernel child-jail FIRST"): a child is confined by, in the child
 * between fork and execve:
 *   - a SCRUBBED environment — only HOME / PATH=/usr/bin:/bin / TERM survive; the API key (or ANY other host
 *     env) NEVER reaches the child;
 *   - cwd = the workspace, which is ALSO the only writable subtree;
 *   - rlimits — CPU, address space, file size, open files, CORE=0. RLIMIT_NPROC is OFF by default: it is
 *     per-REAL-UID and counts TASKS (threads), so on a normal multi-threaded desktop any cap low enough to
 *     bound a fork bomb also blocks the child's own fork(), and a cap high enough not to is meaningless — a
 *     broken floor, not a safe one. Fork-bomb containment is instead the timeout SIGKILL of the whole group +
 *     RLIMIT_AS/CPU per task; the robust per-tree cap is a cgroup pids.max (P07). A caller under a dedicated
 *     quiet UID may set spec.max_procs explicitly;
 *   - its OWN process group (so a timeout SIGKILLs the whole group, not just the leader). Residual: a child
 *     that deliberately setsid()s a grandchild escapes BOTH the group kill AND the wall-clock deadline (the
 *     parent reaps only the leader) — the orphan stays Landlock+seccomp confined (no fs escape, no network),
 *     so it is a resource matter not a jail escape, but across many calls an adversary could accrete live
 *     orphans (each bounded by RLIMIT_CPU/AS). The robust close is a PID namespace (child as PID 1, whole
 *     tree dies with it) or a cgroup `cgroup.kill` — the P07 hardening (needs the unprivileged-userns work);
 *   - PR_SET_NO_NEW_PRIVS, then a Landlock-fs ruleset confining filesystem access to the workspace subtree
 *     (read/write/create — but NOT execute) plus read+execute on the system dirs the loader needs
 *     (/usr,/lib,/lib64,/bin,/etc);
 *   - a seccomp-bpf filter that blocks ptrace, process_vm_readv/writev (no peeking the host's memory),
 *     socket creation, and io_uring (which could otherwise issue socket/openat ops async, bypassing the
 *     socket() block) — failing each with EPERM. execve is DELIBERATELY NOT blocked (it would block the child's own
 *     execve(argv[0]) — nothing would run): instead the Landlock ruleset + the seccomp filter + no_new_privs
 *     are ALL inherited across execve, so a re-exec'd process is confined identically and cannot escalate
 *     (no_new_privs neuters any setuid bit; Landlock denies execute on the workspace + write on the system
 *     dirs), and the network/debugger denials persist.
 * FAIL-CLOSED: if no_new_privs / Landlock / seccomp cannot be applied, the child _exit()s WITHOUT execve — an
 * unconfined binary is NEVER run. So a successful run is ALWAYS fully confined; on a kernel without Landlock
 * (or a non-Linux host) hc_exec_run reports a setup failure rather than degrading to a weaker jail.
 *
 * Owns:      nothing process-global. Each call forks one child, captures via a bounded pipe, reaps it, and
 *            returns a heap-owned output buffer the caller frees (hc_exec_result_free). No threads, no signal
 *            handlers installed (the timeout is a poll loop, not SIGALRM). Reentrant.
 * Threading: hc_exec_run may be called concurrently from different threads (no shared mutable state); each
 *            call is independent. (The HOST serializes exec behind the operator gate regardless.)
 * Lifetime:  hc_exec_spec + the strings it points to are BORROWED for the duration of one hc_exec_run call.
 *            hc_exec_result.output is malloc'd BY the library and owned by the CALLER — free it with
 *            hc_exec_result_free (NULL-safe, idempotent). On a non-OK status the result is zeroed (nothing to
 *            free). hc_exec_spec / hc_exec_result are FIXED-LAYOUT value types (the caller stack-allocates +
 *            zero-initializes them), so adding a field is an ABI break requiring a version bump.
 * Platform:  Linux-only confinement (Landlock + seccomp). On a build/host without them hc_exec_run returns
 *            HC_EXEC_ERR_UNSUPPORTED — exec is then unavailable, by design (no weaker fallback). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side outcome of a spawn attempt (distinct from the CHILD's exit, which is hc_exec_result.exit_code). */
typedef enum {
    HC_EXEC_OK = 0,            /* the child ran to completion (or was killed at the timeout) — see the result */
    HC_EXEC_ERR_ARGS,          /* bad spec: null/empty argv, a non-absolute argv[0], a null cwd               */
    HC_EXEC_ERR_UNSUPPORTED,   /* this build/kernel lacks Landlock+seccomp — exec is refused (fail-closed)    */
    HC_EXEC_ERR_SPAWN,         /* fork/pipe/exec plumbing failed on the host side                             */
    HC_EXEC_ERR_CONFINE,       /* the child could not be confined (it _exited without running anything)       */
    HC_EXEC_ERR_NOMEM          /* allocation failure                                                          */
} hc_exec_status;

/* One execution request. All pointers are BORROWED for the duration of the call. Any numeric limit <= 0 takes
 * a built-in conservative default (documented at hc_exec_run). argv[0] MUST be an absolute path (the host
 * resolves + allowlists it first); the env is fixed/scrubbed by hc_exec (the caller cannot inject env). */
typedef struct {
    const char *const *argv;          /* NULL-terminated; argv[0] = an absolute path to the allowlisted binary */
    const char        *cwd;           /* working dir == the Landlock-writable workspace root (must exist)      */
    int                timeout_ms;    /* wall-clock budget; the group is SIGTERM->SIGKILL'd past it            */
    long               max_output;    /* cap on captured stdout+stderr bytes (excess dropped, truncated flag)  */
    long               cpu_seconds;   /* RLIMIT_CPU                                                            */
    long               mem_bytes;     /* RLIMIT_AS (address space)                                            */
    long               fsize_bytes;   /* RLIMIT_FSIZE (largest file the child may write)                       */
    long               max_procs;     /* RLIMIT_NPROC (fork-bomb floor)                                        */
    long               max_files;     /* RLIMIT_NOFILE                                                         */
} hc_exec_spec;

/* The result of an HC_EXEC_OK run. `output` is a heap buffer (NUL-terminated, <= max_output) the caller frees
 * via hc_exec_result_free; merged stdout+stderr in arrival order. On a non-OK status the result is zeroed. */
typedef struct {
    int    exit_code;    /* the child's exit status (0..255) when it exited normally; -1 if killed by a signal */
    int    term_signal;  /* the signal that killed the child (e.g. SIGKILL on timeout), else 0                 */
    int    timed_out;    /* 1 iff the wall-clock timeout fired (the child was killed)                          */
    int    truncated;    /* 1 iff captured output hit max_output and the rest was dropped                      */
    char  *output;       /* malloc'd, NUL-terminated, bounded; NULL iff output_len==0. Caller frees.           */
    size_t output_len;   /* bytes in `output` (excluding the NUL)                                              */
} hc_exec_result;

/* Run `spec->argv` to completion under the full jail. Returns HC_EXEC_OK iff the child was spawned, confined,
 * and reaped (then *out describes its exit — which may itself be a non-zero exit_code or a timeout kill); any
 * other status means the child never ran (a host/confinement failure) and *out is zeroed. Defaults when a
 * limit is <=0: timeout 10s (capped 120s), max_output 256 KiB, cpu 10s, mem 512 MiB, fsize 64 MiB, nofile 256.
 * RLIMIT_NPROC is OFF unless spec.max_procs > 0 (see the banner — a low per-UID/per-task cap breaks forking).
 * The child runs with the scrubbed env; the parent's env (incl. any API key) is NEVER inherited. */
hc_exec_status hc_exec_run(const hc_exec_spec *spec, hc_exec_result *out);

/* Free the heap members of a result (NULL-safe; idempotent — zeroes the freed pointer). */
void hc_exec_result_free(hc_exec_result *);

/* A human-readable string for a status (never NULL). */
const char *hc_exec_strerror(hc_exec_status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HC_EXEC_H */
