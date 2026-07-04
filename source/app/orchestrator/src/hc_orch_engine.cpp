/* hc_orch_engine — the agenda scheduler. See hc_orch_engine.hpp.
 *
 * Pure decision logic over hc_orch_model. ONE engine schedules N concurrent agendas over ONE shared
 * worker pool: dispatch() assigns startable tasks across all active agendas to idle role-matching workers
 * (round-robin across agendas so none starves) and emits each agenda's terminal verdict once it settles;
 * the event handlers update task/worker state then re-dispatch. A worker lost while holding a task
 * reassigns that task within ITS OWN agenda (back to Pending) if its attempt budget remains, else fails it
 * terminally — so a crashed worker's work moves to a survivor instead of stalling the agenda.
 *
 * Each agenda's state (tasks, deps, deadlines, P04 verify tally) is isolated in its own AgendaCtx; only the
 * worker pool is shared. A result/verdict is routed to its agenda by the driver (which knows, from the
 * AssignTask it dispatched, which agenda a worker is serving) — so per-agenda task-id collisions (two
 * agendas each with a "t1") never confuse routing.
 *
 * P04 verification (sibling self-check) lives here as pure per-agenda policy: a Sibling-mode ok result
 * moves the task to Verifying and emits VerifyTask intents to idle siblings that are NOT the author;
 * on_verdict tallies (hc_verify) into Done/Failed; the tally is fail-closed on a lost or deadline-blown
 * verifier (check_deadlines), so a silent verifier can never hang the task. No I/O, no threads. */

#include "hc_orch_engine.hpp"

#include "hc_verify.hpp"

#include <ctime>
#include <unordered_map>
#include <unordered_set>

namespace hc::orch {

/* Default monotonic clock for task deadlines (CLOCK_MONOTONIC, milliseconds). Overridable at Engine
 * construction so the deadline logic is unit-tested deterministically without sleeping. */
static uint64_t monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The catch-all role: a worker whose role is the generalist absorbs ANY task whose specific provider was never added,
 * so a missing specialist degrades to "runs on the generalist" instead of a hard agenda fail. MUST match the
 * "generalist" key in app/roles (roletable_builtin_defaults). The generalist concept lives HERE — the scheduler's
 * worker-SELECTION policy — never in role_matches, which stays the exact-match pure domain predicate. */
static constexpr char kGeneralistRole[] = "generalist";

/* The in-progress verify tally for one Verifying task — engine-internal (the model stays clean). */
struct VerifyState {
    std::string                     author;         /* excluded from being a verifier               */
    std::string                     claim;          /* the original result under review             */
    std::string                     refute_grounds; /* the first refuter's reason                    */
    int                             upholds = 0, refutes = 0, reported = 0;
    int                             quorum = 1, budget = 1;
    std::unordered_set<std::string> assigned; /* verifiers still expected to report                 */
};

/* One active agenda + its private scheduling state. Only the worker pool (in Impl) is shared. */
struct AgendaCtx {
    Agenda                                       agenda;
    std::unordered_map<std::string, VerifyState> verifying;          /* per-task tally (this agenda) */
    bool                                         done_emitted = false; /* terminal verdict emitted once */
};

struct Engine::Impl {
    std::vector<AgendaCtx> agendas;     /* active agendas, in admit order */
    std::size_t            rr_cursor = 0; /* round-robin start for fair cross-agenda dispatch */

    struct PoolEntry {
        std::string role;
        bool        busy = false;
    };
    std::unordered_map<std::string, PoolEntry> pool; /* schedulable workers, SHARED across agendas */

    int max_attempts = 2; /* total dispatches per task before a terminal Fail (1 retry) */

    Engine::ClockFn now;             /* monotonic-ms source; injectable for deterministic tests   */
    uint64_t        deadline_ms = 0; /* per-task wall-clock budget; 0 = deadline disabled          */

    VerifyPolicy default_verify; /* P04: mode None => verification off (the default) */

    AgendaCtx *find_ctx(const std::string &agenda_id)
    {
        for (auto &c : agendas)
            if (c.agenda.id == agenda_id) return &c;
        return nullptr;
    }

