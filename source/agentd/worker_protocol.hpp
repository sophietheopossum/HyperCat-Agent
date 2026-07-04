#ifndef HC_AGENTD_WORKER_PROTOCOL_HPP
#define HC_AGENTD_WORKER_PROTOCOL_HPP

/* worker_protocol — the worker's bus request/reply PROTOCOL helpers: JSON body builders/parsers for the
 * control protocol (checkin / ping / pingpeer / task.assign / task.result / tool.authorize) and the
 * bounded reply-wait. One responsibility: marshal the worker's framed bus protocol. agentd-internal (no
 * stable ABI). The control-protocol message catalog is documented in worker.cpp's banner.
 * Threading: single-owner — the worker drives one BusClient on one thread. Lifetime: stateless free
 * functions over caller-owned strings + the BusClient. */

#include "hc_bus.hpp" /* BusClient, Message */

#include <cstdint>
#include <string>

namespace hc {

/* Read every byte from fd into out (until EOF). false on a read error (or a >4 KiB runaway writer). */
bool read_fd_all(int fd, std::string &out);

/* Pull a string field out of a JSON object body; returns defv when absent/malformed. */
std::string body_str(const std::string &body, const char *key, const char *defv);

/* The three task.assign fields, parsed in ONE pass. Missing/malformed fields come back empty (untrusted
 * model input — only ever flows into the agent prompt + back as a JSON string value, never interpreted). */
struct TaskFields {
    std::string task_id, title, desc, capability; /* capability "verify" => a skeptic self-check (P04) */
    std::string artifact_path;                    /* W1.3: the file the worker must produce ("" = none) */
};
TaskFields parse_task_fields(const std::string &body);

/* Build a small reply body {"ok":<ok>[, <key>:<val>]} (val omitted when key is empty). */
std::string reply_body(bool ok, const char *key = nullptr, const std::string &val = "");

/* Build the check-in request body {"cmd":"checkin","token":"<token>"}. */
std::string checkin_body(const std::string &token);

/* Wait (bounded) for a reply/err with corr==want on this client. true iff a "reply" arrived (vs. a
 * broker err / timeout / bus drop). The budget is ABSOLUTE so a req flood can't re-arm it; an unrelated
 * mid-wait req is declined "busy". When `out` is non-null and a matching reply arrives, it is copied out
 * (valid only when the function returns true). Shared by the ping, the task-result report, and tool-auth. */
bool await_reply(BusClient &bus, uint64_t want, int budget_ms, Message *out = nullptr);

/* PATIENT verdict wait (B1): the operator may take a long time, so loop bounded recv slices until a reply for
 * `want` lands — WITHOUT silently timing out. A healthy no-verdict slice just loops; a reaped/host-gone worker
 * still unblocks because bus.alive() goes false. `total_ms <= 0` waits indefinitely (the default, "patient"); a
 * positive value caps the wait (then returns false = give up). Returns true + fills *out on a verdict, false on a
 * dropped bus or the optional cap. Use for the human tool-authorization gate (NOT a peer ping). */
bool await_reply_patient(BusClient &bus, uint64_t want, long total_ms, Message *out);

/* X pings <peer> and waits (bounded) for the answer. false if the peer is dead/unreachable. */
bool ping_peer(BusClient &bus, const std::string &peer, uint64_t inner_corr);

/* Build a task.result request body — the worker's report back to the orchestrator. */
std::string task_result_body(const std::string &task_id, bool ok, const std::string &payload);

} // namespace hc

#endif /* HC_AGENTD_WORKER_PROTOCOL_HPP */
