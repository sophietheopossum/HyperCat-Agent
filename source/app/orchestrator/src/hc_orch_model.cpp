/* hc_orch_model — pure domain + task state machine. See hc_orch_model.hpp.
 *
 * Every function here is total and side-effect-free (except task_advance, which mutates only its
 * argument). No I/O, no allocation beyond the std containers the caller already owns. */

#include "hc_orch_model.hpp"

namespace hc::orch {

const char *task_state_str(TaskState s)
{
    switch (s) {
    case TaskState::Pending:   return "pending";
    case TaskState::Assigned:  return "assigned";
    case TaskState::Running:   return "running";
    case TaskState::Verifying: return "verifying";
    case TaskState::Done:      return "done";
    case TaskState::Failed:    return "failed";
    }
    return "unknown";
}

bool role_matches(const std::string &capability, const std::string &role)
{
    return capability.empty() || capability == role;
}

const Task *agenda_find(const Agenda &a, const std::string &task_id)
{
    for (const auto &t : a.tasks)
        if (t.id == task_id) return &t;
    return nullptr;
}

Task *agenda_find(Agenda &a, const std::string &task_id)
{
    for (auto &t : a.tasks)
        if (t.id == task_id) return &t;
    return nullptr;
}

bool task_can_start(const Agenda &a, const Task &task)
{
    if (task.state != TaskState::Pending) return false;
    for (const auto &dep : task.deps) {
        const Task *d = agenda_find(a, dep);
        if (!d || d->state != TaskState::Done) return false; /* a missing dep can never start */
    }
    return true;
}

bool task_dep_unsatisfiable(const Agenda &a, const Task &task)
{
    if (task.state != TaskState::Pending) return false;
    for (const auto &dep : task.deps) {
        const Task *d = agenda_find(a, dep);
        if (!d || d->state == TaskState::Failed) return true; /* missing or failed -> never Done */
    }
    return false;
}

bool task_advance(Task &task, TaskState to)
{
    const TaskState from = task.state;
    bool legal = false;
    switch (from) {
    case TaskState::Pending:
        /* Assigned: dispatched. Failed: it can never start (missing/failed dep, or no provider for
         * its capability) — a terminal edge so a doomed task settles the agenda rather than hanging. */
        legal = (to == TaskState::Assigned || to == TaskState::Failed);
        break;
    case TaskState::Assigned:
        legal = (to == TaskState::Running || to == TaskState::Pending || to == TaskState::Failed);
        break;
    case TaskState::Running:
        /* Verifying: result accepted, awaiting the sibling self-check (P04). */
        legal = (to == TaskState::Done || to == TaskState::Failed || to == TaskState::Pending
                 || to == TaskState::Verifying);
        break;
    case TaskState::Verifying:
        legal = (to == TaskState::Done || to == TaskState::Failed); /* the verdict */
        break;
    case TaskState::Done:
    case TaskState::Failed:
        legal = false; /* terminal */
        break;
    }
    if (!legal) return false;
    task.state = to;
    if (to == TaskState::Pending) {
        task.assignee.clear();   /* unbind on reassignment... */
        task.assigned_at_ms = 0; /* ...and drop the now-stale deadline stamp */
    }
    return true;
}

int agenda_progress(const Agenda &a)
{
    if (a.tasks.empty()) return 100;
    int done = 0;
    for (const auto &t : a.tasks)
        if (t.state == TaskState::Done) done++;
    return (int)((done * 100L) / (long)a.tasks.size());
}

bool agenda_complete(const Agenda &a)
{
    for (const auto &t : a.tasks)
        if (t.state != TaskState::Done) return false;
    return true;
}

bool agenda_failed(const Agenda &a)
{
    for (const auto &t : a.tasks)
        if (t.state == TaskState::Failed) return true;
    return false;
}

bool agenda_settled(const Agenda &a)
{
    for (const auto &t : a.tasks)
        if (t.state != TaskState::Done && t.state != TaskState::Failed) return false;
    return true;
}

AgendaTally agenda_tally(const Agenda &a)
{
    AgendaTally t;
    t.total = a.tasks.size();
    for (const auto &task : a.tasks) {
        if (task.state == TaskState::Done) t.done++;
        else if (task.state == TaskState::Failed) t.failed++;
    }
    return t;
}

} // namespace hc::orch
