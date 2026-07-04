#ifndef HC_REPLAY_HPP
#define HC_REPLAY_HPP

/* hc_replay — deterministic OFFLINE replay of a recorded agent session (P13a / P03).
 *
 * Purpose:   re-drive a recorded session through the SAME agent loop with a REPLAY backend (the recorded
 *            model outputs fed back by turn index) and MOCK tools (the recorded tool results — no real
 *            side effects, no key, no network), producing a re-derived manifest that the hc_manifest
 *            divergence oracle compares against the original. The manifest's turn_sha256 is the oracle.
 * Owns:      nothing durable — replay_run reads the recorded manifest, writes a re-derived one to a temp
 *            dir, diffs, and removes the temp. The RecordedBackend/ManifestRecorder borrow their inputs.
 * Threading: single-threaded; an agent run is synchronous on the caller's thread.
 * Security:  replay is INERT — the mock tools return recorded strings and perform NO I/O, so a replay can
 *            never reach the real filesystem, bus, or AuthGate (a replay cannot launder a call past the
 *            gate, P03). The recorded text is untrusted model output, stored+hashed, never interpreted.
 * Depends:   hc_agent (the backend seam + loop), hc_manifest (read + diff), hc_json. Never the bus/UI. */

#include "hc_agent.h"    /* hc_agent_backend, hc_agent_observer, hc_agent_tool */
#include "hc_manifest.h" /* hc_manifest_rec, hc_manifest_diff_report          */

#include <string>

namespace hc::replay {

/* Cursor over recorded turns, shared by the replay backend (which emits one turn per chat_stream call)
 * and the mock tools (which return THAT turn's recorded results in call order). One shared cursor keeps
 * the tool results aligned to the turn the loop is dispatching. */
struct ReplayCtx {
    const hc_manifest_rec *recs = nullptr;
    std::size_t            n = 0;
    std::size_t            cursor = 0;       /* next turn the backend will emit                     */
    std::size_t            current_turn = 0; /* the turn whose tool calls are being dispatched now  */
    std::size_t            call_index = 0;   /* next recorded result within current_turn            */
};

/* A turn backend backed by recorded manifest turns — drives the agent loop from recs[] instead of a live
 * model. Pair backend() with mock_tool()s so dispatched calls return the recorded results. */
struct RecordedBackend {
    ReplayCtx ctx;
    RecordedBackend(const hc_manifest_rec *recs, std::size_t n)
    {
        ctx.recs = recs;
        ctx.n = n;
    }
    hc_agent_backend backend();                                          /* {&ctx, replay chat_stream} */
    hc_agent_tool    mock_tool(const char *name, const char *spec_json); /* {name, spec, mock invoke}  */
};

/* An observer that appends each completed turn to an open manifest (model fixed at construction). Writes
 * the re-derived manifest during replay; reusable to record an original session in tests. */
struct ManifestRecorder {
    hc_manifest      *m = nullptr;
    std::string       model;
    hc_agent_observer observer();
};

struct Report {
    bool                    ok = false; /* the replay ran + the diff completed                       */
    hc_manifest_diff_report diff = {};  /* match / first divergence (turn_sha256 oracle)             */
    std::string             error;      /* non-empty on a setup / I/O failure                        */
};

/* Re-drive a recorded session OFFLINE and diff the re-derived manifest against the recording. The bulky
 * input (the growing message history) is hashed, not stored (hc_manifest), so `system_prompt` + `task`
 * must equal the original run for a clean match — a mismatch is correctly reported as a divergence at the
 * first turn. Returns the diff report; ok=false + error set on a setup/I-O failure. */
Report replay_run(const std::string &session_dir, const std::string &system_prompt,
                  const std::string &task);

} // namespace hc::replay

#endif /* HC_REPLAY_HPP */
