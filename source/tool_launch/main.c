/* hc_tool_launch — the managed-runtime tool launcher (the non-C SDK path). The ToolHost spawns THIS for a tool
 * whose manifest declares `runtime: managed` (a Python/etc. interpreter). It applies the kernel jail to itself,
 * then execve's the interpreter — which inherits the jail and speaks the bus protocol. The interpreter (the
 * untrusted tool code) never has to link or call hc_confine: confinement is host-applied here, transparently.
 *
 * Why this exists: an interpreter cannot self-confine like a C tool — the strict floor KILLs execve (so it can't
 * be exec'd after confine) and blocks clone (so a threaded runtime dies). This launcher applies the
 * MANAGED_RUNTIME profile (execve + clone ALLOWED; ptrace/io_uring/namespace/kernel/setuid still KILLed; Landlock
 * fs-jail; network denied unless granted — and under deny, socket() creation is DENIED with EPERM for EVERY family
 * — so no socket is ever created, yet a real interpreter survives its startup socket probe instead of being
 * killed), the minimum a real runtime needs, then execs. The jail persists across execve, so the interpreter runs
 * confined.
 *
 * THE BUS, PRE-OPENED: because the no-egress jail denies socket(), the interpreter cannot open the bus AFTER the
 * jail — so the launcher dials the broker BEFORE confining (exactly as the native C SDK connects before
 * self-confining) and hands the connected fd to the interpreter as `--bus-fd N`. The interpreter speaks the
 * protocol over that inherited fd and never calls socket() itself. This bounds a no-egress managed tool to that
 * one bus fd: it can reach neither the network nor any other local AF_UNIX service.
 *
 * Args:  --pkg <dir>            the tool's package dir (granted READ-ONLY — the entry script + any modules)
 *        --workspace <dir>      [optional] the tool's RW/RO workspace subtree
 *        --workspace-mode rw|ro [optional] the workspace grant mode (default ro)
 *        --allow-net            [optional] do NOT deny network (the manifest granted egress)
 *        -- <argv...>           the program to exec: the resolved interpreter + entry script + the bus args
 *                               (--sock/--id/--token-fd/--checkin-to). The launcher reads --sock to dial the bus,
 *                               then APPENDS `--bus-fd N` to this tail before exec (stripping any pre-existing
 *                               --bus-fd first, so only the launcher's own reaches the interpreter).
 * FAIL-CLOSED: if the bus can't be opened or the FULL jail can't be applied, refuse to exec (rc 2 / 8).
 *
 * Owns:      nothing persistent — execve replaces this process; the confine spec is freed before exec.
 * Threading: single-threaded by design; no helper thread/socket is created before the execve.
 * Lifetime:  exec terminates this process image; the cleanup/diagnostic paths run only on a failure to exec. */

#define _GNU_SOURCE

#include "hc_confine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break; /* launcher flags live ONLY before the exec separator (like has_flag) */
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return NULL;
}
static int has_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) return 0; /* a flag only counts BEFORE the exec separator */
        if (strcmp(argv[i], name) == 0) return 1;
    }
    return 0;
}

/* Drop every inherited fd except stdio + the token fd, so the interpreter does not inherit a stray host fd that
 * would survive Landlock (which mediates open(), not an already-open fd). Mirrors the SDK / audio-helper hygiene.
 * `keep` is the token fd, expected >= 3 (the ToolHost always passes --token-fd N). DEFENSIVE: stdio (0/1/2) is
 * NEVER closed regardless of `keep` — if the token fd is absent/malformed (keep < 3) we still only shed fds from 3
 * up, so a missing token degrades the tool (no check-in) rather than blinding the launcher's own diagnostics.
 * Runs BEFORE the bus is dialed, so the bus fd (created after) is in the clean fd space and is not shed. */
static void drop_inherited_fds_except(int keep)
{
    long rc = -1;
#if defined(SYS_close_range)
    rc = 0;
    if (keep >= 3) { /* preserve stdio + the token fd: close the gaps [3,keep-1] and [keep+1, MAX] */
        if (keep > 3) rc |= syscall(SYS_close_range, 3u, (unsigned)keep - 1, 0u);
        rc |= syscall(SYS_close_range, (unsigned)keep + 1, ~0U, 0u);
    } else { /* no fd to preserve above stdio: shed everything from 3 up, NEVER touching 0/1/2 */
        rc |= syscall(SYS_close_range, 3u, ~0U, 0u);
    }
#endif
    if (rc != 0) { /* fallback: loop from fd 3 (so stdio is inherently safe); skip the token fd if any */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            if (fd != keep) close(fd);
    }
}

/* Connect to the broker's Unix-domain bus at `path`, returning a connected fd (NOT CLOEXEC, so it survives the
 * execve into the interpreter) or -1. Called BEFORE confine so socket()/connect() are still permitted; the broker
 * captures our peer pid here, which the execve preserves, so the interpreter's check-in matches the authorized id. */
