/* test_goal_store — Conductor P3a: durable goals on hc_wal (offline, no LLM / network). Exercises the
 * GoalStore mechanism + the pure recovery fold:
 *   - ephemeral mode (wal_dir="")       => in-memory create/get/list/set_status/attach_agenda, no files
 *   - durable round-trip + resume       => mutate, destroy, recover_incomplete + adopt rebuild the state
 *   - multi-transition fold             => the latest status + the union of agenda_ids survive
 *   - settle                            => the stream is removed and the goal is NOT recovered next run
 *   - corrupt / forged WAL lines        => skipped, not fatal; a delta before any `open` is ignored
 *   - a forged inner id can't escape    => recover surfaces it but adopt refuses to open an unsafe stream
 *   - the live-goal cap                 => the (cap+1)th create fails; settling a goal frees a slot
 * Run under ASan/UBSan. Exit non-zero on failure. */

#include "hc_goal.hpp"

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace hc::conductor;

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

/* Remove every entry under `dir` then the dir itself (test temp cleanup; POSIX, no std::filesystem). */
static void rm_rf(const std::string &dir)
{
    if (DIR *d = opendir(dir.c_str())) {
        for (struct dirent *e = readdir(d); e; e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            unlink((dir + "/" + n).c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

static std::string wal_path(const std::string &dir, const std::string &stream_id)
{
    return dir + "/" + stream_id + ".wal";
}

static void append_line(const std::string &path, const std::string &line)
{
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << line << "\n";
}

static bool file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int main(void)
{
    /* 1) ephemeral: no wal_dir -> pure in-memory; create/get/list/set_status/attach_agenda all work. */
    {
        GoalStore gs("");
        auto      g = gs.create("Ship v1", "Make it shippable", "operator", "sess-1");
        CHECK(g.has_value(), "ephemeral create returns a goal");
        CHECK(g->status == GoalStatus::Active, "a created goal is Active");
        CHECK(!g->id.empty(), "the goal has a generated id");
        CHECK(gs.list().size() == 1, "list shows the one goal");
        CHECK(gs.get(g->id).has_value(), "get finds the goal by id");
        CHECK(gs.attach_agenda(g->id, "ag1"), "attach an agenda");
        CHECK(gs.attach_agenda(g->id, "ag1"), "re-attaching the same agenda is an idempotent success");
        CHECK(gs.get(g->id)->agenda_ids.size() == 1, "the agenda is attached exactly once");
        CHECK(gs.set_status(g->id, GoalStatus::Paused), "set status Paused");
        CHECK(gs.get(g->id)->status == GoalStatus::Paused, "the status updated");
        CHECK(!gs.set_status("no-such-id", GoalStatus::Done), "set_status on an unknown id fails");
        CHECK(!gs.attach_agenda("no-such-id", "agX"), "attach_agenda on an unknown id fails");
    }

    /* 2) durable round-trip + resume: mutate, destroy, recover_incomplete + adopt rebuild the goal. */
    char tmpl[] = "/tmp/hc_goal_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "temp wal dir created");
    std::string wdir = dir ? dir : "";
    std::string saved_id;
    {
        GoalStore gs(wdir);
        auto      g = gs.create("Durable goal", "survive a restart", "operator", "sess-42");
        CHECK(g.has_value(), "durable create returns a goal");
        saved_id = g->id;
        CHECK(file_exists(wal_path(wdir, saved_id)), "the goal's WAL stream exists on disk");
        CHECK(gs.attach_agenda(saved_id, "ag1"), "attach ag1");
        CHECK(gs.attach_agenda(saved_id, "ag2"), "attach ag2");
        CHECK(gs.set_status(saved_id, GoalStatus::Paused), "set Paused");
    } /* destroy: closes the handle, leaves the (non-settled) stream for recovery */

    {
        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.size() == 1, "recover_incomplete finds the one incomplete goal");
        if (recovered.size() == 1) {
            const Goal &r = recovered[0];
            CHECK(r.id == saved_id, "the recovered id matches");
            CHECK(r.title == "Durable goal", "title survived");
            CHECK(r.text == "survive a restart", "text survived");
            CHECK(r.source == "operator", "source survived");
            CHECK(r.session_id == "sess-42", "session_id survived");
            CHECK(r.status == GoalStatus::Paused, "the latest status (Paused) survived");
            CHECK(r.agenda_ids.size() == 2, "both agenda ids survived");
        }
        /* adopt into a fresh store -> it is live again + journaling continues */
        GoalStore gs2(wdir);
        gs2.adopt(recovered);
        CHECK(gs2.list().size() == 1, "the adopted goal is live in the new store");
        CHECK(gs2.set_status(saved_id, GoalStatus::Done), "the adopted goal can transition (journaling resumed)");
    }

    /* 3) settle: the stream is removed and the goal is NOT recovered on the next run. */
    {
        CHECK(!file_exists(wal_path(wdir, saved_id)), "settling (Done) removed the goal's WAL stream");
        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.empty(), "a settled goal is not recovered");
    }

    /* 4) multi-transition fold across many deltas -> the latest status wins, agenda ids union. */
    {
        std::string id2;
        {
            GoalStore gs(wdir);
            auto      g = gs.create("Multi", "many deltas", "conductor", "sess-7");
            id2 = g->id;
            gs.set_status(id2, GoalStatus::Paused);
            gs.set_status(id2, GoalStatus::Active);
            gs.attach_agenda(id2, "a1");
            gs.attach_agenda(id2, "a2");
            gs.attach_agenda(id2, "a3");
        }
        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.size() == 1, "the multi-delta goal recovers as one goal");
        if (recovered.size() == 1) {
            CHECK(recovered[0].status == GoalStatus::Active, "the last status (Active) wins over earlier ones");
            CHECK(recovered[0].agenda_ids.size() == 3, "all three agenda ids are folded in");
        }
        /* clean it up so later cases start from an empty dir */
        GoalStore gs(wdir);
        gs.adopt(recovered);
        if (!recovered.empty()) gs.set_status(recovered[0].id, GoalStatus::Abandoned);
    }

    /* 5) corrupt + forged lines are skipped; a valid delta still applies. */
    {
        std::string id3;
        {
            GoalStore gs(wdir);
            id3 = gs.create("Corrupt", "robustness", "operator", "s")->id;
        } /* the stream now holds exactly one valid `open` */
        const std::string path = wal_path(wdir, id3);
        append_line(path, "this is not json at all");                 /* garbage -> skipped */
        append_line(path, "{\"t\":\"status\",\"status\":\"paused\"}"); /* valid delta -> applied */
        append_line(path, "{ broken json ");                          /* garbage -> skipped */
        append_line(path, "{\"t\":\"bogus\",\"x\":1}");                /* unknown type -> ignored */

        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.size() == 1, "the goal survives interleaved corrupt lines");
        if (recovered.size() == 1)
            CHECK(recovered[0].status == GoalStatus::Paused, "the one valid delta applied; garbage was skipped");
        GoalStore gs(wdir);
        gs.adopt(recovered);
        if (!recovered.empty()) gs.set_status(recovered[0].id, GoalStatus::Abandoned); /* cleanup */
    }

    /* 6) a stream of deltas with NO `open` is invalid -> ignored + pruned. */
    {
        const std::string path = wal_path(wdir, "orphan");
        append_line(path, "{\"t\":\"status\",\"status\":\"active\"}");
        append_line(path, "{\"t\":\"agenda\",\"agenda_id\":\"ag9\"}");
        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.empty(), "a stream with no `open` synthesizes no goal");
        CHECK(!file_exists(path), "the invalid stream was pruned (removed)");
    }

    /* 7) a forged INNER id (traversal) is surfaced by recover but adopt refuses to open an unsafe stream. */
    {
        const std::string path = wal_path(wdir, "forged");
        append_line(path, "{\"t\":\"open\",\"id\":\"../escape\",\"title\":\"x\",\"text\":\"\","
                          "\"status\":\"active\",\"session_id\":\"\",\"source\":\"\",\"agenda_ids\":[]}");
        auto recovered = GoalStore::recover_incomplete(wdir);
        CHECK(recovered.size() == 1, "recover folds the forged stream (valid JSON)");
        CHECK(recovered.size() == 1 && recovered[0].id == "../escape", "the forged inner id is surfaced as data");
        GoalStore gs(wdir);
        gs.adopt(recovered);
        CHECK(gs.list().empty(), "adopt REFUSES the goal whose id is not a safe stream id (no escape)");
        CHECK(!file_exists(wdir + "/../escape.wal"), "no WAL file escaped the dir");
        unlink(path.c_str()); /* cleanup the forged stream */
    }

    /* 8) the live-goal cap: creating beyond it fails; settling a goal frees a slot. */
    {
        GoalStore   gs(""); /* ephemeral keeps it fast */
        std::string first_id;
        int         made = 0;
        for (int i = 0; i < 300; i++) {
            auto g = gs.create("g", "x", "operator", "s");
            if (!g) break;
            if (made == 0) first_id = g->id;
            made++;
        }
        CHECK(made == 256, "create is bounded at the live-goal cap (256)");
        CHECK(!gs.create("over", "x", "operator", "s").has_value(), "the (cap+1)th create fails");
        CHECK(gs.set_status(first_id, GoalStatus::Done), "settle one goal");
        CHECK(gs.create("after", "x", "operator", "s").has_value(), "a freed slot allows a new create");
    }

    /* 9) a terminal goal is FINAL — it can never be revived (closes the ephemeral-mode cap back-door). */
    {
        GoalStore gs("");
        auto      g = gs.create("Final", "no revival", "operator", "s");
        CHECK(g.has_value(), "created");
        CHECK(gs.set_status(g->id, GoalStatus::Done), "transition to Done");
        CHECK(!gs.set_status(g->id, GoalStatus::Active), "reviving a Done goal to Active is refused");
        CHECK(!gs.set_status(g->id, GoalStatus::Failed), "moving a Done goal to another terminal is refused");
        CHECK(gs.get(g->id)->status == GoalStatus::Done, "the goal stays Done");
        CHECK(gs.live_count() == 0, "a settled goal is not live (revival cannot push past the cap)");
    }

    /* 10) a goal's agenda fan-out is bounded (defense-in-depth vs a corrupted WAL spiking recovery). */
    {
        GoalStore gs("");
        auto      g = gs.create("Fanout", "bounded", "operator", "s");
        int       ok = 0;
        for (int i = 0; i < 4100; i++)
            if (gs.attach_agenda(g->id, "ag-" + std::to_string(i))) ok++;
        CHECK(ok == 4096, "attach_agenda is bounded at kMaxAgendasPerGoal (4096)");
        CHECK(g.has_value() && gs.get(g->id)->agenda_ids.size() == 4096, "the goal holds exactly the cap");
    }

    rm_rf(wdir);

    if (g_fail) {
        std::fprintf(stderr, "test_goal_store: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_goal_store: all checks passed\n");
    return 0;
}
