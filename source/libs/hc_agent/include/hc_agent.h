#ifndef HC_AGENT_H
#define HC_AGENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The backend seam (P13a/P03) drives one turn from EITHER a live hc_llm OR a replay source, and its
 * signature is expressed in hc_llm's message/handler/status types — so this header includes hc_llm.h
 * (which transitively pulls hc_http.h). For a HOSTED consumer this is free (it already includes hc_llm.h
 * to build the client). A purely OFFLINE consumer (app/replay) does pay a transitive hc_http.h compile +
 * link cost it never uses — an accepted pre-1.0 bend. The clean removal (a self-contained hc_agent turn
 * type so hc_llm.h becomes an hc_agent_new implementation detail) is deferred (EXPANSIONS, P5). */
#include "hc_llm.h"

/* hc_agent — single-agent turn runtime: the loop that drives an hc_llm conversation, dispatches
 * the model's tool calls to registered handlers, feeds results back, and repeats until the model
 * stops calling tools (or a limit / cancel). POSIX.
 *
 * Purpose:   own one agent's message history and tool set, and run a turn end to end. Knows
 *            nothing of the bus, orchestration, or other agents — one process runs one agent.
 * Owns:      the message history (heap strings) and a copy of the system prompt; it BORROWS the
 *            hc_llm handle and the registered tools' name/spec strings (which must outlive it).
 * Threading: an hc_agent is single-owner, not thread-safe. Cancellation is a per-run token the
 *            caller may set from a signal handler / another thread (a single volatile flag).
 * Lifetime:  strings passed to the observer are valid only for that call — copy to keep. A tool
 *            handler returns a malloc'd result string that hc_agent takes ownership of and frees.
 */

typedef enum {
    HC_AGENT_OK = 0,
    HC_AGENT_ERR_INVALID,
    HC_AGENT_ERR_LLM,        /* the model call failed (see hc_llm)        */
    HC_AGENT_ERR_CANCELLED,  /* the cancel token was set                  */
    HC_AGENT_ERR_LIMIT,      /* max tool-call iterations reached          */
    HC_AGENT_ERR_NOMEM
} hc_agent_status;

const char *hc_agent_status_str(hc_agent_status);

/* A tool the agent may call. `spec_json` is the JSON object describing it for the model's
 * "tools" array (e.g. {"type":"function","function":{...}}); `invoke` runs it. */
typedef struct {
    const char *name;
    const char *spec_json;
    /* Run the tool: `args_json` is the model-supplied arguments. Return a malloc'd result string
     * (hc_agent frees it) or NULL on failure. */
    char *(*invoke)(const char *args_json, void *user);
    void *user;
} hc_agent_tool;

/* Per-run cancel token: set `flag` non-zero (from anywhere) to stop the run at the next check. */
typedef struct {
    volatile int flag;
} hc_agent_cancel;

typedef struct {
    void (*on_text)(const char *delta, size_t n, void *user);    /* streamed assistant text */
    void (*on_tool)(const char *name, const char *args, const char *result, void *user);
    /* A COMPLETED turn (one model call + its tool dispatch): the input it saw (a canonical JSON of the
     * message history, for hashing), this turn's assistant text, the tool calls + results as JSON arrays
     * ("" if none), and the finish_reason. Fired once per loop iteration — for per-turn audit + replay
     * recording (the P2 manifest). Optional; all string args are valid only for the call. */
    void (*on_turn)(int turn_index, const char *input, const char *assistant_text,
                    const char *tool_calls_json, const char *tool_results_json,
                    const char *finish_reason, void *user);
    void *user;
} hc_agent_observer;

typedef struct hc_agent hc_agent;

/* A turn backend: the source of one assistant turn. `chat_stream` has the SAME contract as
 * hc_llm_chat_stream (drive `handlers` with text deltas + assembled tool calls; write finish_reason).
 * The hosted backend forwards to hc_llm_chat_stream; a REPLAY backend (offline, no key/network) feeds
 * recorded turns back by index. The agent loop calls this and is otherwise identical live or replayed —
 * so a recorded session replays through the SAME dispatch path (a replay can't launder a tool past the
 * gate, P03). `ctx` is borrowed and must outlive the agent. */
typedef struct {
    void *ctx;
    hc_llm_status (*chat_stream)(void *ctx, const hc_llm_message *msgs, size_t n_msgs,
                                 const char *tools_json, const hc_llm_handlers *handlers,
                                 char *finish_reason_out, size_t fr_cap);
} hc_agent_backend;

hc_agent *hc_agent_new(hc_llm *llm, const char *system_prompt); /* borrows llm; NULL on failure */
/* As hc_agent_new, but driven by an explicit backend (e.g. replay). The backend + its ctx are borrowed
 * and must outlive the agent. NULL on bad arg / OOM. */
hc_agent *hc_agent_new_backend(const hc_agent_backend *backend, const char *system_prompt);
/* Build a HOSTED backend over `llm` (the same turn source hc_agent_new uses internally) — for a caller
 * that drives a backend-taking API live (e.g. the host consolidation passes). `llm` is borrowed and must
 * outlive any agent built from the returned backend. */
