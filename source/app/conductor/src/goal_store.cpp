/* goal_store.cpp — GoalStore: durable goals on per-goal hc_wal streams. See hc_goal.hpp.
 *
 * The record schema mirrors the orchestrator's P14 agenda WAL (app/orchestrator/src/hc_orch_wal.cpp): an
 * `open` writes the full goal baseline, each `status`/`agenda` applies a delta, `settled` flags completion;
 * recovery replays a stream left-to-right and folds it back to one Goal. Every field is hc_json-escaped, so
 * untrusted operator/title text cannot forge a record boundary or break the JSON. Durability is fail-closed:
 * a mutator applies in memory only after its journal append returns durable (ephemeral mode has nothing to
 * journal and always applies). Single-owner; no internal locking (the conductor thread is the sole owner). */

#include "hc_goal.hpp"

#include "hc_json.h"
#include "hc_wal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <unistd.h>

namespace hc::conductor {

/* ---- status <-> stable wire token ---- */

const char *goal_status_str(GoalStatus s)
{
    switch (s) {
    case GoalStatus::Discussing: return "discussing";
    case GoalStatus::Active:     return "active";
    case GoalStatus::Paused:     return "paused";
    case GoalStatus::Done:       return "done";
    case GoalStatus::Failed:     return "failed";
    case GoalStatus::Abandoned:  return "abandoned";
    }
    return "discussing";
}

GoalStatus goal_status_from_str(const char *s)
{
    if (!s) return GoalStatus::Discussing;
    if (std::strcmp(s, "active") == 0) return GoalStatus::Active;
    if (std::strcmp(s, "paused") == 0) return GoalStatus::Paused;
    if (std::strcmp(s, "done") == 0) return GoalStatus::Done;
    if (std::strcmp(s, "failed") == 0) return GoalStatus::Failed;
    if (std::strcmp(s, "abandoned") == 0) return GoalStatus::Abandoned;
    return GoalStatus::Discussing; /* unknown / "discussing" -> Discussing */
}

bool goal_status_is_terminal(GoalStatus s)
{
    return s == GoalStatus::Done || s == GoalStatus::Failed || s == GoalStatus::Abandoned;
}

/* ---- WAL record schema + the pure recovery fold (TU-internal) ---- */

namespace {

constexpr std::size_t kMaxLiveGoals      = 256;  /* bound concurrently-live goals (cf. orchestrator kMaxLiveAgendas) */
constexpr std::size_t kRetainSettled     = 256;  /* bound retained terminal goals in memory (cf. kRetainSettled)    */
constexpr std::size_t kMaxAgendasPerGoal = 4096; /* bound a goal's agenda_ids so a same-uid-corrupted WAL can't spike recovery */

/* RAII so every early-return frees the hc_json root exactly once (same idiom as hc_orch_wal.cpp). */
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
    JsonOwner   own(o);
    char       *s = hc_json_print(o, false);
    std::string out = s ? s : std::string();
    std::free(s);
    return out;
}

std::string rec_open(const Goal &g)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "open");
    hc_json_obj_set_str(o, "id", g.id.c_str());
    hc_json_obj_set_str(o, "title", g.title.c_str());
    hc_json_obj_set_str(o, "text", g.text.c_str());
    hc_json_obj_set_str(o, "status", goal_status_str(g.status));
    hc_json_obj_set_str(o, "session_id", g.session_id.c_str());
    hc_json_obj_set_str(o, "source", g.source.c_str());
    hc_json *arr = hc_json_new_array();
    if (arr) {
        for (const auto &a : g.agenda_ids) hc_json_arr_append_str(arr, a.c_str());
        hc_json_obj_set(o, "agenda_ids", arr);
    }
    return print_and_free(o);
}

std::string rec_status(GoalStatus s)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "status");
    hc_json_obj_set_str(o, "status", goal_status_str(s));
    return print_and_free(o);
}

std::string rec_agenda(const std::string &agenda_id)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "agenda");
    hc_json_obj_set_str(o, "agenda_id", agenda_id.c_str());
    return print_and_free(o);
}

std::string rec_settled(GoalStatus s)
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", "settled");
    hc_json_obj_set_str(o, "status", goal_status_str(s));
    return print_and_free(o);
}

struct GoalFold {
    Goal goal;
    bool valid   = false; /* saw an `open` (a delta before any open is ignored) */
    bool settled = false; /* saw a `settled` */
};