    /* ---- pool helpers (shared) ---- */

    std::string find_idle_worker(const std::string &capability)
    {
        for (auto &kv : pool)
            if (!kv.second.busy && role_matches(capability, kv.second.role)) return kv.first;
        /* no exact-role provider is free -> fall back to an idle generalist (graceful degradation, A2) */
        for (auto &kv : pool)
            if (!kv.second.busy && kv.second.role == kGeneralistRole) return kv.first;
        return std::string();
    }

    bool has_provider(const std::string &capability)
    {
        /* an exact-role provider OR a generalist (the catch-all) keeps a task runnable rather than fail_unrunnable */
        for (auto &kv : pool)
            if (role_matches(capability, kv.second.role) || kv.second.role == kGeneralistRole) return true;
        return false;
    }

    void free_worker(const std::string &id)
    {
        auto it = pool.find(id);
        if (it != pool.end()) it->second.busy = false;
    }

    /* ---- per-agenda helpers ---- */

    /* A task whose worker vanished or reported failure: retry (-> Pending) while the budget holds, else
     * fail terminally. The task is currently Assigned or Running. */
    void fail_or_reassign(Task &t)
    {
        if (t.state != TaskState::Assigned && t.state != TaskState::Running) return;
        task_advance(t, t.attempts < max_attempts ? TaskState::Pending : TaskState::Failed);
    }

