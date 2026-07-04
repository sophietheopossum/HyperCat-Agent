#ifndef HC_CONDUCTOR_TOOLS_HPP
#define HC_CONDUCTOR_TOOLS_HPP

/* conductor_tools — the conductor's control-plane tool surface (Conductor P4). See conductor_tools.cpp.
 *
 * Registers the conductor's tools on its hc_agent, mirroring agentd/worker_tools.cpp's register_agent_tools:
 * each tool is a hc_agent_tool{name, spec_json, invoke, user} whose invoke is a thin body over (a) the
 * conductor's own GoalStore (set_goal / update_goal), (b) the host ControlPlane seam (plan_goal / run_agenda /
 * agenda_status / steer_or_cancel / recall_memory / write_memory / deep_reason / add_worker / remove_worker /
 * list_workers — the P2.4 fleet-shaping ops), or (c) the operator inbox (ask_user). The tool bodies run on the
 * CONDUCTOR THREAD (inside hc_agent_run); they may block (an LLM call,
 * a dispatch, awaiting the operator). They treat the model-supplied args as untrusted data. Most conductor
 * turns call NO tool — the tools are reached for deliberately ([07](Docs/Plan_Conductor/07)).
 */

#include <functional>
#include <string>

struct hc_agent;

namespace hc::conductor {

class GoalStore;
struct ControlPlane;

/* The borrowed context the tool bodies close over. Populated by Conductor::create(); every pointer BORROWS and
 * must outlive the agent (the agent is freed before this ctx in ~Conductor). */
struct ConductorToolCtx {
    GoalStore          *goals = nullptr; /* the conductor's own GoalStore (conductor-thread-only access) */
    const ControlPlane *cp = nullptr;    /* the host facade seam (read-only on the conductor thread)     */
    std::string         session_id;      /* goal provenance: the conductor's conversation session id     */

    /* Surface `question` to the operator and BLOCK until their next line (or the conductor stops). Returns true
     * with the reply in `out`, or false if the conductor is stopping (ask_user then reports a clean abort).
     * Supplied by Conductor (which owns the inbox/cv) so this ctx stays free of the component internals. */
    std::function<bool(const std::string &question, std::string &out)> ask_user;
};

/* Register all conductor control-plane tools on `ag`. `ctx` is BORROWED and must outlive the agent. */
void register_conductor_tools(hc_agent *ag, ConductorToolCtx *ctx);

} // namespace hc::conductor
#endif /* HC_CONDUCTOR_TOOLS_HPP */
