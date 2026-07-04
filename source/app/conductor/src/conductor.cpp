/* conductor.cpp — the Conductor component: the conversational front-door agent on its own thread. See
 * hc_conductor.hpp.
 *
 * Threading model (the host_bridge discipline, TSan-critical): ONE conductor thread runs the blocking
 * hc_agent_run; the host thread only calls say() (enqueue input) and snapshot() (copy state). ONE mutex `mu`
 * guards ALL cross-thread state (the inbox, the snapshot fields, the stopping flag). The mutex is NEVER held
 * across hc_agent_run — streamed assistant text is merged into the snapshot under brief locks from the agent's
 * on_text observer (which runs on the conductor thread). The agent, the GoalStore, and the hc_store session are
 * touched ONLY by the conductor thread (the host never reaches them — it sees a copied snapshot). stop() flips
 * `stopping` under the lock + wakes the loop, which exits at the next turn boundary, then joins. cancel_turn()
 * is the lighter mid-turn stop: it sets the per-run hc_agent_cancel flag (a single volatile word) the conductor
 * thread polls inside hc_agent_run, aborting just the in-flight turn while the loop lives on. new_from_session
 * resume is composed in create():
 * recover the goal WAL + reload the stored conversation into the fresh agent via hc_agent_seed_message. */

#include "hc_conductor.hpp"

#include "hc_conductor_tools.hpp"
#include "hc_store.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hc::conductor {

namespace {
constexpr std::size_t kMaxInbox        = 64;          /* bound the operator-input backlog                       */
constexpr std::size_t kMaxEvents       = 64;          /* #8: bound the pending continuation-event backlog        */
constexpr std::size_t kMaxEventBytes   = 8192;        /* #8: per-event text cap (defensive; the host frames ~250) */
constexpr std::size_t kConversationTail = 256;        /* bound the in-snapshot conversation (full log in store) */
constexpr std::size_t kMaxStreaming    = 256u * 1024; /* bound the in-progress assistant text held in memory    */
constexpr std::size_t kMaxResumeSeed   = 4096;        /* bound resume seeding (keep the most-recent N turns)    */

/* Wall-clock ms since the epoch — the per-turn display timestamp (D). NOT fed to the model and NOT part of the
 * agent's replayable history, so reading the clock here doesn't perturb determinism; it only labels chat turns. */
std::uint64_t now_ms()
{
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* D4c: one third-party proxy's closure — the host seam + the function name + its sensitivity. Held for the
 * agent's lifetime in Impl (the registered hc_agent_tool's `user` points here). */
struct TpProxyCtx {
    const ControlPlane *cp = nullptr;
    std::string         name;
    bool                sensitive = false;
};

/* The proxy tool body: route the call through the host's invoke_third_party seam (which does the gated bus
 * round-trip to the ToolHost). Untrusted model args pass straight through as the tool's `args` JSON. */
char *tp_proxy_invoke(const char *args_json, void *user)
{
    auto       *c = static_cast<TpProxyCtx *>(user);
    std::string out;
    if (c && c->cp && c->cp->invoke_third_party)
        out = c->cp->invoke_third_party(c->name, args_json && args_json[0] ? args_json : "{}", c->sensitive);
    else
        out = "error: third-party tools are not available";
    char *r = static_cast<char *>(malloc(out.size() + 1));
    if (r) memcpy(r, out.c_str(), out.size() + 1);
    return r;
}
} // namespace

struct Conductor::Impl {
    /* config (set once in create(), read-only after the thread starts) */
    hc_agent_backend backend{};
    int              max_turns = 0;
    int              max_continuations = 0; /* #8: autonomous follow-up budget (0 => unlimited) */
    ControlPlane     control;

    /* owned / borrowed resources — touched ONLY by the conductor thread */
    hc_agent                  *agent = nullptr;   /* owned */
    std::unique_ptr<GoalStore> goals;             /* owned */
    hc_store                  *store = nullptr;   /* borrowed */
    hc_session                *session = nullptr; /* owned (opened/created) */
    std::string                session_id_str;    /* fixed in create() BEFORE the thread starts -> lock-free read */
    ConductorToolCtx           tool_ctx;          /* the borrowed ctx the registered tools close over (set in create) */
    /* D4c: the opt-in third-party tool proxies — the defs (the registered tools' name/spec point into these) +
     * one closure per proxy (the tools' `user` points into these). Both OUTLIVE the agent (freed with Impl,
     * after the agent in ~Conductor). Empty unless the operator enabled conductor third-party access. */
    std::vector<ThirdPartyToolDef>            tp_defs;
    std::vector<std::unique_ptr<TpProxyCtx>>  tp_ctxs;