    /* Fail every still-Pending task in `a` whose dependency can never be satisfied, to a fixpoint. */
    void fail_dep_blocked(Agenda &a)
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto &t : a.tasks)
                if (task_dep_unsatisfiable(a, t) && task_advance(t, TaskState::Failed)) {
                    t.result = "blocked: a dependency failed or is missing";
                    changed = true;
                }
        }
    }

    /* Emit `ctx`'s terminal verdict exactly once, the moment it settles. `done_emitted` is the exclusive
     * guard — whichever path first observes the settled agenda emits and sets it; later calls are no-ops. */
    void emit_verdict_if_settled(AgendaCtx &ctx, std::vector<Intent> &out)
    {
        if (!ctx.done_emitted && agenda_settled(ctx.agenda)) {
            ctx.done_emitted = true;
            out.push_back({agenda_failed(ctx.agenda) ? Intent::AgendaFailed : Intent::AgendaDone,
                           ctx.agenda.id, std::string(), std::string(), std::string()});
        }
    }

    /* Begin a sibling self-check of a just-completed task in `ctx`: pick up to pol.verifiers idle workers
     * that are NOT the author, move the task to Verifying, emit VerifyTask intents. Returns false (caller
     * completes the task normally) if no idle sibling is available. */
    bool start_verification(AgendaCtx &ctx, Task &t, const VerifyPolicy &pol, std::vector<Intent> &out)
    {
        std::vector<std::string> verifiers;
        for (auto &kv : pool) {
            if ((int)verifiers.size() >= pol.verifiers) break;
            if (!kv.second.busy && kv.first != t.assignee) verifiers.push_back(kv.first);
        }
        if (verifiers.empty()) return false; /* nobody independent is free -> accept unverified */
        if (!task_advance(t, TaskState::Verifying)) return false;
        t.assigned_at_ms = now(); /* re-stamp: the verify window starts now */
        VerifyState vs;
        vs.author = t.assignee;
        vs.claim = t.result;
        vs.budget = (int)verifiers.size();
        vs.quorum = pol.quorum < (int)verifiers.size() ? pol.quorum : (int)verifiers.size();
        std::vector<std::string> lenses = assign_lenses((int)verifiers.size());
        for (std::size_t i = 0; i < verifiers.size(); i++) {
            pool[verifiers[i]].busy = true;
            vs.assigned.insert(verifiers[i]);
            out.push_back({Intent::VerifyTask, ctx.agenda.id, verifiers[i], t.id, lenses[i]});
        }
        ctx.verifying[t.id] = std::move(vs);
        return true;
    }

    /* Resolve a Verifying task in `ctx` once its tally is conclusive. A no-op while still Pending. */
    void resolve_verification(AgendaCtx &ctx, const std::string &task_id)
    {
        Task *t = agenda_find(ctx.agenda, task_id);
        auto  it = ctx.verifying.find(task_id);
        if (!t || t->state != TaskState::Verifying || it == ctx.verifying.end()) return;
        VerifyState  &vs = it->second;
        VerifyOutcome o = tally(vs.upholds, vs.refutes, vs.reported, vs.quorum, vs.budget);
        if (o == VerifyOutcome::Pending) return;
        if (o == VerifyOutcome::Refuted) {
            task_advance(*t, TaskState::Failed);
            t->result = "verification refuted: " + vs.refute_grounds; /* grounds for P05b replan */
        } else {                                                      /* Upheld or Unproven */
            task_advance(*t, TaskState::Done);
            if (o == VerifyOutcome::Unproven)
                t->result = vs.claim + "\n[verification inconclusive — not refuted]";
        }
        ctx.verifying.erase(it);
    }

    /* Drive the schedule across ALL agendas. Round-robin: each round, each agenda places at most ONE
     * startable task on an idle role-matching worker; repeat until a full pass places nothing (the pool is
     * saturated or nothing is startable). Then per-agenda dep-fail + terminal-verdict.
     * ORDERING INVARIANT: `n`/`agendas` must not be resized mid-loop. add_agenda/cancel_agenda DISPATCH
     * (so they don't re-enter), and remove_agenda is only ever called by the driver AFTER dispatch() has
     * returned its intents (settle_agenda runs in apply(), which the driver calls after dispatch) — never
     * from inside an event handler. Keep it that way: a mid-loop erase would invalidate the modulo index. */
    std::vector<Intent> dispatch()
    {
        std::vector<Intent> out;
        const std::size_t   n = agendas.size();
        if (n > 0) {
            bool placed_any = true;
            while (placed_any) {
                placed_any = false;
                for (std::size_t k = 0; k < n; k++) {
                    AgendaCtx &ctx = agendas[(rr_cursor + k) % n];
                    for (auto &t : ctx.agenda.tasks) {
                        if (!task_can_start(ctx.agenda, t)) continue;
                        std::string w = find_idle_worker(t.capability);
                        if (w.empty()) continue;
                        task_advance(t, TaskState::Assigned);
                        t.assignee = w;
                        t.assigned_at_ms = now();
                        t.attempts++;
                        pool[w].busy = true;
                        out.push_back({Intent::AssignTask, ctx.agenda.id, w, t.id, std::string()});
                        placed_any = true;
                        break; /* one task per agenda per round (fairness) */
                    }
                }
            }
            rr_cursor = (rr_cursor + 1) % n; /* rotate the starting agenda for the next dispatch */
        }
        for (auto &ctx : agendas) {
            fail_dep_blocked(ctx.agenda); /* a task whose dep can never be Done is failed so we can settle */
            emit_verdict_if_settled(ctx, out);
        }
        return out;
    }

    /* Brick-prevention backstop, per agenda. For each QUIESCENT agenda (nothing of ITS Assigned/Running/
     * Verifying) with Pending tasks left, fail those tasks terminally with a reason so that agenda settles
     * instead of hanging — without disturbing the others. */
    std::vector<Intent> fail_unrunnable()
    {
        std::vector<Intent> out;
        for (auto &ctx : agendas) {
            bool in_flight = false;
            for (const auto &t : ctx.agenda.tasks)
                if (t.state == TaskState::Assigned || t.state == TaskState::Running
                    || t.state == TaskState::Verifying) {
                    in_flight = true;
                    break;
                }
            if (in_flight) continue; /* progress still possible in this agenda — leave it */

            /* On a SHARED pool a Pending task is doomed only if it can NEVER run — NOT merely because its
             * provider is busy on another concurrent agenda (that worker will free up). Build the set of
             * tasks that can EVENTUALLY run by a least-fixpoint: a task is runnable iff a worker provides
             * its capability AND every dependency is Done or itself eventually-runnable. A Pending task left
             * OUT of that set is unreachable — a broken/missing dep, a dependency CYCLE (no member is ever
             * startable), or no provider at all — so it is failed (settling the agenda instead of hanging).
             * A task waiting only on a BUSY provider stays runnable, so it is left to run. */
            std::unordered_set<std::string> runnable;
            bool                            changed = true;
            while (changed) {
                changed = false;
                for (const auto &t : ctx.agenda.tasks) {
                    if (t.state != TaskState::Pending || runnable.count(t.id)) continue;
                    if (!has_provider(t.capability)) continue; /* nothing can ever run it */
                    bool deps_ok = true;
                    for (const auto &d : t.deps) {
                        const Task *dt = agenda_find(ctx.agenda, d);
                        if (!dt || dt->state == TaskState::Failed) {
                            deps_ok = false; /* missing or failed dep — permanently unsatisfiable */
                            break;
                        }
                        if (dt->state == TaskState::Done || runnable.count(d)) continue;
                        deps_ok = false; /* the dep is Pending and not yet known to be runnable */
                        break;
                    }
                    if (deps_ok) {
                        runnable.insert(t.id);
                        changed = true;
                    }
                }
            }
            for (auto &t : ctx.agenda.tasks) {
                if (t.state != TaskState::Pending || runnable.count(t.id)) continue;
                bool dep_broken = task_dep_unsatisfiable(ctx.agenda, t);
                if (!task_advance(t, TaskState::Failed)) continue;
                t.result = dep_broken ? "blocked: a dependency failed or is missing"
                           : !has_provider(t.capability)
                               ? "blocked: no agent provides capability '" + t.capability + "'"
                               : "blocked: unsatisfiable dependency (a cycle or deadlock)";
            }
            emit_verdict_if_settled(ctx, out);
        }
        return out;
    }
};

