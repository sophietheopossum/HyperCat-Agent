/* hc_agent.c — single-agent turn loop. See hc_agent.h for the contract.
 *
 * One run: append the user message, then repeat { stream an hc_llm turn, collecting assistant
 * text and tool calls; append the assistant message (echoing tool_calls); dispatch each tool
 * call and append its result as a "tool" message } until a turn yields no tool calls, or the
 * iteration cap / cancel token stops it. History strings are owned here; the per-turn hc_llm
 * request borrows them.
 *
 * Size: marginally past the ~500 LOC smell-test — the P2 compaction builder (struct hc_agent_compaction)
 * holds a borrowed hc_agent* and is inseparable from struct hc_agent, so splitting it to its own TU would
 * either leak the opaque struct across the ABI or add a shared internal header; kept here deliberately.
 */

#include "hc_agent.h"

#include "hc_json.h"
#include "hc_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *role;
    char *content;
    char *tool_call_id;    /* "tool" messages */
    char *tool_calls_json; /* "assistant" messages that called tools */
} hc_msg;

/* The registered-tools array now GROWS on demand (see hc_agent_add_tool — realloc, mirroring hist), so the
 * built-in toolset + any number of runtime (third-party proxy) tools register without a silent drop at a small
 * cap. HC_AGENT_MAX_TOOLS_HARD is only a runaway-caller backstop, far above any real toolset. HC_AGENT_MAX_TOOLS
 * is retained as the per-TURN collected-tool-call cap (the fixed `tc[]` array below — 32 calls/turn is ample). */
#define HC_AGENT_MAX_TOOLS 32
#define HC_AGENT_MAX_TOOLS_HARD 256

/* Bound the assistant text we RETAIN per run: a hostile/buggy provider must not grow the worker's
 * heap without limit by dribbling endless content deltas, and a turn's result must always fit a bus
 * frame. The live stream is still forwarded to the observer in full; only what we keep is capped. */
#define HC_AGENT_MAX_TEXT (256u * 1024)

struct hc_agent {
    hc_agent_backend backend; /* the turn source (hosted hc_llm or replay); ctx borrowed */
    hc_msg *hist;
    size_t  hist_n, hist_cap;
    hc_agent_tool *tools; /* registered tools — grown on demand (realloc; see hc_agent_add_tool) */
    size_t  n_tools, tools_cap;
    int     max_iters;
    /* P2: optional context compaction. compactor null => disabled (no behaviour change). */
    hc_agent_compactor compactor;
    void              *compactor_user; /* borrowed */
    int                compactor_threshold;
    /* The hc_llm status behind the last HC_AGENT_ERR_LLM. The agent collapses every non-OK transport
     * result into one error code, which left "model call failed" as the ONLY thing an operator ever saw --
     * a timeout, an HTTP 429 and a malformed body were indistinguishable. Kept so the caller can say
     * WHICH. Meaningless unless the run returned HC_AGENT_ERR_LLM. */
    hc_llm_status last_llm_status;
};

/* The replacement-history builder a compactor writes into (Conductor P2). Valid only for one compactor
 * call; `a` is borrowed for reads, `out` accumulates the new history (adopted by the agent on success). */
struct hc_agent_compaction {
    hc_agent *a;
    hc_msg   *out;
    size_t    out_n, out_cap;
    bool      oom; /* sticky: any keep()/emit() that hit OOM — the swap is then declined */
};

/* The hosted backend: ctx is the borrowed hc_llm; one turn is a real streamed chat call. */
static hc_llm_status hosted_chat_stream(void *ctx, const hc_llm_message *msgs, size_t n_msgs,
                                        const char *tools_json, const hc_llm_handlers *handlers,
                                        char *finish_reason_out, size_t fr_cap)
{
    return hc_llm_chat_stream((hc_llm *)ctx, msgs, n_msgs, tools_json, handlers, finish_reason_out,
                              fr_cap);
}

