/* sleeper_tool — a DELIBERATELY hung tool. Its one function never returns in time, so the ToolHost's timeout
 * sweep must reap it (SIGKILL) and hand the caller a bounded "timed out" deny instead of stalling the fleet.
 * This is Wave D's TIMEOUT/availability security fixture — NOT a usable tool. Built only under HC_BUILD_TESTS.
 * (If the SDK's seccomp profile were to block nanosleep, the process would die on SIGSYS instead — either way it
 * never replies, which is exactly the condition the test exercises.) */

#include "hc_tool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *sleep_invoke(const char *args_json, void *user)
{
    (void)args_json;
    (void)user;
    struct timespec ts = {60, 0};
    for (int i = 0; i < 60; i++) nanosleep(&ts, NULL); /* ~1hr: far beyond any test timeout; the ToolHost kills us first */
    char *r = (char *)malloc(3);
    if (r) strcpy(r, "{}");
    return r;
}

int main(int argc, char **argv)
{
    hc_tool_def defs[] = {
        {"sleep_forever",
         "{\"type\":\"function\",\"function\":{\"name\":\"sleep_forever\",\"description\":\"Never replies in "
         "time (test fixture).\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}",
         sleep_invoke, NULL},
    };
    return hc_tool_main(argc, argv, defs, sizeof defs / sizeof defs[0]);
}
