#ifndef HC_ORCH_CODEC_HPP
#define HC_ORCH_CODEC_HPP

/* hc_orch_codec — the orchestrator's side of the task wire format. INTERNAL (src/, not public).
 *
 * Responsibility: build the JSON bodies the driver sends and parse the ones it receives, over
 * hc_json. One place owns the wire format, so neither the engine (pure) nor the driver (I/O) does
 * string-bashing. The worker (agentd) implements the MIRROR of this by convention with its own
 * small helpers — it does NOT link the orchestrator, keeping the dependency arrows downward.
 *
 * Protocol (req/reply bodies on the existing bus envelope):
 *   task.assign  orchestrator -> agent : {"cmd":"task.assign","task_id","title","description",
 *                                          "capability"}                 -> reply {"ok":bool}
 *   task.result  agent -> orchestrator : {"cmd":"task.result","task_id","ok":bool,"payload"}
 *                                          -> reply {"ok":true}
 */

#include "hc_orch_model.hpp"

#include <string>

namespace hc::orch::codec {

/* Build the task.assign request body for `t`. */
std::string assign_body(const Task &t);

/* Build a simple {"ok":<ok>} reply body (the orchestrator's ack of a task.result). */
std::string ack_body(bool ok);

/* The "cmd" field of a body ("" if absent/malformed) — for the driver's recv dispatch. */
std::string body_cmd(const std::string &body);

/* Whether a reply body carries {"ok":true} (parses the worker's assign ack). */
bool ack_is_ok(const std::string &body);

struct TaskResult {
    bool        valid = false; /* false if the body was not a well-formed task.result */
    std::string task_id;
    bool        ok = false;
    std::string payload;
};

/* Parse a task.result request body. `valid` is false unless cmd=="task.result" and task_id != "". */
TaskResult parse_result(const std::string &body);

/* P04: a verify task's RESULT payload is the verifier's judgment `{"verdict":"uphold|refute","grounds"}`.
 * Lenient: a payload that is not such JSON yields verdict="" (-> Uncertain) and grounds = the raw payload. */
struct VerdictMsg {
    std::string verdict; /* "uphold" | "refute" | "" (uncertain)  */
    std::string grounds; /* the verifier's reason (untrusted text) */
};
VerdictMsg parse_verdict(const std::string &payload);

} // namespace hc::orch::codec

#endif /* HC_ORCH_CODEC_HPP */
