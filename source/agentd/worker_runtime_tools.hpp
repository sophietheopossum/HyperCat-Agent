#ifndef HC_AGENTD_WORKER_RUNTIME_TOOLS_HPP
#define HC_AGENTD_WORKER_RUNTIME_TOOLS_HPP

/* worker_runtime_tools — register proxy shims for the enabled THIRD-PARTY tools (Custom Tooling, Wave D). At
 * worker startup the worker asks the ToolHost ("tool.list" -> "toolhost") for the enabled tool functions, and
 * registers one hc_agent_tool per function whose invoke does a bus round-trip to "toolhost"
 * (tool.invoke -> the tool's reply), mirroring the memory_recall pattern. If the ToolHost is not running (no
 * third-party tools, or the kill-switch is armed so none were launched), this is a graceful no-op.
 *
 * Ownership: the proxy contexts are returned in `out_ctxs`, OWNED BY THE CALLER — they must OUTLIVE the agent,
 * because each registered hc_agent_tool's name/spec/user point INTO its context. agentd-internal. */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct hc_agent;

namespace hc {

class BusClient;

/* a registered third-party proxy's context (held for the agent's lifetime; the agent points into it). */
struct ToolProxyCtx {
    BusClient  *bus = nullptr;
    uint64_t   *corr = nullptr;  /* the worker's shared monotonic correlation counter */
    std::string fn;              /* the model-facing function name (also the registered tool name) */
    std::string spec;            /* the function spec_json (the registered tool's spec)            */
    int         timeout_ms = 10000;
    bool        sensitive = false; /* the function (or its manifest envelope) needs a per-call human gate (D5) */
};

/* Query the ToolHost and register a proxy per enabled function on `ag`. Returns the count registered;
 * appends the contexts to `out_ctxs`. Safe to call always — a no-op if no ToolHost / no tools. */
std::size_t register_runtime_tools(hc_agent *ag, BusClient *bus, uint64_t *corr,
                                   std::vector<std::unique_ptr<ToolProxyCtx>> &out_ctxs);

} // namespace hc

#endif /* HC_AGENTD_WORKER_RUNTIME_TOOLS_HPP */
