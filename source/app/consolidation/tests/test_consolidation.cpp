/* test_consolidation — the P6 gate (OFFLINE, scripted backend): compaction appends an episode summary
 * beside the raw transcript (raw preserved); distillation emits provenance-tagged facts through the sink
 * with the budget cap holding; a below-threshold session is a no-op. No key, no network. */

#include "consolidation.hpp"

#include "hc_fs.h"
#include "hc_store.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                                 \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

/* a scripted "model": emit a fixed reply as one text delta, then finish */
struct FakeCtx {
    const char *reply;
};
static hc_llm_status fake_chat(void *vctx, const hc_llm_message *, std::size_t, const char *,
                               const hc_llm_handlers *h, char *fr, std::size_t frc)
{
    FakeCtx *c = static_cast<FakeCtx *>(vctx);
    if (h && h->on_text && c->reply) h->on_text(c->reply, std::strlen(c->reply), h->user);
    if (fr && frc) std::snprintf(fr, frc, "stop");
    return HC_LLM_OK;
}

/* create a session with `n` user/assistant messages; returns its id */
static std::string make_session(hc_store *store, size_t n)
{
    hc_session *s = hc_session_new(store, "test session", "test-model");
    for (size_t i = 0; i < n; i++)
        hc_session_append(s, (i % 2 == 0) ? "user" : "assistant", "message body line");
    hc_session_save_meta(s);
    std::string id = hc_session_id(s);
    hc_session_free(s);
    return id;
}

int main()
{
    using namespace hc::consolidation;

    char root[] = "/tmp/hc_consol_test_XXXXXX";
    CHECK(mkdtemp(root) != nullptr, "mkdtemp store root");
    hc_store *store = hc_store_open(root);
    CHECK(store != nullptr, "open store");
    if (!store) return 1;

    /* --- COMPACTION: a session over threshold gets an episode summary appended BESIDE the raw turns --- */
    {
        std::string      id = make_session(store, 8); /* > min_messages (6) */
        FakeCtx          fc{"The session reversed a string and added a prime check; build is cmake."};
        hc_agent_backend be = {&fc, fake_chat};
        CompactResult    r = compact_session(store, be, id);
        CHECK(r.compacted && r.error.empty(), "compaction ran");
        CHECK(r.summary == fc.reply, "summary is the model's output");

        /* the episode summary is beside the transcript; the raw transcript is UNTOUCHED */
        std::string base = std::string(root) + "/" + id;
        size_t      elen = 0, tlen = 0;
        char       *ep = hc_fs_read_file((base + "/episodes.jsonl").c_str(), 1u << 20, &elen);
        char       *tx = hc_fs_read_file((base + "/transcript.jsonl").c_str(), 1u << 20, &tlen);
        CHECK(ep && std::strstr(ep, "episode") && std::strstr(ep, "cmake"), "episodes.jsonl has the summary");
        CHECK(tx != nullptr, "raw transcript still present (ground truth preserved)");
        /* the transcript must contain only the original 8 messages, no summary line mixed in */
        CHECK(tx && !std::strstr(tx, "episode"), "the summary was NOT written into the raw transcript");
        free(ep);
        free(tx);
    }

    /* --- below threshold: a short session is a no-op (compacted=false, no error, no episodes file) --- */
    {
        std::string      id = make_session(store, 4); /* <= min_messages */
        FakeCtx          fc{"should not be called"};
        hc_agent_backend be = {&fc, fake_chat};
        CompactResult    r = compact_session(store, be, id);
        CHECK(!r.compacted && r.error.empty(), "short session is a no-op");
        size_t elen = 0;
        char  *ep = hc_fs_read_file((std::string(root) + "/" + id + "/episodes.jsonl").c_str(), 1u << 20, &elen);
        CHECK(ep == nullptr, "no episode file written below threshold");
        free(ep);
    }

    /* --- DISTILLATION: facts flow to the sink with provenance; the budget cap holds; NONE is skipped --- */
    {
        std::string                                       id = make_session(store, 6);
        std::vector<std::array<std::string, 3>>           writes; /* (scope, text, source) */
        WriteFn sink = [&](const std::string &scope, const std::string &text, const std::string &source) {
            writes.push_back({scope, text, source});
            return true;
        };
        FakeCtx          fc{"Tabs over spaces is the house style.\nThe build command is cmake --build build.\nNONE\nThe QA lead is Sam.\nExtra fact beyond budget."};
        hc_agent_backend be = {&fc, fake_chat};
        DistillOptions   opts; /* default scope "distilled" (quarantine), max_facts 5 */
        opts.max_facts = 3;    /* BUDGET: at most 3 facts */
        DistillResult    r = distill_session(store, be, sink, id, opts);
        CHECK(r.ok, "distill ran");
        CHECK(r.written == 3 && writes.size() == 3, "budget cap holds (3 of the available facts)");
        CHECK(writes.size() == 3 && writes[0][1] == "Tabs over spaces is the house style.",
              "the first fact parsed correctly (list markers/NONE stripped)");
        CHECK(!writes.empty() && writes[0][2] == id, "each fact carries the session id as provenance");
        /* SECURITY: distilled facts default to the QUARANTINE scope, never the auto-recalled "shared" */
        CHECK(!writes.empty() && writes[0][0] == "distilled", "default scope is the quarantine, not shared");
        bool any_shared = false;
        for (auto &w : writes)
            if (w[0] == "shared") any_shared = true;
        CHECK(!any_shared, "distillation never writes shared by default (no P4 gate bypass)");
        /* NONE must have been skipped — the 3 written are the first three real facts, NOT including NONE */
        bool none_written = false;
        for (auto &w : writes)
            if (w[1] == "NONE") none_written = true;
        CHECK(!none_written, "the NONE sentinel is not written as a fact");
    }

    /* --- distillation that finds nothing durable: ok, 0 written --- */
    {
        std::string      id = make_session(store, 6);
        int              calls = 0;
        WriteFn          sink = [&](const std::string &, const std::string &, const std::string &) {
            calls++;
            return true;
        };
        FakeCtx          fc{"None."}; /* trailing punctuation + mixed case — the robust sentinel still skips */
        hc_agent_backend be = {&fc, fake_chat};
        DistillResult    r = distill_session(store, be, sink, id);
        CHECK(r.ok && r.written == 0 && calls == 0, "a NONE-only distill (even 'None.') writes nothing");
    }

    /* --- idempotency marker: a marked session is skipped on a later pass (persistent-store re-run) --- */
    {
        std::string id = make_session(store, 4);
        CHECK(!is_consolidated(store, id), "a fresh session is not yet consolidated");
        mark_consolidated(store, id);
        CHECK(is_consolidated(store, id), "after marking, the session reads as consolidated (skipped later)");
    }

    hc_store_close(store);
    if (g_fails == 0) std::printf("test_consolidation: OK\n");
    return g_fails ? 1 : 0;
}
