/* system_tool_ids — implementation. See system_tool_ids.hpp. PURE. */

#include "system_tool_ids.hpp"

namespace hcapp {

namespace {

/* The built-in tool names. MIRRORS the k*Name registrations:
 *   worker System Tools     — agentd/worker_tools.cpp
 *   conductor control plane — app/conductor/src/conductor_tools.cpp
 * Keep in sync when a built-in tool is added. */
const char *const kReservedNames[] = {
    /* worker System Tools */
    "fs_write", "fs_read", "fs_list", "fs_update", "deep_reason", "memory_recall", "memory_write", "run",
    "load_skill",
    /* conductor control-plane tools */
    "set_goal", "update_goal", "plan_goal", "run_agenda", "agenda_status", "agenda_results", "read_artifact",
    "steer_or_cancel", "recall_memory", "write_memory", "ask_user", "add_worker", "remove_worker", "list_workers",
    "list_skills", "create_skill", "list_audio", "set_mood", "control_audio",
};

bool has_prefix(const std::string &s, const char *p)
{
    return s.rfind(p, 0) == 0; /* rfind(.,0)==0 => starts-with */
}

} // namespace

bool is_reserved_tool_name(const std::string &name)
{
    for (const char *r : kReservedNames)
        if (name == r) return true;
    /* reserved system namespaces (future built-ins live here) — does not over-block ordinary third-party names */
    return has_prefix(name, "hc_") || has_prefix(name, "system_");
}

} // namespace hcapp
