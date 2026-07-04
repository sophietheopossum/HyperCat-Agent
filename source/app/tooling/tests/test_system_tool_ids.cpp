/* test_system_tool_ids — the reserved built-in tool-name guard (anti-spoof). Offline. */

#include "system_tool_ids.hpp"

#include <cstdio>

using hcapp::is_reserved_tool_name;

static int g_fail = 0;
#define CHECK(c, m)                                                                                              \
    do {                                                                                                         \
        if (!(c)) {                                                                                              \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                             \
            ++g_fail;                                                                                            \
        }                                                                                                        \
    } while (0)

int main()
{
    /* every built-in worker + conductor tool name is reserved */
    for (const char *n : {"fs_write", "fs_read", "fs_list", "fs_update", "deep_reason", "memory_recall",
                          "memory_write", "run", "load_skill", "set_goal", "plan_goal", "run_agenda",
                          "agenda_status", "read_artifact", "recall_memory", "write_memory", "ask_user",
                          "add_worker", "remove_worker", "set_mood", "control_audio"})
        CHECK(is_reserved_tool_name(n), n);

    /* the reserved namespaces */
    CHECK(is_reserved_tool_name("hc_internal"), "hc_ namespace reserved");
    CHECK(is_reserved_tool_name("system_probe"), "system_ namespace reserved");

    /* ordinary third-party names are allowed (not over-blocked) */
    for (const char *n : {"csv_stats", "web_search", "qoi_pack", "memory_export", "fsck", "runner"})
        CHECK(!is_reserved_tool_name(n), n);

    CHECK(!is_reserved_tool_name(""), "empty name is not 'reserved' (it is rejected elsewhere as malformed)");

    if (g_fail == 0) std::printf("system_tool_ids: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
