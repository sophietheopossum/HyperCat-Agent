/* test_plan_validate — the P05a gate (PURE, offline): a hostile-plan battery on validate_plan + the
 * plan-JSON parser. No LLM, no network. Proves a malformed / oversized / cyclic / dangling-dep /
 * unknown-capability / plan-bomb proposal is REJECTED before any task could reach the bus. */

#include "hc_planner.hpp"
#include "hc_replan.hpp" /* P05b pure helpers: plan_fingerprint + build_repair_goal */

#include <cstdio>
#include <string>
#include <vector>

using hc::orch::Agenda;
using hc::orch::Task;
using hc::orch::TaskState;
using namespace hc::planner;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                                 \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

static Task mk(const std::string &id, const std::string &cap, std::vector<std::string> deps = {})
{
    Task t;
    t.id = id;
    t.title = id + " title";
    t.description = "do " + id;
    t.capability = cap;
    t.deps = std::move(deps);
    return t;
}

int main()
{
    const std::vector<std::string> caps = {"dev", "qa", "research", "ops"};
    const PlanLimits               lim; /* defaults: 32 tasks, 8192 bytes/field, 16 deps */
    std::string                    why;

    /* --- a valid acyclic multi-task plan VALIDATES --- */
    {
        std::vector<Task> ok = {mk("t1", "dev"), mk("t2", "qa", {"t1"}), mk("t3", "ops", {"t1", "t2"})};
        CHECK(validate_plan(ok, caps, lim, &why), "a valid acyclic plan validates");
    }

    /* --- the hostile battery: each must be REJECTED --- */
    CHECK(!validate_plan({}, caps, lim, &why), "empty plan rejected");

    {
        std::vector<Task> dup = {mk("t1", "dev"), mk("t1", "qa")};
        CHECK(!validate_plan(dup, caps, lim, &why), "duplicate id rejected");
    }
    {
        std::vector<Task> unk = {mk("t1", "wizard")}; /* not an offered capability */
        CHECK(!validate_plan(unk, caps, lim, &why), "unknown capability rejected");
    }
    {
        std::vector<Task> empt = {mk("t1", "")}; /* empty capability is not a known role */
        CHECK(!validate_plan(empt, caps, lim, &why), "empty capability rejected");
    }
    {
        std::vector<Task> dangle = {mk("t1", "dev", {"tX"})}; /* dep on a missing id */
        CHECK(!validate_plan(dangle, caps, lim, &why), "dangling dependency rejected");
    }
    {
        std::vector<Task> selfcyc = {mk("t1", "dev", {"t1"})};
        CHECK(!validate_plan(selfcyc, caps, lim, &why), "self-cycle rejected");
    }
    {
        std::vector<Task> cyc = {mk("t1", "dev", {"t2"}), mk("t2", "qa", {"t1"})};
        CHECK(!validate_plan(cyc, caps, lim, &why), "2-node cycle rejected");
    }
    {
        std::vector<Task> noid = {mk("", "dev")};
        CHECK(!validate_plan(noid, caps, lim, &why), "empty id rejected");
    }
    {
        std::vector<Task> bomb;
        for (int i = 0; i < (int)lim.max_tasks + 5; i++) bomb.push_back(mk("t" + std::to_string(i), "dev"));
        CHECK(!validate_plan(bomb, caps, lim, &why), "plan-bomb (too many tasks) rejected");
    }
    {
        Task big = mk("t1", "dev");
        big.description = std::string(lim.max_field_bytes + 1, 'x');
        CHECK(!validate_plan({big}, caps, lim, &why), "oversized field rejected");
    }

    /* --- W1.3: the artifact_path (the deliverable target) --- */
    {
        Task good = mk("t1", "dev");
        good.artifact_path = "src/reverse.py"; /* workspace-relative is fine */
        CHECK(validate_plan({good}, caps, lim, &why), "a workspace-relative artifact_path validates");
        Task abs = mk("t1", "dev");
        abs.artifact_path = "/etc/passwd"; /* absolute -> out of jail */
        CHECK(!validate_plan({abs}, caps, lim, &why), "an absolute artifact_path is rejected");
        Task esc = mk("t1", "dev");
        esc.artifact_path = "../other/secret"; /* .. -> escape */
        CHECK(!validate_plan({esc}, caps, lim, &why), "an artifact_path with .. is rejected");
        Task big = mk("t1", "dev");
        big.artifact_path = std::string(lim.max_field_bytes + 1, 'a');
        CHECK(!validate_plan({big}, caps, lim, &why), "an oversized artifact_path is rejected");
    }
    {
        std::vector<Task> out;
        CHECK(parse_plan_json("{\"tasks\":[{\"id\":\"t1\",\"capability\":\"dev\","
                              "\"artifact_path\":\"a/b.txt\"}]}",
                              out) &&
                  out.size() == 1 && out[0].artifact_path == "a/b.txt",
              "parse: artifact_path round-trips");
    }

    /* --- the parser: strict, no partial plans --- */
    {
        std::vector<Task> out;
        CHECK(parse_plan_json("{\"tasks\":[{\"id\":\"t1\",\"title\":\"a\",\"description\":\"b\","
                              "\"capability\":\"dev\",\"deps\":[]},{\"id\":\"t2\",\"capability\":\"qa\","
                              "\"deps\":[\"t1\"]}]}",
                              out)
                  && out.size() == 2,
              "parse: a well-formed plan parses to 2 tasks");
        CHECK(out.size() == 2 && out[1].deps.size() == 1 && out[1].deps[0] == "t1", "parse: deps parsed");
        /* and the parsed plan validates end-to-end */
        CHECK(validate_plan(out, caps, lim, &why), "parse -> validate round-trip");
    }
    {
        std::vector<Task> out;
        CHECK(!parse_plan_json("not json at all", out), "parse: non-JSON rejected");
        CHECK(!parse_plan_json("{\"nope\":1}", out), "parse: missing tasks rejected");
        CHECK(!parse_plan_json("{\"tasks\":[{\"title\":\"no id\"}]}", out), "parse: task without id rejected");
    }

    /* --- A3: weak-model capability NORMALIZATION (repair, don't reject) — the fix for the screenshot failure --- */
    {
        const std::vector<std::string> capg = {"dev", "qa", "research", "ops", "generalist"};
        CHECK(nearest_role("dev", capg) == "dev", "normalize: an exact role is unchanged");
        CHECK(nearest_role("generalist", capg) == "generalist", "normalize: generalist is unchanged");
        CHECK(nearest_role("developer", capg) == "dev", "normalize: developer -> dev");
        CHECK(nearest_role("Backend Engineer", capg) == "dev", "normalize: case-insensitive substring -> dev");
        CHECK(nearest_role("unit tester", capg) == "qa", "normalize: tester -> qa");
        CHECK(nearest_role("investigator", capg) == "research", "normalize: investigate -> research");
        CHECK(nearest_role("deploy the build", capg) == "ops", "normalize: deploy -> ops");
        CHECK(nearest_role("game designer — alligator dirt casino", capg) == "generalist",
              "normalize: an invented capability -> generalist (the actual screenshot case)");
        CHECK(nearest_role("", capg) == "generalist", "normalize: an empty capability -> generalist");
        CHECK(nearest_role("wizard", caps) == "dev", "normalize: no generalist in vocab -> the first known role");
        /* validate stays STRICT (its hostile-plan contract is unchanged); normalize is what repairs the plan */
        std::vector<Task> weak = {mk("t1", "game designer"), mk("t2", "alligator wrangler", {"t1"})};
        CHECK(!validate_plan(weak, capg, lim, &why), "validate STILL rejects invented caps (contract intact)");
        normalize_capabilities(weak, capg);
        CHECK(weak[0].capability == "generalist" && weak[1].capability == "generalist",
              "normalize: both invented caps repaired to generalist");
        CHECK(validate_plan(weak, capg, lim, &why), "the normalized plan now validates");
    }

    /* --- P05b pure helpers: plan_fingerprint (the no-progress backstop) --- */
    {
        std::vector<Task> a = {mk("t1", "dev"), mk("t2", "qa", {"t1"})};
        std::vector<Task> a_reordered = {mk("t2", "qa", {"t1"}), mk("t1", "dev")};
        CHECK(plan_fingerprint(a) == plan_fingerprint(a_reordered),
              "fingerprint: task order does not matter (it is canonical)");

        /* a pure id-rename that keeps titles + capabilities + dep STRUCTURE is still 'no progress'
         * (deps are mapped id->title before fingerprinting) */
        Task s1;
        s1.id = "x9";
        s1.title = "t1 title";
        s1.capability = "dev";
        Task s2;
        s2.id = "x8";
        s2.title = "t2 title";
        s2.capability = "qa";
        s2.deps = {"x9"};
        CHECK(plan_fingerprint(a) == plan_fingerprint({s1, s2}),
              "fingerprint: a pure id-rename of an identical plan fingerprints the same");

        std::vector<Task> b = {mk("t1", "dev"), mk("t2b", "qa", {"t1"})}; /* a different title */
        CHECK(plan_fingerprint(a) != plan_fingerprint(b),
              "fingerprint: a genuinely changed plan differs (progress is detectable)");
    }

    /* --- P05b pure helpers: build_repair_goal (the failure->prompt framing) --- */
    {
        Agenda ag;
        ag.goal = "ship the feature";
        Task ok = mk("t1", "dev");
        ok.state = TaskState::Done;
        ok.result = "built it";
        Task bad = mk("t2", "qa", {"t1"});
        bad.state = TaskState::Failed;
        bad.result = "verification refuted: the output is wrong";
        ag.tasks = {ok, bad};
        std::string rg = build_repair_goal(ag.goal, ag);
        CHECK(rg.find("ship the feature") != std::string::npos, "repair goal keeps the original goal");
        CHECK(rg.find("verification refuted") != std::string::npos, "repair goal quotes the failure reason");
        CHECK(rg.find("t2 title") != std::string::npos, "repair goal names the failed task");
        CHECK(rg.find("built it") == std::string::npos, "repair goal omits a SUCCEEDED task's result");

        /* a huge UNTRUSTED failure reason is length-bounded — no unbounded repair-prompt growth */
        Agenda big;
        big.goal = "g";
        Task huge = mk("t1", "dev");
        huge.state = TaskState::Failed;
        huge.result = std::string(5000, 'Z');
        big.tasks = {huge};
        std::string rgb = build_repair_goal(big.goal, big);
        CHECK(rgb.size() < 2000, "repair goal bounds a huge untrusted failure reason");
    }

    if (g_fails == 0) std::printf("test_plan_validate: OK\n");
    return g_fails ? 1 : 0;
}
