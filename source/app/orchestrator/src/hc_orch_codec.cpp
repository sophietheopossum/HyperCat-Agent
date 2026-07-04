/* hc_orch_codec — task wire format (orchestrator side). See hc_orch_codec.hpp.
 * cJSON is confined to hc_json; this is the only orchestrator TU that builds/parses task JSON. */

#include "hc_orch_codec.hpp"

#include "hc_json.h"

#include <cstdlib>

namespace hc::orch::codec {

std::string assign_body(const Task &t)
{
    hc_json *o = hc_json_new_object();
    if (!o) return "{\"cmd\":\"task.assign\"}";
    hc_json_obj_set_str(o, "cmd", "task.assign");
    hc_json_obj_set_str(o, "task_id", t.id.c_str());
    hc_json_obj_set_str(o, "title", t.title.c_str());
    hc_json_obj_set_str(o, "description", t.description.c_str());
    hc_json_obj_set_str(o, "capability", t.capability.c_str());
    hc_json_obj_set_str(o, "artifact_path", t.artifact_path.c_str()); /* W1.3: the file the worker must produce */
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

std::string ack_body(bool ok)
{
    hc_json *o = hc_json_new_object();
    if (!o) return ok ? "{\"ok\":true}" : "{\"ok\":false}";
    hc_json_obj_set_bool(o, "ok", ok);
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

std::string body_cmd(const std::string &body)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return std::string();
    std::string v = hc_json_get_str(o, "cmd", "");
    hc_json_free(o);
    return v;
}

bool ack_is_ok(const std::string &body)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return false;
    bool ok = hc_json_get_bool(o, "ok", false);
    hc_json_free(o);
    return ok;
}

TaskResult parse_result(const std::string &body)
{
    TaskResult r;
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return r;
    std::string cmd = hc_json_get_str(o, "cmd", "");
    std::string tid = hc_json_get_str(o, "task_id", "");
    if (cmd == "task.result" && !tid.empty()) {
        r.valid = true;
        r.task_id = tid;
        r.ok = hc_json_get_bool(o, "ok", false);
        r.payload = hc_json_get_str(o, "payload", "");
    }
    hc_json_free(o);
    return r;
}

VerdictMsg parse_verdict(const std::string &payload)
{
    VerdictMsg v;
    hc_json   *o = hc_json_parse(payload.data(), payload.size());
    if (o) {
        v.verdict = hc_json_get_str(o, "verdict", "");
        v.grounds = hc_json_get_str(o, "grounds", "");
        hc_json_free(o);
    }
    if (v.verdict.empty() && v.grounds.empty()) v.grounds = payload; /* not verdict-JSON -> raw as grounds */
    return v;
}

} // namespace hc::orch::codec
