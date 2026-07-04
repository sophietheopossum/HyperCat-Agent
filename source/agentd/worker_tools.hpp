#ifndef HC_AGENTD_WORKER_TOOLS_HPP
#define HC_AGENTD_WORKER_TOOLS_HPP

/* worker_tools — the agent tools the worker exposes to its model during a task turn: the token-stream
 * observer (token_on_text), the human-gated fs_write tool, and the deep_reason staged-chain tool, plus
 * their registration. One responsibility: define + register the worker's agent tools. agentd-internal
 * (no stable ABI). The tool contexts are CALLER-owned and borrowed for one hc_agent_run — the worker is
 * single-threaded and the tools run synchronously inside the run, so a stack-owned ctx is safe. */

#include "worker_role.hpp" /* RoleToolset — the role's tool subset register_agent_tools filters on */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct hc_agent;   /* opaque — hc_agent.h   */
struct hc_llm;     /* opaque — hc_llm.h     */
struct hc_sandbox; /* opaque — hc_sandbox.h */

namespace hc {
class BusClient;

/* P09.3: a capability the worker HOLDS, delivered on an AuthGate approval reply. The token is OPAQUE to the
 * worker (it has no signing key — only the host's "capabilities" service verifies it); the cleartext
 * {verb, scope_kind, scope} let the worker pick a covering cap LOCALLY before presenting the token. */
struct HeldCap {
    std::string token;
    int         verb = 0;       /* an hc_cap_verb value (see hc_caps.h)        */
    int         scope_kind = 0; /* an hc_cap_scope_kind value                  */
    std::string scope;
};

/* The worker's in-RAM capability store — agent-lifetime, NEVER persisted (dies with the process, like the
 * spawn token). Single-threaded (the worker is one recv/dispatch loop), so no lock. The local cap-coverage
 * pre-filter that selects which held token to present is file-internal to worker_tools.cpp (non-authoritative
 * — the host's cap.check re-checks the scope on the signed claims). */
struct CapStore {
    std::vector<HeldCap> held;
};

/* Per-task cap on deep_reason invocations (each is ~5 chained LLM calls; this caps the fan-out). */
constexpr int kDeepReasonBudget = 4;

/* Streaming observer state: token_on_text publishes each model on_text delta on the bus "tokens" topic. */
struct TokenSink {
    BusClient *bus;
};
void token_on_text(const char *delta, size_t n, void *user);

/* fs_write context: the bus (to ask the host to approve), the jail to write in, the worker's shared monotonic
 * corr source (== &inner in worker_loop), and (P09.3) the agent-lifetime capability store. All borrowed for one
 * run_task call; `caps` outlives the task (it is owned by worker_loop), so held grants persist across tasks. */
struct FsToolCtx {
    BusClient  *bus;
    hc_sandbox *sb;
    uint64_t   *corr;
    CapStore   *caps = nullptr; /* P09.3: held capabilities (skip the human prompt when one covers the write) */
};

/* deep_reason context: the bus (to publish the chain) + the worker's hc_llm (the Reasoner drives it
 * sequentially while hc_agent is idle); `remaining` is the per-task budget, decremented per invoke. */
struct ReasonToolCtx {
    BusClient *bus;
    ::hc_llm  *llm;
    int        remaining;
};

/* memory context: the bus (to ask the host's MemoryBroker) + the worker's shared corr source. Used by the
 * memory_recall tool (read-only), memory_write (self auto / shared human-gated), and task-start auto-recall. */
struct MemToolCtx {
    BusClient *bus;
    uint64_t  *corr;
};

/* run context (W4.3): the bus (to ask the host's ExecGate via tool.exec) + the worker's shared corr source. The
 * worker NEVER execve's — it sends a validated argv to the host, which re-validates + operator-gates + runs it
 * in the kernel jail (hc_exec) and returns the captured output. Registered ONLY when exec is enabled. */
struct RunToolCtx {
    BusClient *bus;
    uint64_t  *corr;
};

/* skills context (W6 P6.2): the jailed skills root (== --skills-dir, opened by the worker). The load_skill tool
 * reads "<name>/SKILL.md" relative to it (the sandbox O_NOFOLLOW walk confines + symlink-refuses it). Registered
 * ONLY when a skills dir was provided AND the role allows load_skill. `sb` null => the tool is not registered. */
struct SkillToolCtx {
    hc_sandbox *sb;
};

/* The memory.query bus round-trip, shared by the memory_recall tool and task-start auto-recall: returns
 * the broker's fenced "reference, not instruction" text, or "" if memory is unavailable / the query is
 * empty / it timed out. Bounded + safe to call when no broker exists (returns ""). */
std::string query_memory(BusClient &bus, uint64_t *corr, const std::string &query);

/* Register the worker's agent tools on `ag`, filtered by the role's toolset `on` (default all-on): deep_reason
 * / memory_recall / memory_write (via `rz`/`mem`) + the file tools fs_read/fs_list/fs_write/fs_update (iff
 * `fs->sb` AND the toolset allows each, via `fs`) + the `run` exec tool (iff `exec_enabled` AND `run`, via
 * `run`). The ctxs are BORROWED by hc_agent — the caller owns them and they must outlive hc_agent_run. */
void register_agent_tools(hc_agent *ag, FsToolCtx *fs, ReasonToolCtx *rz, MemToolCtx *mem,
                          const RoleToolset &on = {}, RunToolCtx *run = nullptr, bool exec_enabled = false,
                          SkillToolCtx *skills = nullptr);

} // namespace hc

#endif /* HC_AGENTD_WORKER_TOOLS_HPP */
