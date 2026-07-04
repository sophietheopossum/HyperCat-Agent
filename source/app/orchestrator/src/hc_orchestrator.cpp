/* hc_orchestrator — the driver / public facade. See hc_orchestrator.hpp.
 *
 * ONE driver thread owns ONE multi-agenda Engine and runs a single CONTINUOUS demuxing event loop. Each
 * iteration it: (1) admits any agendas run_agenda queued (decompose, register the shared pool, open a WAL,
 * add_agenda); (2) services any cancel requests; (3) processes one bus frame — an assign ACK (reply), a
 * worker's task.result (req), or a worker DEATH (polled from the borrowed supervisor) — feeding it back
 * into the engine and applying the intents. Bounded waits mean a dead/silent worker never stalls the loop.
 * The owner thread only ever reads copied-out per-agenda snapshots under the mutex; it never touches the
 * engine, so the engine needs no lock of its own (the SOLE-mutator invariant is preserved with ONE thread).
 *
 * Multi-agenda routing (Conductor P1): a task.result is keyed only by the broker-stamped sender, so the
 * driver asks the engine which (agenda,task) that worker is serving (Engine::worker_assignment) — this
 * sidesteps per-agenda task-id collisions (two agendas each with a "t1"). Assign ACKs are keyed by corr
 * (pending_ack / pending_verify carry the agenda id). Each agenda has its OWN WAL + settles independently.
 *
 * P04: a VerifyTask intent rides the EXISTING task.assign (capability "verify", id "verify:<orig>") — no
 * new bus message; a "verify:"-prefixed task.result is parsed and routed to on_verdict.
 *
 * P14: the driver is the SOLE WAL writer (single-writer by construction); it journals EACH agenda's
 * lifecycle (open before its first dispatch, done/failed per transition, settled at its settle) into a
 * per-agenda WAL, and recover_incomplete folds a crashed run's WALs back into resumable Agendas.
 *
 * driver_loop is intentionally monolithic (~130 lines): its demux phases share per-iteration locals
 * (ack_to/ack_corr/ack_result) and a strict ordering (journal-before-ack, then publish, then fire the #8
 * settle observers OFF the lock); extracting helpers would thread that state without reducing real complexity.
 */

#include "hc_orchestrator.hpp"

#include "hc_orch_codec.hpp"
#include "hc_orch_engine.hpp"
#include "hc_orch_wal.hpp"
#include "hc_verify.hpp"

#include "hc_bus.hpp"
#include "hc_supervisor.hpp"
#include "hc_util.h"
#include "hc_wal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace hc {

using namespace hc::orch;

/* Per-task wall-clock budget: a task Assigned/Running longer than this is treated as a stuck-worker loss
 * and reassigned. Kept ABOVE the worker's per-call HTTP cap (kLlmCallTotalMs=120s) so a legitimately long
 * turn completes rather than being reassigned out from under itself. */
constexpr uint64_t kTaskDeadlineMs = 300000; /* 5 minutes */

/* How many settled agendas' final state we retain for snapshot(id)/wait_until_done(id) after they leave
 * the engine — bounded so a long-lived host running many agendas does not accumulate state without limit
 * (FIFO eviction of the oldest settled agenda). */
constexpr std::size_t kRetainSettled = 128;

/* Upper bound on concurrently-LIVE agendas (queued + active). Generous — no legitimate operator workload
 * approaches it — but it bounds a runaway/adversarial caller (a future LLM-driven Conductor calls run_agenda
 * programmatically), mirroring the broker's connection cap. run_agenda returns false past this. */
constexpr std::size_t kMaxLiveAgendas = 256;

struct Orchestrator::Impl {
    std::string id;
    BusClient  *bus = nullptr;
    Supervisor *sup = nullptr; /* borrowed */

