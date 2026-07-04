/* test_conductor — Conductor P3b: the threaded component (offline, a stub backend — no LLM / network).
 * The stub echoes the message COUNT it was given ("msgs=<n>"), so a reply doubles as a probe of how many
 * messages the agent actually holds — which lets us prove that resume SEEDED the prior turns into the agent
 * (not merely re-displayed them). Coverage:
 *   - say() -> the conductor thread runs a turn -> the reply appears in the snapshot; busy clears
 *   - the per-session turn budget halts processing + rejects further input
 *   - resume: a new conductor over the same store+session reloads the transcript (display) AND seeds the agent
 *     (the next reply's msgs= count reflects system + seeded + new)
 *   - goal recovery: goals journaled before construction appear in the snapshot
 *   - stop() is idempotent and joins cleanly; concurrent snapshot() readers race the conductor thread (TSan)
 *   - #8: notify_event injects a continuation turn (no operator say()), budgeted APART from the turn budget,
 *     bounded by max_continuations, and re-armed by an operator say()
 * Build under ASan/UBSan AND TSan. Exit non-zero on failure. */

#include "hc_conductor.hpp"

#include "hc_agent.h"
#include "hc_goal.hpp"
#include "hc_store.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hc::conductor;

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

/* A stub turn backend: reply with the message count it saw, finish "stop" (no tool calls). */
static hc_llm_status stub_stream(void *ctx, const hc_llm_message *msgs, size_t n_msgs, const char *tools_json,
                                 const hc_llm_handlers *h, char *fr, size_t cap)
{
    (void)ctx;
    (void)msgs;
    (void)tools_json;
    char buf[48];
    int  len = std::snprintf(buf, sizeof buf, "msgs=%zu", n_msgs);
    if (h && h->on_text && len > 0) h->on_text(buf, (size_t)len, h->user);
    if (fr && cap) {
        std::strncpy(fr, "stop", cap - 1);
        fr[cap - 1] = '\0';
    }
    return HC_LLM_OK;
}

static ConductorParams params(const std::string &wal_dir, hc_store *store, const std::string &session_id,
                              int max_turns)
{
    ConductorParams p;
    p.backend.ctx = nullptr;
    p.backend.chat_stream = stub_stream;
    p.system_prompt = "SYS";
    p.wal_dir = wal_dir;
    p.store = store;
    p.session_id = session_id;
    p.max_turns = max_turns;
    return p;
}

/* Poll snapshot() until `pred` holds or the timeout elapses (the conductor processes turns asynchronously). */
template <class Pred> static bool wait_until(const Conductor &c, Pred pred, int timeout_ms = 3000)
{
    for (int i = 0; i < timeout_ms; i++) {
        if (pred(c.snapshot())) return true;
        usleep(1000);
    }
    return false;
}