Engine::Engine(ClockFn now_ms) : p_(new Impl)
{
    p_->now = now_ms ? std::move(now_ms) : ClockFn(monotonic_ms);
}

Engine::~Engine() { delete p_; }

std::vector<Intent> Engine::add_agenda(Agenda agenda)
{
    if (p_->find_ctx(agenda.id)) return {}; /* a duplicate active id — reject, leave state unchanged */
    AgendaCtx ctx;
    ctx.agenda = std::move(agenda);
    p_->agendas.push_back(std::move(ctx));
    return p_->dispatch(); /* the new agenda may immediately schedule onto idle workers */
}

std::vector<Intent> Engine::cancel_agenda(const std::string &agenda_id)
{
    AgendaCtx *ctx = p_->find_ctx(agenda_id);
    if (!ctx) return {};
    /* free every pool worker this agenda occupies (in-flight task assignees + its verifiers), then
     * force-fail its non-terminal tasks so it settles to AgendaFailed. */
    for (const auto &kv : ctx->verifying)
        for (const auto &v : kv.second.assigned) p_->free_worker(v);
    ctx->verifying.clear();
    for (auto &t : ctx->agenda.tasks) {
        if (t.state == TaskState::Assigned || t.state == TaskState::Running
            || t.state == TaskState::Verifying)
            p_->free_worker(t.assignee);
        if (t.state != TaskState::Done && t.state != TaskState::Failed) {
            task_advance(t, TaskState::Failed);
            t.result = "cancelled by operator";
        }
    }
    std::vector<Intent> out;
    p_->emit_verdict_if_settled(*ctx, out); /* AgendaFailed (all terminal now) */
    auto more = p_->dispatch();             /* the freed workers may unblock other agendas */
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

void Engine::remove_agenda(const std::string &agenda_id)
{
    for (auto it = p_->agendas.begin(); it != p_->agendas.end(); ++it)
        if (it->agenda.id == agenda_id) {
            p_->agendas.erase(it);
            /* re-anchor the round-robin cursor into the now-shorter list; the resulting one-slot offset is
             * within the fairness margin (round-robin guarantees no starvation over rounds, not a fixed
             * start slot). */
            if (!p_->agendas.empty()) p_->rr_cursor %= p_->agendas.size();
            else p_->rr_cursor = 0;
            return;
        }
}

std::vector<Intent> Engine::worker_ready(const std::string &id, const std::string &role)
{
    p_->pool[id] = {role, false};
    return p_->dispatch(); /* a new idle worker may unblock pending tasks in any agenda */
}

std::vector<Intent> Engine::worker_lost(const std::string &id)
{
    /* Reassign (or fail) any task this worker held in ANY agenda, settle any verification it owed
     * (fail-closed), then forget the worker so it is never scheduled again (a respawn re-enters via
     * worker_ready). */
    for (auto &ctx : p_->agendas) {
        for (auto &t : ctx.agenda.tasks)
            if (t.assignee == id
                && (t.state == TaskState::Assigned || t.state == TaskState::Running))
                p_->fail_or_reassign(t);
        std::vector<std::string> to_resolve;
        for (auto &kv : ctx.verifying)
            if (kv.second.assigned.erase(id)) {
                kv.second.reported++;
                to_resolve.push_back(kv.first);
            }
        for (const auto &tid : to_resolve) p_->resolve_verification(ctx, tid);
    }
    p_->pool.erase(id);
    return p_->dispatch();
}

std::vector<Intent> Engine::step() { return p_->dispatch(); }

std::vector<Intent> Engine::fail_unrunnable() { return p_->fail_unrunnable(); }

std::vector<Intent> Engine::on_ack(const std::string &agenda_id, const std::string &task_id)
{
    AgendaCtx *ctx = p_->find_ctx(agenda_id);
    if (!ctx) return {};
    Task *t = agenda_find(ctx->agenda, task_id);
    if (t && t->state == TaskState::Assigned) task_advance(*t, TaskState::Running);
    return {}; /* the worker stays busy; nothing new to schedule */
}

std::vector<Intent> Engine::on_result(const std::string &agenda_id, const std::string &task_id, bool ok,
                                      const std::string &payload)
{
    AgendaCtx *ctx = p_->find_ctx(agenda_id);
    if (!ctx) return {};
    Task *t = agenda_find(ctx->agenda, task_id);
    if (!t) return {};
    if (t->state != TaskState::Assigned && t->state != TaskState::Running)
        return p_->dispatch(); /* a stale/duplicate result for a settled task — ignore the payload */
    p_->free_worker(t->assignee);
    if (ok) {
        if (t->state == TaskState::Assigned) task_advance(*t, TaskState::Running); /* no separate ack */
        t->result = payload; /* preserve the result — it is the claim a verifier reviews */
        VerifyPolicy pol = (t->verify.mode != VerifyMode::None) ? t->verify : p_->default_verify;
        std::vector<Intent> out;
        if (pol.mode == VerifyMode::Sibling && p_->start_verification(*ctx, *t, pol, out)) {
            std::vector<Intent> more = p_->dispatch(); /* the freed author may unblock other work */
            out.insert(out.end(), more.begin(), more.end());
            return out; /* the task is now Verifying; the agenda stays open for the verdict */
        }
        task_advance(*t, TaskState::Done); /* verification off, or no idle sibling -> accept as-is */
    } else {
        t->result = payload;
        p_->fail_or_reassign(*t);
    }
    return p_->dispatch(); /* the freed worker + any newly-unblocked deps may enable more work */
}

std::vector<Intent> Engine::on_assign_failed(const std::string &agenda_id, const std::string &task_id)
{
    AgendaCtx *ctx = p_->find_ctx(agenda_id);
    if (!ctx) return {};
    Task *t = agenda_find(ctx->agenda, task_id);
    if (!t) return {};
    p_->free_worker(t->assignee);
    p_->fail_or_reassign(*t);
    return p_->dispatch();
}

void Engine::set_default_verify(VerifyPolicy pol) { p_->default_verify = pol; }

std::vector<Intent> Engine::on_verdict(const std::string &agenda_id, const std::string &task_id,
                                       const std::string &verifier, Verdict v, const std::string &grounds)
{
    AgendaCtx *ctx = p_->find_ctx(agenda_id);
    if (!ctx) return {};
    Task *t = agenda_find(ctx->agenda, task_id);
    auto  it = ctx->verifying.find(task_id);
    if (!t || t->state != TaskState::Verifying || it == ctx->verifying.end())
        return p_->dispatch(); /* the task already resolved (a prior refute, say) — late verdict ignored */
    auto &vs = it->second;
    /* Accept (and free) ONLY a verifier WE asked. A forged/stray "verify:" result from a worker that is
     * NOT in `assigned` must not free that worker — it may be busy on real work (would double-dispatch). */
    if (vs.assigned.erase(verifier) == 0) return p_->dispatch();
    p_->free_worker(verifier); /* a real verifier reported — now free it for more work */
    vs.reported++;
    if (v == Verdict::Uphold) {
        vs.upholds++;
    } else if (v == Verdict::Refute) {
        vs.refutes++;
        if (vs.refute_grounds.empty()) vs.refute_grounds = grounds;
    } /* Uncertain: counts toward the budget but is neither an uphold nor a refute */
    p_->resolve_verification(*ctx, task_id);
    return p_->dispatch();
}

std::vector<std::string> Engine::agenda_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(p_->agendas.size());
    for (const auto &c : p_->agendas) ids.push_back(c.agenda.id);
    return ids;
}