    std::atomic<bool>       stopping{false};
    std::atomic<bool>       work_pending{false}; /* a run_agenda/cancel queued work; wake the driver */
    std::mutex              mu;
    std::condition_variable cv_done; /* the owner waits here for an agenda to settle */

    /* shared (mu-guarded) between the owner thread and the driver thread */
    std::deque<std::pair<Agenda, std::vector<std::pair<std::string, std::string>>>> pending_runs;
    std::deque<std::string>                  cancel_reqs;
    Decomposer                               decomposer;
    DeliverableVerifier                      deliverable_verifier; /* W1.3: host file-existence check */
    VerifyPolicy                             verify_policy; /* applied (engine-wide) at each admit */
    SettleObserver                           settle_observer; /* #8: fired on settle (owner sets, driver reads) */
    bool                    settle_firing = false; /* the driver is mid-fire of a settle_observer it captured BEFORE
                                                    * a later unbind; set_settle_observer waits on this so an UNBIND
                                                    * barriers any in-flight fire (the conductor-swap/teardown UAF fix) */
    std::condition_variable settle_done_cv;        /* signalled when a fire completes */
    std::unordered_map<std::string, Agenda>  snaps;         /* per-agenda last-known state (active+settled) */
    std::unordered_map<std::string, Verdict> verdicts;      /* per-agenda verdict (Running until settled)   */
    std::deque<std::string>                  agenda_order;  /* admit order, for list_agendas (active+settled) */
    std::deque<std::string>                  settled_order; /* settle order, for FIFO retention eviction      */
    std::string                              last_agenda_id; /* most-recently-run, for the no-id overloads   */

    /* A dedicated reader thread does BLOCKING recv and queues whole frames; the driver pops with a condvar
     * timeout (keeps any deadline OFF the framed recv). */
    std::deque<Message>     inbox;
    std::mutex              inbox_mu;
    std::condition_variable inbox_cv;
    std::thread             reader;
    std::thread             driver;

    std::string wal_dir; /* set ONCE at create, read-only after — the driver is the sole WAL writer */

    /* driver-thread-only */
    std::unique_ptr<Engine>                                                            engine;
    std::unordered_map<uint64_t, std::pair<std::string, std::string>>                  pending_ack;
    std::unordered_map<uint64_t, std::tuple<std::string, std::string, std::string>>    pending_verify;
    uint64_t                                                                           corr_seq = 1;
    bool                                                                               fatal = false;
    std::unordered_map<std::string, hc_wal *>                                          wals; /* agenda->WAL */
    std::unordered_map<std::string, std::unordered_map<std::string, TaskState>>        journaled;
    std::vector<AgendaSettled> settle_events; /* #8: driver-only buffer; drained + fired per loop iteration */

    void reader_loop();
    bool next_message(Message &out, int timeout_ms);
    void driver_loop();
    void admit_pending();
    void service_cancels();
    void                apply(const std::vector<Intent> &intents);
    void                settle_agenda(const std::string &agenda_id, bool failed);
    std::vector<Intent> poll_liveness(); /* returns intents (does NOT send) so the loop can order them */
    void                publish_active();

    void journal_open(const std::string &agenda_id);
    void journal_transitions();
    void retain_settled(const std::string &agenda_id, const Agenda &final_state, Verdict v);
};

/* ---- P14 journaling (driver-thread only; a no-op when the agenda has no WAL) ---- */

void Orchestrator::Impl::journal_open(const std::string &agenda_id)
{
    auto wit = wals.find(agenda_id);
    if (wit == wals.end() || !wit->second) return;
    const Agenda *a = engine->find_agenda(agenda_id);
    if (!a) return;
    std::string line = wal::rec_open(*a);
    if (!line.empty()) hc_wal_append(wit->second, line.c_str(), line.size());
    auto &j = journaled[agenda_id];
    for (const auto &t : a->tasks)
        if (t.state == TaskState::Done || t.state == TaskState::Failed) j[t.id] = t.state;
}

