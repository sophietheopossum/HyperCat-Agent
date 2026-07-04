/* hc_tool — the Tool SDK harness. See hc_tool.h. A C leaf: hc_transport + hc_json + hc_confine only (no
 * C++ bus — a tool binary must link no host code). It re-implements the bus client's hello + check-in + the
 * bounded reply-wait BY CONVENTION over hc_transport (exactly as agentd's worker_protocol does on its side).
 *
 * Threading: single-threaded by design — confinement denies clone/socket, so the bus connection is acquired
 * BEFORE hc_confine_apply and no thread/socket is created afterward.
 *
 * hc_tool_main runs long (~190 LOC): it is a single sequential lifecycle pipeline (arg parse -> token read ->
 * connect -> checkin -> self-confine -> serve), the phases labelled below; splitting it would thread the
 * transport handle + the frame helpers through every helper for no isolation gain. */

#define _GNU_SOURCE /* close_range / SYS_close_range (fd hygiene before confinement) */

#include "hc_tool.h"

#include "hc_confine.h"
#include "hc_json.h"
#include "hc_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Drop every inherited fd except stdio (0,1,2) and the one-time token fd, BEFORE the bus connect and any
 * untrusted byte. The host marks its own long-lived fds close-on-exec, but a third-party library fd (e.g. a
 * libcurl socket opened without SOCK_CLOEXEC) that happened to be live at spawn would survive into the tool and
 * bypass Landlock (which mediates open(), never an already-open fd). Mirrors audio_helper's drop_inherited_fds,
 * but preserves --token-fd (the audio helper reads its token from stdin, so it closes 3.. wholesale). */
static void drop_inherited_fds_except(int keep)
{
    long rc = -1;
#if defined(SYS_close_range)
    rc = 0;
    if (keep > 3) rc |= syscall(SYS_close_range, (unsigned)3, (unsigned)keep - 1, 0u);
    rc |= syscall(SYS_close_range, (unsigned)keep + 1, ~0U, 0u);
#endif
    if (rc != 0) { /* no close_range (kernel < 5.9): brute-close up to the soft limit, skipping the token fd */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            if (fd != keep) close(fd);
    }
}

/* ABI layout tripwire (policy 5: stable C ABI at the seam). hc_tool_def is the public SDK struct — four
 * pointers. If this fires, the struct layout changed: bump HC_TOOL_ABI_VERSION (the wire version) so an
 * author binary built against the old layout is rejected rather than silently mis-dispatching its callbacks. */
_Static_assert(sizeof(hc_tool_def) == 4 * sizeof(void *),
               "hc_tool_def layout changed — bump HC_TOOL_ABI_VERSION and update this assert");

/* ---------- arg parsing ---------- */

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
static int has_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

/* ---------- wire helpers (the Message envelope: {"t","from","to","corr","body"}) ---------- */

static int send_frame(hc_transport *ep, const char *type, const char *from, const char *to, long corr,
                      const char *body)
{
    hc_json *o = hc_json_new_object();
    if (!o) return 0;
    hc_json_obj_set_str(o, "t", type);
    if (from && from[0]) hc_json_obj_set_str(o, "from", from);
    if (to && to[0]) hc_json_obj_set_str(o, "to", to);
    if (corr) hc_json_obj_set_int(o, "corr", (int64_t)corr);
    if (body && body[0]) hc_json_obj_set_str(o, "body", body);
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    if (!s) return 0;
    int ok = hc_transport_send(ep, s, strlen(s)) == HC_TRANSPORT_OK;
    free(s);
    return ok;
}

typedef struct {
    char *type;
    char *from;
    long  corr;
    char *body;
} frame;

static void frame_free(frame *f)
{
    free(f->type);
    free(f->from);
    free(f->body);
    f->type = f->from = f->body = NULL;
}

/* recv one frame, strdup'ing the fields (caller frame_free's). 1 on OK, 0 on timeout/close/parse error. */
static int recv_frame(hc_transport *ep, frame *f)
{
    void  *buf = NULL;
    size_t len = 0;
    memset(f, 0, sizeof *f);
    if (hc_transport_recv(ep, &buf, &len) != HC_TRANSPORT_OK) {
        free(buf);
        return 0;
    }
    hc_json *o = hc_json_parse((const char *)buf, len);
    free(buf);
    if (!o) return 0;
    f->type = strdup(hc_json_get_str(o, "t", ""));
    f->from = strdup(hc_json_get_str(o, "from", ""));
    f->corr = (long)hc_json_get_int(o, "corr", 0);
    f->body = strdup(hc_json_get_str(o, "body", ""));
    hc_json_free(o);
    return f->type && f->from && f->body;
}