    /* threading */
    std::thread             thread;
    mutable std::mutex      mu;
    std::condition_variable cv;
    std::deque<std::string> inbox;        /* operator turns; guarded by mu */
    std::deque<std::string> events;       /* #8: system continuation turns; guarded by mu — kept SEPARATE from inbox so
                                           * an ask_user answer (which pops inbox) is never mistaken for an event */
    std::string             pending_title; /* P3a: a pending RENAME of the live session, applied on the conductor
                                            * thread (the SOLE owner of `session`); guarded by mu, woken via cv */
    bool                    stopping = false;
    hc_agent_cancel         turn_cancel{}; /* per-turn mid-cancel token (E): cancel_turn() sets flag=1 from the host
                                            * thread to abort the in-flight hc_agent_run; process_turn resets it to 0
                                            * before each run. A single volatile word — hc_agent.h's "set from
                                            * anywhere" contract — so read unlocked inside the run, NOT mu-guarded;
                                            * unlike `stopping` it never joins the thread (the loop survives). */
    int                     accepted = 0; /* operator turns admitted via say() (guarded by mu); bounds the budget.
                                           * Invariant: accepted >= turns_used (equal when idle; ahead while a turn
                                           * is in flight). An ask_user answer advances neither counter. */
    int                     continuations_accepted = 0; /* #8: continuation turns admitted (guarded by mu); bounds the
                                                         * allowance. Reset to 0 by an operator say() (re-arm). */
    bool                    awaiting_user = false; /* an ask_user tool is blocking for the next line (guarded by mu) */

    /* snapshot state — guarded by mu */
    std::vector<ConductorTurn> conversation;
    std::string                streaming;
    bool                       busy = false;
    std::string                active_tool;  /* the most recent tool dispatched this turn (P5-S2 cue; guarded by mu) */
    std::vector<Goal>          goals_view;
    int                        turns_used = 0;
    bool                       budget_reached = false;
    std::string                notice;

    void        push_turn(const char *role, const std::string &text, std::uint64_t at_ms = 0); /* caller holds mu */
    void        run_loop();
    void        process_turn(const std::string &line, bool is_continuation);
    bool        wait_for_user(const std::string &question, std::string &out); /* ask_user: surface + block on next line */
    static void on_text_cb(const char *delta, std::size_t n, void *user);     /* the agent's stream observer */
    static void on_tool_cb(const char *name, const char *args, const char *result, void *user); /* tool-dispatch observer */
};

/* Append a completed message to the bounded in-snapshot conversation. The caller holds `mu`. */
void Conductor::Impl::push_turn(const char *role, const std::string &text, std::uint64_t at_ms)
{
    conversation.push_back(ConductorTurn{role, text, at_ms});
    if (conversation.size() > kConversationTail) conversation.erase(conversation.begin());
}

/* ask_user: surface the question into the conversation, then block until the operator's next line (or stop).
 * Runs on the conductor thread (inside the ask_user tool invoke); the answering say() bypasses the budget. */
bool Conductor::Impl::wait_for_user(const std::string &question, std::string &out)
{
    std::unique_lock<std::mutex> lk(mu);
    push_turn("assistant", question, now_ms()); /* the operator sees the question in chat and can answer */
    awaiting_user = true;
    cv.wait(lk, [this] { return stopping || !inbox.empty(); });
    if (stopping) {
        awaiting_user = false;
        return false; /* shutting down -> ask_user reports a clean abort */
    }
    out = std::move(inbox.front());
    inbox.pop_front();
    awaiting_user = false;
    push_turn("user", out, now_ms()); /* record the operator's answer */
    return true;
}

/* The agent's streamed-text observer — runs ON the conductor thread, merges into the snapshot under mu. */
void Conductor::Impl::on_text_cb(const char *delta, std::size_t n, void *user)
{
    auto                       *im = static_cast<Conductor::Impl *>(user);
    std::lock_guard<std::mutex> lk(im->mu);
    if (im->streaming.size() >= kMaxStreaming) return;
    std::size_t room = kMaxStreaming - im->streaming.size();
    im->streaming.append(delta, n < room ? n : room);
}