hc_agent_backend hc_agent_hosted_backend(hc_llm *llm);
void      hc_agent_free(hc_agent *);
/* The hc_llm status underlying the last HC_AGENT_ERR_LLM from this agent -- a timeout, an HTTP error, a
 * non-2xx status and an unparseable body all surface as that ONE code, so this is the only way to tell
 * an operator which of them happened. Only meaningful after a run returned HC_AGENT_ERR_LLM. */
hc_llm_status hc_agent_last_llm_status(const hc_agent *);

/* Register a tool. The hc_agent_tool struct is COPIED; the pointers it holds (name, spec_json, and
 * user) are BORROWED and must stay valid until hc_agent_free — so passing a local struct is fine, but
 * the strings/user it points to must outlive the agent. Returns false if full or on bad arg. */
bool hc_agent_add_tool(hc_agent *, const hc_agent_tool *tool);
void hc_agent_set_max_iterations(hc_agent *, int max_iters); /* default 8 */

/* Seed one PRIOR message into the history WITHOUT running a turn — for a host-side resume wrapper that reloads
 * a stored conversation into a fresh agent. hc_agent stays session-agnostic: it knows NOTHING of hc_store; the
 * host reads the session and calls this per message, in order. The message is appended after the system prompt.
 * `role` must be one of "user"|"assistant"|"tool"|"system" (any other role is REJECTED); do NOT re-seed the
 * system prompt — it is set at construction. Returns false on a bad arg (null/unknown role) or OOM. Intended
 * BEFORE the first hc_agent_run; the caller owns ordering/validity (e.g. a "tool" result should follow its
 * assistant tool-call). */
bool hc_agent_seed_message(hc_agent *, const char *role, const char *content);

/* ---- context compaction (optional; Conductor P2) ----
 * History grows unbounded by message count; a long conversation would exhaust the context window. This ONE
 * optional hook lets a host compact the history at a turn boundary — it is NOT a fork: the run loop gains a
 * single "if over threshold, compact" check, and the default (no compactor) is byte-for-byte the old
 * behaviour. The compactor itself (the tiered hot/warm/cold summarization) lives host-side. */

/* An opaque handle, valid ONLY for the duration of one compactor call, over the agent's CURRENT history.
 * The compactor reads the existing messages and BUILDS the replacement: keep() copies a current message
 * verbatim (the recent "hot" window), emit() appends a fresh summary message (the "warm"/"cold" tiers). */
typedef struct hc_agent_compaction hc_agent_compaction;

size_t      hc_agent_compaction_count(const hc_agent_compaction *);            /* current message count    */
const char *hc_agent_compaction_role(const hc_agent_compaction *, size_t i);   /* role of msg i, or NULL   */
const char *hc_agent_compaction_content(const hc_agent_compaction *, size_t i); /* content of msg i, or NULL */

/* Copy current message `i` (role + content + any tool_call_id / tool_calls_json) verbatim into the
 * replacement — the recent "hot" turns kept as-is. Returns false on a bad index or OOM. */
bool hc_agent_compaction_keep(hc_agent_compaction *, size_t i);
/* Append a NEW message (role + content; no tool fields) to the replacement — a "warm"/"cold" summary.
 * Returns false on OOM. */
bool hc_agent_compaction_emit(hc_agent_compaction *, const char *role, const char *content);

/* The compactor: read the current history via the handle, build the replacement via keep()/emit(). Return
 * true to APPLY (the agent adopts the built history, freeing the old), false to leave the history UNCHANGED
 * (a failure or a decision not to compact). It SHOULD keep(0) the leading system message: that preserves the
 * byte-stable system-prompt prefix (the provider prompt cache), and most providers reject a history that does
 * not begin with the system message; likewise keep a valid sequence (do not orphan a tool result from its
 * assistant tool-call). Runs once per hc_agent_run — at the turn boundary, just AFTER the new user message is
 * appended and BEFORE the first model call (so the triggering turn is visible to keep() as the hot tail);
 * never mid-turn. It may BLOCK (the host's call — typically a summarization LLM round-trip), but MUST NOT call
 * back into hc_agent_run on this agent (single-owner: re-entry would free the history under the in-flight run). */
typedef bool (*hc_agent_compactor)(hc_agent_compaction *, void *user);

/* Install the compactor. fn=NULL (the default) or threshold<=0 disables compaction — NO behaviour change.
 * When set, fn is invoked once per hc_agent_run (just after the new user message is appended, before the first
 * model call) whenever the history message count THEN exceeds `threshold`. `user` is borrowed, outlives agent. */
void hc_agent_set_compactor(hc_agent *, hc_agent_compactor fn, void *user, int threshold);

/* Run one user turn to completion: append `user_message`, then loop (stream the assistant, run
 * any tool calls, append results) until the model returns no tool calls. `obs`/`cancel` optional. */
hc_agent_status hc_agent_run(hc_agent *, const char *user_message, const hc_agent_observer *obs,
                             hc_agent_cancel *cancel);

size_t      hc_agent_message_count(const hc_agent *);            /* history length             */
const char *hc_agent_last_text(const hc_agent *);               /* last assistant content, or NULL */

#ifdef __cplusplus
}
#endif
#endif /* HC_AGENT_H */
