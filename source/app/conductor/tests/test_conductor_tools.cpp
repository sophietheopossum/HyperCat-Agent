/* test_conductor_tools — Conductor P4: the control-plane tools (offline; a stub backend that EMITS tool calls
 * + a fake ControlPlane that records them). No LLM / network. Coverage:
 *   - set_goal / update_goal       -> mutate the conductor's own GoalStore (visible in the snapshot)
 *   - plan_goal / run_agenda / agenda_status / steer_or_cancel / recall_memory / write_memory / deep_reason
 *                                  -> route to the right ControlPlane fn with correctly-parsed args; run_agenda
 *                                     attaches the new agenda id to the goal
 *   - ask_user                     -> surfaces the question + BLOCKS until the next say(), which answers it
 *                                     WITHOUT spending the turn budget (TSan-critical)
 *   - graceful degradation         -> an UNSET ControlPlane fn yields an error result, never a crash
 * The stub emits the configured tool when the last history message is a `user` turn (turn start) and stops
 * after the `tool` result. Build under ASan/UBSan AND TSan. Exit non-zero on failure. */

#include "hc_conductor.hpp"

#include "hc_agent.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace hc::conductor;

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

/* A stub that emits a configured tool call at the start of a turn, then stops after the tool result. The
 * `tool`/`args` are mutex-guarded so the test thread can reconfigure between turns race-free. */
struct Stub {
    std::mutex  mu;
    std::string tool;
    std::string args;
};

static hc_llm_status stub_stream(void *ctx, const hc_llm_message *msgs, size_t n, const char *tools_json,
                                 const hc_llm_handlers *h, char *fr, size_t cap)
{
    (void)tools_json;
    Stub       *s = static_cast<Stub *>(ctx);
    const char *last = (n > 0 && msgs[n - 1].role) ? msgs[n - 1].role : "";
    std::string tool, args;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        tool = s->tool;
        args = s->args;
    }
    if (std::strcmp(last, "user") == 0 && !tool.empty()) {
        if (h && h->on_tool_call) h->on_tool_call("call-1", tool.c_str(), args.c_str(), h->user);
        if (fr && cap) std::strncpy(fr, "tool_calls", cap - 1), fr[cap - 1] = '\0';
    } else {
        if (h && h->on_text) h->on_text("ok", 2, h->user);
        if (fr && cap) std::strncpy(fr, "stop", cap - 1), fr[cap - 1] = '\0';
    }
    return HC_LLM_OK;
}

/* The fake ControlPlane's recorder — the conductor thread writes via the lambdas, the test reads under `mu`. */
struct FakeCP {
    std::mutex  mu;
    std::string last_plan, last_run_text, last_run_id, last_status, last_cancel, last_recall, last_write_scope,
        last_write_text, last_reason;
    std::string run_returns = "ag-1";
    std::string last_add_role, last_remove_id; /* W2 P2.4 fleet tools */
    bool        list_called = false;
    /* Phase D: the Music Player "set the mood" tools */
    std::string last_mood_track, last_mood_vibe, last_control_action;
    int         last_control_level = -999;
    bool        list_audio_called = false;
};

static ControlPlane make_fake_cp(FakeCP *f)
{
    ControlPlane cp;
    cp.plan_goal = [f](const std::string &g) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_plan = g;
        return std::string("PLAN: 2 tasks for ") + g;
    };
    cp.run_agenda = [f](const std::string &text, const std::string &id) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_run_text = text;
        f->last_run_id = id;
        return f->run_returns;
    };
    cp.agenda_status = [f](const std::string &id) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_status = id;
        return std::string("running: 1/2 tasks done");
    };
    cp.cancel_agenda = [f](const std::string &id) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_cancel = id;
        return true;
    };
    cp.recall_memory = [f](const std::string &q) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_recall = q;
        return std::string("[retrieved memory] prior decision");
    };
    cp.write_memory = [f](const std::string &scope, const std::string &text) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_write_scope = scope;
        f->last_write_text = text;
        return true;
    };
    cp.deep_reason = [f](const std::string &q) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_reason = q;
        return std::string("[answer] 42");
    };
    cp.add_worker = [f](const std::string &role) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_add_role = role;
        return std::string("agent:A");
    };
    cp.remove_worker = [f](const std::string &id) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_remove_id = id;
        return true;
    };
    cp.list_workers = [f]() {
        std::lock_guard<std::mutex> lk(f->mu);
        f->list_called = true;
        return std::string("  agent:A [dev] ready\n");
    };
    cp.list_audio = [f]() {
        std::lock_guard<std::mutex> lk(f->mu);
        f->list_audio_called = true;
        return std::string("[audio library]\n  calm.ogg — Calm (3:00)\n[end audio library]");
    };
    cp.set_mood = [f](const std::string &track, const std::string &vibe) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_mood_track = track;
        f->last_mood_vibe = vibe;
        return std::string("now playing ") + track;
    };
    cp.control_audio = [f](const std::string &action, int level) {
        std::lock_guard<std::mutex> lk(f->mu);
        f->last_control_action = action;
        f->last_control_level = level;
        return std::string("ok: ") + action;
    };
    return cp;
}

