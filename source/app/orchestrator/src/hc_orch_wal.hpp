#ifndef HC_ORCH_WAL_HPP
#define HC_ORCH_WAL_HPP

/* hc_orch_wal — the orchestrator's WAL record SCHEMA + the pure recovery FOLD (P14). Internal to
 * hc_orchestrator (like hc_orch_codec): the generic durable log lives in libs/hc_wal; THIS layer is the
 * domain knowledge — how an agenda's lifecycle maps to JSON record lines, and how a replayed stream folds
 * back into an Agenda. Kept separate from the driver so the schema + fold are unit-testable offline with no
 * bus, no threads, no disk.
 *
 * Records (one JSON line each, written by the driver thread):
 *   open    {"t":"open","id","title","goal","tasks":[{id,title,description,capability,deps,state,result}]}
 *           — the FULL task snapshot at run start. On a fresh run every task is "pending"/""; on a RESUME
 *             it carries the recovered Done/Failed states+results, so a second crash still preserves them.
 *   done    {"t":"done","id","result"}      — a task reached Done with its result.
 *   failed  {"t":"failed","id","result"}    — a task reached Failed with its reason.
 *   settled {"t":"settled","verdict"}       — the agenda settled (the driver then removes the WAL).
 *
 * The cheap Assigned/Running/Verifying edges are deliberately NOT journaled (P14 MVP): a task with no
 * done/failed record recovers as Pending and is re-dispatched — the Done RESULTS are the value worth
 * persisting. Ownership: pure value functions; the caller owns every Agenda/string. No I/O here. */

#include "hc_orch_model.hpp"

#include <string>
#include <vector>

namespace hc::orch::wal {

/* Serialize one record to a single JSON line (NO trailing newline; hc_wal_append adds it). Empty string on
 * an allocation failure (the driver treats that as "journaling unavailable" and degrades, never crashes). */
std::string rec_open(const Agenda &ag);
std::string rec_done(const std::string &task_id, const std::string &result);
std::string rec_failed(const std::string &task_id, const std::string &result);
std::string rec_settled(bool failed);

/* The pure recovery fold: replay `lines` (in append order) back into an Agenda. `valid` is false when the
 * stream has no usable `open` record (empty/corrupt). `settled` is true if a `settled` record was seen.
 * Non-terminal tasks come back Pending (cleared assignee, fresh attempt budget — recovery re-dispatches
 * them); Done/Failed tasks keep their state AND result (the durability guarantee). A corrupt line is
 * skipped, not fatal. */
struct Folded {
    Agenda agenda;
    bool   settled = false;
    bool   valid = false;
};
Folded fold(const std::vector<std::string> &lines);

} // namespace hc::orch::wal

#endif /* HC_ORCH_WAL_HPP */
