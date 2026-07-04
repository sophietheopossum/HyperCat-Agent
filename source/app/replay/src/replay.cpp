/* hc_replay — see replay.hpp. The replay backend + mock tools + the read→replay→diff driver. */

#include "replay.hpp"

#include "hc_json.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h> /* mkdtemp, unlink, rmdir */
#include <vector>

namespace hc::replay {
namespace {

char *dup_cstr(const char *s)
{
    s = s ? s : "";
    std::size_t n = std::strlen(s);
    char       *p = static_cast<char *>(std::malloc(n + 1));
    if (p) std::memcpy(p, s, n + 1);
    return p;
}

/* The replay backend: emit the cursor's recorded turn into the handlers (assistant text + the recorded
 * tool calls in OpenAI shape), set finish_reason, mark this as the turn being dispatched, advance. Past
 * the recording it returns a benign empty "stop" so a mis-sized run terminates rather than reads OOB. */
hc_llm_status replay_chat_stream(void *vctx, const hc_llm_message * /*msgs*/, std::size_t /*n_msgs*/,
                                 const char * /*tools_json*/, const hc_llm_handlers *h, char *fr_out,
                                 std::size_t fr_cap)
{
    ReplayCtx *c = static_cast<ReplayCtx *>(vctx);
    if (c->cursor >= c->n) {
        if (fr_out && fr_cap) std::snprintf(fr_out, fr_cap, "stop");
        c->current_turn = c->cursor;
        c->call_index = 0;
        return HC_LLM_OK;
    }
    const hc_manifest_rec &r = c->recs[c->cursor];
    if (h && h->on_text && r.assistant_text && r.assistant_text[0])
        h->on_text(r.assistant_text, std::strlen(r.assistant_text), h->user);
    if (h && h->on_tool_call && r.tool_calls_json && r.tool_calls_json[0]) {
        if (hc_json *arr = hc_json_parse(r.tool_calls_json, std::strlen(r.tool_calls_json))) {
            std::size_t k = hc_json_arr_len(arr);
            for (std::size_t i = 0; i < k; i++) {
                const hc_json *e = hc_json_arr_at(arr, i);
                const char    *id = hc_json_get_str(e, "id", "");
                const hc_json *fn = hc_json_get(e, "function");
                const char    *name = fn ? hc_json_get_str(fn, "name", "") : "";
                const char    *args = fn ? hc_json_get_str(fn, "arguments", "") : "";
                h->on_tool_call(id, name, args, h->user);
            }
            hc_json_free(arr);
        }
    }
    if (fr_out && fr_cap) std::snprintf(fr_out, fr_cap, "%s", r.finish_reason[0] ? r.finish_reason : "stop");
    c->current_turn = c->cursor;
    c->call_index = 0;
    c->cursor++;
    return HC_LLM_OK;
}

/* A mock tool: return the current turn's next recorded result (call order), no real side effect. The
 * shared cursor (set by replay_chat_stream) keeps results aligned to the turn being dispatched. */
char *replay_mock_invoke(const char * /*args_json*/, void *user)
{
    ReplayCtx  *c = static_cast<ReplayCtx *>(user);
    const char *fallback = "{\"error\":\"replay: no recorded result\"}";
    char       *out = nullptr;
    if (c->current_turn < c->n) {
        const hc_manifest_rec &r = c->recs[c->current_turn];
        if (r.tool_results_json && r.tool_results_json[0]) {
            if (hc_json *arr = hc_json_parse(r.tool_results_json, std::strlen(r.tool_results_json))) {
                const hc_json *e = hc_json_arr_at(arr, c->call_index);
                out = dup_cstr(e ? hc_json_as_str(e, fallback) : fallback);
                hc_json_free(arr);
            }
        }
    }
    c->call_index++;
    return out ? out : dup_cstr(fallback);
}

/* Recorder observer: append each completed turn to the open manifest (model fixed at construction). */
void recorder_on_turn(int turn_index, const char *input, const char *assistant_text,
                      const char *tool_calls_json, const char *tool_results_json,
                      const char *finish_reason, void *user)
{
    ManifestRecorder *rec = static_cast<ManifestRecorder *>(user);
    hc_manifest_turn  t;
    t.turn_index = turn_index;
    t.model = rec->model.c_str();
    t.input = input;
    t.assistant_text = assistant_text;
    t.tool_calls_json = tool_calls_json;
    t.tool_results_json = tool_results_json;
    t.finish_reason = finish_reason;
    hc_manifest_append(rec->m, &t);
}

/* Collect the distinct tool names referenced by a recording — one mock tool is registered per name. */
std::vector<std::string> distinct_tool_names(const hc_manifest_rec *recs, std::size_t n)
{
    std::vector<std::string> names;
    for (std::size_t i = 0; i < n; i++) {
        const char *tcj = recs[i].tool_calls_json;
        if (!tcj || !tcj[0]) continue;
        if (hc_json *arr = hc_json_parse(tcj, std::strlen(tcj))) {
            std::size_t k = hc_json_arr_len(arr);
            for (std::size_t j = 0; j < k; j++) {
                const hc_json *fn = hc_json_get(hc_json_arr_at(arr, j), "function");
                const char    *nm = fn ? hc_json_get_str(fn, "name", "") : "";
                if (nm[0] && std::find(names.begin(), names.end(), nm) == names.end())
                    names.emplace_back(nm);
            }
            hc_json_free(arr);
        }
    }
    return names;
}

void remove_temp_manifest(const char *dir)
{
    std::string mpath = std::string(dir) + "/manifest.jsonl";
    unlink(mpath.c_str());
    rmdir(dir);
}

} // namespace

hc_agent_backend RecordedBackend::backend() { return {&ctx, replay_chat_stream}; }

hc_agent_tool RecordedBackend::mock_tool(const char *name, const char *spec_json)
{
    hc_agent_tool t;
    t.name = name;
    t.spec_json = spec_json;
    t.invoke = replay_mock_invoke;
    t.user = &ctx;
    return t;
}

hc_agent_observer ManifestRecorder::observer()
{
    hc_agent_observer o = {};
    o.on_turn = recorder_on_turn;
    o.user = this;
    return o;
}

namespace {

/* Re-drive the loop with the replay backend + mock tools, writing the re-derived manifest (M2) into
 * `m2_dir`. Owns the agent + M2 lifecycle; the caller owns recs + the temp dir. false on a setup
 * failure (the caller still removes the temp dir). */
bool drive_replay(const hc_manifest_rec *recs, std::size_t n, const std::string &system_prompt,
                  const std::string &task, const char *m2_dir)
{
    hc_manifest *m2 = hc_manifest_open(m2_dir);
    if (!m2) return false;
    /* one mock tool per distinct recorded tool name; the name+spec strings outlive the agent's borrow */
    std::vector<std::string> names = distinct_tool_names(recs, n);
    std::vector<std::string> specs;
    specs.reserve(names.size());
    for (const auto &nm : names)
        specs.push_back("{\"type\":\"function\",\"function\":{\"name\":\"" + nm
                        + "\",\"parameters\":{\"type\":\"object\"}}}");
    RecordedBackend  rb(recs, n);
    hc_agent_backend be = rb.backend();
    hc_agent        *ag = hc_agent_new_backend(&be, system_prompt.c_str());
    if (!ag) {
        hc_manifest_close(m2);
        return false;
    }
    for (std::size_t i = 0; i < names.size(); i++) {
        hc_agent_tool t = rb.mock_tool(names[i].c_str(), specs[i].c_str());
        hc_agent_add_tool(ag, &t);
    }
    ManifestRecorder recorder;
    recorder.m = m2;
    recorder.model = recs[0].model; /* a session shares one model — match it for the digest */
    hc_agent_observer obs = recorder.observer();
    hc_agent_run(ag, task.c_str(), &obs, nullptr);
    hc_agent_free(ag);
    hc_manifest_close(m2);
    return true;
}

} // namespace

Report replay_run(const std::string &session_dir, const std::string &system_prompt,
                  const std::string &task)
{
    Report rep;

    /* read the recorded manifest (M1) */
    hc_manifest *m1 = hc_manifest_open(session_dir.c_str());
    if (!m1) {
        rep.error = "cannot open session manifest: " + session_dir;
        return rep;
    }
    hc_manifest_rec *recs1 = nullptr;
    std::size_t      n1 = 0;
    int              rd = hc_manifest_read(m1, &recs1, &n1);
    hc_manifest_close(m1);
    if (rd != 0) {
        rep.error = "manifest read failed";
        return rep; /* recs1 is NULL on a read failure */
    }
    if (n1 == 0) {
        hc_manifest_recs_free(recs1, n1);
        rep.error = "empty manifest — nothing to replay";
        return rep;
    }

    /* re-derive a manifest (M2) in a temp dir, diff M1 vs M2, then a SINGLE cleanup path */
    char  tmpl[] = "/tmp/hc_replay_XXXXXX";
    char *tdir = mkdtemp(tmpl);
    if (!tdir) {
        hc_manifest_recs_free(recs1, n1);
        rep.error = "mkdtemp failed";
        return rep;
    }
    if (!drive_replay(recs1, n1, system_prompt, task, tdir)) {
        rep.error = "replay setup failed (manifest / agent init)";
    } else {
        hc_manifest     *m2r = hc_manifest_open(tdir);
        hc_manifest_rec *recs2 = nullptr;
        std::size_t      n2 = 0;
        if (m2r && hc_manifest_read(m2r, &recs2, &n2) == 0) {
            hc_manifest_diff(recs1, n1, recs2, n2, &rep.diff);
            rep.ok = true;
        } else {
            rep.error = "replay manifest read failed";
        }
        if (m2r) hc_manifest_close(m2r);
        hc_manifest_recs_free(recs2, n2); /* NULL-safe no-op on the read-fail branch */
    }

    remove_temp_manifest(tdir);
    hc_manifest_recs_free(recs1, n1);
    return rep;
}

} // namespace hc::replay