static ConductorParams make_params(Stub *stub, const ControlPlane &cp)
{
    ConductorParams p;
    p.backend.ctx = stub;
    p.backend.chat_stream = stub_stream;
    p.system_prompt = "SYS";
    p.control = cp;
    return p;
}

template <class Pred> static bool wait_until(const Conductor &c, Pred pred, int timeout_ms = 3000)
{
    for (int i = 0; i < timeout_ms; i++) {
        if (pred(c.snapshot())) return true;
        usleep(1000);
    }
    return false;
}

static void set_stub(Stub &s, const std::string &tool, const std::string &args)
{
    std::lock_guard<std::mutex> lk(s.mu);
    s.tool = tool;
    s.args = args;
}

/* Run one configured tool turn to completion (turns_used advances by 1). */
static bool run_one(Conductor &c, int expect_turns)
{
    c.say("go");
    return wait_until(c, [&](const ConductorView &v) { return !v.busy && v.turns_used == expect_turns; });
}

int main(void)
{
    /* 1) set_goal mutates the conductor's own GoalStore. */
    {
        Stub stub;
        set_stub(stub, "set_goal", "{\"title\":\"Ship\",\"text\":\"make it shippable\"}");
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        CHECK(c != nullptr, "conductor created");
        CHECK(run_one(*c, 1), "set_goal turn completed");
        auto v = c->snapshot();
        CHECK(v.goals.size() == 1, "set_goal created a goal");
        CHECK(v.goals.size() == 1 && v.goals[0].title == "Ship", "the goal title is recorded");

        /* 2) update_goal transitions it (reconfigure the stub for a second turn). */
        std::string gid = v.goals.empty() ? "" : v.goals[0].id;
        set_stub(stub, "update_goal", "{\"goal_id\":\"" + gid + "\",\"status\":\"paused\"}");
        CHECK(run_one(*c, 2), "update_goal turn completed");
        v = c->snapshot();
        CHECK(v.goals.size() == 1 && v.goals[0].status == GoalStatus::Paused, "update_goal set Paused");
    }

    /* 3) plan_goal routes to the ControlPlane with the parsed goal. */
    {
        Stub stub;
        set_stub(stub, "plan_goal", "{\"goal\":\"build the widget\"}");
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        CHECK(run_one(*c, 1), "plan_goal turn completed");
        std::lock_guard<std::mutex> lk(fake.mu);
        CHECK(fake.last_plan == "build the widget", "plan_goal called the ControlPlane with the goal text");
    }

    /* 4) run_agenda: set a goal, then dispatch — the ControlPlane gets (goal text, id) and the agenda attaches. */
    {
        Stub stub;
        set_stub(stub, "set_goal", "{\"title\":\"T\",\"text\":\"the work\"}");
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        CHECK(run_one(*c, 1), "set_goal turn completed");
        std::string gid = c->snapshot().goals.at(0).id;
        set_stub(stub, "run_agenda", "{\"goal_id\":\"" + gid + "\"}");
        CHECK(run_one(*c, 2), "run_agenda turn completed");
        {
            std::lock_guard<std::mutex> lk(fake.mu);
            CHECK(fake.last_run_text == "the work", "run_agenda passed the goal text to the ControlPlane");
            CHECK(fake.last_run_id == gid, "run_agenda passed the goal id");
        }
        auto v = c->snapshot();
        CHECK(v.goals.size() == 1 && v.goals[0].agenda_ids.size() == 1 && v.goals[0].agenda_ids[0] == "ag-1",
              "the dispatched agenda id is attached to the goal");
    }

    /* 5) the read/act ControlPlane tools route with parsed args. */
    {
        Stub   stub;
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        set_stub(stub, "agenda_status", "{\"agenda_id\":\"ag-7\"}");
        CHECK(run_one(*c, 1), "agenda_status turn");
        set_stub(stub, "steer_or_cancel", "{\"agenda_id\":\"ag-7\"}");
        CHECK(run_one(*c, 2), "steer_or_cancel turn");
        set_stub(stub, "recall_memory", "{\"query\":\"what did we decide\"}");
        CHECK(run_one(*c, 3), "recall_memory turn");
        set_stub(stub, "deep_reason", "{\"query\":\"is X sound\"}");
        CHECK(run_one(*c, 4), "deep_reason turn");
        set_stub(stub, "write_memory", "{\"text\":\"a durable note\",\"scope\":\"shared\"}");
        CHECK(run_one(*c, 5), "write_memory turn");
        std::lock_guard<std::mutex> lk(fake.mu);
        CHECK(fake.last_status == "ag-7", "agenda_status got the id");
        CHECK(fake.last_cancel == "ag-7", "steer_or_cancel got the id");
        CHECK(fake.last_recall == "what did we decide", "recall_memory got the query");
        CHECK(fake.last_reason == "is X sound", "deep_reason got the query");
        CHECK(fake.last_write_scope == "shared" && fake.last_write_text == "a durable note",
              "write_memory got the scope + text");
    }

    /* 5b) W2 P2.4 fleet tools route with parsed args (add_worker/remove_worker/list_workers). */
    {
        Stub   stub;
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        set_stub(stub, "add_worker", "{\"role\":\"dev\"}");
        CHECK(run_one(*c, 1), "add_worker turn");
        set_stub(stub, "remove_worker", "{\"id\":\"agent:A\"}");
        CHECK(run_one(*c, 2), "remove_worker turn");
        set_stub(stub, "list_workers", "{}");
        CHECK(run_one(*c, 3), "list_workers turn");
        std::lock_guard<std::mutex> lk(fake.mu);
        CHECK(fake.last_add_role == "dev", "add_worker got the role");
        CHECK(fake.last_remove_id == "agent:A", "remove_worker got the id");
        CHECK(fake.list_called, "list_workers was called");
    }

    /* 5c) Phase D Music Player tools route with parsed args (list_audio / set_mood / control_audio). */
    {
        Stub   stub;
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        set_stub(stub, "list_audio", "{}");
        CHECK(run_one(*c, 1), "list_audio turn");
        set_stub(stub, "set_mood", "{\"track\":\"calm.ogg\",\"vibe\":\"focused\"}");
        CHECK(run_one(*c, 2), "set_mood turn");
        set_stub(stub, "control_audio", "{\"action\":\"volume\",\"level\":40}");
        CHECK(run_one(*c, 3), "control_audio turn");
        std::lock_guard<std::mutex> lk(fake.mu);
        CHECK(fake.list_audio_called, "list_audio was called");
        CHECK(fake.last_mood_track == "calm.ogg", "set_mood got the track");
        CHECK(fake.last_mood_vibe == "focused", "set_mood got the vibe");
        CHECK(fake.last_control_action == "volume" && fake.last_control_level == 40,
              "control_audio parsed action + the integer level");
    }

    /* 6) ask_user surfaces the question, BLOCKS, and the answering say() bypasses the turn budget. */
    {
        Stub stub;
        set_stub(stub, "ask_user", "{\"question\":\"which env?\"}");
        FakeCP fake;
        auto   c = Conductor::create(make_params(&stub, make_fake_cp(&fake)));
        c->say("go");
        /* the conductor pushes the question and blocks (busy, awaiting the answer) */
        bool asked = wait_until(*c, [](const ConductorView &v) {
            return v.busy && v.conversation.size() >= 2 && v.conversation.back().role == "assistant"
                   && v.conversation.back().text == "which env?";
        });
        CHECK(asked, "ask_user surfaced the question and is blocking");
        CHECK(c->say("staging"), "the operator's answer is accepted");
        bool done = wait_until(*c, [](const ConductorView &v) { return !v.busy && v.turns_used == 1; });
        CHECK(done, "the turn completed after the answer");
        auto v = c->snapshot();
        CHECK(v.conversation.size() == 4, "conversation = go / question / answer / final");
        CHECK(v.conversation.size() == 4 && v.conversation[2].role == "user"
                  && v.conversation[2].text == "staging",
              "the operator's answer is recorded as a user turn");
        CHECK(v.turns_used == 1, "the ask_user answer did not count as a second turn (budget not spent)");
    }

    /* 7) graceful degradation: an UNSET ControlPlane fn yields an error result, never a crash. */
    {
        Stub stub;
        set_stub(stub, "plan_goal", "{\"goal\":\"x\"}");
        ControlPlane empty; /* no fns wired */
        auto         c = Conductor::create(make_params(&stub, empty));
        CHECK(run_one(*c, 1), "the turn completes even with an unwired ControlPlane (no crash)");
        /* the Phase D audio tools also degrade gracefully when unwired (the mood feature off / no host) */
        set_stub(stub, "set_mood", "{\"track\":\"x.ogg\"}");
        CHECK(run_one(*c, 2), "set_mood with an unwired ControlPlane completes (no crash)");
        set_stub(stub, "list_audio", "{}");
        CHECK(run_one(*c, 3), "list_audio with an unwired ControlPlane completes (no crash)");
    }

    if (g_fail) {
        std::fprintf(stderr, "test_conductor_tools: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_conductor_tools: all checks passed\n");
    return 0;
}