static char *dup_or_null(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static bool hist_push(hc_agent *a, const char *role, const char *content,
                      const char *tool_call_id, const char *tool_calls_json)
{
    if (a->hist_n == a->hist_cap) {
        size_t ncap = a->hist_cap ? a->hist_cap * 2 : 8;
        hc_msg *nh = realloc(a->hist, ncap * sizeof *nh);
        if (!nh) return false;
        a->hist = nh;
        a->hist_cap = ncap;
    }
    hc_msg *m = &a->hist[a->hist_n];
    m->role = dup_or_null(role);
    m->content = dup_or_null(content ? content : "");
    m->tool_call_id = dup_or_null(tool_call_id);
    m->tool_calls_json = dup_or_null(tool_calls_json);
    if (!m->role || !m->content || (tool_call_id && !m->tool_call_id)
        || (tool_calls_json && !m->tool_calls_json)) { /* OOM on any requested field */
        free(m->role);
        free(m->content);
        free(m->tool_call_id);
        free(m->tool_calls_json);
        return false;
    }
    a->hist_n++;
    return true;
}

/* Free a history array + all its message strings. */
static void free_hist(hc_msg *h, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(h[i].role);
        free(h[i].content);
        free(h[i].tool_call_id);
        free(h[i].tool_calls_json);
    }
    free(h);
}

static hc_agent *agent_new_common(hc_agent_backend backend, const char *system_prompt)
{
    hc_agent *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->backend = backend;
    a->max_iters = 8;
    if (system_prompt && system_prompt[0] && !hist_push(a, "system", system_prompt, NULL, NULL)) {
        hc_agent_free(a); /* frees a->hist, which hist_push may have grown before failing */
        return NULL;
    }
    return a;
}

hc_agent_backend hc_agent_hosted_backend(hc_llm *llm)
{
    hc_agent_backend b = {llm, hosted_chat_stream}; /* wrap the borrowed client as a turn backend */
    return b;
}

hc_agent *hc_agent_new(hc_llm *llm, const char *system_prompt)
{
    if (!llm) return NULL;
    return agent_new_common(hc_agent_hosted_backend(llm), system_prompt);
}

hc_agent *hc_agent_new_backend(const hc_agent_backend *backend, const char *system_prompt)
{
    if (!backend || !backend->chat_stream) return NULL;
    return agent_new_common(*backend, system_prompt);
}

void hc_agent_free(hc_agent *a)
{
    if (!a) return;
    free_hist(a->hist, a->hist_n);
    free(a->tools);
    free(a);
}

bool hc_agent_add_tool(hc_agent *a, const hc_agent_tool *tool)
{
    if (!a || !tool || !tool->name || !tool->invoke || !tool->spec_json || !tool->spec_json[0]
        || a->n_tools >= HC_AGENT_MAX_TOOLS_HARD) /* a runaway-caller backstop, far above any real toolset */
        return false;
    if (a->n_tools == a->tools_cap) { /* grow on demand (mirrors hist_push) — no silent drop at a small cap */
        size_t         ncap = a->tools_cap ? a->tools_cap * 2 : 8;
        hc_agent_tool *nt = realloc(a->tools, ncap * sizeof *nt);
        if (!nt) return false;
        a->tools = nt;
        a->tools_cap = ncap;
    }
    a->tools[a->n_tools++] = *tool;
    return true;
}

void hc_agent_set_max_iterations(hc_agent *a, int max_iters)
{
    if (a && max_iters > 0) a->max_iters = max_iters;
}

bool hc_agent_seed_message(hc_agent *a, const char *role, const char *content)
{
    if (!a || !role) return false;
    /* accept only the known chat roles — a bogus role would otherwise serialize into the request verbatim */
    if (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0 && strcmp(role, "tool") != 0
        && strcmp(role, "system") != 0)
        return false;
    return hist_push(a, role, content ? content : "", NULL, NULL); /* content may be NULL -> "" */
}

