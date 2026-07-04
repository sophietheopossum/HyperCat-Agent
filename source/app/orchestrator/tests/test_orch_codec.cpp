/* Unit tests for the internal task wire codec (hc_orch_codec). Reached via a PRIVATE src include,
 * as hc_http tests its internal SSE parser. No I/O. Exit non-zero on any failure. */

#include "hc_orch_codec.hpp"

#include "hc_json.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace hc::orch;
using namespace hc::orch::codec;

static int g_fails = 0;
#define CHECK(c, m)                                                                            \
    do {                                                                                       \
        if (!(c)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                           \
            g_fails++;                                                                         \
        }                                                                                      \
    } while (0)

int main()
{
    /* --- assign_body: valid JSON with the expected fields --- */
    {
        Task t;
        t.id = "t1";
        t.title = "Build";
        t.description = "do x";
        t.capability = "dev";
        std::string ab = assign_body(t);
        CHECK(body_cmd(ab) == "task.assign", "assign cmd");
        hc_json *o = hc_json_parse(ab.data(), ab.size());
        CHECK(o != nullptr, "assign body parses");
        CHECK(o && std::string(hc_json_get_str(o, "task_id", "")) == "t1", "assign task_id");
        CHECK(o && std::string(hc_json_get_str(o, "description", "")) == "do x", "assign description");
        CHECK(o && std::string(hc_json_get_str(o, "capability", "")) == "dev", "assign capability");
        if (o) hc_json_free(o);
    }

    /* --- parse_result: well-formed task.result round-trips; junk is rejected --- */
    {
        hc_json *r = hc_json_new_object();
        hc_json_obj_set_str(r, "cmd", "task.result");
        hc_json_obj_set_str(r, "task_id", "t1");
        hc_json_obj_set_bool(r, "ok", true);
        hc_json_obj_set_str(r, "payload", "done: Build");
        char *rs = hc_json_print(r, false);
        std::string rbody = rs ? rs : "";
        free(rs);
        hc_json_free(r);

        TaskResult tr = parse_result(rbody);
        CHECK(tr.valid && tr.task_id == "t1" && tr.ok && tr.payload == "done: Build",
              "task.result parses all fields");
        CHECK(!parse_result("{\"cmd\":\"task.assign\",\"task_id\":\"t1\"}").valid,
              "a non-result body is invalid");
        CHECK(!parse_result("{\"cmd\":\"task.result\"}").valid, "missing task_id is invalid");
        CHECK(!parse_result("not json").valid, "garbage is invalid");
    }

    /* --- ack round-trip --- */
    CHECK(ack_is_ok(ack_body(true)), "ack_body(true) -> ok");
    CHECK(!ack_is_ok(ack_body(false)), "ack_body(false) -> not ok");
    CHECK(!ack_is_ok("{}"), "missing ok -> false");
    CHECK(body_cmd("garbage").empty(), "body_cmd of junk is empty");

    if (g_fails) {
        std::fprintf(stderr, "orch_codec: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("orch_codec: all checks passed\n");
    return 0;
}