/* The agent's tool-dispatch observer — fires after each tool call completes. We record the tool NAME as the
 * most-recent-tool signal that drives the chat tool cue (P5-S2). Runs on the conductor thread. */
void Conductor::Impl::on_tool_cb(const char *name, const char *args, const char *result, void *user)
{
    (void)args;
    (void)result;
    auto                       *im = static_cast<Conductor::Impl *>(user);
    std::lock_guard<std::mutex> lk(im->mu);
    im->active_tool = name ? name : "";
}

void Conductor::Impl::process_turn(const std::string &line, bool is_continuation)
{
    {
        std::lock_guard<std::mutex> lk(mu);
        push_turn("user", line, now_ms()); /* a continuation event is host-framed text the model reads as its turn input */
        streaming.clear();
        active_tool.clear();
        busy = true;
    }
    if (session) hc_session_append(session, "user", line.c_str());

    /* The blocking turn — NO mutex held; on_text/on_tool merge under brief locks. Clear the cancel flag first so a
     * stop aimed at an EARLIER turn cannot bleed into this one (the reset is the conductor thread's; cancel_turn()
     * is the host thread's — a single volatile word, benign by the hc_agent cancel contract). */
    turn_cancel.flag = 0;
    hc_agent_observer obs{};
    obs.on_text = &Impl::on_text_cb;
    obs.on_tool = &Impl::on_tool_cb;
    obs.user = this;
    const hc_agent_status st = hc_agent_run(agent, line.c_str(), &obs, &turn_cancel);

    const char *final_text = hc_agent_last_text(agent);
    std::string reply = final_text ? final_text : "";
    std::vector<Goal> gv = goals->list(); /* conductor-thread-only access to the GoalStore */
    {
        std::lock_guard<std::mutex> lk(mu);
        if (st == HC_AGENT_ERR_CANCELLED) {
            /* Mid-turn stop: settle the PARTIAL text the operator actually saw stream this turn — prefer the live
             * `streaming` buffer over hc_agent_last_text, which after a cancel taken BETWEEN tool iterations can
             * still point at the prior turn's assistant message (and would duplicate it). Mark it visibly stopped. */
            reply = streaming;
            reply += reply.empty() ? "*(stopped)*" : "\n\n*(stopped)*";
        }
        push_turn("assistant", reply, now_ms());
        streaming.clear();
        active_tool.clear();
        busy = false;
        if (!is_continuation) turns_used++; /* turns_used counts OPERATOR turns; continuations are budgeted apart */
        goals_view = std::move(gv);
    }
    if (session) hc_session_append(session, "assistant", reply.c_str());
}

void Conductor::Impl::run_loop()
{
    for (;;) {
        std::string line;
        std::string title_to_set;
        bool        is_continuation = false;
        bool        have_turn = false;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [this] { return stopping || !inbox.empty() || !events.empty() || !pending_title.empty(); });
            if (stopping) return;
            if (!pending_title.empty()) { title_to_set = std::move(pending_title); pending_title.clear(); }
            if (!inbox.empty()) { /* operator input takes priority over a queued continuation event */
                line = std::move(inbox.front()); /* admitted within budget by say() -> always run it */
                inbox.pop_front();
                have_turn = true;
            } else if (!events.empty()) {
                line = std::move(events.front()); /* admitted within the continuation allowance by notify_event() */
                events.pop_front();
                is_continuation = true;
                have_turn = true;
            }
        }
        /* P3a: apply a pending RENAME on the conductor thread — the SOLE owner of `session` — so the host never
         * touches the live hc_session (a host-side write would be reverted by the next append's meta refresh). The
         * picker (hc_store_list on the conductor store) reflects the new title once meta.json is rewritten. */
        if (!title_to_set.empty() && session) hc_session_set_title(session, title_to_set.c_str());
        if (have_turn) process_turn(line, is_continuation);
    }
}

Conductor::Conductor() : p_(std::make_unique<Impl>()) {}

Conductor::~Conductor()
{
    stop();                          /* joins the conductor thread first -> no tool invoke runs past here */
    p_->tool_ctx.ask_user = nullptr; /* drop the im-capturing callback BEFORE freeing the agent (robust even if a
                                      * future hc_agent_free ever touched a tool body) */
    if (p_->agent) hc_agent_free(p_->agent);
    if (p_->session) hc_session_free(p_->session);
    /* goals: unique_ptr auto-frees; store is borrowed (not freed here). */
}

