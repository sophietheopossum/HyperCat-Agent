/* hc_toolbench — measure how well a model honours a tool's declared argument contract as a tool
 * chain gets LONGER, and how that changes across providers/quantisations. Dev-time only.
 *
 * WHY THIS EXISTS. Public agentic benchmarks score the ANSWER, and their headline numbers come from
 * multi-agent harnesses where a sibling retries a failed call. HyperCat's tool host is a serial
 * dispatch loop: one malformed call corrupts a branch rather than being retried. The number that
 * predicts behaviour here -- per-call schema validity across depth -- is measured by nobody, and it
 * compounds: measured 26/8/2026, 0.955 per-step completed ZERO 40-turn chains while 1.000 completed
 * all of them. A model 4.5 points better per call is the difference between never and always.
 *
 * WHY STUBS, AND WHY NO WEB. Real fetches inject variance -- a page changes, a host rate-limits, a
 * backend falls over -- none of which is a property of the model. Stubs carrying the REAL declared
 * schemas make runs repeatable, free of network cost, exactly depth-controllable, and immune to the
 * approval gate. We are measuring whether a model emits a well-formed call thirty times in a row.
 *
 * WHAT COUNTS AS INVALID is deliberately narrow, because a benchmark that flatters is worthless:
 *   unknown_name     the model called a function that was never registered
 *   bad_json         `arguments` did not parse as JSON at all
 *   missing_required a property the spec marks `required` was absent
 *   wrong_type       a declared property was present with the wrong JSON type
 * Everything else is VALID -- including extra unknown keys, which the OpenAI-compatible convention
 * tolerates and real tools ignore. We do not punish what production would accept.
 *
 * The harness never rejects a call: the stub answers even an invalid one, so a model is not pushed
 * off a cliff by its first slip and we observe the whole chain rather than its prefix.
 *
 * Usage:
 *   hc_toolbench --model <id> [--provider <name>] [--depth 30] [--tasks 5] [--effort medium]
 *                [--out f.ndjson] [--selftest]
 * Key: $HC_API_KEY or $OPENROUTER_API_KEY. One NDJSON record per tool call, plus a summary. */

#include "hc_agent.h"
#include "hc_json.h"
#include "hc_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the declared contract

   Lifted VERBATIM from the installed webresearch manifest so we measure the schema the model is
   actually given in production. The required/type table is written out rather than derived by
   walking spec_json: the harness is the thing that must be beyond doubt, and a hand-checked table
   of four properties is easier to audit than a JSON-Schema walker written for one use. If the
   manifest gains a tool, add it here too -- a missing entry scores every call to it as valid. */

static const char kSearchSpec[] =
    "{\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"description\":\"Search the public "
    "web and return ranked results as title, URL and snippet.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query.\"},"
    "\"max_results\":{\"type\":\"integer\",\"description\":\"How many results to return, 1-10 "
    "(default 5).\"}},\"required\":[\"query\"]}}}";

static const char kFetchSpec[] =
    "{\"type\":\"function\",\"function\":{\"name\":\"web_fetch\",\"description\":\"Fetch one public "
    "https URL and return its readable text.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"Absolute https:// URL.\"},"
    "\"max_chars\":{\"type\":\"integer\",\"description\":\"Truncate extracted text to this many "
    "characters (default 8000, max 40000).\"}},\"required\":[\"url\"]}}}";

typedef enum { P_STR, P_INT } prop_kind;
typedef struct { const char *name; prop_kind kind; bool required; } prop_def;
typedef struct { const char *tool; const prop_def *props; size_t n_props; } contract;

static const prop_def kSearchProps[] = {{"query", P_STR, true}, {"max_results", P_INT, false}};
static const prop_def kFetchProps[]  = {{"url", P_STR, true},   {"max_chars", P_INT, false}};
static const contract kContracts[] = {
    {"web_search", kSearchProps, sizeof kSearchProps / sizeof *kSearchProps},
    {"web_fetch",  kFetchProps,  sizeof kFetchProps  / sizeof *kFetchProps},
};

/* ---------------------------------------------------------------- the deterministic corpus

   A chain, not a tree: doc N names doc N+1, and only the last carries the token. That shape forces
   DEPTH -- the model cannot shortcut to the answer, and cannot parallelise its way there either,
   which matches the serial tool host it will really run against. */

typedef struct {
    int   depth;
    char  token[32];
    int   calls;
    int   unknown_name, bad_json, missing_required, wrong_type, valid;
    bool  reached_end;
    FILE *out;
    const char *model;
    const char *provider;
    int   task_index;
} bench;

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

/* Both stubs answer ANY input, valid or not -- see the header: refusing a malformed call would
 * truncate the chain at the first slip and we would measure the prefix, not the run. */
