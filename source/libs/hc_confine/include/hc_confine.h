#ifndef HC_CONFINE_H
#define HC_CONFINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_confine — drop the CALLING process to a kernel-enforced least-privilege floor (P07). An agent worker
 * calls this AFTER it has opened every long-lived resource (bus, TLS+CA, sandbox fds, store) and BEFORE its
 * turn loop, so a compromised model/tool cannot open new files, sockets, or processes beyond the floor.
 *
 * Purpose:   build a profile (the fs subtrees the worker may touch + the syscall posture), then apply it
 *            IRREVERSIBLY to the current process. Linux = prctl(NO_NEW_PRIVS) + a Landlock fs ruleset + a
 *            seccomp-bpf syscall filter. macOS / other = unsupported (honestly reported). It only TIGHTENS.
 * Owns:      nothing process-global; the opaque spec is a single-owner builder (new -> fill -> apply -> free).
 *            apply() retains nothing — the KERNEL holds the policy afterward.
 * Threading: build + apply on the MAIN thread before any helper thread exists (a Landlock/seccomp restriction
 *            applies to the calling thread-group; applying with threads live is undefined for our purposes).
 *            The free functions are reentrant over caller-owned specs.
 * Lifetime:  apply() is a ONE-WAY door on the calling process. A spec is freed by the caller after apply().
 *
 * SAFETY (why a wrong filter does not crash the worker): the seccomp default action is ERRNO(EPERM), NOT KILL
 * — a syscall the worker legitimately needs that is missing from the allow-set returns EPERM (the tool fails
 * gracefully + is diagnosable) rather than SIGSYS-killing the process. Only the never-legitimate syscalls
 * (execve, ptrace, io_uring, mount/namespace/priv-change, kernel-module load) are KILL. HC_CONFINE_SYSCALL_STRICT
 * flips the DEFAULT to KILL for locked-down deployments. The leaf depends on no hc_* lib, only the kernel.
 *
 * CALLER OBLIGATIONS (the agent worker, P07.2):
 *   - NEW THREAD/PROCESS creation is DENIED (clone/fork are not in the allow-set). So acquire everything that
 *     would spawn a thread BEFORE apply(): in particular, libcurl's THREADED DNS resolver spins a thread per
 *     lookup — PRE-RESOLVE + pin the LLM endpoint IP (CURLOPT_RESOLVE) before apply() so no post-confine DNS
 *     thread is needed (this also aligns with the egress DNS-pin design). Apply on the MAIN thread before any
 *     helper thread exists.
 *   - NEVER grant /proc to a worker (allow_dir): the worker's API key lives in its own /proc/self/environ, which
 *     Landlock otherwise keeps unreadable because /proc is not granted — keep it that way.
 *   - SSRF/egress is NOT enforced here (a raw connect() to any IP is allowed at this floor); the egress policy is
 *     enforced one layer up by hc_policy's guard inside hc_http. This floor contains NEW capability, not routing. */

typedef struct hc_confine_spec hc_confine_spec; /* opaque single-owner builder */

typedef enum {
    HC_CONFINE_OK = 0,          /* the FULL requested profile is live                                  */
    HC_CONFINE_ERR_INVALID,     /* bad arg (null spec, empty path, > the entry cap)                    */
    HC_CONFINE_ERR_UNSUPPORTED, /* the kernel lacks BOTH facilities (pre-5.13 Landlock, no seccomp)    */
    HC_CONFINE_ERR_PARTIAL,     /* a WEAKER-than-requested profile is live (the caller logs *out)      */
    HC_CONFINE_ERR_DENIED,      /* the kernel refused to install a ruleset/filter we did request       */
    HC_CONFINE_ERR_NOMEM
} hc_confine_status;
const char *hc_confine_strerror(hc_confine_status);