void Orchestrator::Impl::journal_transitions()
{
    if (wals.empty()) return;
    for (const auto &id : engine->agenda_ids()) {
        auto wit = wals.find(id);
        if (wit == wals.end() || !wit->second) continue;
        const Agenda *a = engine->find_agenda(id);
        if (!a) continue;
        auto &j = journaled[id];
        for (const auto &t : a->tasks) {
            if (t.state != TaskState::Done && t.state != TaskState::Failed) continue;
            auto jt = j.find(t.id);
            if (jt != j.end() && jt->second == t.state) continue; /* already durable */
            std::string line = (t.state == TaskState::Done) ? wal::rec_done(t.id, t.result)
                                                            : wal::rec_failed(t.id, t.result);
            if (!line.empty() && hc_wal_append(wit->second, line.c_str(), line.size()) == 0)
                j[t.id] = t.state;
        }
    }
}

void Orchestrator::Impl::retain_settled(const std::string &agenda_id, const Agenda &final_state, Verdict v)
{
    std::lock_guard<std::mutex> lk(mu);
    snaps[agenda_id] = final_state;
    verdicts[agenda_id] = v;
    settled_order.push_back(agenda_id);
    /* FIFO-evict the oldest settled agenda once over the retention cap. The back-compat "last" agenda is
     * never evicted (the owner's no-id snapshot()/wait reads it) — it is RE-QUEUED to the back instead of
     * dropped, so it still counts toward the cap (the bound holds) and is re-evaluated once `last` rotates.
     * Ids are unique, so at most one entry equals `last_agenda_id`; the loop always evicts a non-last entry
     * and terminates. */
    while (settled_order.size() > kRetainSettled) {
        std::string old = settled_order.front();
        settled_order.pop_front();
        if (old == last_agenda_id) {
            settled_order.push_back(old); /* protect the back-compat agenda, but keep it counted */
            continue;
        }
        snaps.erase(old);
        verdicts.erase(old);
        for (auto it = agenda_order.begin(); it != agenda_order.end(); ++it)
            if (*it == old) {
                agenda_order.erase(it);
                break;
            }
    }
    cv_done.notify_all();
}

/* ---- driver mechanics ---- */

void Orchestrator::Impl::reader_loop()
{
    for (;;) {
        Message m;
        if (!bus->recv(m)) break; /* blocking, no timeout — shutdown()/drop ends it */
        {
            std::lock_guard<std::mutex> lk(inbox_mu);
            inbox.push_back(std::move(m));
        }
        inbox_cv.notify_one();
    }
}

bool Orchestrator::Impl::next_message(Message &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(inbox_mu);
    inbox_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return !inbox.empty() || stopping.load() || work_pending.load(); });
    if (inbox.empty()) return false; /* timed out / stopping / woken for queued work (no frame) */
    out = std::move(inbox.front());
    inbox.pop_front();
    return true;
}

void Orchestrator::Impl::publish_active()
{
    std::lock_guard<std::mutex> lk(mu);
    if (!engine) return;
    for (const auto &id : engine->agenda_ids()) {
        const Agenda *a = engine->find_agenda(id);
        if (a) snaps[id] = *a;
    }
}

void Orchestrator::Impl::admit_pending()
{
    for (;;) {
        Agenda                                          agenda;
        std::vector<std::pair<std::string, std::string>> pool;
        Decomposer                                      decomp;
        VerifyPolicy                                    vpol;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (pending_runs.empty()) return;
            agenda = std::move(pending_runs.front().first);
            pool = std::move(pending_runs.front().second);
            pending_runs.pop_front();
            decomp = decomposer;
            vpol = verify_policy;
        }
        if (agenda.tasks.empty() && decomp) agenda.tasks = decomp(agenda.goal); /* overseer decompose */

        engine->set_default_verify(vpol); /* engine-wide default; applied to results from here on */

        /* register the shared pool (idempotent — re-readying an existing worker just refreshes it) */
        std::vector<Intent> start;
        for (const auto &w : pool) {
            auto r = engine->worker_ready(w.first, w.second);
            start.insert(start.end(), r.begin(), r.end());
        }

        /* open this agenda's WAL BEFORE add_agenda journals its pristine task snapshot */
        const std::string aid = agenda.id;
        if (!wal_dir.empty()) {
            hc_wal *w = hc_wal_open(wal_dir.c_str(), aid.c_str());
            if (w) wals[aid] = w;
        }

        auto added = engine->add_agenda(std::move(agenda));
        journal_open(aid); /* rec_open + mark already-terminal (resumed) tasks journaled */

        apply(start);
        apply(added);
        publish_active();
    }
}