GoalFold fold(const std::vector<std::string> &lines)
{
    GoalFold f;
    for (const auto &line : lines) {
        hc_json *root = hc_json_parse(line.c_str(), line.size());
        if (!root) continue; /* a corrupt record is skipped, not fatal */
        JsonOwner   own(root);
        const char *type = hc_json_get_str(root, "t", "");
        if (std::strcmp(type, "open") == 0) {
            f.goal = Goal{}; /* a later open (a resume re-baseline) supersedes — last open wins */
            f.goal.id = hc_json_get_str(root, "id", "");
            f.goal.title = hc_json_get_str(root, "title", "");
            f.goal.text = hc_json_get_str(root, "text", "");
            f.goal.status = goal_status_from_str(hc_json_get_str(root, "status", "discussing"));
            f.goal.session_id = hc_json_get_str(root, "session_id", "");
            f.goal.source = hc_json_get_str(root, "source", "");
            const hc_json *arr = hc_json_get(root, "agenda_ids");
            std::size_t    n = arr ? hc_json_arr_len(arr) : 0;
            for (std::size_t i = 0; i < n && f.goal.agenda_ids.size() < kMaxAgendasPerGoal; i++) {
                const char *a = hc_json_as_str(hc_json_arr_at(arr, i), "");
                if (a[0]) f.goal.agenda_ids.push_back(a);
            }
            f.valid = true;
        } else if (std::strcmp(type, "status") == 0) {
            if (f.valid) f.goal.status = goal_status_from_str(hc_json_get_str(root, "status", "discussing"));
        } else if (std::strcmp(type, "agenda") == 0) {
            if (f.valid && f.goal.agenda_ids.size() < kMaxAgendasPerGoal) {
                const char *a = hc_json_get_str(root, "agenda_id", "");
                if (a[0] && std::find(f.goal.agenda_ids.begin(), f.goal.agenda_ids.end(), a)
                                == f.goal.agenda_ids.end())
                    f.goal.agenda_ids.push_back(a);
            }
        } else if (std::strcmp(type, "settled") == 0) {
            f.settled = true;
            if (f.valid) f.goal.status = goal_status_from_str(hc_json_get_str(root, "status", "done"));
        }
        /* an unknown `t`, or a delta before any `open`, is ignored — a forged line can never synthesize a goal */
    }
    return f;
}

} // namespace

/* ---- GoalStore ---- */

struct GoalStore::Impl {
    std::string                              wal_dir; /* "" => ephemeral (no journaling, in-memory only) */
    std::vector<Goal>                        goals;   /* all retained goals, in creation order */
    std::unordered_map<std::string, hc_wal *> wals;   /* id -> open stream (live goals only)             */
    std::uint64_t                            counter = 0;

    bool durable() const { return !wal_dir.empty(); }

    Goal       *find(const std::string &id)
    {
        for (auto &g : goals)
            if (g.id == id) return &g;
        return nullptr;
    }
    const Goal *find(const std::string &id) const
    {
        for (const auto &g : goals)
            if (g.id == id) return &g;
        return nullptr;
    }

    std::size_t live_count() const
    {
        std::size_t n = 0;
        for (const auto &g : goals)
            if (!goal_status_is_terminal(g.status)) n++;
        return n;
    }

    /* Append one record to a live goal's open stream. Ephemeral mode is a no-op success (nothing to journal).
     * Returns false if the stream is missing or the append did not make it durable (fail-closed). */
    bool journal(const std::string &id, const std::string &line)
    {
        if (!durable()) return true;
        if (line.empty()) return false; /* a serializer OOM -> refuse (do not apply a non-journaled change) */
        auto it = wals.find(id);
        if (it == wals.end() || !it->second) return false;
        return hc_wal_append(it->second, line.c_str(), line.size()) == 0;
    }

    /* Bound retained terminal goals: drop the oldest terminal goal while over the cap (its stream is already
     * removed, so this only frees memory; a live goal is never dropped). */
    void prune_settled()
    {
        std::size_t terminal = 0;
        for (const auto &g : goals)
            if (goal_status_is_terminal(g.status)) terminal++;
        while (terminal > kRetainSettled) {
            for (std::size_t i = 0; i < goals.size(); i++) {
                if (goal_status_is_terminal(goals[i].status)) {
                    goals.erase(goals.begin() + static_cast<std::ptrdiff_t>(i));
                    terminal--;
                    break;
                }
            }
        }
    }
};

GoalStore::GoalStore(std::string wal_dir) : p_(std::make_unique<Impl>())
{
    p_->wal_dir = std::move(wal_dir);
}

GoalStore::~GoalStore()
{
    /* Close handles but LEAVE non-settled streams on disk for the next restart's recover_incomplete(). */
    for (auto &kv : p_->wals)
        if (kv.second) hc_wal_close(kv.second);
}

std::optional<Goal> GoalStore::create(const std::string &title, const std::string &text,
                                      const std::string &source, const std::string &session_id)
{
    if (p_->live_count() >= kMaxLiveGoals) return std::nullopt;

    Goal g;
    {
        /* POSIX time()+getpid()+a per-store counter -> unique within and across runs; the id is digits +
         * hyphens ONLY, so it is traversal-safe by construction (hc_wal_open re-validates regardless) AND stable
         * for the conductor's model to echo back into update_goal / run_agenda (the [0-9-] charset is load-bearing
         * for that round-trip). */
        char buf[64];
        std::snprintf(buf, sizeof buf, "goal-%lld-%d-%llu", static_cast<long long>(std::time(nullptr)),
                      static_cast<int>(getpid()), static_cast<unsigned long long>(p_->counter++));
        g.id = buf;
    }
    g.title = title;
    g.text = text;
    g.source = source;
    g.session_id = session_id;
    g.status = GoalStatus::Active; /* a created goal is an active pursuit (past the Discussing phase) */

    if (p_->durable()) {
        hc_wal *w = hc_wal_open(p_->wal_dir.c_str(), g.id.c_str());
        if (!w) return std::nullopt; /* unsafe id (cannot happen with our generator) or OOM */
        std::string line = rec_open(g);
        if (line.empty() || hc_wal_append(w, line.c_str(), line.size()) != 0) {
            hc_wal_close(w);
            hc_wal_remove(p_->wal_dir.c_str(), g.id.c_str()); /* nothing durable -> clean up */
            return std::nullopt;
        }
        p_->wals[g.id] = w;
    }
    p_->goals.push_back(g);
    return g;
}