typedef enum {
    HC_CONFINE_SYSCALL_WORKER_DEFAULT = 0, /* curated worker allow-set; default action ERRNO(EPERM)     */
    HC_CONFINE_SYSCALL_STRICT,             /* same allow-set; default action KILL_PROCESS                */
    /* MANAGED_RUNTIME: the worker allow-set, default ERRNO, but execve/execveat/clone/clone3 are additionally
     * ALLOWED — the minimum a managed interpreter (Python, Node, a threaded runtime) needs to START and run.
     * For the host TOOL LAUNCHER (hc_tool_launch) to jail a non-C tool then exec the interpreter: the launcher
     * applies this profile to itself, then execve's the interpreter, which inherits the jail. The never-legitimate
     * KILL set (ptrace/io_uring/mount/namespace/priv-change/module/perf/handle-ops) is UNCHANGED — escape +
     * escalation stay blocked; only exec + thread creation are permitted (a managed tool may spawn threads/
     * subprocesses WITHIN its Landlock fs + network jail). Looser than WORKER_DEFAULT (which KILLs execve); used
     * ONLY for the managed-runtime launcher, never for a worker or a native self-confining tool. */
    HC_CONFINE_SYSCALL_MANAGED_RUNTIME
} hc_confine_syscall_policy;

/* What the kernel ACTUALLY ended up enforcing — apply() always writes this so the caller can report its TRUE
 * posture honestly (governance: a degraded layer is logged as degraded, never silently claimed). Deliberately a
 * FIXED-LAYOUT value type the caller stack-allocates (int-only fields, write-once) — adding a field is a
 * breaking ABI change; that is the intentional trade for not heap-allocating a one-way-door result. */
typedef struct {
    int seccomp_active; /* a syscall filter is installed                       */
    int landlock_fs;    /* a filesystem ruleset is installed                   */
    int landlock_abi;   /* the Landlock ABI level the kernel reports (0 = none) */
    int default_kill;   /* 1 if the seccomp default action is KILL, else ERRNO  */
} hc_confine_applied;

hc_confine_spec *hc_confine_spec_new(void);
void             hc_confine_spec_free(hc_confine_spec *);

/* Grant a directory subtree. writable != 0 => read + write + create + remove (NO execute — a binary written
 * into it can never be run). writable == 0 => read-only. A MISSING path is skipped (returns OK — mirrors the
 * loader-dir tolerance). The path is COPIED into the spec. */
hc_confine_status hc_confine_allow_dir(hc_confine_spec *, const char *path, int writable);

/* Grant a subtree read-only (convenience for allow_dir(path, 0)). */
hc_confine_status hc_confine_allow_read(hc_confine_spec *, const char *path);

hc_confine_status hc_confine_set_syscall_policy(hc_confine_spec *, hc_confine_syscall_policy);

/* Additionally DENY all network-creating syscalls (socket/socketpair/connect/bind/listen/accept) — for a confined
 * helper that has NO legitimate egress and must never reach the network (the audio decode/probe helper: it parses
 * untrusted bytes only). Implemented as a STACKED seccomp filter layered on TOP of the worker allow-set (seccomp
 * filters combine most-restrictive-wins under NO_NEW_PRIVS), so the worker profile is byte-identical when this is
 * NOT set. The DENIAL ACTION depends on the syscall policy: KILL under WORKER_DEFAULT/STRICT (a probe helper
 * touching the network is a compromise, and the helper is throwaway), but ERRNO(EPERM) under MANAGED_RUNTIME — a
 * real interpreter legitimately probes a socket at startup (glibc NSS opening an nscd socket), so EPERM lets it
 * fail gracefully and fall back rather than aborting the process; the containment is the same either way (no
 * socket of any family is ever created). No effect where seccomp is unavailable (the honest posture still reports
 * seccomp_active=0). The worker itself does NOT set this (it needs libcurl egress, gated by hc_policy). */
hc_confine_status hc_confine_deny_network(hc_confine_spec *);

/* Apply IRREVERSIBLY to the CALLING process. Installs the STRONGEST AVAILABLE subset: returns HC_CONFINE_OK
 * if the FULL profile is live, HC_CONFINE_ERR_PARTIAL if a weaker subset is live (*out describes exactly what),
 * HC_CONFINE_ERR_UNSUPPORTED if NOTHING could be applied. *out is ALWAYS written (zeroed first). Call on the
 * main thread before spawning any helper thread. */
hc_confine_status hc_confine_apply(const hc_confine_spec *, hc_confine_applied *out);

#ifdef __cplusplus
}
#endif
#endif /* HC_CONFINE_H */