void Orchestrator::Impl::service_cancels()
{
    for (;;) {
        std::string id;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancel_reqs.empty()) return;
            id = std::move(cancel_reqs.front());
            cancel_reqs.pop_front();
        }
        apply(engine->cancel_agenda(id));
    }
}

void Orchestrator::Impl::apply(const std::vector<Intent> &intents)
{
    for (const auto &i : intents) {
        if (i.kind == Intent::AssignTask) {
            const Agenda *a = engine->find_agenda(i.agenda_id);
            const Task   *t = a ? agenda_find(*a, i.task_id) : nullptr;
            if (!t) continue;
            uint64_t corr = ++corr_seq;
            pending_ack[corr] = {i.agenda_id, i.task_id};
            if (!bus->send_request(i.worker_id, corr, codec::assign_body(*t))) {
                pending_ack.erase(corr);
                fatal = true; /* our own bus is gone — settle everything Failed on the way out */
                return;
            }
        } else if (i.kind == Intent::VerifyTask) {
            const Agenda *a = engine->find_agenda(i.agenda_id);
            const Task   *orig = a ? agenda_find(*a, i.task_id) : nullptr;
            if (!orig) continue;
            Task v;
            v.id = "verify:" + orig->id;
            v.title = "verify: " + orig->title;
            v.description = "Another worker produced the result below for the task \"" + orig->title
                            + "\". Independently CHECK it for "
                            + (i.lens.empty() ? std::string("correctness") : i.lens)
                            + " — be a skeptic and look for any real flaw.\n\nResult to verify:\n"
                            + orig->result;
            v.capability = "verify";
            uint64_t corr = ++corr_seq;
            pending_verify[corr] = std::make_tuple(i.agenda_id, orig->id, i.worker_id);
            if (!bus->send_request(i.worker_id, corr, codec::assign_body(v))) {
                pending_verify.erase(corr);
                fatal = true;
                return;
            }
        } else if (i.kind == Intent::AgendaDone) {
            settle_agenda(i.agenda_id, false);
        } else if (i.kind == Intent::AgendaFailed) {
            settle_agenda(i.agenda_id, true);
        }
    }
}

