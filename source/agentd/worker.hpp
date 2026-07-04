#ifndef HC_AGENTD_WORKER_HPP
#define HC_AGENTD_WORKER_HPP

/* agentd worker loop — see worker.cpp for the contract and the control protocol.
 *
 * Purpose:   the body of an agent worker process. It connects to the host bus as a given id,
 *            proves it is the legitimately-spawned worker for that id with a one-time spawn token
 *            (read from an inherited fd, never on argv/env), then serves a small control protocol
 *            (ping / pingpeer / task.assign / shutdown) until told to stop or the bus drops. A
 *            task.assign from the configured controller runs the unit of work end to end: a REAL
 *            hc_agent turn when a model + env key are configured (run_worker builds the hc_llm),
 *            otherwise a deterministic offline echo so the key-free gates still pass. A task whose
 *            capability is "verify" (P04 sibling self-check) instead runs a no-tools SKEPTIC turn
 *            over the carried claim and reports a {verdict,grounds} JSON payload — same bus message,
 *            different stance; offline it refutes iff the claim carries the PLEASE_REFUTE sentinel.
 * Owns:      nothing process-global. One BusClient and (when live) one hc_http + hc_llm for the
 *            worker's lifetime; the env-inherited API key lives only inside hc_llm and is zeroized
 *            on free — never on argv, never logged, never serialized onto the bus.
 * Threading: single-threaded — one recv/dispatch loop, no background threads. A streamed LLM call
 *            is bounded by a per-call HTTP total timeout; the orchestrator's per-task deadline is
 *            the outer backstop for a worker that goes silent.
 * Lifetime:  run_worker() does not return until the worker stops. The WorkerConfig and the strings
 *            it holds must outlive the call; base_url/model are copied into hc_llm, the rest used in
 *            place. The role fields (role/role_prompt/role_tools) are copied from argv into the
 *            WorkerConfig std::strings at startup, then copied again into TaskCtx (role_prompt) /
 *            parsed into a RoleToolset (role_tools) for the run — none aliases argv past construction.
 *            A live worker's hc_http + hc_llm are freed before run_worker returns (the API key
 *            zeroized with the hc_llm).
 * Layout:    the worker is split across three TUs (acyclic: tools -> protocol, loop -> both):
 *            worker_protocol.* (the bus request/reply marshaling), worker_tools.* (the fs_write +
 *            deep_reason agent tools + the token-stream observer), and worker.cpp (the recv/dispatch
 *            loop + TaskCtx + session persistence + the LLM build + run_worker lifecycle).
 */

#include <string>
#include <vector>

namespace hc {

struct WorkerConfig {
    std::string sock;          /* bus socket path                                              */
    std::string id;            /* this worker's bus id, e.g. "agent:A"                         */
    int         token_fd;      /* inherited fd to read the one-time spawn token; <0 = none     */
    bool        crash_on_task = false; /* test fault injection: _exit on a task.assign         */
    /* The only bus id permitted to send task.assign (the orchestrator). The host sets it; a
     * non-empty value is enforced so a same-uid peer cannot inject a task/prompt or spend model
     * budget. Empty (the default) leaves assignment open — for lower-level tests that drive a
     * worker directly. The broker has already authenticated the sender id it stamps. */
    std::string controller_id;
    /* Role definition (Worker Revamp W2; all OPTIONAL — empty reproduces a persona-agnostic full-tool worker).
     * `role` is the spawn-time identity (telemetry + the selector); `role_prompt` is APPENDED after the worker
     * base prompt; `role_tools` is a csv of the tool ids this role registers ("" => all). NONE is secret — only
     * the API key stays env-only; these non-secret strings ride on argv. */
    std::string role;
    std::string role_prompt;
    std::string role_tools;
    /* W4.3: when set (--exec-enabled), the worker registers the `run` tool, which asks the host's ExecGate to
     * run an allowlisted binary in the kernel jail. The host sets this iff the operator's exec allowlist is
     * non-empty; the worker still NEVER execve's (only the host does, after re-validating + the operator gate). */
    bool exec_enabled = false;
    /* If `model` is set AND OPENROUTER_API_KEY is in the env, a task.assign runs a REAL hc_agent
     * turn against that model (key read from env, never argv); otherwise the task action is a
     * deterministic offline echo (so the gates stay key-free). base_url defaults to OpenRouter. */
    std::string model;
    std::string base_url;
    /* If set, the worker opens an hc_sandbox jailed to this root and registers the human-gated
     * fs_write agent tool (each write blocks on a tool.authorize verdict from the host). Empty (the
     * default) => no filesystem tool, so the key-free gates and a tool-less worker are unaffected. The
     * host provisions a per-agent dir (it must already exist; the sandbox never creates its root). */
    std::string workspace;
    /* If set, the worker opens an hc_store at this root and persists each task as a session (the
     * user prompt + the assistant result) for the host's session browser. Empty => no persistence. */
    std::string sessions;
    /* Egress allowlist (anti-SSRF, WI-2 E0). When the worker builds its HTTP client it installs the
     * hc_policy guard: the default DENIES any non-Public resolved peer (public unicast — e.g. OpenRouter
     * — is always allowed), and each entry here RE-PERMITS one exact numeric peer IP (the local-LLM-server
     * case). Collected from repeated --egress-allow args; merged with the HC_EGRESS_ALLOW env (csv) at
     * build. Empty (the default) = pure default-deny-non-Public — strictly a STRENGTHENING over the prior
     * no-guard state. */
    std::vector<std::string> egress_allow;
    /* Per-project Skills (W6 P6.2 — the mechanism for models WITHOUT native skills). `skills_catalog` is the
     * host-built, already FENCED + defanged catalog block (skill names + one-line descriptions) APPENDED to the
     * system prompt after the role overlay (progressive disclosure); empty => nothing appended. Host-bounded to
     * 16 KiB (format_skills_catalog), so it rides argv well within ARG_MAX. `skills_dir` is the project's skills/
     * root the worker opens jailed so the load_skill tool can read a body on demand; empty => no load_skill tool.
     * NON-secret (rides argv). */
    std::string skills_catalog;
    std::string skills_dir;
};

/* Run the worker to completion. Returns a process exit code (0 = clean shutdown, non-zero =
 * connect/check-in failure or bus drop). Does not return until the worker stops. */
int run_worker(const WorkerConfig &cfg);

} // namespace hc

#endif /* HC_AGENTD_WORKER_HPP */
