#ifndef HC_CONSOLIDATION_HPP
#define HC_CONSOLIDATION_HPP

/* hc_consolidation — the host-side memory consolidation passes (P03 / P6): COMPACTION + DISTILLATION.
 * Closes the Memory loop — long sessions are summarized (without losing ground truth) and recurring
 * episodes become durable, traceable semantic memories.
 *
 * Purpose:   COMPACTION: when a session grows past a threshold, summarize its OLDER turns into one bounded
 *            "episode summary" appended BESIDE the raw transcript (episodes.jsonl in the session dir) —
 *            the raw turns are NEVER modified or destroyed (ground-truth preserving).
 *            DISTILLATION: reflect over a session (its transcript + episode summary) and emit durable
 *            observations as memory writes, each carrying `source = the session id` (episodic -> semantic,
 *            traceable). Budget-capped (one LLM turn + a fact cap). The actual store write goes through a
 *            caller-supplied sink (the host wires it to the human-trusted memory path) so this module
 *            never depends on hc_memory / the bus — it is a pure consolidation policy over hc_agent + the
 *            store.
 * Runs:      in the HOST process (NOT an agent worker) — it is a trusted operator-initiated pass.
 * Threading: single-threaded; each pass is a synchronous one-shot hc_agent run on the caller's thread.
 * Security:  the summarized/distilled text is UNTRUSTED model output. A distilled fact carries MANDATORY
 *            provenance (source = session id) so it is never mistaken for ground truth; the write SINK
 *            (not this module) owns scope + the human-trust policy — a distilled fact inherits exactly the
 *            trust the sink grants, it cannot escalate. Compaction only ever APPENDS beside the raw turns.
 * Depends:   hc_agent (the one-shot summarize/distill turn via the P5 backend seam), hc_store (read the
 *            transcript + the session dir), hc_fs (append episodes.jsonl), hc_json. Never hc_memory/bus/UI. */

#include "hc_agent.h" /* hc_agent_backend — the one-shot summarize/distill turn (hosted or scripted) */

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct hc_store; /* opaque — held only by pointer; the real header stays in the .cpp */

namespace hc::consolidation {

struct CompactOptions {
    std::size_t min_messages = 6; /* only compact a session with MORE than this many messages   */
    std::size_t keep_recent = 2;  /* the most recent N messages stay verbatim (only older summarized) */
};

struct CompactResult {
    bool        compacted = false; /* false (no error) when the session is below threshold        */
    std::string summary;           /* the episode summary written (empty if not compacted)        */
    std::string error;             /* non-empty on a setup / I-O failure                          */
};

/* Summarize a session's older turns into one bounded episode summary appended to episodes.jsonl beside the
 * transcript (raw turns untouched). `backend` provides the summarizing turn (hosted live, or scripted in
 * tests). Below threshold -> compacted=false, no error, nothing written. */
CompactResult compact_session(hc_store *store, const hc_agent_backend &backend,
                              const std::string &session_id, const CompactOptions &opts = {});

/* A sink for one distilled fact: (scope, text, source=session id). The host wires this to the human-
 * trusted memory write path; a test captures the calls. Returns true if the fact was written (the sink
 * may dedup/reject). The sink owns the scope/trust policy — this module only supplies text + provenance. */
using WriteFn = std::function<bool(const std::string &scope, const std::string &text,
                                   const std::string &source)>;

struct DistillOptions {
    /* Distilled facts land in a QUARANTINE scope that agents do NOT auto-recall (recall is {own,shared}).
     * They are model-authored from an UNTRUSTED transcript, so they must NOT silently become fleet-trusted
     * `shared` memory — that would bypass the P4 per-write human gate. The operator reviews them (provenance-
     * tagged, in the Memory panel) and promotes any to `shared` deliberately. Never default this to "shared". */
    std::string scope = "distilled";
    std::size_t max_facts = 5; /* BUDGET: at most this many facts written per session                 */
};

struct DistillResult {
    bool        ok = false;  /* the distill turn ran + parsed                                        */
    std::size_t written = 0; /* facts actually written via the sink (<= max_facts)                   */
    std::string error;
};

/* Reflect over a session (transcript + any episode summary) in ONE budget-capped LLM turn and emit up to
 * opts.max_facts durable facts via `write`, each tagged with source = the session id. ok=false + error on
 * a setup/run failure; ok=true with written=0 when the model finds nothing durable. */
DistillResult distill_session(hc_store *store, const hc_agent_backend &backend, const WriteFn &write,
                              const std::string &session_id, const DistillOptions &opts = {});

/* Idempotency guard for a PERSISTENT store: each session is consolidated at most once across runs. After
 * a session is compacted+distilled, mark it; the pass skips marked sessions on later runs (so re-running
 * the consolidation over a persistent store does NOT re-distill old sessions or append duplicate
 * episodes). The marker is a `consolidated` file in the session's own dir. */
bool is_consolidated(hc_store *store, const std::string &session_id);
void mark_consolidated(hc_store *store, const std::string &session_id);

} // namespace hc::consolidation

#endif /* HC_CONSOLIDATION_HPP */