void Orchestrator::Impl::settle_agenda(const std::string &agenda_id, bool failed)
{
    /* journal the settle + drop the WAL so a recovery scan no longer lists it; retain the agenda's final
     * state for the owner's snapshot(id)/wait_until_done(id), then prune it from the engine. */
    const Agenda *a = engine->find_agenda(agenda_id);
    Agenda        final_state = a ? *a : Agenda{};
    auto          wit = wals.find(agenda_id);
    if (wit != wals.end() && wit->second) {
        std::string line = wal::rec_settled(failed);
        if (!line.empty()) hc_wal_append(wit->second, line.c_str(), line.size());
        hc_wal_close(wit->second);
        if (!wal_dir.empty()) hc_wal_remove(wal_dir.c_str(), agenda_id.c_str());
    }
    if (wit != wals.end()) wals.erase(wit);
    journaled.erase(agenda_id);
    retain_settled(agenda_id, final_state, failed ? Verdict::Failed : Verdict::Done);
    /* #8: buffer a settle event; the driver fires the observer for it at the BOTTOM of the loop iteration,
     * OFF the lock (retain_settled holds mu, and the observer may take another component's mutex). This buffer
     * push MUST stay AFTER retain_settled: the observer may call snapshot(id), which must already find the
     * settled final state that retain_settled committed under mu. */
    AgendaTally tly = agenda_tally(final_state);
    std::string summary = std::to_string(tly.done) + "/" + std::to_string(tly.total) + " done";
    if (tly.failed) {
        summary += ", " + std::to_string(tly.failed) + " failed";
        /* A4: name the FIRST failure's reason (e.g. "blocked: no agent provides capability 'qa'") so the conductor's
         * settle-wake can ACT on it — add the missing worker + retry — instead of guessing why dispatch stalled.
         * BOUND the (worker-authored, untrusted) result to a small reason budget: an unbounded result would push the
         * framed [system event] past the conductor's notify_event cap and DROP the wake entirely (defeating A4). The
         * host_conductor settle seam additionally DEFANGS it (collapses newlines + neutralizes fence markers). */
        for (const auto &t : final_state.tasks)
            if (t.state == TaskState::Failed && !t.result.empty()) {
                summary += " — "
                           + (t.result.size() > 512 ? t.result.substr(0, 512) + "\xE2\x80\xA6" : t.result);
                break;
            }
    }
    settle_events.push_back({agenda_id, failed ? Verdict::Failed : Verdict::Done, std::move(summary)});
    engine->remove_agenda(agenda_id);
}

