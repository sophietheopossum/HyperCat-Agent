/* test_hc_agent_compaction — the Conductor P2 hook gate (offline, no LLM/network). Drives hc_agent through
 * a STUB backend (one "stop" turn, no tool calls) and exercises the optional compaction hook:
 *   - default (no compactor)        => history grows unbounded, behaviour unchanged
 *   - a compactor over the threshold => invoked at the turn boundary; history is swapped (keep(0) system +
 *                                       an emitted summary + the kept hot tail); the system prompt survives
 *                                       byte-identical across repeated compactions
 *   - below the threshold            => not invoked
 *   - a declining compactor (false)  => history left untouched (safe no-op)
 * Run under ASan/UBSan (the swap frees the old history + adopts the built one). Exit non-zero on failure. */

#include "hc_agent.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            fprintf(stderr, "FAIL: %s\n", (m));                                                            \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

/* A stub turn backend: emit a tiny assistant text, finish "stop" (no tool calls -> the run loop ends after
 * one iteration). No hc_llm, no network. */
static hc_llm_status stub_stream(void *ctx, const hc_llm_message *msgs, size_t n_msgs,
                                 const char *tools_json, const hc_llm_handlers *h, char *fr, size_t cap)
{
    (void)ctx;
    (void)msgs;
    (void)n_msgs;
    (void)tools_json;
    if (h && h->on_text) h->on_text("ok", 2, h->user);
    if (fr && cap) {
        strncpy(fr, "stop", cap - 1);
        fr[cap - 1] = '\0';
    }
    return HC_LLM_OK;
}

static hc_agent *new_stub_agent(const char *system)
{
    hc_agent_backend b = {NULL, stub_stream};
    return hc_agent_new_backend(&b, system);
}

/* A tiered compactor: verify the system prompt is intact at [0], then rebuild as [system, one summary, the
 * last two messages]. Records its call count + whether the system prompt was ever wrong at [0]. */
typedef struct {
    int calls;
    int system_bad;
} CompCtx;

static bool compactor(hc_agent_compaction *c, void *user)
{
    CompCtx    *cc = (CompCtx *)user;
    cc->calls++;
    const char *r0 = hc_agent_compaction_role(c, 0);
    const char *c0 = hc_agent_compaction_content(c, 0);
    if (!r0 || strcmp(r0, "system") != 0 || !c0 || strcmp(c0, "SYS") != 0) cc->system_bad = 1;

    size_t n = hc_agent_compaction_count(c);
    hc_agent_compaction_keep(c, 0); /* the system message, verbatim (byte-stable prefix) */
    hc_agent_compaction_emit(c, "assistant", "[earlier turns summarized]");
    if (n >= 2) { /* the recent "hot" tail, verbatim */
        hc_agent_compaction_keep(c, n - 2);
        hc_agent_compaction_keep(c, n - 1);
    }
    return true;
}

static bool decline_compactor(hc_agent_compaction *c, void *user)
{
    (void)c;
    (*(int *)user)++;
    return false; /* decline -> the agent must leave the history unchanged */
}

int main(void)
{
    /* 1) default (no compactor): history grows unbounded — the old behaviour, unchanged. */
    {
        hc_agent *a = new_stub_agent("SYS");
        CHECK(a != NULL, "agent created");
        size_t before = hc_agent_message_count(a); /* [system] */
        for (int i = 0; i < 5; i++) CHECK(hc_agent_run(a, "hi", NULL, NULL) == HC_AGENT_OK, "run ok");
        CHECK(hc_agent_message_count(a) > before + 5, "no compactor -> history grows (default unchanged)");
        hc_agent_free(a);
    }

    /* 2) a compactor over the threshold: invoked, history compacted, system preserved across compactions. */
    {
        hc_agent *a = new_stub_agent("SYS");
        CompCtx   cc = {0, 0};
        hc_agent_set_compactor(a, compactor, &cc, 4); /* compact once the history exceeds 4 messages */
        for (int i = 0; i < 8; i++) hc_agent_run(a, "hi", NULL, NULL);
        CHECK(cc.calls > 0, "compactor invoked once the history exceeds the threshold");
        CHECK(cc.system_bad == 0, "the system prompt stays intact at [0] across repeated compactions");
        CHECK(hc_agent_message_count(a) <= 6, "history is held compact (does not grow unbounded)");
        CHECK(hc_agent_last_text(a) != NULL, "the agent still functions after compaction");
        hc_agent_free(a);
    }

    /* 3) below the threshold: the compactor is never invoked. */
    {
        hc_agent *a = new_stub_agent("SYS");
        CompCtx   cc = {0, 0};
        hc_agent_set_compactor(a, compactor, &cc, 1000); /* never reached */
        for (int i = 0; i < 3; i++) hc_agent_run(a, "hi", NULL, NULL);
        CHECK(cc.calls == 0, "compactor NOT invoked below the threshold");
        hc_agent_free(a);
    }

    /* 4) a declining compactor (returns false): the history is left untouched (safe no-op). */
    {
        hc_agent *a = new_stub_agent("SYS");
        int       calls = 0;
        hc_agent_set_compactor(a, decline_compactor, &calls, 4);
        for (int i = 0; i < 6; i++) hc_agent_run(a, "hi", NULL, NULL);
        CHECK(calls > 0, "declining compactor was invoked");
        CHECK(hc_agent_message_count(a) > 6, "a declined compaction leaves the history intact (keeps growing)");
        hc_agent_free(a);
    }

    /* 5) clearing the compactor (null) restores the default. */
    {
        hc_agent *a = new_stub_agent("SYS");
        CompCtx   cc = {0, 0};
        hc_agent_set_compactor(a, compactor, &cc, 4);
        hc_agent_set_compactor(a, NULL, NULL, 0); /* disable */
        for (int i = 0; i < 6; i++) hc_agent_run(a, "hi", NULL, NULL);
        CHECK(cc.calls == 0, "a null compactor disables compaction (no behaviour change)");
        hc_agent_free(a);
    }

    if (g_fail) {
        fprintf(stderr, "test_hc_agent_compaction: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_hc_agent_compaction: all checks passed\n");
    return 0;
}