/* extract a string / bool field from a body JSON object (malloc'd string; caller frees). */
static char *body_str_dup(const char *body, const char *key)
{
    hc_json *o = hc_json_parse(body, strlen(body));
    char    *r = strdup(o ? hc_json_get_str(o, key, "") : "");
    if (o) hc_json_free(o);
    return r;
}
static int body_bool(const char *body, const char *key, int defv)
{
    hc_json *o = hc_json_parse(body, strlen(body));
    int      r = o ? (hc_json_get_bool(o, key, defv) ? 1 : 0) : defv;
    if (o) hc_json_free(o);
    return r;
}

/* ---------- the kernel least-privilege floor (shared by the C harness + a compiled non-C tool via FFI) ---------- */

int hc_tool_confine(const char *workspace, int workspace_rw, int allow_net)
{
    hc_confine_spec *cs = hc_confine_spec_new();
    if (!cs) return 1;
    if (workspace && workspace[0])
        hc_confine_allow_dir(cs, workspace, workspace_rw); /* the tool's workspace, at the manifest-declared mode */
    hc_confine_allow_read(cs, "/usr");
    hc_confine_allow_read(cs, "/lib");
    hc_confine_allow_read(cs, "/lib64");
    hc_confine_allow_read(cs, "/bin");
    hc_confine_allow_read(cs, "/etc");
    hc_confine_set_syscall_policy(cs, HC_CONFINE_SYSCALL_WORKER_DEFAULT);
    if (!allow_net) hc_confine_deny_network(cs); /* no egress grant => no network at all */
    hc_confine_applied ap;
    hc_confine_status  st = hc_confine_apply(cs, &ap);
    hc_confine_spec_free(cs);
    fprintf(stderr, "hc_tool: confine: seccomp=%d landlock_fs=%d abi=%d ws=%s net=%s (%s)\n", ap.seccomp_active,
            ap.landlock_fs, ap.landlock_abi, (workspace && workspace[0]) ? (workspace_rw ? "rw" : "ro") : "none",
            allow_net ? "allowed" : "denied", hc_confine_strerror(st));
    /* FAIL-CLOSED FOR UNTRUSTED CODE: require the FULL jail (HC_CONFINE_OK). A PARTIAL apply (only one of
     * Landlock/seccomp live, e.g. a pre-5.13 kernel or a non-x86_64/aarch64 seccomp arch) would leave the tool
     * with HALF a sandbox (no fs confinement, or no syscall filter) — exactly the floor this subsystem leans on.
     * We refuse rather than run degraded; third-party tools are simply unavailable where both layers can't apply. */
    return st == HC_CONFINE_OK ? 0 : 1;
}

/* ---------- the harness ---------- */