std::vector<Intent> Orchestrator::Impl::poll_liveness()
{
    std::vector<Intent> out;
    if (!sup || !engine) return out;
    std::set<std::string> dead;
    for (const auto &id : engine->agenda_ids()) {
        const Agenda *a = engine->find_agenda(id);
        if (!a) continue;
        for (const auto &t : a->tasks)
            if ((t.state == TaskState::Assigned || t.state == TaskState::Running) && !t.assignee.empty()
                && !sup->is_alive(t.assignee))
                dead.insert(t.assignee);
    }
    for (const auto &w : dead) {
        auto r = engine->worker_lost(w);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

void Orchestrator::Impl::driver_loop()
{
    engine.reset(new Engine());
    int call_cap = hc_getenv_int("HC_LLM_CALL_TOTAL_MS", 120000);
    if (call_cap < 5000) call_cap = 5000;
    else if (call_cap > 600000) call_cap = 600000;
    int deadline = hc_getenv_int("HC_TASK_DEADLINE_MS", (int)kTaskDeadlineMs);
    if (deadline < call_cap) deadline = call_cap;
    engine->set_task_deadline_ms((uint64_t)deadline);

    for (;;) {
        if (stopping.load()) break;
        work_pending.store(false);
        admit_pending();   /* add any agendas run_agenda queued */
        service_cancels(); /* fail any agendas cancel() requested */
        if (fatal) break;  /* the bus died mid-apply — bail; teardown settles the rest */

        Message     m;
        bool        ack_result = false;
        std::string ack_to;
        uint64_t    ack_corr = 0;
        if (next_message(m, 150)) {
            if (m.type == "reply" || m.type == "err") {
                auto vit = pending_verify.find(m.corr);
                if (vit != pending_verify.end()) { /* P04: an ack for a VERIFY assign */
                    std::string aid = std::get<0>(vit->second);
                    std::string orig = std::get<1>(vit->second);
                    std::string verifier = std::get<2>(vit->second);
                    pending_verify.erase(vit);
                    bool ok = (m.type == "reply") && codec::ack_is_ok(m.body);
                    if (!ok) /* undeliverable -> count the verifier lost so the tally resolves */
                        apply(engine->on_verdict(aid, orig, verifier, hc::orch::Verdict::Uncertain, ""));
                } else {
                    auto it = pending_ack.find(m.corr);
                    if (it != pending_ack.end()) {
                        std::string aid = it->second.first, tid = it->second.second;
                        pending_ack.erase(it);
                        bool ok = (m.type == "reply") && codec::ack_is_ok(m.body);
                        apply(ok ? engine->on_ack(aid, tid) : engine->on_assign_failed(aid, tid));
                    }
                }
            } else if (m.type == "req") {
                if (codec::body_cmd(m.body) == "task.result") {
                    codec::TaskResult tr = codec::parse_result(m.body);
                    ack_result = true; /* defer the ack until after journal_transitions (journal-before-ack) */
                    ack_to = m.from;
                    ack_corr = m.corr;
                    /* Route by the broker-stamped sender: the engine knows which (agenda,task) m.from is
                     * serving. This is the integrity gate too — a worker not assigned anything (idle/forged)
                     * has no assignment, so its result is dropped; it cannot complete another's task. */
                    std::string aid, atid;
                    bool        is_verify = false;
                    if (tr.valid && engine->worker_assignment(m.from, aid, atid, is_verify)) {
                        if (is_verify && tr.task_id == "verify:" + atid) {
                            codec::VerdictMsg vm = codec::parse_verdict(tr.payload);
                            apply(engine->on_verdict(aid, atid, m.from, verdict_from_str(vm.verdict),
                                                     vm.grounds));
                        } else if (!is_verify && tr.task_id == atid) {
                            /* W1.3: before accepting a Done, confirm the worker actually materialized its
                             * declared deliverable file. A claimed-success-but-no-file result is converted to
                             * a failure (with a clear reason), so the EXISTING retry/reassign machinery
                             * re-prompts the worker (which usually writes the file on the retry) and the agenda
                             * never reports a false Done. The verifier does the (host-side) filesystem check. */
                            bool        rok = tr.ok;
                            std::string payload = tr.payload;
                            if (rok) {
                                DeliverableVerifier verifier;
                                {
                                    std::lock_guard<std::mutex> lk(mu);
                                    verifier = deliverable_verifier;
                                }
                                if (verifier) {
                                    const Agenda *a = engine->find_agenda(aid);
                                    const Task   *t = a ? agenda_find(*a, atid) : nullptr;
                                    if (t && !t->artifact_path.empty() && !verifier(m.from, t->artifact_path)) {
                                        rok = false;
                                        payload = "claimed complete but the deliverable file '" +
                                                  t->artifact_path + "' is missing or empty in the workspace\n\n"
                                                  "[original claim]\n" +
                                                  tr.payload;
                                    }
                                }
                            }
                            apply(engine->on_result(aid, atid, rok, payload));
                        }
                    }
                } else {
                    bus->send_reply(m.from, m.corr, codec::ack_body(false)); /* unknown cmd */
                }
            }
        }

        apply(poll_liveness());           /* a worker may have died while we waited / msgs flowed */
        apply(engine->check_deadlines()); /* ...or gone silent past its task deadline */
        apply(engine->fail_unrunnable()); /* settle any now-unrunnable agenda (never starve the backstop) */
        journal_transitions();            /* persist Done/Failed BEFORE the ack below */
        if (ack_result) bus->send_reply(ack_to, ack_corr, codec::ack_body(true)); /* result now durable */
        publish_active();
        /* #8: fire the settle observer for agendas that settled this iteration — OFF the lock. The observer is
         * copied under a brief lock then invoked unlocked, so it may safely take another component's mutex; it
         * fires exactly once per settle and never re-enters the engine. */
        if (!settle_events.empty()) {
            SettleObserver obs;
            {
                std::lock_guard<std::mutex> lk(mu);
                obs = settle_observer;
                settle_firing = (bool)obs; /* mark in-flight so a concurrent unbind barriers on this fire */
            }
            if (obs)
                for (const auto &e : settle_events) obs(e);
            if (obs) {
                std::lock_guard<std::mutex> lk(mu);
                settle_firing = false;
                settle_done_cv.notify_all(); /* release a set_settle_observer() unbinding before freeing the target */
            }
            settle_events.clear();
        }
        if (fatal) break;
    }

    /* teardown: close any still-open WALs (their agendas were left INCOMPLETE — leave the files for
     * recovery, just release the handles). A genuine settle already removed its WAL above. */
    for (auto &kv : wals)
        if (kv.second) hc_wal_close(kv.second);
    wals.clear();

    {
        std::lock_guard<std::mutex> lk(mu);
        /* on a fatal bus loss, mark every still-active agenda Failed so a waiter is not left hanging */
        if (fatal && engine)
            for (const auto &id : engine->agenda_ids()) {
                if (verdicts.count(id) && verdicts[id] != Verdict::Running) continue;
                const Agenda *a = engine->find_agenda(id);
                if (a) snaps[id] = *a;
                verdicts[id] = Verdict::Failed;
            }
        cv_done.notify_all();
    }
}

Orchestrator::Orchestrator() : p_(new Impl) {}

Orchestrator *Orchestrator::create(const std::string &sock_path, const std::string &id, Supervisor *sup,
                                   const std::string &wal_dir)
{
    BusClient *bus = BusClient::connect(sock_path.c_str(), id);
    if (!bus) return nullptr;
    std::unique_ptr<Orchestrator> o(new Orchestrator());
    o->p_->id = id;
    o->p_->bus = bus;
    o->p_->sup = sup;
    o->p_->wal_dir = wal_dir; /* set BEFORE the driver starts -> read-only on the driver, no lock */
    Impl *p = o->p_;
    p->reader = std::thread([p] { p->reader_loop(); });
    p->driver = std::thread([p] { p->driver_loop(); });
    return o.release();
}

std::vector<hc::orch::Agenda> Orchestrator::recover_incomplete(const std::string &wal_dir)
{
    std::vector<hc::orch::Agenda> out;
    if (wal_dir.empty()) return out;

    std::vector<std::string> ids;
    hc_wal_list(
        wal_dir.c_str(),
        [](const char *id, void *u) {
            auto *v = static_cast<std::vector<std::string> *>(u);
            v->emplace_back(id);
            return 0;
        },
        &ids);

    for (const auto &sid : ids) {
        std::vector<std::string> lines;
        hc_wal_replay(
            wal_dir.c_str(), sid.c_str(),
            [](const char *line, std::size_t len, void *u) {
                auto *v = static_cast<std::vector<std::string> *>(u);
                v->emplace_back(line, len);
                return 0;
            },
            &lines);
        wal::Folded f = wal::fold(lines);
        if (!f.valid || f.settled || agenda_settled(f.agenda)) {
            hc_wal_remove(wal_dir.c_str(), sid.c_str());
            continue;
        }
        out.push_back(std::move(f.agenda));
    }
    return out;
}

Orchestrator::~Orchestrator()
{
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->stopping = true;
    }
    p_->work_pending.store(true);
    p_->inbox_cv.notify_all();        /* wake the driver if waiting on the inbox */
    if (p_->bus) p_->bus->shutdown(); /* unblock the reader's blocking recv */
    if (p_->reader.joinable()) p_->reader.join();
    if (p_->driver.joinable()) p_->driver.join();
    delete p_->bus; /* both threads joined; nobody else touches the bus */
    delete p_;
}

bool Orchestrator::run_agenda(const hc::orch::Agenda &agenda,
                              const std::vector<std::pair<std::string, std::string>> &pool)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    if (agenda.id.empty()) return false; /* a concurrent agenda needs a routable id */
    /* reject a duplicate id that is still ACTIVE (a settled/retained id may be re-run) */
    auto vit = p_->verdicts.find(agenda.id);
    if (vit != p_->verdicts.end() && vit->second == Verdict::Running) return false;
    for (const auto &pr : p_->pending_runs)
        if (pr.first.id == agenda.id) return false; /* already queued */
    /* bound concurrently-live (queued + active) agendas against a runaway caller */
    std::size_t live = 0;
    for (const auto &kv : p_->verdicts)
        if (kv.second == Verdict::Running) live++;
    if (live >= kMaxLiveAgendas) return false;

    p_->pending_runs.push_back({agenda, pool});
    p_->snaps[agenda.id] = agenda;
    p_->verdicts[agenda.id] = Verdict::Running;
    p_->last_agenda_id = agenda.id;
    bool known = false;
    for (const auto &x : p_->agenda_order)
        if (x == agenda.id) known = true;
    if (!known) p_->agenda_order.push_back(agenda.id);
    p_->work_pending.store(true);
    p_->inbox_cv.notify_all();
    return true;
}

