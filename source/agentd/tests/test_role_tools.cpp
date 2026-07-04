/* test_role_tools — the W2.6 role TOOLSET subset policy. parse_role_tools maps a --role-tools csv (the agentd
 * tool ids the host threads at spawn) into the RoleToolset that gates register_agent_tools. The invariants
 * pinned here: an EMPTY csv => all-on (an unconfigured role keeps full capability — byte-identical to the
 * pre-W2.6 worker); a NON-empty csv => EXACTLY the listed tools (a role SUBTRACTS, it never silently keeps
 * extras); an UNKNOWN id is ignored (a role config can never grant a tool that doesn't exist); an all-unknown
 * csv => everything off (a typo fails SAFE — no tools, never all). The register_agent_tools gating itself is
 * straight-line over these booleans (+ the fs->sb AND-gate for the file tools, so a role can never grant a
 * file tool without a workspace) — verified by inspection; this pins the decision logic it consumes. */

#include "worker_role.hpp"

#include <cstdio>

using hc::parse_role_tools;
using hc::RoleToolset;

static int fails = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                         \
            ++fails;                                                                                         \
        }                                                                                                    \
    } while (0)

int main()
{
    /* EMPTY => all-on (today's behavior: a persona-agnostic / unconfigured role keeps every tool). */
    {
        RoleToolset r = parse_role_tools("");
        CHECK(r.deep_reason && r.memory_recall && r.memory_write);
        CHECK(r.fs_read && r.fs_list && r.fs_write && r.fs_update);
        CHECK(r.load_skill); /* W6 P6.2: load_skill is on by default (additionally gated by a skills dir) */
    }
    /* a non-empty list => EXACTLY the listed tools; all others OFF. */
    {
        RoleToolset r = parse_role_tools("deep_reason,memory_recall,fs_read,fs_list");
        CHECK(r.deep_reason && r.memory_recall && r.fs_read && r.fs_list);
        CHECK(!r.memory_write && !r.fs_write && !r.fs_update);
        CHECK(!r.load_skill); /* W6 P6.2: not listed -> off (a role can subtract load_skill) */
    }
    /* W6 P6.2: load_skill is parseable + isolatable like any other tool id. */
    {
        RoleToolset r = parse_role_tools("fs_read,load_skill");
        CHECK(r.fs_read && r.load_skill);
        CHECK(!r.deep_reason && !r.memory_recall && !r.memory_write && !r.fs_list && !r.fs_write && !r.fs_update);
    }
    /* the research builtin's csv: read-only file access, no write tools (research can SUBTRACT writes). */
    {
        RoleToolset r = parse_role_tools("deep_reason,memory_recall,memory_write,fs_read,fs_list");
        CHECK(r.deep_reason && r.memory_recall && r.memory_write && r.fs_read && r.fs_list);
        CHECK(!r.fs_write && !r.fs_update);
    }
    /* a single tool: exactly it, nothing else. */
    {
        RoleToolset r = parse_role_tools("fs_read");
        CHECK(r.fs_read);
        CHECK(!r.deep_reason && !r.memory_recall && !r.memory_write && !r.fs_list && !r.fs_write &&
              !r.fs_update);
    }
    /* an unknown id is IGNORED (never grants a nonexistent tool); the known ids around it still parse. */
    {
        RoleToolset r = parse_role_tools("fs_read,bogus_tool,fs_write");
        CHECK(r.fs_read && r.fs_write);
        CHECK(!r.deep_reason && !r.memory_recall && !r.memory_write && !r.fs_list && !r.fs_update);
    }
    /* an all-unknown csv => everything OFF (a typo'd role gets NO tools, never all — fail safe). */
    {
        RoleToolset r = parse_role_tools("nope,nada");
        CHECK(!r.deep_reason && !r.memory_recall && !r.memory_write);
        CHECK(!r.fs_read && !r.fs_list && !r.fs_write && !r.fs_update);
    }
    /* empty segments tolerated (a trailing comma + an internal empty field don't enable anything). */
    {
        RoleToolset r = parse_role_tools("fs_read,,fs_write,");
        CHECK(r.fs_read && r.fs_write);
        CHECK(!r.fs_list && !r.deep_reason);
    }
    /* the full 7-tool csv (what the dev/qa/ops builtins serialize to) == all-on, same as EMPTY. */
    {
        RoleToolset r =
            parse_role_tools("deep_reason,memory_recall,memory_write,fs_read,fs_list,fs_write,fs_update");
        CHECK(r.deep_reason && r.memory_recall && r.memory_write);
        CHECK(r.fs_read && r.fs_list && r.fs_write && r.fs_update);
    }
    if (fails == 0) std::printf("test_role_tools: all passed\n");
    return fails ? 1 : 0;
}