std::unique_ptr<Conductor> Conductor::create(ConductorParams params)
{
    if (!params.backend.chat_stream) return nullptr; /* a turn source is required */

    std::unique_ptr<Conductor> c(new Conductor());
    Impl                      *im = c->p_.get();
    im->backend = params.backend;
    im->max_turns = params.max_turns > 0 ? params.max_turns : 0;
    im->max_continuations = params.max_continuations > 0 ? params.max_continuations : 0;
    im->control = params.control;
    im->store = params.store;

    im->agent = hc_agent_new_backend(&im->backend, params.system_prompt.c_str());
    if (!im->agent) return nullptr; /* ~Conductor cleans up (thread not started) */

    /* Resume part 1: recover incomplete goals from the WAL + adopt them. */
    im->goals = std::make_unique<GoalStore>(params.wal_dir);
    im->goals->adopt(GoalStore::recover_incomplete(params.wal_dir));
    im->goals_view = im->goals->list();

    /* Resume part 2: reload (or open) the conversation session; seed the RECENT prior turns into the fresh
     * agent. The seed is bounded to the most-recent kMaxResumeSeed user/assistant messages, so a very long
     * stored transcript cannot re-inflate the agent's history on restart (the P2/P4 compactor is the live
     * bound; this caps the one-time resume spike). The full transcript stays in hc_store regardless. */
    if (im->store) {
        if (!params.session_id.empty()) im->session = hc_session_load(im->store, params.session_id.c_str());
        if (im->session) {
            std::size_t n = hc_session_count(im->session);
            std::size_t ua = 0; /* count user/assistant turns to skip the oldest beyond the cap */
            for (std::size_t i = 0; i < n; i++) {
                const char *role = nullptr;
                const char *content = nullptr;
                if (hc_session_message(im->session, i, &role, &content) && role
                    && (std::strcmp(role, "user") == 0 || std::strcmp(role, "assistant") == 0))
                    ua++;
            }
            std::size_t skip = ua > kMaxResumeSeed ? ua - kMaxResumeSeed : 0;
            std::size_t seen = 0, seeded = 0;
            for (std::size_t i = 0; i < n; i++) {
                const char *role = nullptr;
                const char *content = nullptr;
                if (!hc_session_message(im->session, i, &role, &content) || !role) continue;
                /* Seed ONLY user/assistant turns — the conversation context. Skip "system" (the agent already
                 * holds its prompt) and "tool" (a stored tool result has no tool_call linkage in hc_store, so
                 * re-seeding it bare would orphan it; P3 has no tool turns anyway). */
                if (std::strcmp(role, "user") != 0 && std::strcmp(role, "assistant") != 0) continue;
                if (seen++ < skip) continue; /* drop the oldest beyond the cap, keep the most recent */
                hc_agent_seed_message(im->agent, role, content ? content : "");
                seeded++;
                im->push_turn(role, content ? content : ""); /* pre-thread: no lock needed yet */
            }
            if (seeded)
                im->notice = "resumed " + std::to_string(seeded)
                             + (skip ? " recent message(s) [older truncated]" : " prior message(s)");
        }
        if (!im->session) { /* fresh (new or load failed) — title it with the host-supplied label (a timestamp) */
            const char *t = params.new_title.empty() ? "conductor" : params.new_title.c_str();
            im->session = hc_session_new(im->store, t, "");
        }
        if (im->session) im->session_id_str = hc_session_id(im->session); /* cache BEFORE the thread starts */
    }

    /* Wire the control-plane tools: the ctx borrows the GoalStore + the (host-filled) ControlPlane + an
     * ask_user that blocks on the operator inbox. Registered BEFORE the thread starts -> the tool set is then
     * read-only on the conductor thread. P3 conversation behaviour is unchanged when the model calls no tool. */
    im->tool_ctx.goals = im->goals.get();
    im->tool_ctx.cp = &im->control;
    im->tool_ctx.session_id = im->session_id_str;
    im->tool_ctx.ask_user = [im](const std::string &q, std::string &out) { return im->wait_for_user(q, out); };
    register_conductor_tools(im->agent, &im->tool_ctx);

    /* D4c (opt-in): register a proxy per host-supplied third-party function. Each invoke routes through
     * im->control.invoke_third_party (the gated bus round-trip the host wired). The defs are MOVED into Impl so
     * the registered tools' name/spec_json stay valid for the agent's life; one closure per proxy lives in
     * tp_ctxs. Registered BEFORE the thread starts, so the tool set stays read-only on the conductor thread. */
    im->tp_defs = std::move(params.third_party_tools);
    for (const auto &def : im->tp_defs) {
        if (def.name.empty() || def.spec_json.empty()) continue;
        auto ctx = std::make_unique<TpProxyCtx>();
        ctx->cp = &im->control;
        ctx->name = def.name;
        ctx->sensitive = def.sensitive;
        hc_agent_tool t = {};
        t.name = def.name.c_str();      /* points into im->tp_defs (stable for the agent's life) */
        t.spec_json = def.spec_json.c_str();
        t.invoke = tp_proxy_invoke;
        t.user = ctx.get();
        if (hc_agent_add_tool(im->agent, &t)) im->tp_ctxs.push_back(std::move(ctx));
    }

    im->thread = std::thread([im] { im->run_loop(); });
    return c;
}