Orchestrator::Verdict Orchestrator::wait_until_done(const std::string &agenda_id, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(p_->mu);
    auto settled = [&] {
        auto it = p_->verdicts.find(agenda_id);
        return p_->stopping.load() || (it != p_->verdicts.end() && it->second != Verdict::Running)
               || it == p_->verdicts.end();
    };
    p_->cv_done.wait_for(lk, std::chrono::milliseconds(timeout_ms), settled);
    auto it = p_->verdicts.find(agenda_id);
    if (it == p_->verdicts.end()) return Verdict::Running; /* unknown id */
    return it->second;
}

Orchestrator::Verdict Orchestrator::wait_until_done(int timeout_ms)
{
    std::string id;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        id = p_->last_agenda_id;
    }
    if (id.empty()) return Verdict::Running;
    return wait_until_done(id, timeout_ms);
}

int Orchestrator::progress(const std::string &agenda_id)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->snaps.find(agenda_id);
    return it == p_->snaps.end() ? 0 : agenda_progress(it->second);
}

int Orchestrator::progress()
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->snaps.find(p_->last_agenda_id);
    return it == p_->snaps.end() ? 0 : agenda_progress(it->second);
}

hc::orch::Agenda Orchestrator::snapshot(const std::string &agenda_id)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->snaps.find(agenda_id);
    return it == p_->snaps.end() ? Agenda{} : it->second;
}

