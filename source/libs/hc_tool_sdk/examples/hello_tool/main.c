/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Savannah Goring. Example for the HyperCat Tool SDK (Apache-2.0) — copy it as your starting
 * point; you own the tools you build. See ../../LICENSE + docs/BUILDING.md. */
/* hello_tool — the minimal HyperCat third-party tool example: read-only, zero permissions. It links the SDK,
 * declares one function ("hello"), and returns a fixed greeting. This is the "safe by default" starting point
 * an author copies — no fs, no network, no exec; the manifest grants nothing. It is also Wave D's end-to-end
 * integration fixture (the ToolHost launches it; an agent invokes "hello"). */

#include "hc_tool.h"

#include <stdlib.h>
#include <string.h>

static char *hello_invoke(const char *args_json, void *user)
{
    (void)args_json; /* this example ignores its arguments */
    (void)user;
    const char *msg = "{\"greeting\":\"hello from a HyperCat third-party tool (=^.^=)\"}";
    char       *r = (char *)malloc(strlen(msg) + 1);
    if (r) strcpy(r, msg);
    return r; /* malloc'd; the harness frees it */
}

int main(int argc, char **argv)
{
    hc_tool_def defs[] = {
        {"hello",
         "{\"type\":\"function\",\"function\":{\"name\":\"hello\",\"description\":\"Return a friendly "
         "greeting.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}",
         hello_invoke, NULL},
    };
    return hc_tool_main(argc, argv, defs, sizeof defs / sizeof defs[0]);
}
