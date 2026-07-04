/* test_roles — the W2.1 offline gate: the built-in defaults are sane, serialize<->parse round-trips, validate
 * drops empty roles + dedups tools, the tool name<->enum mapping is total, and malformed/missing input is
 * graceful. Pure — no I/O beyond an in-memory parse. */

#include "hc_roles.hpp"

#include <cstdio>
#include <string>

using namespace hcapp;

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                   \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

static bool has_tool(const RoleDef &d, RoleTool t)
{
    for (RoleTool x : d.tools)
        if (x == t) return true;
    return false;
}

int main()
{
    /* --- built-in defaults --- */
    RoleTable def = roletable_builtin_defaults();
    CHECK(def.roles.size() == 5, "five built-in roles (dev/qa/research/ops/generalist)");
    const RoleDef *dev = roletable_find(def, "dev");
    const RoleDef *research = roletable_find(def, "research");
    const RoleDef *generalist = roletable_find(def, "generalist");
    CHECK(dev && !dev->prompt_overlay.empty(), "dev has a prompt overlay");
    CHECK(roletable_find(def, "wizard") == nullptr, "an unknown role is not found");
    /* the generalist catch-all: present, with the full write-capable toolset (== dev; no new privilege) */
    CHECK(generalist && !generalist->prompt_overlay.empty(), "generalist has a prompt overlay");
    CHECK(generalist && has_tool(*generalist, RoleTool::FsWrite) && has_tool(*generalist, RoleTool::FsUpdate),
          "generalist carries the write tools (dev-equivalent)");
    CHECK(dev && has_tool(*dev, RoleTool::FsWrite) && has_tool(*dev, RoleTool::FsUpdate),
          "dev carries the write tools");
    CHECK(research && !has_tool(*research, RoleTool::FsWrite) && !has_tool(*research, RoleTool::FsUpdate),
          "research carries NO write tools");
    CHECK(research && has_tool(*research, RoleTool::FsRead), "research can still read");

    /* --- tool name <-> enum is total + the csv reflects the toolset --- */
    for (int i = 0; i < (int)role_tool_count(); i++) {
        RoleTool    rt = (RoleTool)i;
        const char *nm = role_tool_name(rt);
        RoleTool    back;
        CHECK(nm[0] && role_tool_from_name(nm, back) && back == rt, "tool name<->enum round-trips");
    }
    RoleTool junk;
    CHECK(!role_tool_from_name("not_a_tool", junk), "an unknown tool name is rejected");
    CHECK(dev && role_tools_csv(*dev).find("fs_write") != std::string::npos, "dev csv includes fs_write");
    CHECK(research && role_tools_csv(*research).find("fs_write") == std::string::npos,
          "research csv excludes fs_write");

    /* --- serialize <-> parse round-trip --- */
    {
        std::string  js = roletable_serialize(def);
        CHECK(!js.empty(), "serialize produces JSON");
        RoleTable    rt;
        CHECK(roletable_parse(js.c_str(), js.size(), rt), "parse the serialized table");
        CHECK(rt.roles.size() == def.roles.size(), "round-trip preserves the role count");
        const RoleDef *rdev = roletable_find(rt, "dev");
        CHECK(rdev && rdev->prompt_overlay == dev->prompt_overlay, "round-trip preserves the overlay");
        CHECK(rdev && rdev->tools.size() == dev->tools.size(), "round-trip preserves the toolset");
        const RoleDef *rres = roletable_find(rt, "research");
        CHECK(rres && !has_tool(*rres, RoleTool::FsWrite), "round-trip preserves research's restriction");
    }

    /* --- P06: the fs/exec scope record (exec_allow / fs_write_paths / fs_read_paths) --- */
    {
        const char *js = "{\"version\":1,\"roles\":[{\"role\":\"ops\",\"exec_allow\":[\"/usr/bin/git\",\"/bin/ls\"],"
                         "\"fs_write_paths\":[\"build/\",\"out/\"],\"fs_read_paths\":[\"src/\"]}]}";
        RoleTable   t;
        CHECK(roletable_parse(js, std::string(js).size(), t), "parse a table with scope lists");
        const RoleDef *d = roletable_find(t, "ops");
        CHECK(d && d->exec_allow.size() == 2 && d->exec_allow[0] == "/usr/bin/git", "exec_allow parsed");
        CHECK(d && d->fs_write_paths.size() == 2 && d->fs_write_paths[1] == "out/", "fs_write_paths parsed");
        CHECK(d && d->fs_read_paths.size() == 1 && d->fs_read_paths[0] == "src/", "fs_read_paths parsed");
        /* serialize -> parse round-trips the scope lists */
        std::string    js2 = roletable_serialize(t);
        RoleTable      rt;
        CHECK(roletable_parse(js2.c_str(), js2.size(), rt), "re-parse the serialized scope table");
        const RoleDef *rd = roletable_find(rt, "ops");
        CHECK(rd && rd->exec_allow.size() == 2 && rd->fs_write_paths.size() == 2 && rd->fs_read_paths.size() == 1,
              "round-trip preserves the scope lists");
    }
    {
        /* a ';'-bearing entry (the future csv separator -> unsafe) and an empty entry are DROPPED */
        const char *js = "{\"roles\":[{\"role\":\"dev\",\"exec_allow\":[\"/bin/ok\",\"/bin/ev;il\",\"\"]}]}";
        RoleTable   t;
        CHECK(roletable_parse(js, std::string(js).size(), t), "parse a table with a ';'/empty exec entry");
        const RoleDef *d = roletable_find(t, "dev");
        CHECK(d && d->exec_allow.size() == 1 && d->exec_allow[0] == "/bin/ok",
              "a ';'-bearing entry + an empty entry are dropped (csv-safe)");
    }
    {
        /* validate bounds: >32 entries capped at 32; an over-long entry trimmed to 512 bytes */
        RoleTable t;
        RoleDef   d;
        d.role = "dev";
        for (int i = 0; i < 50; i++) d.exec_allow.push_back("/bin/c" + std::to_string(i));
        d.fs_write_paths.push_back(std::string(900, 'p'));
        t.roles.push_back(d);
        roletable_validate(t);
        CHECK(t.roles[0].exec_allow.size() == 32, "validate caps a scope list at 32 entries");
        CHECK(t.roles[0].fs_write_paths.size() == 1 && t.roles[0].fs_write_paths[0].size() == 512,
              "validate trims an over-long scope entry to 512 bytes");
    }
    {
        /* backward-compat: the built-in roles declare NO scope lists, and serialize OMITS the keys */
        const RoleDef *bdev = roletable_find(def, "dev");
        CHECK(bdev && bdev->exec_allow.empty() && bdev->fs_write_paths.empty() && bdev->fs_read_paths.empty(),
              "built-in roles carry empty scope lists (no narrowing — today's behaviour)");
        std::string js = roletable_serialize(def);
        CHECK(js.find("exec_allow") == std::string::npos,
              "an unconfigured role omits the scope keys on serialize (clean, backward-compatible)");
    }

    /* --- validate: drop empty-role entries, dedup tools, keep the unknown-tool out (via parse) --- */
    {
        RoleTable t;
        t.roles.push_back({"", "x", {}, ""});                                       /* empty role -> dropped */
        t.roles.push_back({"dev", "o", {RoleTool::FsRead, RoleTool::FsRead}, ""}); /* dup tool -> deduped   */
        roletable_validate(t);
        CHECK(t.roles.size() == 1 && t.roles[0].role == "dev", "validate drops the empty-role entry");
        CHECK(t.roles[0].tools.size() == 1, "validate dedups a role's tools");
    }
    {
        /* parse drops an unknown tool name (never grants it) */
        const char *js = "{\"version\":1,\"roles\":[{\"role\":\"dev\",\"tools\":[\"fs_read\",\"hack\"]}]}";
        RoleTable   t;
        CHECK(roletable_parse(js, (size_t)0 + std::string(js).size(), t), "parse a table with an unknown tool");
        const RoleDef *d = roletable_find(t, "dev");
        CHECK(d && d->tools.size() == 1 && d->tools[0] == RoleTool::FsRead,
              "an unknown tool name is dropped (not granted)");
    }

    /* --- role_tool_count() agrees with the enum (P2.3b: the UI enumerates checkboxes by it) --- */
    {
        CHECK(role_tool_count() == 8, "role_tool_count() == 8 (the RoleTool enumerators incl. load_skill)");
        for (size_t i = 0; i < role_tool_count(); i++) {
            RoleTool back;
            const char *nm = role_tool_name((RoleTool)i);
            CHECK(nm[0] && role_tool_from_name(nm, back) && (size_t)back == i,
                  "every index in [0,count) names a round-tripping tool");
        }
    }

    /* --- P2.3b bounds: validate caps the prompt overlay (UTF-8-safe) + the role name --- */
    {
        RoleTable t;
        t.roles.push_back({"dev", std::string(9000, 'a'), {RoleTool::FsRead}, ""}); /* 9000 ASCII overlay */
        roletable_validate(t);
        CHECK(t.roles.size() == 1 && t.roles[0].prompt_overlay.size() == 8u * 1024,
              "validate caps an oversized overlay at 8 KB");
    }
    {
        /* 8191 'a' then a 2-byte UTF-8 char STRADDLING the 8192 cut (its continuation byte is the cut point) ->
         * the whole char is dropped (the loop backs off the continuation byte). */
        RoleTable   t;
        std::string ov(8191, 'a');
        ov += "\xC3\xA9"; /* 'é' = 0xC3 0xA9 — lead at 8191, continuation at 8192 (the cut) */
        t.roles.push_back({"dev", ov, {RoleTool::FsRead}, ""});
        roletable_validate(t);
        CHECK(t.roles.size() == 1 && t.roles[0].prompt_overlay.size() == 8191,
              "the overlay cut backs off a straddling UTF-8 sequence (no partial char)");
    }
    {
        /* 8192 'a' then a 2-byte char whose LEAD byte is exactly at the 8192 cut -> resize(8192) keeps [0,8192)
         * and excludes byte[8192], so the lead byte is dropped cleanly (no orphaned lead byte survives). */
        RoleTable   t;
        std::string ov(8192, 'a');
        ov += "\xC3\xA9"; /* lead 0xC3 at index 8192 (the cut), continuation 0xA9 at 8193 */
        t.roles.push_back({"dev", ov, {RoleTool::FsRead}, ""});
        roletable_validate(t);
        CHECK(t.roles.size() == 1 && t.roles[0].prompt_overlay.size() == 8u * 1024,
              "a lead byte AT the cut is dropped (resize excludes byte[n]; no orphaned lead byte)");
    }
    {
        RoleTable t;
        t.roles.push_back({std::string(100, 'r'), "o", {RoleTool::FsRead}, ""}); /* 100-char role name */
        roletable_validate(t);
        CHECK(t.roles.size() == 1 && t.roles[0].role.size() == 64, "validate caps an over-long role name at 64");
    }

    /* --- graceful on malformed / empty input --- */
    {
        RoleTable t;
        CHECK(!roletable_parse("not json", 8, t), "malformed input -> false");
        CHECK(!roletable_parse("", 0, t), "empty input -> false");
        CHECK(!roletable_load("/no/such/role/file.json", t), "a missing file -> false (caller uses defaults)");
    }

    if (g_fail) {
        std::fprintf(stderr, "test_roles: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_roles: all checks passed\n");
    return 0;
}
