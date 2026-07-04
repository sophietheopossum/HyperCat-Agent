/* hc_orch_wal — WAL record schema + the pure recovery fold. See hc_orch_wal.hpp.
 *
 * Serialization is hc_json (so every field is escaped — a task title/result is untrusted text and must
 * never be able to forge a record boundary or break the JSON). The fold is a left-to-right replay: an
 * `open` sets the baseline task snapshot, each `done`/`failed` applies a terminal delta, `settled` flags
 * completion; then every still-non-terminal task is normalized to Pending for re-dispatch. */

#include "hc_orch_wal.hpp"

#include "hc_json.h"

#include <cstdlib>
#include <cstring>

namespace hc::orch::wal {

/* A small RAII guard so every early-return frees the hc_json root exactly once. */
namespace {
struct JsonOwner {
    hc_json *p;
    explicit JsonOwner(hc_json *j) : p(j) {}
    ~JsonOwner() { hc_json_free(p); }
    JsonOwner(const JsonOwner &) = delete;
    JsonOwner &operator=(const JsonOwner &) = delete;
};

std::string print_and_free(hc_json *o)
{
    if (!o) return std::string();
    JsonOwner    own(o);
    char        *s = hc_json_print(o, false);
    std::string  out = s ? s : std::string();
    free(s);
    return out;
}

std::string rec_terminal(const char *type, const std::string &id, const std::string &result)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", type);
    hc_json_obj_set_str(o, "id", id.c_str());
    hc_json_obj_set_str(o, "result", result.c_str());
    return print_and_free(o);
}

TaskState state_from_str(const char *s)
{
    if (s && std::strcmp(s, "done") == 0) return TaskState::Done;
    if (s && std::strcmp(s, "failed") == 0) return TaskState::Failed;
    return TaskState::Pending; /* pending/assigned/running/verifying all recover as Pending (re-dispatch) */
}
} // namespace

std::string rec_open(const Agenda &ag)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "open");
    hc_json_obj_set_str(o, "id", ag.id.c_str());
    hc_json_obj_set_str(o, "title", ag.title.c_str());
    hc_json_obj_set_str(o, "goal", ag.goal.c_str());
    hc_json *arr = hc_json_new_array();
    if (arr) {
        for (const auto &t : ag.tasks) {
            hc_json *to = hc_json_new_object();
            if (!to) continue;
            hc_json_obj_set_str(to, "id", t.id.c_str());
            hc_json_obj_set_str(to, "title", t.title.c_str());
            hc_json_obj_set_str(to, "description", t.description.c_str());
            hc_json_obj_set_str(to, "capability", t.capability.c_str());
            hc_json_obj_set_str(to, "state", task_state_str(t.state));
            hc_json_obj_set_str(to, "result", t.result.c_str());
            hc_json *deps = hc_json_new_array();
            if (deps) {
                for (const auto &d : t.deps) hc_json_arr_append_str(deps, d.c_str());
                hc_json_obj_set(to, "deps", deps);
            }
            hc_json_arr_append(arr, to);
        }
        hc_json_obj_set(o, "tasks", arr);
    }
    return print_and_free(o);
}

std::string rec_done(const std::string &task_id, const std::string &result)
{
    return rec_terminal("done", task_id, result);
}

std::string rec_failed(const std::string &task_id, const std::string &result)
{
    return rec_terminal("failed", task_id, result);
}

std::string rec_settled(bool failed)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "settled");
    hc_json_obj_set_str(o, "verdict", failed ? "failed" : "done");
    return print_and_free(o);
}

Folded fold(const std::vector<std::string> &lines)
{
    Folded f;
    for (const auto &line : lines) {
        hc_json *root = hc_json_parse(line.c_str(), line.size());
        if (!root) continue; /* a corrupt record is skipped, not fatal */
        JsonOwner   own(root);
        const char *type = hc_json_get_str(root, "t", "");
        if (std::strcmp(type, "open") == 0) {
            f.agenda = Agenda{}; /* a later open (a resume) supersedes — last open wins */
            f.agenda.id = hc_json_get_str(root, "id", "");
            f.agenda.title = hc_json_get_str(root, "title", "");
            f.agenda.goal = hc_json_get_str(root, "goal", "");
            const hc_json *tasks = hc_json_get(root, "tasks");
            size_t         n = tasks ? hc_json_arr_len(tasks) : 0;
            for (size_t i = 0; i < n; i++) {
                const hc_json *to = hc_json_arr_at(tasks, i);
                if (!to) continue;
                Task t;
                t.id = hc_json_get_str(to, "id", "");
                t.title = hc_json_get_str(to, "title", "");
                t.description = hc_json_get_str(to, "description", "");
                t.capability = hc_json_get_str(to, "capability", "");
                t.state = state_from_str(hc_json_get_str(to, "state", "pending"));
                t.result = hc_json_get_str(to, "result", "");
                const hc_json *deps = hc_json_get(to, "deps");
                size_t         dn = deps ? hc_json_arr_len(deps) : 0;
                for (size_t j = 0; j < dn; j++) {
                    const char *d = hc_json_as_str(hc_json_arr_at(deps, j), "");
                    if (d[0]) t.deps.push_back(d);
                }
                if (!t.id.empty()) f.agenda.tasks.push_back(std::move(t));
            }
            f.valid = true;
        } else if (std::strcmp(type, "done") == 0 || std::strcmp(type, "failed") == 0) {
            Task *t = agenda_find(f.agenda, hc_json_get_str(root, "id", ""));
            if (t) { /* a delta for a task the open declared (a forged id for a non-task is ignored) */
                t->state = (type[0] == 'd') ? TaskState::Done : TaskState::Failed;
                t->result = hc_json_get_str(root, "result", "");
            }
        } else if (std::strcmp(type, "settled") == 0) {
            f.settled = true;
        }
    }
    /* Recovery normalization: a task that never reached Done/Failed comes back Pending with a cleared
     * assignee + a fresh attempt budget (a host crash is not charged against the task; the resumed engine's
     * fail_or_reassign bounds subsequent retries). Done/Failed tasks keep their state AND result — that is
     * the durability win (no re-run of finished work). */
    for (auto &t : f.agenda.tasks) {
        if (t.state != TaskState::Done && t.state != TaskState::Failed) {
            t.state = TaskState::Pending;
            t.assignee.clear();
            t.attempts = 0;
            t.assigned_at_ms = 0;
        }
    }
    return f;
}

} // namespace hc::orch::wal