const Agenda *Engine::find_agenda(const std::string &agenda_id) const
{
    for (const auto &c : p_->agendas)
        if (c.agenda.id == agenda_id) return &c.agenda;
    return nullptr;
}

std::size_t Engine::active_count() const { return p_->agendas.size(); }

bool Engine::worker_assignment(const std::string &worker, std::string &agenda_id, std::string &task_id,
                               bool &is_verify) const
{
    /* The driver routes a result by THIS lookup, so its single-valuedness is load-bearing: a worker is
     * marked pool `busy` the instant it is selected (dispatch / start_verification) and only ever selected
     * when !busy, so it holds AT MOST ONE assignment pool-wide. We therefore return the first (and only)
     * match. INVARIANT: never let a worker hold two roles at once (e.g. an author also verifying) — that
     * would make this ambiguous and the cross-agenda routing would rest solely on the body task_id check. */
    for (const auto &ctx : p_->agendas) {
        for (const auto &t : ctx.agenda.tasks)
            if (t.assignee == worker
                && (t.state == TaskState::Assigned || t.state == TaskState::Running)) {
                agenda_id = ctx.agenda.id;
                task_id = t.id;
                is_verify = false;
                return true;
            }
        for (const auto &kv : ctx.verifying)
            if (kv.second.assigned.count(worker)) {
                agenda_id = ctx.agenda.id;
                task_id = kv.first; /* the ORIGINAL task being verified */
                is_verify = true;
                return true;
            }
    }
    return false;
}

