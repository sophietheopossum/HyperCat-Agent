/* test_replay — the P5 gate: record a known OFFLINE session (a scripted backend + a real tool), replay
 * it (recorded outputs fed back, the tool mocked from the record), and assert the re-derived manifest
 * matches the recording EXACTLY (the turn_sha256 oracle) — plus a negative case where a changed task
 * diverges at turn 0. No key, no network. */

#include "replay.hpp"

#include "hc_agent.h"
#include "hc_manifest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                                 \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

static char *dupit(const char *s)
{
    size_t n = std::strlen(s);
    char  *p = static_cast<char *>(std::malloc(n + 1));
    if (p) std::memcpy(p, s, n + 1);
    return p;
}

/* a real tool used while RECORDING: returns a fixed result the manifest then captures */
static char *note_invoke(const char * /*args*/, void * /*user*/) { return dupit("noted: ok"); }

static const char *kSys = "You are a focused test agent.";
static const char *kTask = "Record a note, then finish.";
static const char *kNoteCall =
    "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"note\",\"arguments\":\"{}\"}}]";

int main()
{
    /* --- build a 2-turn script for the RECORD backend (turn 0 calls a tool, turn 1 finishes) --- */
    hc_manifest_rec script[2];
    std::memset(script, 0, sizeof script);
    std::snprintf(script[0].model, sizeof script[0].model, "test-model");
    script[0].turn_index = 0;
    script[0].assistant_text = dupit("Let me record that.");
    script[0].tool_calls_json = dupit(kNoteCall);
    script[0].tool_results_json = dupit(""); /* the REAL tool supplies results while recording */
    std::snprintf(script[0].finish_reason, sizeof script[0].finish_reason, "tool_calls");
    std::snprintf(script[1].model, sizeof script[1].model, "test-model");
    script[1].turn_index = 1;
    script[1].assistant_text = dupit("All done.");
    script[1].tool_calls_json = dupit("");
    script[1].tool_results_json = dupit("");
    std::snprintf(script[1].finish_reason, sizeof script[1].finish_reason, "stop");

    /* --- RECORD: drive the loop with the scripted backend + a real tool, writing manifest M1 --- */
    char sessdir[] = "/tmp/hc_replay_test_XXXXXX";
    CHECK(mkdtemp(sessdir) != nullptr, "mkdtemp session dir");
    {
        hc_manifest *m1 = hc_manifest_open(sessdir);
        CHECK(m1 != nullptr, "open M1");
        hc::replay::RecordedBackend rb(script, 2);
        hc_agent_backend            be = rb.backend();
        hc_agent                   *ag = hc_agent_new_backend(&be, kSys);
        CHECK(ag != nullptr, "agent (record) init");
        hc_agent_tool note = {"note",
                              "{\"type\":\"function\",\"function\":{\"name\":\"note\",\"parameters\":{"
                              "\"type\":\"object\"}}}",
                              note_invoke, nullptr};
        hc_agent_add_tool(ag, &note);
        hc::replay::ManifestRecorder rec;
        rec.m = m1;
        rec.model = "test-model";
        hc_agent_observer obs = rec.observer();
        hc_agent_status   st = hc_agent_run(ag, kTask, &obs, nullptr);
        CHECK(st == HC_AGENT_OK, "record run OK");
        hc_agent_free(ag);
        hc_manifest_close(m1);
    }

    /* confirm M1 has 2 turns on disk */
    {
        hc_manifest     *m = hc_manifest_open(sessdir);
        hc_manifest_rec *recs = nullptr;
        size_t           n = 0;
        CHECK(m && hc_manifest_read(m, &recs, &n) == 0, "read M1");
        CHECK(n == 2, "M1 recorded 2 turns");
        hc_manifest_recs_free(recs, n);
        if (m) hc_manifest_close(m);
    }

    /* --- REPLAY (positive): same system+task => the re-derived manifest matches exactly --- */
    {
        hc::replay::Report r = hc::replay::replay_run(sessdir, kSys, kTask);
        CHECK(r.ok, "replay ran");
        CHECK(r.diff.match, "replay reproduces the recorded manifest EXACTLY (turn_sha256 oracle)");
        CHECK(r.diff.matched == 2, "both turns matched");
        if (!r.diff.match) std::fprintf(stderr, "  diff: %s\n", r.diff.detail);
    }

    /* --- REPLAY (negative): a different task diverges at turn 0 (input hash differs) --- */
    {
        hc::replay::Report r = hc::replay::replay_run(sessdir, kSys, "An entirely different task.");
        CHECK(r.ok, "negative replay ran");
        CHECK(!r.diff.match, "a changed task is NOT a match");
        CHECK(r.diff.first_diff == 0, "divergence is detected at turn 0");
    }

    /* cleanup */
    std::string mpath = std::string(sessdir) + "/manifest.jsonl";
    unlink(mpath.c_str());
    rmdir(sessdir);
    for (int i = 0; i < 2; i++) {
        free(script[i].assistant_text);
        free(script[i].tool_calls_json);
        free(script[i].tool_results_json);
    }

    if (g_fails == 0) std::printf("test_replay: OK\n");
    return g_fails ? 1 : 0;
}