bool GoalStore::set_status(const std::string &goal_id, GoalStatus status)
{
    Goal *g = p_->find(goal_id);
    if (!g) return false;
    if (g->status == status) return true; /* no-op */
    /* A terminal goal is FINAL — never revived. The durable path already refuses (its stream is gone, so the
     * journal append misses and fail-closes); this guard makes ephemeral mode agree and keeps the live count
     * within kMaxLiveGoals on every path (a settled goal cannot be flipped back to live). */
    if (goal_status_is_terminal(g->status)) return false;

    if (goal_status_is_terminal(status)) {
        if (!p_->journal(goal_id, rec_settled(status))) return false; /* fail-closed */
        auto it = p_->wals.find(goal_id);
        if (it != p_->wals.end()) {
            if (it->second) hc_wal_close(it->second);
            if (p_->durable()) hc_wal_remove(p_->wal_dir.c_str(), goal_id.c_str());
            p_->wals.erase(it);
        }
        g->status = status; /* `g` is used here BEFORE prune_settled may invalidate it */
        p_->prune_settled();
    } else {
        if (!p_->journal(goal_id, rec_status(status))) return false; /* fail-closed */
        g->status = status;
    }
    return true;
}

bool GoalStore::attach_agenda(const std::string &goal_id, const std::string &agenda_id)
{
    Goal *g = p_->find(goal_id);
    if (!g) return false;
    if (std::find(g->agenda_ids.begin(), g->agenda_ids.end(), agenda_id) != g->agenda_ids.end())
        return true; /* idempotent — already attached, nothing to journal */
    if (g->agenda_ids.size() >= kMaxAgendasPerGoal) return false; /* bound a goal's agenda fan-out */
    if (!p_->journal(goal_id, rec_agenda(agenda_id))) return false; /* fail-closed */
    g->agenda_ids.push_back(agenda_id);
    return true;
}

std::vector<Goal> GoalStore::list() const { return p_->goals; }

std::optional<Goal> GoalStore::get(const std::string &goal_id) const
{
    const Goal *g = p_->find(goal_id);
    if (!g) return std::nullopt;
    return *g;
}

std::size_t GoalStore::live_count() const { return p_->live_count(); }

void GoalStore::adopt(std::vector<Goal> recovered)
{
    for (auto &g : recovered) {
        if (goal_status_is_terminal(g.status)) continue;   /* a settled goal should not be here */
        if (p_->live_count() >= kMaxLiveGoals) break;       /* respect the cap */
        if (p_->find(g.id)) continue;                       /* already adopted */
        if (p_->durable()) {
            hc_wal *w = hc_wal_open(p_->wal_dir.c_str(), g.id.c_str());
            if (!w) continue;
            std::string line = rec_open(g); /* re-baseline so a second crash still has a complete picture */
            if (line.empty() || hc_wal_append(w, line.c_str(), line.size()) != 0) {
                hc_wal_close(w);
                continue; /* could not re-baseline -> skip; the stream is left for the next attempt */
            }
            p_->wals[g.id] = w;
        }
        p_->goals.push_back(std::move(g));
    }
}

std::vector<Goal> GoalStore::recover_incomplete(const std::string &wal_dir)
{
    std::vector<Goal> out;
    if (wal_dir.empty()) return out;

    std::vector<std::string> ids;
    hc_wal_list(
        wal_dir.c_str(),
        [](const char *id, void *u) {
            static_cast<std::vector<std::string> *>(u)->emplace_back(id);
            return 0;
        },
        &ids);

    for (const auto &sid : ids) {
        std::vector<std::string> lines;
        hc_wal_replay(
            wal_dir.c_str(), sid.c_str(),
            [](const char *line, std::size_t len, void *u) {
                static_cast<std::vector<std::string> *>(u)->emplace_back(line, len);
                return 0;
            },
            &lines);

        GoalFold f = fold(lines);
        if (!f.valid || f.settled || goal_status_is_terminal(f.goal.status)) {
            hc_wal_remove(wal_dir.c_str(), sid.c_str()); /* settled / invalid -> prune (idempotent) */
            continue;
        }
        out.push_back(std::move(f.goal));
    }
    return out;
}

} // namespace hc::conductor
