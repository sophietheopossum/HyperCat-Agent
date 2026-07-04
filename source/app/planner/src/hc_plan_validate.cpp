/* hc_plan_validate — the PURE half of hc_planner.
 *
 * Purpose:   parse plan JSON, NORMALIZE a weak model's capabilities to known roles (repair, above the strict
 *            validator), VALIDATE the proposed DAG (bounds, unique ids, known capabilities, no dangling deps,
 *            acyclic), and build the decompose prompt. No LLM, no I/O — the offline unit-test surface (the
 *            hostile-plan battery; normalization NEVER loosens validate_plan, it only runs ahead of it).
 * Owns:      nothing — all inputs/outputs are by value or borrowed for the call.
 * Threading: re-entrant; no global or shared state.
 * Security:  the LLM plan is UNTRUSTED — validate_plan is the gate that runs BEFORE any task is built, so
 *            an oversized/cyclic/dangling/unknown-capability/plan-bomb proposal can never reach the bus. */

#include "hc_planner.hpp"

#include "hc_json.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace hc::planner {

namespace {
bool caps_has(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}
std::string to_lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::string nearest_role(const std::string &capability, const std::vector<std::string> &allowed_caps)
{
    if (caps_has(allowed_caps, capability)) return capability; /* already a known role — leave it untouched */

    /* a tiny case-insensitive substring map for a weak model's common inventions. A synonym only wins if its
     * canonical role is actually offered (the operator may have pruned the table). Order = most-specific-first. */
    struct Syn {
        const char *frag;
        const char *role;
    };
    static const Syn kSyn[] = {
        {"develop", "dev"},   {"coder", "dev"},     {"engineer", "dev"},  {"program", "dev"},
        {"implement", "dev"}, {"backend", "dev"},   {"frontend", "dev"},  {"software", "dev"},
        {"test", "qa"},       {"verif", "qa"},      {"validat", "qa"},    {"debug", "qa"},
        {"qa", "qa"},         {"research", "research"}, {"investig", "research"}, {"analy", "research"},
        {"study", "research"},{"explore", "research"},  {"deploy", "ops"},    {"infra", "ops"},
        {"config", "ops"},    {"devops", "ops"},    {"pipeline", "ops"},  {"script", "ops"},
        {"ops", "ops"},
    };
    const std::string lc = to_lower(capability);
    for (const Syn &s : kSyn)
        if (lc.find(s.frag) != std::string::npos && caps_has(allowed_caps, s.role)) return s.role;

    /* the catch-all landing. "generalist" MUST match kGeneralistRole in app/orchestrator (hc_orch_engine.cpp) and
     * the "generalist" key in app/roles (roletable_builtin_defaults) — kept in sync by convention (different layers,
     * no shared dep); the roles + engine tests pin the string on their side. */
    if (caps_has(allowed_caps, "generalist")) return "generalist";
    return allowed_caps.empty() ? std::string("generalist") : allowed_caps.front(); /* last resort */
}

void normalize_capabilities(std::vector<hc::orch::Task> &tasks, const std::vector<std::string> &allowed_caps)
{
    for (auto &t : tasks) t.capability = nearest_role(t.capability, allowed_caps);
}

bool parse_plan_json(const std::string &json, std::vector<hc::orch::Task> &out)
{
    out.clear();
    hc_json *root = hc_json_parse(json.data(), json.size());
    if (!root) return false;
    const hc_json *tasks = hc_json_get(root, "tasks");
    bool           ok = (tasks != nullptr);
    std::size_t    n = ok ? hc_json_arr_len(tasks) : 0;
    for (std::size_t i = 0; i < n && ok; i++) {
        const hc_json *e = hc_json_arr_at(tasks, i);
        if (!e) {
            ok = false;
            break;
        }
        hc::orch::Task t;
        t.id = hc_json_get_str(e, "id", "");
        t.title = hc_json_get_str(e, "title", "");
        t.description = hc_json_get_str(e, "description", "");
        t.capability = hc_json_get_str(e, "capability", "");
        t.artifact_path = hc_json_get_str(e, "artifact_path", ""); /* W1.3: the file this task must produce */
        if (t.id.empty()) { /* id is the only REQUIRED field — no id => not a task */
            ok = false;
            break;
        }
        if (const hc_json *deps = hc_json_get(e, "deps")) { /* optional; absent/non-array => no deps */
            std::size_t dn = hc_json_arr_len(deps);
            for (std::size_t j = 0; j < dn; j++) {
                const char *d = hc_json_as_str(hc_json_arr_at(deps, j), "");
                if (d && d[0]) t.deps.emplace_back(d);
            }
        }
        out.push_back(std::move(t));
    }
    hc_json_free(root);
    if (!ok) out.clear();
    return ok;
}

bool validate_plan(const std::vector<hc::orch::Task> &tasks,
                   const std::vector<std::string> &allowed_caps, const PlanLimits &lim, std::string *why)
{
    auto fail = [&](const std::string &m) {
        if (why) *why = m;
        return false;
    };
    if (tasks.empty()) return fail("empty plan");
    if (tasks.size() > lim.max_tasks) return fail("too many tasks");

    std::unordered_set<std::string>          ids;
    std::unordered_map<std::string, std::size_t> idx;
    for (std::size_t i = 0; i < tasks.size(); i++) {
        const hc::orch::Task &t = tasks[i];
        if (t.id.empty()) return fail("empty task id");
        if (t.id.size() > lim.max_field_bytes || t.title.size() > lim.max_field_bytes
            || t.description.size() > lim.max_field_bytes || t.capability.size() > lim.max_field_bytes
            || t.artifact_path.size() > lim.max_field_bytes)
            return fail("a task field exceeds the size cap");
        /* W1.3: a plan that names an out-of-jail deliverable is malformed — fail closed (the sandbox would
         * refuse it anyway, but a workspace-relative path is the only sane target). */
        if (!t.artifact_path.empty() &&
            (t.artifact_path.front() == '/' || t.artifact_path.find("..") != std::string::npos))
            return fail("artifact_path must be workspace-relative (no leading / or ..): " + t.artifact_path);
        if (!ids.insert(t.id).second) return fail("duplicate task id: " + t.id);
        idx[t.id] = i;
        if (std::find(allowed_caps.begin(), allowed_caps.end(), t.capability) == allowed_caps.end())
            return fail("unknown capability: " + (t.capability.empty() ? "(empty)" : t.capability));
        if (t.deps.size() > lim.max_deps) return fail("too many deps on task " + t.id);
    }
    for (const auto &t : tasks) /* no dangling deps (every dep id is present in the plan) */
        for (const auto &d : t.deps)
            if (ids.find(d) == ids.end()) return fail("dangling dependency: " + d);

    /* ACYCLIC: 3-colour DFS over the dependency graph (edge t -> d means t needs d done first). A back
     * edge into a Gray node is a cycle. Bounded recursion (<= max_tasks). */
    enum Colour { White, Gray, Black };
    std::vector<Colour>             colour(tasks.size(), White);
    std::function<bool(std::size_t)> cyclic = [&](std::size_t u) -> bool {
        colour[u] = Gray;
        for (const auto &d : tasks[u].deps) {
            std::size_t v = idx[d]; /* present (dangling already rejected) */
            if (colour[v] == Gray) return true;
            if (colour[v] == White && cyclic(v)) return true;
        }
        colour[u] = Black;
        return false;
    };
    for (std::size_t i = 0; i < tasks.size(); i++)
        if (colour[i] == White && cyclic(i)) return fail("dependency cycle");

    return true;
}

std::string build_plan_prompt(const std::string &goal, const std::vector<std::string> &allowed_caps)
{
    std::string caps;
    for (std::size_t i = 0; i < allowed_caps.size(); i++) {
        if (i) caps += ", ";
        caps += allowed_caps[i];
    }
    return "Goal: " + goal
           + "\n\nDecompose this goal into a MINIMAL set of concrete tasks (prefer a handful, never more "
             "than is genuinely needed). Output ONLY a JSON object — no prose, no markdown, no code "
             "fences — of exactly this shape:\n"
             "{\"tasks\":[{\"id\":\"t1\",\"title\":\"short title\",\"description\":\"the concrete "
             "instruction for a worker\",\"capability\":\"one-of-the-roles\",\"artifact_path\":\"dir/"
             "file.ext\",\"deps\":[]}]}\n"
             "Rules: ids are unique (t1, t2, ...); capability is EXACTLY one of ["
           + caps
           + "]; deps lists the ids that must finish first (use [] for none); the dependency graph MUST be "
             "acyclic; description is the actionable instruction handed to a worker. artifact_path is the "
             "WORKSPACE-RELATIVE file the task must produce (e.g. \"src/reverse.py\", \"README.md\") — set it "
             "for any task whose output is a file (most coding tasks), and OMIT it (\"\") only for a pure "
             "analysis/decision task that writes nothing. Never use an absolute path or \"..\".";
}

} // namespace hc::planner
