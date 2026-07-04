/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Savannah Goring. Part of the HyperCat Tool SDK — see the LICENSE file (Apache-2.0).
 * (The HyperCat application itself is licensed separately; this SDK header stands alone.) */
#ifndef HC_TOOL_H
#define HC_TOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_tool — the HyperCat third-party Tool SDK (Custom Tooling, Wave D). An author writes a tool BINARY that
 * links this static lib, declares its function(s), and calls hc_tool_main() — that single call IS the whole
 * tool main(). The harness:
 *   1. connects to the host message bus (a Unix-domain socket; path + the tool's bus id + a one-time token
 *      arrive on argv / an inherited fd from the host ToolHost — the same handoff agentd workers use);
 *   2. completes the hello + token check-in handshake so the broker confirms the tool's id;
 *   3. drops the process to a kernel least-privilege floor via hc_confine (Landlock fs + seccomp) — FAIL-CLOSED:
 *      the tool runs UNTRUSTED code, so it requires the FULL jail (both layers); if confinement is unavailable
 *      or only partial (a non-Linux host, or a kernel missing Landlock or the seccomp arch), it REFUSES to run.
 *      Network is DENIED unless the host passes --allow-net, which grants UNRESTRICTED egress (v1: the manifest's
 *      egress_hosts list is declared intent + drives the per-call operator gate, but is NOT yet kernel-enforced
 *      per-host — every network-granted call is operator-approved at the host; see docs/AUTHORING.md);
 *   4. serves tool.invoke requests — dispatching each to the matching author callback — until shutdown.
 *
 * SECURITY MODEL (what the author can rely on): the tool runs in its OWN process + address space (a crash or
 * RCE cannot touch the host or another agent), confined to its declared workspace subtree with no ambient
 * network/exec, and every sensitive call is still gated by the operator at the host. The author's invoke()
 * runs AFTER confinement, so it must NOT spawn threads/processes or open new sockets (clone/socket are denied
 * at the floor) — do blocking, single-threaded work over args_json and return a result. See docs/AUTHORING.md.
 *
 * The wire contract (the bus message bodies + this struct) is a PUBLIC ABI — versioned from day one. */

#define HC_TOOL_ABI_VERSION 1

/* One function the tool binary exposes to the model. `invoke` receives the call's arguments as a JSON object
 * string and returns a malloc'd result STRING (the harness frees it), or NULL on failure. Same shape as the
 * host's internal hc_agent_tool.invoke, so the mental model is identical. `user` is passed back untouched. */
typedef struct {
    const char *name;      /* the model-facing function name; MUST match the manifest + not be a reserved built-in */
    const char *spec_json; /* the OpenAI-style function spec (informational here; the manifest is authoritative)   */
    char *(*invoke)(const char *args_json, void *user);
    void *user;
} hc_tool_def;

/* The entire tool main(): connect, check in, self-confine, then serve invokes until the host shuts it down.
 * Returns a process exit code (0 = clean shutdown; non-zero = a handshake/confine failure). Recognized argv:
 *   --sock <path>  --id <tool:NAME>  --token-fd <fd>  [--checkin-to <id>]
 *   [--workspace <dir>]  [--workspace-mode rw|ro]  [--allow-net]
 * (the host ToolHost supplies these; --workspace-mode defaults to ro — least privilege). */
int hc_tool_main(int argc, char **argv, const hc_tool_def *defs, size_t n_defs);

/* Apply the SDK's kernel least-privilege floor (Landlock fs + seccomp) to the CURRENT process, FAIL-CLOSED:
 * grants `workspace` (if non-NULL/non-empty) read-only or read-write per `workspace_rw`, plus /usr,/lib,/lib64,
 * /bin,/etc read-only, the native strict syscall floor (no exec, no clone/threads, no new sockets), and denies
 * all network unless `allow_net`. Returns 0 only if the FULL jail (both layers) is live; non-zero if confinement
 * is unavailable or partial — the caller MUST then refuse to run (the tool is untrusted).
 *
 * hc_tool_main() calls this itself after check-in. It is exposed for a COMPILED non-C tool (e.g. a Rust crate
 * that links this static lib and implements the bus protocol in its own language) so it can self-confine the
 * native strict way by a single FFI call, without reimplementing the seccomp/Landlock setup. A MANAGED-runtime
 * tool (an interpreter) does NOT call this — the host launcher jails it externally; see docs/PROTOCOL.md. */
int hc_tool_confine(const char *workspace, int workspace_rw, int allow_net);

#ifdef __cplusplus
}
#endif
#endif /* HC_TOOL_H */