/* ---- P2: context compaction ---- */

void hc_agent_set_compactor(hc_agent *a, hc_agent_compactor fn, void *user, int threshold)
{
    if (!a) return;
    a->compactor = fn;
    a->compactor_user = user;
    a->compactor_threshold = threshold;
}

/* Append a message to the compaction builder's replacement history. Sets `oom` and returns false on any
 * allocation failure (so the whole swap is then declined — never a partial/corrupt history). */
static bool comp_push(hc_agent_compaction *c, const char *role, const char *content, const char *tcid,
                      const char *tcj)
{
    if (c->out_n == c->out_cap) {
        size_t  ncap = c->out_cap ? c->out_cap * 2 : 8;
        hc_msg *nh = realloc(c->out, ncap * sizeof *nh);
        if (!nh) {
            c->oom = true;
            return false;
        }
        c->out = nh;
        c->out_cap = ncap;
    }
    hc_msg *m = &c->out[c->out_n];
    m->role = dup_or_null(role);
    m->content = dup_or_null(content ? content : "");
    m->tool_call_id = dup_or_null(tcid);
    m->tool_calls_json = dup_or_null(tcj);
    if (!m->role || !m->content || (tcid && !m->tool_call_id) || (tcj && !m->tool_calls_json)) {
        free(m->role);
        free(m->content);
        free(m->tool_call_id);
        free(m->tool_calls_json);
        c->oom = true;
        return false;
    }
    c->out_n++;
    return true;
}

size_t hc_agent_compaction_count(const hc_agent_compaction *c) { return c ? c->a->hist_n : 0; }

const char *hc_agent_compaction_role(const hc_agent_compaction *c, size_t i)
{
    return (c && i < c->a->hist_n) ? c->a->hist[i].role : NULL;
}

const char *hc_agent_compaction_content(const hc_agent_compaction *c, size_t i)
{
    return (c && i < c->a->hist_n) ? c->a->hist[i].content : NULL;
}

bool hc_agent_compaction_keep(hc_agent_compaction *c, size_t i)
{
    if (!c || i >= c->a->hist_n) return false;
    const hc_msg *m = &c->a->hist[i];
    return comp_push(c, m->role, m->content, m->tool_call_id, m->tool_calls_json);
}

bool hc_agent_compaction_emit(hc_agent_compaction *c, const char *role, const char *content)
{
    if (!c || !role) return false;
    return comp_push(c, role, content, NULL, NULL);
}

/* At a turn boundary: if a compactor is installed and the history exceeds the threshold, let it build a
 * replacement; adopt it only on a clean, non-empty success (else discard the build, leaving history intact
 * — so a failing/declining compactor is always a safe no-op). */
static void maybe_compact(hc_agent *a)
{
    if (!a->compactor || a->compactor_threshold <= 0 || a->hist_n <= (size_t)a->compactor_threshold) return;
    hc_agent_compaction c = {a, NULL, 0, 0, false};
    bool                apply = a->compactor(&c, a->compactor_user);
    if (apply && !c.oom && c.out_n > 0) {
        free_hist(a->hist, a->hist_n); /* drop the old history; adopt the compacted one */
        a->hist = c.out;
        a->hist_n = c.out_n;
        a->hist_cap = c.out_cap;
    } else {
        free_hist(c.out, c.out_n); /* declined / OOM / empty -> discard the build, keep the original */
    }
}

/* ---- per-turn collection of streamed assistant output ---- */

typedef struct {
    char   id[80];
    char   name[128];
    char  *args;
} coll_tc;

typedef struct {
    const hc_agent_observer *obs;
    hc_agent_cancel         *cancel;
    char   *text;
    size_t  text_len, text_cap;
    coll_tc tc[HC_AGENT_MAX_TOOLS];
    size_t  n_tc;
    bool    oom;
} run_ctx;

