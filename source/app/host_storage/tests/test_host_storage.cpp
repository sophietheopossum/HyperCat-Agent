/* test_host_storage — the persistence gate's offline unit: data-dir resolution precedence (HC_DATA_DIR >
 * XDG_DATA_HOME > HOME), the 0700 create, the single-host flock (acquire / contend / release), and the
 * open_host_storage decision (ephemeral vs persistent vs lock-contended fallback). No network. */

#include "host_storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                                 \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

int main()
{
    using namespace hcapp;

    /* preserve + clear the env vars we manipulate */
    std::string save_data = getenv("HC_DATA_DIR") ? getenv("HC_DATA_DIR") : "";
    std::string save_xdg = getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : "";
    std::string save_home = getenv("HOME") ? getenv("HOME") : "";
    unsetenv("HC_DATA_DIR");
    unsetenv("XDG_DATA_HOME");

    /* --- resolve_data_dir precedence --- */
    setenv("HOME", "/home/tester", 1);
    CHECK(resolve_data_dir() == "/home/tester/.local/share/hypercat", "default: HOME/.local/share");
    setenv("XDG_DATA_HOME", "/xdg/data", 1);
    CHECK(resolve_data_dir() == "/xdg/data/hypercat", "XDG_DATA_HOME wins over HOME");
    setenv("HC_DATA_DIR", "/explicit/dir", 1);
    CHECK(resolve_data_dir() == "/explicit/dir", "HC_DATA_DIR wins over all");
    unsetenv("HC_DATA_DIR");
    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");
    CHECK(resolve_data_dir().empty(), "no override + no HOME -> empty");

    /* --- acquire_host_lock: acquire, contend, release --- */
    char lockdir[] = "/tmp/hc_storage_lock_XXXXXX";
    CHECK(mkdtemp(lockdir) != nullptr, "mkdtemp lock dir");
    int fd1 = acquire_host_lock(lockdir);
    CHECK(fd1 >= 0, "first lock acquires");
    int fd2 = acquire_host_lock(lockdir);
    CHECK(fd2 < 0, "second lock on the same dir is REFUSED (single-host guard)");
    if (fd1 >= 0) close(fd1);
    int fd3 = acquire_host_lock(lockdir);
    CHECK(fd3 >= 0, "lock re-acquires after the first is released");
    if (fd3 >= 0) close(fd3);

    /* --- open_host_storage: ephemeral mode --- */
    {
        StorageRoots r = open_host_storage(/*want_ephemeral=*/true, "/tmp/sockdir");
        CHECK(r.ephemeral && r.lock_fd == -1, "ephemeral mode: flagged, no lock");
        CHECK(r.sessions == "/tmp/sockdir/sessions" && r.workspaces == "/tmp/sockdir/workspaces",
              "ephemeral roots are under the socket temp dir");
    }

    /* --- open_host_storage: persistent mode (HC_DATA_DIR -> a fresh dir), 0700, locked --- */
    char datadir[] = "/tmp/hc_storage_data_XXXXXX";
    CHECK(mkdtemp(datadir) != nullptr, "mkdtemp data dir");
    /* point HC_DATA_DIR at a NOT-yet-existing child so we exercise the 0700 create path */
    std::string want = std::string(datadir) + "/hypercat";
    setenv("HC_DATA_DIR", want.c_str(), 1);
    {
        StorageRoots r = open_host_storage(/*want_ephemeral=*/false, "/tmp/sockdir");
        CHECK(!r.ephemeral && r.lock_fd >= 0, "persistent mode: not ephemeral, lock held");
        CHECK(r.data_dir == want && r.memory == want + "/memory", "persistent roots under the data dir");
        struct stat st;
        CHECK(stat(want.c_str(), &st) == 0 && (st.st_mode & 0077) == 0, "data dir is 0700 (host-private)");

        /* a SECOND host on the same data dir must fall back to ephemeral (the lock is held) */
        StorageRoots r2 = open_host_storage(false, "/tmp/sockdir2");
        CHECK(r2.ephemeral && r2.lock_fd == -1, "second host falls back to ephemeral (lock contended)");
        CHECK(r2.sessions == "/tmp/sockdir2/sessions", "fallback roots under the second socket temp dir");

        if (r.lock_fd >= 0) close(r.lock_fd);
    }

    /* --- SECURITY: a pre-existing data dir with LOOSE perms is REFUSED (falls back to ephemeral) --- */
    {
        char loose[] = "/tmp/hc_storage_loose_XXXXXX";
        CHECK(mkdtemp(loose) != nullptr, "mkdtemp loose dir");
        chmod(loose, 0777); /* simulate an attacker pre-creating the predictable path world-writable */
        setenv("HC_DATA_DIR", loose, 1);
        StorageRoots r = open_host_storage(false, "/tmp/sockdir3");
        CHECK(r.ephemeral && r.lock_fd == -1, "a world-writable data dir is refused -> ephemeral");
        unsetenv("HC_DATA_DIR");
        rmdir(loose);
    }

    /* --- SECURITY: a SYMLINKED data dir is REFUSED (no store-tree redirection) --- */
    {
        char target[] = "/tmp/hc_storage_tgt_XXXXXX";
        CHECK(mkdtemp(target) != nullptr, "mkdtemp symlink target");
        std::string linkp = std::string(target) + "_link";
        CHECK(symlink(target, linkp.c_str()) == 0, "make HC_DATA_DIR a symlink");
        setenv("HC_DATA_DIR", linkp.c_str(), 1);
        StorageRoots r = open_host_storage(false, "/tmp/sockdir4");
        CHECK(r.ephemeral, "a symlinked data dir is refused -> ephemeral (no redirection)");
        /* a TRAILING SLASH must not let lstat follow the symlink */
        std::string linkslash = linkp + "/";
        setenv("HC_DATA_DIR", linkslash.c_str(), 1);
        StorageRoots rs = open_host_storage(false, "/tmp/sockdir5");
        CHECK(rs.ephemeral, "a symlinked data dir WITH a trailing slash is also refused");
        unsetenv("HC_DATA_DIR");
        unlink(linkp.c_str());
        rmdir(target);
    }

    /* --- open_project_storage: the per-project subtree layout (one level deeper, + skills/, no lock) --- */
    {
        StorageRoots r = open_project_storage("/data/projects/myproj", /*ephemeral=*/false);
        CHECK(r.data_dir == "/data/projects/myproj", "project data_dir == the project dir");
        CHECK(r.sessions == "/data/projects/myproj/sessions" && r.memory == "/data/projects/myproj/memory" &&
                  r.workspaces == "/data/projects/myproj/workspaces" &&
                  r.wal == "/data/projects/myproj/agendas" && r.artifacts == "/data/projects/myproj/artifacts",
              "project subroots are one level deeper");
        CHECK(r.skills == "/data/projects/myproj/skills", "the reserved per-project skills root (Wave 6)");
        CHECK(r.lock_fd == -1, "a project subtree holds NO lock (the data-dir lock is global)");
        CHECK(!r.ephemeral, "ephemeral flag passes through (false)");
        CHECK(open_project_storage("/x/projects/p", true).ephemeral, "ephemeral flag passes through (true)");
    }

    /* --- migrate_legacy_to_default: move legacy store dirs into projects/default/, idempotent, symlink-skip --- */
    {
        char md[] = "/tmp/hc_migrate_XXXXXX";
        CHECK(mkdtemp(md) != nullptr, "mkdtemp migration data dir");
        std::string base(md);
        for (const char *n : {"sessions", "memory", "agendas"}) { /* plant legacy subtrees + a marker each */
            std::string d = base + "/" + n;
            mkdir(d.c_str(), 0700);
            std::string f = d + "/marker";
            FILE       *fp = fopen(f.c_str(), "w");
            if (fp) { std::fputs("x", fp); fclose(fp); }
        }
        std::string victim = base + "/victim";
        mkdir(victim.c_str(), 0700);
        std::string symlegacy = base + "/workspaces"; /* a planted symlink legacy dir must be SKIPPED */
        CHECK(symlink(victim.c_str(), symlegacy.c_str()) == 0, "plant workspaces -> victim symlink");

        migrate_legacy_to_default(base);

        struct stat mst;
        CHECK(stat((base + "/projects/default/sessions/marker").c_str(), &mst) == 0,
              "sessions moved into projects/default (marker intact)");
        CHECK(stat((base + "/projects/default/memory/marker").c_str(), &mst) == 0, "memory moved into projects/default");
        CHECK(lstat((base + "/sessions").c_str(), &mst) != 0, "the original sessions dir is gone (renamed)");
        CHECK(lstat(symlegacy.c_str(), &mst) == 0 && S_ISLNK(mst.st_mode),
              "the planted symlink legacy dir was NOT moved (still a symlink at the old path)");
        CHECK(lstat((base + "/projects/default/workspaces").c_str(), &mst) != 0,
              "the symlink was not migrated into projects/default");
        migrate_legacy_to_default(base); /* idempotent: projects/ now present -> no-op */
        CHECK(stat((base + "/projects/default/sessions/marker").c_str(), &mst) == 0,
              "idempotent: a second run leaves the migrated data intact");
    }

    /* --- P1 conductor conversations: the active-conversation-id validator + the host-private file round-trip --- */
    {
        /* the validator: a real session id passes; traversal / out-of-charset / oversized fail */
        CHECK(conductor_session_id_ok("sess-1718-123-0042"), "a real session id is accepted");
        CHECK(!conductor_session_id_ok(""), "empty id rejected");
        CHECK(!conductor_session_id_ok("../etc/passwd"), "traversal id rejected ('/' and '.')");
        CHECK(!conductor_session_id_ok("a/b"), "id with '/' rejected");
        CHECK(!conductor_session_id_ok(".."), "'..' rejected");
        CHECK(!conductor_session_id_ok("a.b"), "id with '.' rejected (no dotted ids)");
        CHECK(!conductor_session_id_ok(std::string(81, 'a')), "over-long id (>80) rejected");

        char pd[] = "/tmp/hc_condsess_XXXXXX";
        CHECK(mkdtemp(pd) != nullptr, "mkdtemp project dir");
        std::string project_dir(pd);
        std::string path = project_dir + "/conductor_session";

        write_conductor_session(project_dir, "sess-aaa-111-0001"); /* round-trip */
        CHECK(read_conductor_session(project_dir) == "sess-aaa-111-0001", "write then read round-trips the id");

        { /* a trailing newline is trimmed on read */
            FILE *fp = fopen(path.c_str(), "w");
            if (fp) { std::fputs("sess-bbb-222-0002\n", fp); fclose(fp); }
        }
        CHECK(read_conductor_session(project_dir) == "sess-bbb-222-0002", "trailing newline trimmed on read");

        { /* a planted traversal payload is REFUSED on read (-> "" -> the conductor starts fresh) */
            FILE *fp = fopen(path.c_str(), "w");
            if (fp) { std::fputs("../../etc/passwd", fp); fclose(fp); }
        }
        CHECK(read_conductor_session(project_dir).empty(), "a planted '../' payload is refused on read");

        { /* an over-long id (readable but >80) is REFUSED by the validator */
            FILE *fp = fopen(path.c_str(), "w");
            if (fp) {
                for (int i = 0; i < 120; i++) std::fputc('a', fp);
                fclose(fp);
            }
        }
        CHECK(read_conductor_session(project_dir).empty(), "an over-long id payload is refused on read");

        CHECK(read_conductor_session("").empty(), "empty project_dir (ephemeral) reads empty");
        write_conductor_session("", "sess-x"); /* ephemeral write is a no-op (must not crash) */

        unlink(path.c_str());
        rmdir(pd);
    }

    { /* per-project persona override: write -> read round-trip; empty REMOVES it; ephemeral is a no-op */
        char pd[] = "/tmp/hc_persona_XXXXXX";
        CHECK(mkdtemp(pd) != nullptr, "mkdtemp persona project dir");
        std::string project_dir(pd);
        std::string ppath = project_dir + "/conductor_persona";

        CHECK(read_project_persona(project_dir).empty(), "absent persona reads empty");
        write_project_persona(project_dir, "Be playful and warm.\nUse short sentences.");
        CHECK(read_project_persona(project_dir) == "Be playful and warm.\nUse short sentences.",
              "multi-line persona round-trips verbatim (storage does not validate)");
        write_project_persona(project_dir, ""); /* clearing REMOVES the override -> read "" -> global default */
        CHECK(read_project_persona(project_dir).empty(), "empty write removes the override");

        CHECK(read_project_persona("").empty(), "empty project_dir (ephemeral) reads empty");
        write_project_persona("", "x"); /* ephemeral write is a no-op (must not crash) */

        unlink(ppath.c_str());
        rmdir(pd);
    }

    /* restore the env */
    unsetenv("HC_DATA_DIR");
    if (!save_data.empty()) setenv("HC_DATA_DIR", save_data.c_str(), 1);
    if (!save_xdg.empty()) setenv("XDG_DATA_HOME", save_xdg.c_str(), 1);
    if (!save_home.empty()) setenv("HOME", save_home.c_str(), 1);

    if (g_fails == 0) std::printf("test_host_storage: OK\n");
    return g_fails ? 1 : 0;
}