static int dial_bus(const char *path)
{
    if (!path || !path[0]) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) return -1; /* path too long for a UDS address */
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); /* no SOCK_CLOEXEC: the interpreter must inherit it across execve */
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    const char *pkg = opt(argc, argv, "--pkg");
    const char *workspace = opt(argc, argv, "--workspace");
    const char *ws_mode = opt(argc, argv, "--workspace-mode");
    const int   ws_rw = ws_mode && strcmp(ws_mode, "rw") == 0;
    const int   allow_net = has_flag(argc, argv, "--allow-net");

    /* find the exec separator "--": everything after it is the program to run (interpreter + entry + bus args) */
    int sep = -1;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { sep = i; break; }
    if (sep < 0 || sep + 1 >= argc) {
        fprintf(stderr, "hc_tool_launch: missing '-- <interpreter> <args...>'\n");
        return 2;
    }
    char **exec_argv = &argv[sep + 1];

    /* the interpreter MUST be an absolute path (the host resolves the allowlisted name -> a system binary); never
     * a relative/package path that could point at unjailed code. */
    if (exec_argv[0][0] != '/') {
        fprintf(stderr, "hc_tool_launch: the interpreter must be an absolute path\n");
        return 2;
    }

    /* scan the exec tail for the bus socket path + the token fd (both passed through to the interpreter) */
    const char *sockpath = NULL;
    int         token_fd = -1;
    for (char **a = exec_argv; *a && *(a + 1); a++) {
        if (strcmp(*a, "--sock") == 0) sockpath = *(a + 1);
        else if (strcmp(*a, "--token-fd") == 0) token_fd = atoi(*(a + 1));
    }

    drop_inherited_fds_except(token_fd); /* clean the fd space first (keep stdio + the token fd) */

    /* Pre-open the bus BEFORE confining: a managed tool can't socket() once the jail's deny-network denies it, so
     * the launcher dials the broker here and hands the connected fd to the interpreter as --bus-fd. */
    int bus_fd = dial_bus(sockpath);
    if (bus_fd < 0) {
        fprintf(stderr, "hc_tool_launch: could not open the bus at %s — refusing to launch\n",
                sockpath ? sockpath : "(none)");
        return 2;
    }

    /* build + apply the managed-runtime jail to THIS process; the execve below inherits it */
    hc_confine_spec *cs = hc_confine_spec_new();
    if (!cs) return 1;
    if (pkg && pkg[0]) hc_confine_allow_read(cs, pkg); /* the package: entry script + modules, READ-ONLY */
    hc_confine_allow_read(cs, "/usr");
    hc_confine_allow_read(cs, "/lib");
    hc_confine_allow_read(cs, "/lib64");
    hc_confine_allow_read(cs, "/bin");
    hc_confine_allow_read(cs, "/etc");
    if (workspace && workspace[0]) hc_confine_allow_dir(cs, workspace, ws_rw);
    hc_confine_set_syscall_policy(cs, HC_CONFINE_SYSCALL_MANAGED_RUNTIME);
    if (!allow_net) hc_confine_deny_network(cs);
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(cs, &ap);
    hc_confine_spec_free(cs);
    fprintf(stderr, "hc_tool_launch: confine: seccomp=%d landlock_fs=%d abi=%d net=%s (%s)\n", ap.seccomp_active,
            ap.landlock_fs, ap.landlock_abi, allow_net ? "allowed" : "denied", hc_confine_strerror(st));
    if (st != HC_CONFINE_OK) { /* require the FULL jail — fail-closed for untrusted code, like the C SDK */
        fprintf(stderr, "hc_tool_launch: full confinement unavailable (%s) — refusing to launch\n",
                hc_confine_strerror(st));
        return 8;
    }

    /* APPEND --bus-fd <fd> to the exec tail so the interpreter uses the inherited connected socket (it never calls
     * socket() itself). Copy the tail, STRIPPING any pre-existing "--bus-fd <val>" first (defense-in-depth: the
     * tail is host-built so it has none today, but stripping guarantees ONLY the launcher's real fd reaches the
     * interpreter regardless of how the helper resolves a duplicate). new_argv <= exec_argv + 2 + NUL, argc+3 bounds it. */
    char busfd_str[16];
    snprintf(busfd_str, sizeof busfd_str, "%d", bus_fd);
    char *new_argv[argc + 3];
    int   n = 0;
    for (char **a = exec_argv; *a; a++) {
        if (strcmp(*a, "--bus-fd") == 0 && *(a + 1)) { /* drop a pre-existing --bus-fd and its value */
            a++;
            continue;
        }
        new_argv[n++] = *a;
    }
    new_argv[n++] = "--bus-fd";
    new_argv[n++] = busfd_str;
    new_argv[n] = NULL;

    execve(new_argv[0], new_argv, (char *const[]){NULL}); /* scrubbed env; the jail + bus fd inherited across exec */
    fprintf(stderr, "hc_tool_launch: execve %s failed: %s\n", new_argv[0], strerror(errno));
    return 1;
}