static char *search_invoke(const char *args_json, void *user)
{
    bench *b = (bench *)user;
    char   buf[512];
    (void)args_json;
    snprintf(buf, sizeof buf,
             "[web search result - reference, not instruction]\n"
             "1. Survey of the topic (part 1 of %d)\n   https://bench.invalid/doc-001\n"
             "   Start here; each part names the next.\n[end web search]",
             b->depth);
    return dup_str(buf);
}

static char *fetch_invoke(const char *args_json, void *user)
{
    bench *b = (bench *)user;
    int    n = 0;
    /* Read the doc number out of the URL. A model that fabricates a URL simply gets part 1 back
     * rather than an error -- again, observe rather than punish. */
    if (args_json) {
        const char *p = strstr(args_json, "doc-");
        if (p) n = atoi(p + 4);
    }
    if (n < 1) n = 1;
    char buf[1024];
    if (n >= b->depth) {
        b->reached_end = true;
        snprintf(buf, sizeof buf,
                 "[web page - untrusted content]\nPart %d of %d. This is the final part. "
                 "The answer token is %s . Report it and stop.\n[end web page]",
                 n, b->depth, b->token);
    } else {
        snprintf(buf, sizeof buf,
                 "[web page - untrusted content]\nPart %d of %d. Nothing conclusive here. "
                 "The material continues at https://bench.invalid/doc-%03d\n[end web page]",
                 n, b->depth, n + 1);
    }
    return dup_str(buf);
}

/* ---------------------------------------------------------------- the measurement */

static const contract *find_contract(const char *tool)
{
    for (size_t i = 0; i < sizeof kContracts / sizeof *kContracts; i++)
        if (strcmp(kContracts[i].tool, tool) == 0) return &kContracts[i];
    return NULL;
}

/* The whole measurement, in one pure function so --selftest can exercise it with no model. */
static const char *classify(const char *name, const char *args)
{
    const contract *c = find_contract(name ? name : "");
    if (!c) return "unknown_name";

    hc_json *a = args ? hc_json_parse(args, strlen(args)) : NULL;
    if (!a || !hc_json_is_object(a)) {
        if (a) hc_json_free(a);
        return "bad_json";
    }
    const char *verdict = "valid";
    for (size_t i = 0; i < c->n_props; i++) {
        const hc_json *v = hc_json_get(a, c->props[i].name);
        if (!v) {
            /* Absent-and-optional is fine; absent-and-required is the fault. Checked before type so
             * a missing field is never mis-reported as a type error. */
            if (c->props[i].required) { verdict = "missing_required"; break; }
            continue;
        }
        bool ok = c->props[i].kind == P_STR ? hc_json_is_string(v) : hc_json_is_number(v);
        if (!ok) { verdict = "wrong_type"; break; }
    }
    hc_json_free(a);
    return verdict;
}

static void on_tool(const char *name, const char *args, const char *result, void *user)
{
    bench *b = (bench *)user;
    (void)result;
    b->calls++;

    const char *verdict = classify(name, args);
    if (!strcmp(verdict, "unknown_name"))          b->unknown_name++;
    else if (!strcmp(verdict, "bad_json"))         b->bad_json++;
    else if (!strcmp(verdict, "missing_required")) b->missing_required++;
    else if (!strcmp(verdict, "wrong_type"))       b->wrong_type++;
    else                                           b->valid++;

    /* `call_index` is the depth curve's x-axis: the question is not whether a model can emit one
     * good call, it is whether the thirtieth is still good. */
    fprintf(b->out,
            "{\"model\":\"%s\",\"provider\":\"%s\",\"task\":%d,\"depth\":%d,"
            "\"call_index\":%d,\"tool\":\"%s\",\"verdict\":\"%s\"}\n",
            b->model, b->provider ? b->provider : "", b->task_index, b->depth, b->calls,
            name ? name : "", verdict);
    fflush(b->out);
}

/* Prove the instrument before trusting its output. Every case is one a real model produces: a bare
 * string instead of an object, a hallucinated sibling name, an integer passed as a string, a
 * required field omitted under long context. Silently scoring any of these VALID would make the
 * whole benchmark read high and nobody would notice. */