static void rc_on_text(const char *delta, size_t n, void *user)
{
    run_ctx *rc = user;
    if (rc->obs && rc->obs->on_text) rc->obs->on_text(delta, n, rc->obs->user); /* forward in full */
    if (rc->text_len >= HC_AGENT_MAX_TEXT) return;             /* retention cap hit: stop storing */
    if (n > HC_AGENT_MAX_TEXT - rc->text_len) n = HC_AGENT_MAX_TEXT - rc->text_len; /* keep what fits */
    if (rc->text_len + n + 1 > rc->text_cap) {
        size_t ncap = rc->text_cap ? rc->text_cap : 256;
        while (ncap < rc->text_len + n + 1) ncap *= 2;
        char *nb = realloc(rc->text, ncap);
        if (!nb) {
            rc->oom = true;
            return;
        }
        rc->text = nb;
        rc->text_cap = ncap;
    }
    memcpy(rc->text + rc->text_len, delta, n);
    rc->text_len += n;
    rc->text[rc->text_len] = '\0';
}

static void rc_on_tool(const char *id, const char *name, const char *args, void *user)
{
    run_ctx *rc = user;
    if (rc->n_tc >= HC_AGENT_MAX_TOOLS) return;
    coll_tc *t = &rc->tc[rc->n_tc++];
    snprintf(t->id, sizeof t->id, "%s", id ? id : "");
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->args = dup_or_null(args ? args : "");
}

static bool rc_should_cancel(void *user)
{
    run_ctx *rc = user;
    return rc->cancel && rc->cancel->flag;
}

/* Serialize the assistant's collected tool calls back into the OpenAI "tool_calls" JSON array,
 * so the next request echoes them (required for the tool-result round-trip). */
static char *serialize_tool_calls(const coll_tc *tc, size_t n)
{
    hc_json *arr = hc_json_new_array();
    if (!arr) return NULL;
    for (size_t i = 0; i < n; i++) {
        hc_json *fn = hc_json_new_object();
        if (!fn || !hc_json_obj_set_str(fn, "name", tc[i].name)
            || !hc_json_obj_set_str(fn, "arguments", tc[i].args ? tc[i].args : "")) {
            hc_json_free(fn);
            hc_json_free(arr);
            return NULL;
        }
        hc_json *o = hc_json_new_object();
        if (!o || !hc_json_obj_set_str(o, "id", tc[i].id)
            || !hc_json_obj_set_str(o, "type", "function")) {
            hc_json_free(fn);
            hc_json_free(o);
            hc_json_free(arr);
            return NULL;
        }
        if (!hc_json_obj_set(o, "function", fn)) { /* adopts fn / frees it on failure */
            hc_json_free(o);
            hc_json_free(arr);
            return NULL;
        }
        if (!hc_json_arr_append(arr, o)) { /* adopts o (and fn) / frees on failure */
            hc_json_free(arr);
            return NULL;
        }
    }
    char *out = hc_json_print(arr, false);
    hc_json_free(arr);
    return out;
}

/* Build the request "tools" array from the registered tools' spec_json, or NULL if none. */
static char *build_tools_json(hc_agent *a)
{
    if (a->n_tools == 0) return NULL;
    hc_json *arr = hc_json_new_array();
    if (!arr) return NULL;
    for (size_t i = 0; i < a->n_tools; i++) {
        const char *spec = a->tools[i].spec_json;
        hc_json *o = spec && spec[0] ? hc_json_parse(spec, strlen(spec)) : NULL;
        if (!o) { /* missing / invalid spec */
            hc_json_free(arr);
            return NULL;
        }
        if (!hc_json_arr_append(arr, o)) { /* adopts o / frees it on failure — do not free again */
            hc_json_free(arr);
            return NULL;
        }
    }
    char *out = hc_json_print(arr, false);
    hc_json_free(arr);
    return out;
}