hc::orch::Agenda Orchestrator::snapshot()
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->snaps.find(p_->last_agenda_id);
    return it == p_->snaps.end() ? Agenda{} : it->second;
}

std::vector<std::string> Orchestrator::list_agendas()
{
    std::lock_guard<std::mutex> lk(p_->mu);
    return std::vector<std::string>(p_->agenda_order.begin(), p_->agenda_order.end());
}

bool Orchestrator::cancel(const std::string &agenda_id)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->verdicts.find(agenda_id);
    if (it == p_->verdicts.end() || it->second != Verdict::Running) return false; /* not active */
    p_->cancel_reqs.push_back(agenda_id);
    p_->work_pending.store(true);
    p_->inbox_cv.notify_all();
    return true;
}

void Orchestrator::set_decomposer(Decomposer d)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->decomposer = std::move(d);
}

void Orchestrator::set_deliverable_verifier(DeliverableVerifier v)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->deliverable_verifier = std::move(v);
}

void Orchestrator::set_verify(hc::orch::VerifyPolicy pol)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->verify_policy = pol;
}

void Orchestrator::set_settle_observer(SettleObserver obs)
{
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->settle_observer = std::move(obs);
    /* BARRIER: if the driver is mid-fire (it captured the PREVIOUS observer before this swap), wait for that fire to
     * finish. So a caller that UNBINDS (sets null) before freeing what the observer captured — a conductor swap or
     * teardown — is guaranteed no in-flight callback dereferences the freed object. The fire is a non-blocking
     * enqueue (notify_event), so this wait is brief; it never re-enters the engine, so no deadlock. */
    while (p_->settle_firing) p_->settle_done_cv.wait(lk);
}

} // namespace hc