bool Conductor::say(const std::string &line)
{
    if (line.empty()) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->stopping) return false;
    if (p_->inbox.size() >= kMaxInbox) return false;
    if (!p_->awaiting_user) {
        /* a NEW turn -> the session budget applies. A line that ANSWERS an in-flight ask_user does not start a
         * turn, so it bypasses the budget (else an exhausted budget could deadlock the waiting tool). */
        if (p_->max_turns > 0 && p_->accepted >= p_->max_turns) {
            p_->budget_reached = true; /* exact: reject here, so an admitted turn always runs (no silent drop) */
            return false;
        }
        p_->accepted++;
        p_->continuations_accepted = 0; /* #8: an operator turn re-arms the autonomous continuation allowance */
    }
    p_->inbox.push_back(line);
    p_->cv.notify_one();
    return true;
}

bool Conductor::notify_event(const std::string &text)
{
    if (text.empty()) return false;
    if (text.size() > kMaxEventBytes) return false; /* defensive: never stream unbounded text into the prompt */
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->stopping) return false;
    if (p_->events.size() >= kMaxEvents) return false;
    /* The allowance bounds an autonomous chain (an agenda settles -> a continuation turn runs another agenda ->
     * it settles -> ...). 0 => unlimited; the host sets a finite default. An operator say() resets the count, so
     * an interactive session keeps continuing while a no-operator loop is capped. */
    if (p_->max_continuations > 0 && p_->continuations_accepted >= p_->max_continuations) return false;
    p_->continuations_accepted++;
    p_->events.push_back(text);
    p_->cv.notify_one();
    return true;
}

ConductorView Conductor::snapshot() const
{
    std::lock_guard<std::mutex> lk(p_->mu);
    ConductorView v;
    v.conversation = p_->conversation;
    v.streaming = p_->streaming;
    v.busy = p_->busy;
    v.active_tool = p_->active_tool;
    v.goals = p_->goals_view;
    v.turns_used = p_->turns_used;
    v.max_turns = p_->max_turns;
    v.budget_reached = p_->budget_reached;
    v.continuations_used = p_->continuations_accepted;
    v.max_continuations = p_->max_continuations;
    v.notice = p_->notice;
    return v;
}

std::string Conductor::session_id() const
{
    /* Read the cached id (set in create() BEFORE the thread starts, never mutated after) — NOT p_->session,
     * which the conductor thread / ~Conductor own; reading the immutable string is race-free without the lock. */
    return p_->session_id_str;
}

void Conductor::set_session_title(const std::string &title)
{
    /* P3a: rename the LIVE conversation. The hc_session is conductor-thread-only, so stash the title + wake the loop
     * to apply it there (run_loop) rather than writing meta.json from the host thread (which the next append would
     * revert). Non-blocking; takes effect within one loop wake. */
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->pending_title = title;
    }
    p_->cv.notify_one();
}

void Conductor::stop()
{
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->stopping = true;
        p_->cv.notify_all();
    }
    if (p_->thread.joinable()) p_->thread.join();
}

void Conductor::cancel_turn()
{
    /* Abort the in-flight turn WITHOUT ending the session (the "stop generating" button): set the per-run cancel
     * flag the conductor thread polls inside hc_agent_run (hc_agent.h's "set from anywhere" contract — a single
     * volatile word, so no lock and no cv wake). The worker observes it at the next stream chunk / tool-iteration
     * boundary, settles the partial reply, and loops back to wait — the thread is NOT joined (that is stop()).
     * Harmless when idle: the next turn clears the flag before it runs, so a late stop is simply ignored. */
    p_->turn_cancel.flag = 1;
}

} // namespace hc::conductor
