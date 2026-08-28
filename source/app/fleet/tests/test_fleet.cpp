/* test_fleet — the fleet's PURE spawn-arg helpers (no Supervisor / no spawn): model_override, the
 * resolve_role_model chain (operator role_models > the role table's model > the global model), and
 * role_spawn_args composition (the --model/--role/--role-prompt/--role-tools the worker spawns with).
 * Exit non-zero on any failure. */

#include "hc_fleet.hpp"
#include "hc_roles.hpp"
#include "hc_settings.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace hcapp;

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

static bool has_flag(const std::vector<std::string> &a, const std::string &f)
{
    for (const auto &x : a)
        if (x == f) return true;
    return false;
}
static bool has_pair(const std::vector<std::string> &a, const std::string &f, const std::string &v)
{
    for (size_t i = 0; i + 1 < a.size(); i++)
        if (a[i] == f && a[i + 1] == v) return true;
    return false;
}

int main()
{
    RoleTable rt;
    rt.roles.push_back({"dev", "You write code.", {RoleTool::FsRead, RoleTool::FsWrite}, ""});
    rt.roles.push_back({"qa", "", {}, "table-qa-model"}); /* a table model, no overlay, all tools */

    Settings st;
    st.role_models["dev"] = "operator-dev-model"; /* an operator override for dev only */

    /* model_override: the operator's sparse per-role map */
    CHECK(model_override(st, "dev") == "operator-dev-model", "model_override returns the operator's dev model");
    CHECK(model_override(st, "qa").empty(), "model_override is empty for an unassigned role");

    /* resolve_role_model: operator > role table > global */
    CHECK(resolve_role_model(rt, st, "dev", "global") == "operator-dev-model", "operator override wins");
    CHECK(resolve_role_model(rt, st, "qa", "global") == "table-qa-model", "the role table's model beats the global");
    CHECK(resolve_role_model(rt, st, "ops", "global") == "global", "no override + no table entry -> the global");
    CHECK(resolve_role_model(rt, st, "ops", nullptr).empty(), "nothing applies -> empty");

    /* per-role provider routing: TWO tiers (operator map > global), and INDEPENDENT of the model chain --
     * a role with a model override but no routing override still gets the global routing. That decoupling
     * is the whole feature: the planner can be pinned to fp8 while the conductor routes freely. */
    st.role_providers["planner"] = "{\"quantizations\":[\"fp8\"]}";
    CHECK(provider_override(st, "planner") == "{\"quantizations\":[\"fp8\"]}", "provider_override returns the map entry");
    CHECK(provider_override(st, "dev").empty(), "provider_override is empty for an unrouted role");
    CHECK(resolve_role_provider(st, "planner", "{\"sort\":\"price\"}") == "{\"quantizations\":[\"fp8\"]}",
          "the operator's per-role routing beats the global");
    CHECK(resolve_role_provider(st, "dev", "{\"sort\":\"price\"}") == "{\"sort\":\"price\"}",
          "a role with a MODEL override but no routing override still inherits the global routing");
    CHECK(resolve_role_provider(st, "dev", nullptr).empty(), "no override + no global -> free routing");

    /* role_spawn_args carries --provider only when one resolves, and only when live */
    auto planner_live = role_spawn_args(rt, st, "planner", true, "global", nullptr);
    CHECK(has_pair(planner_live, "--provider", "{\"quantizations\":[\"fp8\"]}"), "planner carries its routing");
    auto dev_noroute = role_spawn_args(rt, st, "dev", true, "global", nullptr);
    CHECK(!has_flag(dev_noroute, "--provider"), "an unrouted role emits no --provider");
    auto dev_global = role_spawn_args(rt, st, "dev", true, "global", "{\"sort\":\"price\"}");
    CHECK(has_pair(dev_global, "--provider", "{\"sort\":\"price\"}"), "the global routing reaches an unrouted role");
    auto planner_off = role_spawn_args(rt, st, "planner", false, "global", "{\"sort\":\"price\"}");
    CHECK(!has_flag(planner_off, "--provider"), "offline mode carries no routing (nor a model)");

    /* role_spawn_args: live dev -> --model + --role + --role-prompt + --role-tools (a subset) */
    auto dev_live = role_spawn_args(rt, st, "dev", true, "global");
    CHECK(has_pair(dev_live, "--model", "operator-dev-model"), "dev (live) carries its resolved model");
    CHECK(has_pair(dev_live, "--role", "dev"), "dev carries --role dev");
    CHECK(has_pair(dev_live, "--role-prompt", "You write code."), "dev carries its prompt overlay");
    CHECK(has_flag(dev_live, "--role-tools"), "dev (a tool subset) carries --role-tools");

    /* offline: NO --model, but the non-secret identity still rides */
    auto dev_off = role_spawn_args(rt, st, "dev", false, "global");
    CHECK(!has_flag(dev_off, "--model"), "offline dev carries NO --model");
    CHECK(has_pair(dev_off, "--role", "dev"), "offline dev still carries --role");
    CHECK(has_flag(dev_off, "--role-prompt"), "offline dev still carries its overlay");

    /* qa: empty overlay + all-tools -> OMIT --role-prompt + --role-tools (preserves the base behavior) */
    auto qa_live = role_spawn_args(rt, st, "qa", true, "global");
    CHECK(has_pair(qa_live, "--model", "table-qa-model"), "qa carries the table model");
    CHECK(has_pair(qa_live, "--role", "qa"), "qa carries --role qa");
    CHECK(!has_flag(qa_live, "--role-prompt"), "qa (empty overlay) OMITS --role-prompt");
    CHECK(!has_flag(qa_live, "--role-tools"), "qa (all tools) OMITS --role-tools");

    /* Custom Tooling: a GLOBALLY-disabled System Tool (settings.system_tools[name]=false) is SUBTRACTED from a
     * worker's spawn toolset (effective_role_tools_csv), so a disabled tool can never reach a worker. */
    auto csv_of = [](const std::vector<std::string> &a) {
        for (size_t i = 0; i + 1 < a.size(); i++)
            if (a[i] == "--role-tools") return a[i + 1];
        return std::string();
    };
    {
        Settings sg;
        sg.system_tools["fs_write"] = false; /* disable fs_write globally */
        /* qa is all-tools + empty overlay: it previously OMITTED --role-tools; now it must carry an explicit
         * subset that EXCLUDES fs_write (the global disable is enforced at spawn). */
        auto        qa_dis = role_spawn_args(rt, sg, "qa", true, "global");
        std::string csv = csv_of(qa_dis);
        CHECK(has_flag(qa_dis, "--role-tools"), "a global disable forces an explicit --role-tools on an all-tools role");
        CHECK(csv.find("fs_write") == std::string::npos, "globally-disabled fs_write is absent from the spawn toolset");
        CHECK(csv.find("fs_read") != std::string::npos, "non-disabled tools remain after a global disable");
        /* dev's explicit subset {FsRead, FsWrite} minus the global fs_write -> only fs_read */
        std::string dcsv = csv_of(role_spawn_args(rt, sg, "dev", true, "global"));
        CHECK(dcsv.find("fs_write") == std::string::npos && dcsv.find("fs_read") != std::string::npos,
              "a global disable also narrows a role's explicit subset");
    }
    {
        /* disabling EVERY System Tool yields a non-empty sentinel (zero tools), NOT "" (which means all-on). */
        Settings sall;
        for (size_t i = 0; i < role_tool_count(); i++) sall.system_tools[role_tool_name((RoleTool)i)] = false;
        std::string csv = csv_of(role_spawn_args(rt, sall, "qa", true, "global"));
        CHECK(!csv.empty(), "all-disabled emits a non-empty --role-tools sentinel (never \"\" = all-on)");
        CHECK(csv.find("fs_read") == std::string::npos && csv.find("fs_write") == std::string::npos,
              "the sentinel grants no real tool");
    }

    if (g_fail) {
        std::fprintf(stderr, "test_fleet: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_fleet: all checks passed\n");
    return 0;
}