static const hc_agent_tool *find_tool(hc_agent *a, const char *name)
{
    for (size_t i = 0; i < a->n_tools; i++)
        if (strcmp(a->tools[i].name, name) == 0) return &a->tools[i];
    return NULL;
}

/* Convert the history into a borrowed hc_llm_message array for one request. */
static hc_llm_message *history_as_messages(hc_agent *a)
{
    hc_llm_message *lm = calloc(a->hist_n ? a->hist_n : 1, sizeof *lm);
    if (!lm) return NULL;
    for (size_t i = 0; i < a->hist_n; i++) {
        lm[i].role = a->hist[i].role;
        lm[i].content = a->hist[i].content;
        lm[i].tool_call_id = a->hist[i].tool_call_id;
        lm[i].tool_calls_json = a->hist[i].tool_calls_json;
    }
    return lm;
}

/* A stable canonical JSON of the request messages — the per-turn observer's `input`, which the manifest
 * hashes (so the replay oracle can verify the input matched). NULL on OOM (recorded as "" by the caller). */
static char *serialize_messages(const hc_llm_message *lm, size_t n)
{
    hc_json *arr = hc_json_new_array();
    if (!arr) return NULL;
    for (size_t i = 0; i < n; i++) {
        hc_json *o = hc_json_new_object();
        if (!o || !hc_json_obj_set_str(o, "role", lm[i].role ? lm[i].role : "")
            || !hc_json_obj_set_str(o, "content", lm[i].content ? lm[i].content : "")
            || !hc_json_arr_append(arr, o)) { /* arr_append adopts o / frees it on failure */
            hc_json_free(o);
            hc_json_free(arr);
            return NULL;
        }
    }
    char *out = hc_json_print_canonical(arr);
    hc_json_free(arr);
    return out;
}