int hc_tool_main(int argc, char **argv, const hc_tool_def *defs, size_t n_defs)
{
    const char *sock = opt(argc, argv, "--sock");
    const char *id = opt(argc, argv, "--id");
    const char *token_fd_s = opt(argc, argv, "--token-fd");
    const char *checkin_to = opt(argc, argv, "--checkin-to");
    const char *workspace = opt(argc, argv, "--workspace");
    const char *ws_mode = opt(argc, argv, "--workspace-mode"); /* "rw" | "ro" (absent => ro, least-privilege) */
    const int   ws_rw = ws_mode && strcmp(ws_mode, "rw") == 0;
    const int   allow_net = has_flag(argc, argv, "--allow-net");
    if (!checkin_to) checkin_to = "toolhost";
    if (!sock || !id || !token_fd_s) {
        fprintf(stderr, "hc_tool: missing --sock / --id / --token-fd\n");
        return 2;
    }
    const int token_fd = atoi(token_fd_s);
    if (token_fd < 3) {
        fprintf(stderr, "hc_tool: bad --token-fd\n");
        return 2;
    }

    /* fd hygiene: shed any inherited fd (except stdio + the token) before we touch the bus or untrusted bytes */
    drop_inherited_fds_except(token_fd);

    /* read the one-time token from the inherited fd (tiny; capped) */
    char   token[4097];
    size_t tlen = 0;
    for (;;) {
        ssize_t r = read(token_fd, token + tlen, sizeof token - 1 - tlen);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        tlen += (size_t)r;
        if (tlen >= sizeof token - 1) break;
    }
    close(token_fd);
    token[tlen] = '\0';
    if (tlen == 0) {
        fprintf(stderr, "hc_tool: empty token\n");
        return 2;
    }

    /* connect + hello + welcome */
    hc_transport *ep = hc_transport_connect(sock);
    if (!ep) {
        fprintf(stderr, "hc_tool: connect failed\n");
        return 1;
    }
    if (!send_frame(ep, "hello", id, NULL, 0, NULL)) {
        fprintf(stderr, "hc_tool: hello send failed\n");
        hc_transport_close(ep);
        return 1;
    }
    hc_transport_set_recv_timeout(ep, 5000);
    {
        frame w;
        if (!recv_frame(ep, &w) || strcmp(w.type, "welcome") != 0) {
            fprintf(stderr, "hc_tool: no welcome from broker\n");
            frame_free(&w);
            hc_transport_close(ep);
            return 1;
        }
        frame_free(&w);
    }

    /* check in to the ToolHost with the one-time token, await {"ok":true} */
    {
        hc_json *cb = hc_json_new_object();
        hc_json_obj_set_str(cb, "cmd", "checkin");
        hc_json_obj_set_int(cb, "v", HC_TOOL_ABI_VERSION);
        hc_json_obj_set_str(cb, "token", token);
        char *cbs = hc_json_print(cb, false);
        hc_json_free(cb);
        int sent = cbs && send_frame(ep, "req", id, checkin_to, 1, cbs);
        free(cbs);
        if (!sent) {
            fprintf(stderr, "hc_tool: checkin send failed\n");
            hc_transport_close(ep);
            return 1;
        }
        int ok = 0;
        for (;;) { /* await the corr=1 reply within the recv timeout */
            frame r;
            if (!recv_frame(ep, &r)) {
                frame_free(&r);
                break;
            }
            if (r.corr == 1 && strcmp(r.type, "reply") == 0) ok = body_bool(r.body, "ok", 0);
            int done = (r.corr == 1);
            frame_free(&r);
            if (done) break;
        }
        if (!ok) {
            fprintf(stderr, "hc_tool: check-in rejected (bad token / unknown id)\n");
            hc_transport_close(ep);
            return 1;
        }
    }

    /* drop to the kernel least-privilege floor (Landlock fs + seccomp). The bus fd is already open; no new
     * socket/thread is created after this. Fail-closed: hc_tool_confine requires the FULL jail (see its banner). */
    if (hc_tool_confine(workspace, ws_rw, allow_net) != 0) {
        fprintf(stderr, "hc_tool %s: full confinement unavailable — refusing to run (fail-closed)\n", id);
        hc_transport_close(ep);
        return 8;
    }

    /* serve invokes until the host shuts us down (or the connection drops) */
    hc_transport_set_recv_timeout(ep, 0); /* block indefinitely waiting for the next invoke */
    int rc = 0;
    for (;;) {
        frame r;
        if (!recv_frame(ep, &r)) { /* host gone */
            frame_free(&r);
            break;
        }
        if (strcmp(r.type, "req") != 0) { /* tools only answer requests */
            frame_free(&r);
            continue;
        }
        char *cmd = body_str_dup(r.body, "cmd");
        if (strcmp(cmd, "tool.shutdown") == 0) {
            send_frame(ep, "reply", id, r.from, r.corr, "{\"ok\":true}");
            free(cmd);
            frame_free(&r);
            break;
        }
        if (strcmp(cmd, "tool.invoke") == 0) {
            char              *fn = body_str_dup(r.body, "tool");
            char              *args = body_str_dup(r.body, "args");
            const hc_tool_def *d = NULL;
            for (size_t i = 0; i < n_defs; i++)
                if (defs[i].name && strcmp(defs[i].name, fn) == 0) {
                    d = &defs[i];
                    break;
                }
            char    *result = d && d->invoke ? d->invoke(args, d->user) : NULL;
            hc_json *rb = hc_json_new_object();
            if (rb) {
                hc_json_obj_set_int(rb, "v", HC_TOOL_ABI_VERSION);
                if (d && result) {
                    hc_json_obj_set_bool(rb, "ok", true);
                    hc_json_obj_set_str(rb, "result", result);
                } else {
                    hc_json_obj_set_bool(rb, "ok", false);
                    hc_json_obj_set_str(rb, "error", d ? "tool returned no result" : "unknown tool function");
                }
                char *rbs = hc_json_print(rb, false);
                hc_json_free(rb);
                send_frame(ep, "reply", id, r.from, r.corr, rbs ? rbs : "{\"ok\":false}");
                free(rbs);
            }
            free(result);
            free(fn);
            free(args);
        } else {
            send_frame(ep, "reply", id, r.from, r.corr, "{\"ok\":false,\"error\":\"unknown command\"}");
        }
        free(cmd);
        frame_free(&r);
    }

    hc_transport_close(ep);
    return rc;
}
