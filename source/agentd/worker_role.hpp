#ifndef HC_AGENTD_WORKER_ROLE_HPP
#define HC_AGENTD_WORKER_ROLE_HPP

/* worker_role — the spawn-time role's TOOL-SUBSET policy: the RoleToolset flags + the PURE parser that turns
 * the host's --role-tools csv into them. ONE responsibility: decide which of the worker's tools a role
 * registers. Deliberately pure (depends only on <string>, no bus/llm/agent/sandbox) so the subset decision is
 * unit-tested in isolation; the registration that CONSUMES a RoleToolset lives in worker_tools. agentd-internal
 * (no stable ABI). */

#include <string>

namespace hc {

/* W2.6: which of the worker's tools a role registers. Default ALL-on (so an unconfigured role keeps full
 * capability — today's behaviour). The four file tools are ADDITIONALLY gated by a workspace (fs->sb) at the
 * registration site: a role can SUBTRACT a file tool but can NEVER grant one without a sandbox. */
struct RoleToolset {
    bool deep_reason = true, memory_recall = true, memory_write = true;
    bool fs_read = true, fs_list = true, fs_write = true, fs_update = true;
    bool load_skill = true; /* W6 P6.2: read a per-project skill body on demand (ALSO gated by a skills dir) */
};

/* Parse a --role-tools csv (the agentd tool ids) into a RoleToolset. An EMPTY csv => all-on (today's
 * behaviour); a non-empty csv => EXACTLY the listed tools (all others off); an unknown name is ignored (a
 * role config can never grant a tool that does not exist — and an all-unknown csv fails SAFE to no tools). */
RoleToolset parse_role_tools(const std::string &csv);

} // namespace hc

#endif /* HC_AGENTD_WORKER_ROLE_HPP */