hc_agent_status hc_agent_run(hc_agent *a, const char *user_message, const hc_agent_observer *obs,
                             hc_agent_cancel *cancel)
{
    if (!a || !user_message) return HC_AGENT_ERR_INVALID;
    /* Forget the previous turn's transport cause up front. The contract says this is only meaningful
     * after HC_AGENT_ERR_LLM, so a stale value is technically legal -- but a caller reading it after a
     * DIFFERENT failure would be told a confident, wrong story about a call that never happened. Cheaper
     * to make a misread harmless than to rely on everyone having read the contract. */
    a->last_llm_status = HC_LLM_OK;
    if (!hist_push(a, "user", user_message, NULL, NULL)) return HC_AGENT_ERR_NOMEM;
    maybe_compact(a); /* P2: at this turn boundary, compact if over threshold (no-op without a compactor) */

    char *tools_json = build_tools_json(a);
    hc_agent_status rv = HC_AGENT_ERR_LIMIT;

    for (int iter = 0; iter < a->max_iters; iter++) {
        if (cancel && cancel->flag) {
            rv = HC_AGENT_ERR_CANCELLED;
            break;
        }

        run_ctx rc;
        memset(&rc, 0, sizeof rc);
        rc.obs = obs;
        rc.cancel = cancel;
        hc_llm_handlers h = {rc_on_text, rc_on_tool, rc_should_cancel, &rc};

        hc_llm_message *lm = history_as_messages(a);
        if (!lm) {
            rv = HC_AGENT_ERR_NOMEM;
            break;
        }
        char *input_str = serialize_messages(lm, a->hist_n); /* the input this turn saw (manifest hash) */
        char  finish[32] = {0};
        hc_llm_status lst =
            a->backend.chat_stream(a->backend.ctx, lm, a->hist_n, tools_json, &h, finish, sizeof finish);
        free(lm);

        char *tcs_json = rc.n_tc ? serialize_tool_calls(rc.tc, rc.n_tc) : NULL;
        bool  appended = hist_push(a, "assistant", rc.text ? rc.text : "", NULL, tcs_json);
        /* tcs_json + rc.text + input_str are kept alive for the on_turn record below, then freed on every
         * path (hist_push copied them) — do NOT free them early. */

        if (lst == HC_LLM_ERR_CANCELLED || lst != HC_LLM_OK || !appended || rc.oom) {
            for (size_t i = 0; i < rc.n_tc; i++) free(rc.tc[i].args);
            free(tcs_json);
            free(rc.text);
            free(input_str);
            a->last_llm_status = lst; /* so the caller can report the CAUSE, not just "it failed" */
            rv = (lst == HC_LLM_ERR_CANCELLED) ? HC_AGENT_ERR_CANCELLED
                 : (lst != HC_LLM_OK)          ? HC_AGENT_ERR_LLM
                                               : HC_AGENT_ERR_NOMEM;
            break;
        }

        if (rc.n_tc == 0) { /* no tool calls -> the turn is complete */
            if (obs && obs->on_turn)
                obs->on_turn(iter, input_str ? input_str : "", rc.text ? rc.text : "", "", "", finish,
                             obs->user);
            free(tcs_json);
            free(rc.text);
            free(input_str);
            rv = HC_AGENT_OK;
            break;
        }

        /* dispatch each tool call, append its result, and collect the results into a JSON array */
        hc_json *results = hc_json_new_array();
        bool     dispatch_ok = (results != NULL);
        for (size_t i = 0; i < rc.n_tc; i++) {
            const hc_agent_tool *tool = find_tool(a, rc.tc[i].name);
            char                *result = NULL;
            if (tool)
                result = tool->invoke(rc.tc[i].args ? rc.tc[i].args : "", tool->user);
            const char *result_str = result ? result : "{\"error\":\"unknown or failed tool\"}";
            if (obs && obs->on_tool)
                obs->on_tool(rc.tc[i].name, rc.tc[i].args ? rc.tc[i].args : "", result_str, obs->user);
            if (results) hc_json_arr_append_str(results, result_str); /* literal result -> array element */
            if (!hist_push(a, "tool", result_str, rc.tc[i].id, NULL)) dispatch_ok = false;
            free(result);
            free(rc.tc[i].args);
        }
        char *results_json = results ? hc_json_print(results, false) : NULL;
        hc_json_free(results);
        if (obs && obs->on_turn)
            obs->on_turn(iter, input_str ? input_str : "", rc.text ? rc.text : "",
                         tcs_json ? tcs_json : "", results_json ? results_json : "", finish, obs->user);
        free(results_json);
        free(tcs_json);
        free(rc.text);
        free(input_str);
        if (!dispatch_ok) {
            rv = HC_AGENT_ERR_NOMEM;
            break;
        }
        /* loop: call the model again with the tool results in history */
    }

    free(tools_json);
    return rv;
}

size_t hc_agent_message_count(const hc_agent *a) { return a ? a->hist_n : 0; }

const char *hc_agent_last_text(const hc_agent *a)
{
    if (!a) return NULL;
    for (size_t i = a->hist_n; i > 0; i--)
        if (strcmp(a->hist[i - 1].role, "assistant") == 0) return a->hist[i - 1].content;
    return NULL;
}

hc_llm_status hc_agent_last_llm_status(const hc_agent *a) { return a ? a->last_llm_status : HC_LLM_OK; }

const char *hc_agent_status_str(hc_agent_status s)
{
    switch (s) {
    case HC_AGENT_OK:            return "ok";
    case HC_AGENT_ERR_INVALID:   return "invalid argument";
    case HC_AGENT_ERR_LLM:       return "model call failed";
    case HC_AGENT_ERR_CANCELLED: return "cancelled";
    case HC_AGENT_ERR_LIMIT:     return "tool-iteration limit reached";
    case HC_AGENT_ERR_NOMEM:     return "out of memory";
    }
    return "unknown";
}