void Engine::set_max_attempts(int n) { p_->max_attempts = n < 1 ? 1 : n; }
int  Engine::max_attempts() const { return p_->max_attempts; }

void Engine::set_task_deadline_ms(uint64_t ms) { p_->deadline_ms = ms; }

std::vector<Intent> Engine::check_deadlines()
{
    if (p_->deadline_ms == 0) return {};
    uint64_t                        now_ms = p_->now();
    std::unordered_set<std::string> stuck; /* workers whose in-flight task OR verify window blew the deadline */
    for (auto &ctx : p_->agendas) {
        for (const auto &t : ctx.agenda.tasks) {
            if (now_ms - t.assigned_at_ms < p_->deadline_ms) continue; /* unsigned-safe: stamp is in the past */
            if ((t.state == TaskState::Assigned || t.state == TaskState::Running) && !t.assignee.empty()) {
                stuck.insert(t.assignee);
            } else if (t.state == TaskState::Verifying) {
                /* P04 fail-closed: a verify window overran (a verifier acked then wedged while its process
                 * stayed alive). The window was re-stamped at Verifying entry, so this fires on the
                 * verifiers, not the long-since-freed author. Drop each verifier still owing a verdict. */
                auto it = ctx.verifying.find(t.id);
                if (it != ctx.verifying.end())
                    for (const auto &v : it->second.assigned) stuck.insert(v);
            }
        }
    }
    /* A worker that overran is alive but presumed stuck — treat it exactly like a loss: worker_lost drops
     * it from scheduling, reassigns any real task it held (in any agenda) to a survivor, and settles any
     * verification it owed (fail-closed). */
    std::vector<Intent> out;
    for (const auto &id : stuck) {
        auto r = worker_lost(id);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

} // namespace hc::orch