static void rm_rf(const std::string &path)
{
    if (DIR *d = opendir(path.c_str())) {
        for (struct dirent *e = readdir(d); e; e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            rm_rf(path + "/" + n);
        }
        closedir(d);
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

static std::string make_tmpdir(const char *tag)
{
    std::string tmpl = std::string("/tmp/hc_cond_") + tag + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char *d = mkdtemp(buf.data());
    return d ? std::string(d) : std::string();
}

int main(void)
{
    /* 1) say() -> a turn runs on the conductor thread -> the reply lands in the snapshot. */
    {
        auto c = Conductor::create(params("", nullptr, "", 0));
        CHECK(c != nullptr, "conductor created");
        CHECK(c->say("hello"), "say accepted");
        bool got = wait_until(*c, [](const ConductorView &v) {
            return !v.busy && v.conversation.size() == 2;
        });
        CHECK(got, "the turn completed (user + assistant in the conversation)");
        auto v = c->snapshot();
        CHECK(v.conversation.size() == 2 && v.conversation[0].role == "user", "the operator line is recorded");
        CHECK(v.conversation[1].role == "assistant" && v.conversation[1].text == "msgs=2",
              "the assistant reply reflects system + user = 2 messages");
        CHECK(!c->say(""), "an empty line is rejected");
    }

    /* 2) the per-session turn budget halts processing and then rejects further input. */
    {
        auto c = Conductor::create(params("", nullptr, "", 2)); /* max_turns = 2 */
        c->say("a");
        c->say("b");
        CHECK(!c->say("c"), "the 3rd say is rejected up-front (exact budget — no silent drop)");
        bool reached =
            wait_until(*c, [](const ConductorView &v) { return v.turns_used == 2 && v.budget_reached; });
        CHECK(reached, "the budget is reported reached after exactly max_turns turns");
        auto v = c->snapshot();
        CHECK(v.turns_used == 2, "exactly max_turns turns ran");
        CHECK(v.conversation.size() == 4, "only the two budgeted turns produced conversation");
        CHECK(!c->say("d"), "further input is rejected once the budget is reached");
    }

    /* 3) resume: reload transcript (display) AND seed the agent (the reply's msgs= count proves seeding). */
    std::string store_dir = make_tmpdir("store");
    CHECK(!store_dir.empty(), "temp store dir created");
    hc_store   *store = hc_store_open(store_dir.c_str());
    CHECK(store != nullptr, "hc_store opened");
    std::string session_id;
    {
        auto c = Conductor::create(params("", store, "", 0));
        CHECK(c != nullptr, "persistent conductor created");
        c->say("one");
        wait_until(*c, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 2; });
        c->say("two");
        wait_until(*c, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 4; });
        session_id = c->session_id();
        CHECK(!session_id.empty(), "the conductor opened a conversation session");
    } /* destroy -> stop + close session (the transcript persists in the store) */

    {
        auto c = Conductor::create(params("", store, session_id, 0)); /* resume the same session */
        CHECK(c != nullptr, "resumed conductor created");
        auto v0 = c->snapshot();
        CHECK(v0.conversation.size() == 4, "the prior 4 turns are reloaded for display");
        CHECK(v0.notice.find("resumed") != std::string::npos, "a resume notice is set");
        /* the agent now holds system + 4 seeded = 5; a new turn -> 6 -> reply "msgs=6" (proves seeding). */
        c->say("three");
        bool got = wait_until(*c, [](const ConductorView &v) {
            return !v.busy && !v.conversation.empty() && v.conversation.back().text == "msgs=6";
        });
        CHECK(got, "the resumed agent's reply reflects system + 4 seeded + new user = 6 (history was seeded)");
    }
    hc_store_close(store);
    rm_rf(store_dir);

    /* 3b) P1: a FRESH session is titled with the host-supplied new_title (not the hardcoded "conductor"). */
    {
        std::string sd = make_tmpdir("titlestore");
        hc_store   *st = hc_store_open(sd.c_str());
        CHECK(st != nullptr, "title store opened");
        ConductorParams p = params("", st, "", 0);
        p.new_title = "My Chat 2026";
        std::string sid;
        {
            auto c = Conductor::create(std::move(p));
            CHECK(c != nullptr, "titled conductor created");
            sid = c->session_id();
        }
        hc_session_info *infos = nullptr;
        size_t           n = 0;
        bool             titled = false;
        if (hc_store_list(st, &infos, &n) == 0) {
            for (size_t i = 0; i < n; i++)
                if (sid == infos[i].id) {
                    titled = std::string(infos[i].title) == "My Chat 2026";
                    break;
                }
            hc_store_list_free(infos, n);
        }
        CHECK(titled, "the fresh session carries the host-supplied new_title");
        hc_store_close(st);
        rm_rf(sd);
    }

    /* 3c) P2: the switch's core — re-create the conductor on the SAME store with a DIFFERENT session (a "new chat"),
     * then resume the first. Proves the teardown+rebuild cycle + per-session isolation on one store (the host-level
     * switch composes this with the observer re-bind + the svc.conductor swap). */
    {
        std::string sd = make_tmpdir("switchstore");
        hc_store   *st = hc_store_open(sd.c_str());
        CHECK(st != nullptr, "switch store opened");
        std::string s1;
        {
            auto a = Conductor::create(params("", st, "", 0));
            CHECK(a != nullptr, "chat A created");
            a->say("alpha");
            wait_until(*a, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 2; });
            s1 = a->session_id();
        }
        std::string s2;
        {
            auto b = Conductor::create(params("", st, "", 0)); /* a NEW chat (fresh session on the same store) */
            CHECK(b != nullptr, "chat B (new) created");
            s2 = b->session_id();
        }
        CHECK(!s1.empty() && !s2.empty() && s1 != s2, "a new chat gets a DISTINCT session on the same store");
        {
            auto cc = Conductor::create(params("", st, s1, 0)); /* switch back to chat A */
            CHECK(cc != nullptr, "switch back to chat A");
            auto v = cc->snapshot();
            CHECK(v.conversation.size() == 2, "switching back resumes chat A's 2 turns");
        }
        hc_store_close(st);
        rm_rf(sd);
    }

    /* 3d) P2 TSan smoke: the conductor APPENDS (its thread) while a concurrent reader LISTS the same store — the
     * dedicated-store picker fill (host thread) vs the conductor's appends. hc_store_list reads root + fresh
     * atomically-written meta.json files; no shared mutable handle state -> race-free (TSan validates). */
    {
        std::string sd = make_tmpdir("racestore");
        hc_store   *st = hc_store_open(sd.c_str());
        CHECK(st != nullptr, "race store opened");
        auto c = Conductor::create(params("", st, "", 0));
        CHECK(c != nullptr, "race conductor created");
        std::atomic<bool> stop{false};
        std::thread       lister([st, &stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                hc_session_info *infos = nullptr;
                size_t           n = 0;
                if (hc_store_list(st, &infos, &n) == 0) hc_store_list_free(infos, n);
            }
        });
        for (int i = 0; i < 6; i++) {
            c->say("turn");
            wait_until(*c, [i](const ConductorView &v) {
                return !v.busy && v.conversation.size() >= (size_t)(2 * (i + 1));
            });
        }
        stop.store(true, std::memory_order_relaxed);
        lister.join();
        c.reset();
        hc_store_close(st);
        rm_rf(sd);
        CHECK(true, "concurrent list-vs-append completed (TSan checks for a race)");
    }

    /* 3e) P3a: rename the LIVE session via the conductor thread (the SOLE owner of the hc_session) — the host never
     * touches the live session. set_session_title stashes + wakes the loop, which rewrites meta.json. */
    {
        std::string sd = make_tmpdir("renamestore");
        hc_store   *st = hc_store_open(sd.c_str());
        CHECK(st != nullptr, "rename store opened");
        auto        c = Conductor::create(params("", st, "", 0));
        CHECK(c != nullptr, "rename conductor created");
        std::string sid = c->session_id();
        c->set_session_title("My Renamed Chat");
        bool renamed = false;
        for (int i = 0; i < 200 && !renamed; i++) { /* the conductor applies it on a loop wake; poll the store meta */
            hc_session_info *infos = nullptr;
            size_t           n = 0;
            if (hc_store_list(st, &infos, &n) == 0) {
                for (size_t j = 0; j < n; j++)
                    if (sid == infos[j].id && std::string(infos[j].title) == "My Renamed Chat") renamed = true;
                hc_store_list_free(infos, n);
            }
            if (!renamed) usleep(5000);
        }
        CHECK(renamed, "set_session_title renamed the live session (applied on the conductor thread)");
        c.reset();
        hc_store_close(st);
        rm_rf(sd);
    }

    /* 4) goal recovery: a goal journaled before construction appears in the conductor's snapshot. */
    {
        std::string wal_dir = make_tmpdir("wal");
        CHECK(!wal_dir.empty(), "temp wal dir created");
        {
            GoalStore gs(wal_dir);
            auto      g = gs.create("Recovered goal", "persisted before the conductor", "operator", "s");
            CHECK(g.has_value(), "a goal was journaled");
            gs.attach_agenda(g->id, "ag1");
        } /* the goal's WAL stream is left for recovery */

        auto c = Conductor::create(params(wal_dir, nullptr, "", 0));
        CHECK(c != nullptr, "conductor created over the populated wal dir");
        auto v = c->snapshot();
        CHECK(v.goals.size() == 1, "the incomplete goal was recovered into the snapshot");
        CHECK(v.goals.size() == 1 && v.goals[0].title == "Recovered goal", "the recovered goal's data is intact");
        CHECK(v.goals.size() == 1 && v.goals[0].agenda_ids.size() == 1, "its attached agenda survived");
        c.reset(); /* stop + join */
        rm_rf(wal_dir);
    }

    /* 5) stop() is idempotent + joins cleanly; concurrent snapshot() readers race the conductor (TSan). */
    {
        auto              c = Conductor::create(params("", nullptr, "", 0));
        std::atomic<bool> reading{true};
        std::atomic<long> reads{0};
        std::vector<std::thread> readers;
        for (int i = 0; i < 3; i++)
            readers.emplace_back([&] {
                while (reading.load()) {
                    volatile auto v = c->snapshot(); /* race the conductor thread's mutations */
                    (void)v;
                    reads.fetch_add(1);
                }
            });
        for (int i = 0; i < 20; i++) c->say("turn-" + std::to_string(i));
        wait_until(*c, [](const ConductorView &v) { return v.turns_used >= 20; }, 5000);
        reading.store(false);
        for (auto &t : readers) t.join();
        CHECK(reads.load() > 0, "concurrent readers ran against the live conductor");
        CHECK(c->snapshot().turns_used == 20, "all 20 turns were processed");
        c->stop();
        c->stop(); /* idempotent */
    }

    /* 6) #8 continuation: notify_event runs a turn WITHOUT an operator say(); it is budgeted separately from the
     *    operator turn budget (turns_used does not move), bounded by max_continuations, and re-armed by say(). */
    {
        ConductorParams p = params("", nullptr, "", 0); /* unlimited operator turns */
        p.max_continuations = 2;
        auto c = Conductor::create(std::move(p));
        CHECK(c != nullptr, "conductor created");

        CHECK(c->notify_event("[event] agenda one done"), "1st continuation accepted (allowance not spent)");
        CHECK(wait_until(*c, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 2; }),
              "the continuation ran a turn without any operator say()");
        CHECK(c->snapshot().turns_used == 0, "a continuation does NOT consume the operator turn budget");

        CHECK(c->notify_event("[event] agenda two done"), "2nd continuation accepted");
        CHECK(wait_until(*c, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 4; }),
              "the 2nd continuation ran");
        CHECK(!c->notify_event("[event] third"), "the 3rd continuation is rejected (allowance exhausted)");
        CHECK(!c->notify_event(""), "an empty event is rejected");

        CHECK(c->say("operator here"), "operator say accepted");
        CHECK(wait_until(*c, [](const ConductorView &v) { return v.turns_used == 1 && !v.busy; }),
              "the operator turn ran (turns_used = 1)");
        CHECK(c->notify_event("[event] post re-arm"),
              "notify_event is accepted again — an operator turn re-armed the allowance");
        CHECK(wait_until(*c, [](const ConductorView &v) { return !v.busy && v.conversation.size() == 8; }),
              "the re-armed continuation ran (2 continuations + 1 operator + 1 continuation = 8 messages)");
        CHECK(c->snapshot().turns_used == 1, "only the operator turn counted toward turns_used (continuations apart)");
    }

    if (g_fail) {
        std::fprintf(stderr, "test_conductor: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_conductor: all checks passed\n");
    return 0;
}