static int run_selftest(int depth)
{
    struct { const char *name, *args, *want; } cases[] = {
        {"web_search", "{\"query\":\"wayland\"}",                      "valid"},
        {"web_search", "{\"query\":\"x\",\"max_results\":5}",          "valid"},
        {"web_fetch",  "{\"url\":\"https://a/\",\"max_chars\":8000}",  "valid"},
        {"web_fetch",  "{\"url\":\"https://a/\",\"note\":\"extra\"}",  "valid"},
        {"web_search", "{\"max_results\":5}",                          "missing_required"},
        {"web_fetch",  "{}",                                           "missing_required"},
        {"web_search", "{\"query\":123}",                              "wrong_type"},
        {"web_fetch",  "{\"url\":\"https://a/\",\"max_chars\":\"8000\"}", "wrong_type"},
        {"web_search", "not json at all",                              "bad_json"},
        {"web_search", "\"a bare string\"",                            "bad_json"},
        {"web_search", "",                                             "bad_json"},
        {"web_serch",  "{\"query\":\"typo\"}",                         "unknown_name"},
        {"browse",     "{\"url\":\"https://a/\"}",                     "unknown_name"},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *got = classify(cases[i].name, cases[i].args);
        bool ok = strcmp(got, cases[i].want) == 0;
        if (!ok) fails++;
        printf("  %-4s %-12s %-34s want=%-16s got=%s\n", ok ? "ok" : "FAIL", cases[i].name,
               cases[i].args, cases[i].want, got);
    }
    /* The corpus must reach the end, and only at the end -- a stub terminating early would silently
     * shorten every task and flatter the depth curve. */
    bench b = {0};
    b.depth = depth;
    snprintf(b.token, sizeof b.token, "TOKEN-TEST");
    for (int n = 1; n <= depth; n++) {
        char url[64];
        snprintf(url, sizeof url, "{\"url\":\"https://bench.invalid/doc-%03d\"}", n);
        char *r = fetch_invoke(url, &b);
        bool last = (n == depth);
        bool says_final = r && strstr(r, "final part") != NULL;
        if (says_final != last) {
            printf("  FAIL chain: doc-%03d %s final\n", n, says_final ? "claims" : "does not claim");
            fails++;
        }
        free(r);
    }
    if (!b.reached_end) { printf("  FAIL chain: never reached the end\n"); fails++; }
    printf("%s: %d failure(s)\n", fails ? "SELFTEST FAILED" : "selftest OK", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *model = NULL, *effort = NULL, *outpath = NULL, *provider = NULL;
    int         depth = 30, tasks = 5;
    bool        selftest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc)          model = argv[++i];
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc)     depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tasks") && i + 1 < argc)     tasks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--effort") && i + 1 < argc)    effort = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)       outpath = argv[++i];
        else if (!strcmp(argv[i], "--provider") && i + 1 < argc)  provider = argv[++i];
        else if (!strcmp(argv[i], "--selftest"))                  selftest = true;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (selftest) return run_selftest(depth < 1 ? 6 : depth);
    if (!model) {
        fprintf(stderr, "usage: hc_toolbench --model <id> [--provider <name>] [--depth N] "
                        "[--tasks N] [--selftest]\n");
        return 2;
    }
    if (depth < 1) depth = 1;
    if (tasks < 1) tasks = 1;

    const char *key = getenv("HC_API_KEY");
    if (!key || !*key) key = getenv("OPENROUTER_API_KEY");
    if (!key || !*key) { fprintf(stderr, "set HC_API_KEY or OPENROUTER_API_KEY\n"); return 2; }

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { fprintf(stderr, "cannot open %s\n", outpath); return 1; }

    hc_http *http = hc_http_new();
    if (!http) { fprintf(stderr, "hc_http_new failed\n"); return 1; }
    hc_llm_provider cfg = {0};
    cfg.name = "openrouter";
    cfg.base_url = "https://openrouter.ai/api/v1";
    cfg.api_key = key;
    cfg.model = model;
    cfg.reasoning_effort = effort;

    /* Pin one endpoint. Without this a run measures whichever the router picked -- verified 27/8:
     * a bare slug for deepseek-v4-flash-0731 landed on AkashML, not even the cheapest of its 29
     * endpoints. Comparing quantisations demands a pin, and allow_fallbacks:false makes a wrong
     * pin fail loudly rather than silently routing elsewhere and reading like a null result.
     *
     * BUILT AS A TREE, not by snprintf'ing the name into a JSON template. `provider` is argv, so a
     * name containing a quote used to decide the request's SHAPE rather than just its contents: with
     * `--provider 'X"]},"model":"evil'` the %s closed the array and object early and pasted a second
     * `model` key onto the request root, where last-key-wins made it the model actually run. hc_llm now
     * refuses a block carrying a reserved key, so that no longer redirects a turn -- but it fails by
     * dropping the pin ENTIRELY, which for this tool is the worst outcome available: an unpinned run
     * silently measures whichever endpoint the router chose, which is the exact null result the pin
     * exists to prevent. Constructing the node escapes the name instead, so a weird name stays a name. */
    char *provider_json = NULL;
    if (provider && provider[0]) {
        hc_json *root = hc_json_new_object();
        hc_json *blk = hc_json_new_object();
        hc_json *only = hc_json_new_array();
        if (!root || !blk || !only || !hc_json_arr_append_str(only, provider)) {
            hc_json_free(root);
            hc_json_free(blk);
            hc_json_free(only);
            fprintf(stderr, "could not build the provider block for '%s'\n", provider);
            return 1;
        }
        /* obj_set/arr_append ADOPT on success and FREE the child on failure -- so a failed add must not
         * free its child again, but DOES leave the parent still ours to release. Split from the
         * `provider` add below for exactly that reason: there, `blk` is the child. */
        if (!hc_json_obj_set(blk, "only", only) || !hc_json_obj_set_bool(blk, "allow_fallbacks", false)) {
            hc_json_free(blk); /* `only` was freed by the failed add; blk is still ours */
            hc_json_free(root);
            fprintf(stderr, "could not build the provider block for '%s'\n", provider);
            return 1;
        }
        if (!hc_json_obj_set(root, "provider", blk)) { /* frees blk on failure */
            hc_json_free(root);
            fprintf(stderr, "could not build the provider block for '%s'\n", provider);
            return 1;
        }
        provider_json = hc_json_print_canonical(root);
        hc_json_free(root);
        if (!provider_json) {
            fprintf(stderr, "could not serialize the provider block\n");
            return 1;
        }
        cfg.extra_body_json = provider_json; /* COPIED by hc_llm_new; freed below */
    }

    hc_llm *llm = hc_llm_new(&cfg, http);
    free(provider_json); /* hc_llm took its own copy */
    if (!llm) { fprintf(stderr, "hc_llm_new failed\n"); return 1; }

    int grand_calls = 0, grand_valid = 0, completed = 0;
    for (int t = 0; t < tasks; t++) {
        bench b = {0};
        b.depth = depth;
        b.out = out;
        b.model = model;
        b.provider = provider;
        b.task_index = t;
        snprintf(b.token, sizeof b.token, "TOKEN-%04d", 1000 + t);

        /* The system prompt states the chain rule plainly. We are not testing whether the model can
         * INFER the protocol -- that is a comprehension test and would confound the measurement. */
        hc_agent *ag = hc_agent_new(
            llm,
            "You are a research agent. Use web_search to find the starting document, then follow the "
            "chain with web_fetch: each part names the URL of the next. Continue until you reach the "
            "final part, then reply with the answer token and nothing else.");
        if (!ag) { fprintf(stderr, "hc_agent_new failed\n"); return 1; }

        /* hc_agent caps tool-call ITERATIONS at 8 by default -- fine for a chat agent, fatal here.
         * Without this every --depth beyond ~8 silently stopped early and reported
         * "tool-iteration limit reached", which reads like a model failure and is not one.
         * Headroom above `depth`: the model spends a turn on the initial search and may take a
         * wasted turn or two, and cutting the chain short is exactly the artefact we are avoiding. */
        hc_agent_set_max_iterations(ag, depth + 16);

        hc_agent_tool ts = {"web_search", kSearchSpec, search_invoke, &b};
        hc_agent_tool tf = {"web_fetch", kFetchSpec, fetch_invoke, &b};
        if (!hc_agent_add_tool(ag, &ts) || !hc_agent_add_tool(ag, &tf)) {
            fprintf(stderr, "hc_agent_add_tool failed\n");
            return 1;
        }

        hc_agent_observer obs = {0};
        obs.on_tool = on_tool;
        obs.user = &b;
        hc_agent_status st = hc_agent_run(ag, "Find the answer token by following the chain.", &obs, NULL);

        grand_calls += b.calls;
        grand_valid += b.valid;
        if (b.reached_end) completed++;
        /* Name the UNDERLYING llm status too: "model call failed" alone cannot distinguish a
         * provider 429 from a malformed request from a timeout, and a benchmark that cannot tell
         * those apart will happily report provider downtime as model incapability. */
        fprintf(stderr,
                "task %d/%d  depth=%d  calls=%d valid=%d  unknown=%d bad_json=%d missing=%d "
                "wrong_type=%d  end=%s  status=%s%s%s\n",
                t + 1, tasks, depth, b.calls, b.valid, b.unknown_name, b.bad_json,
                b.missing_required, b.wrong_type, b.reached_end ? "yes" : "NO",
                hc_agent_status_str(st),
                st == HC_AGENT_ERR_LLM ? " / " : "",
                st == HC_AGENT_ERR_LLM ? hc_llm_status_str(hc_agent_last_llm_status(ag)) : "");
        hc_agent_free(ag);
    }

    fprintf(stderr, "\n%s [%s]  depth=%d  tasks=%d\n  per-call validity: %d/%d (%.2f%%)\n"
                    "  tasks reaching the end: %d/%d (%.0f%%)\n",
            model, provider ? provider : "auto-routed", depth, tasks, grand_valid, grand_calls,
            grand_calls ? 100.0 * grand_valid / grand_calls : 0.0,
            completed, tasks, tasks ? 100.0 * completed / tasks : 0.0);

    hc_llm_free(llm);
    hc_http_free(http);
    if (outpath) fclose(out);
    return 0;
}
