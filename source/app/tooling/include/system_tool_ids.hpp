#ifndef HC_SYSTEM_TOOL_IDS_HPP
#define HC_SYSTEM_TOOL_IDS_HPP

/* system_tool_ids — the authoritative list of BUILT-IN tool names (the worker System Tools + the conductor
 * control-plane tools) that a third-party tool MUST NOT shadow. One responsibility: answer "is this name
 * reserved?" so the ToolHost can reject a colliding third-party manifest at INSTALL (the anti-spoof guard).
 * This is defense-in-depth on top of the worker's structural defense (System Tools register FIRST and
 * find_tool returns the first match, so even a registered collision can't win). PURE: <string> only.
 *
 * The list MIRRORS the k*Name registrations in agentd/worker_tools.cpp + app/conductor/src/conductor_tools.cpp —
 * keep it in sync when adding a built-in tool (a missing entry only weakens the anti-spoof check, never crashes).
 * The `hc_` / `system_` name prefixes are also reserved so future built-ins in those namespaces are protected
 * without over-blocking ordinary third-party names. */

#include <string>

namespace hcapp {

/* True iff `name` is a built-in tool name (or a reserved namespace prefix) a third-party tool may not take. */
bool is_reserved_tool_name(const std::string &name);

} // namespace hcapp

#endif /* HC_SYSTEM_TOOL_IDS_HPP */
