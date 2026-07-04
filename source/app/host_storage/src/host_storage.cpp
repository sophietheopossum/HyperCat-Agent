/* host_storage — see host_storage.hpp. Data-dir resolution (XDG), the 0700 create, and the advisory
 * single-host lock; falls back to the run's throwaway temp dir when ephemeral or contended. */

#include "host_storage.hpp"

#include "hc_fs.h" /* hc_fs_mkdirs / hc_fs_read_file / hc_fs_atomic_write */

#include <cctype> /* isalnum — the conductor-session-id whitelist */
#include <cstdio> /* rename, fprintf — the legacy migration */
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h> /* flock */
#include <sys/stat.h> /* lstat — validate the resolved data dir is host-private */
#include <unistd.h>

namespace hcapp {

/* The data dir lives at a PREDICTABLE path, so (unlike mkdtemp's atomic 0700) it could be pre-created by
 * an attacker — a same-uid one with loose perms, another uid, or a symlink redirecting the whole tree.
 * hc_fs_mkdirs leaves an existing dir's mode untouched and follows symlinks on components, so we must
 * re-validate the FINAL dir before trusting it: it must be a real dir we OWN, 0700 (no group/other), and
 * NOT a symlink. Fail closed -> the caller falls back to ephemeral (or, for the WAL subdir, disables
 * journaling). This is the same-uid v1 bar (an intermediate symlinked PARENT and the validate-then-use
 * race are accepted under that model — an attacker who controls a dir in our $HOME is already inside the
 * boundary; see EXPANSIONS for the O_NOFOLLOW-dirfd hardening). A trailing slash is trimmed first, else
 * lstat would FOLLOW a symlink. Exposed (header) so the WAL provisioning re-validates its subdir too. */
bool dir_is_host_private(const std::string &path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back(); /* lstat("link/") resolves the symlink — strip it */
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return false;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != geteuid()) return false;
    return (st.st_mode & (S_IRWXG | S_IRWXO)) == 0; /* owner-only => 0700-equivalent */
}

bool conductor_session_id_ok(const std::string &id)
{
    if (id.empty() || id.size() > 80) return false;
    for (char c : id)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false; /* => no '/', '.', '..' */
    return true;
}

std::string read_conductor_session(const std::string &project_dir)
{
    if (project_dir.empty()) return std::string();
    std::string path = project_dir + "/conductor_session";
    size_t      len = 0;
    char       *data = hc_fs_read_file(path.c_str(), 256, &len); /* one short line; tiny cap (untrusted same-uid file) */
    if (!data) return std::string();
    std::string id(data, len);
    free(data);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' || id.back() == '\t'))
        id.pop_back();
    return conductor_session_id_ok(id) ? id : std::string();
}

void write_conductor_session(const std::string &project_dir, const std::string &id)
{
    if (project_dir.empty() || id.empty()) return;
    std::string path = project_dir + "/conductor_session";
    hc_fs_atomic_write(path.c_str(), id.data(), id.size()); /* temp+fsync+rename — no torn pointer on a crash */
}

std::string read_project_persona(const std::string &project_dir)
{
    if (project_dir.empty()) return std::string();
    std::string path = project_dir + "/conductor_persona";
    size_t      len = 0;
    /* generous read cap — validate_persona enforces the real 8 KiB bound at assembly; this just bounds a
     * planted/huge file. NOT validated here (the host sanitizes at the prompt-assembly trust boundary). */
    char *data = hc_fs_read_file(path.c_str(), 64u * 1024, &len);
    if (!data) return std::string();
    std::string text(data, len);
    free(data);
    return text;
}

void write_project_persona(const std::string &project_dir, const std::string &text)
{
    if (project_dir.empty()) return;
    std::string path = project_dir + "/conductor_persona";
    if (text.empty()) {           /* clearing the override falls the project back to the global default */
        unlink(path.c_str());     /* best-effort; absent file is fine */
        return;
    }
    hc_fs_atomic_write(path.c_str(), text.data(), text.size()); /* temp+fsync+rename */
}

namespace {

StorageRoots roots_under(const std::string &base, bool ephemeral, int lock_fd)
{
    StorageRoots r;
    r.data_dir = base;
    r.sessions = base + "/sessions";
    r.artifacts = base + "/artifacts";
    r.memory = base + "/memory";
    r.workspaces = base + "/workspaces";
    r.wal = base + "/agendas"; /* P14 durable agendas — provisioned + validated like the workspace root */
    r.skills = base + "/skills"; /* Wave 6 (reserved); populated for layout consistency, no store opens it in P3.1 */
    r.ephemeral = ephemeral;
    r.lock_fd = lock_fd;
    return r;
}

} // namespace

std::string resolve_data_dir()
{
    if (const char *d = getenv("HC_DATA_DIR"); d && *d) return d;
    if (const char *x = getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/hypercat";
    if (const char *h = getenv("HOME"); h && *h) return std::string(h) + "/.local/share/hypercat";
    return "";
}

int acquire_host_lock(const std::string &data_dir)
{
    std::string lockpath = data_dir + "/.lock";
    int         fd = open(lockpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { /* another host holds it (or EINTR) — fail closed */
        close(fd);
        return -1;
    }
    return fd; /* held until the caller close()s it */
}

StorageRoots open_host_storage(bool want_ephemeral, const std::string &socket_temp_dir)
{
    if (want_ephemeral) return roots_under(socket_temp_dir, /*ephemeral=*/true, -1);

    std::string data = resolve_data_dir();
    if (data.empty() || hc_fs_mkdirs(data.c_str()) != 0)
        return roots_under(socket_temp_dir, true, -1); /* cannot resolve/create -> ephemeral */

    /* refuse a pre-existing dir that isn't ours+0700+a-real-dir (a planted/loose/symlinked path) — only
     * then is it safe to write the durable stores + the .lock here */
    if (!dir_is_host_private(data)) return roots_under(socket_temp_dir, true, -1);

    int lock = acquire_host_lock(data);
    if (lock < 0) return roots_under(socket_temp_dir, true, -1); /* another host owns it -> ephemeral */

    return roots_under(data, /*ephemeral=*/false, lock);
}

StorageRoots open_project_storage(const std::string &project_dir, bool ephemeral)
{
    /* The project subtree carries the same subroots one level deeper. lock_fd stays -1 (the data-dir lock is
     * held globally); ephemeral is the caller's global flag. The parent project_dir was created + validated
     * host-private by hc_projects; the per-store *_open calls create each subdir 0700. */
    return roots_under(project_dir, ephemeral, -1);
}

void migrate_legacy_to_default(const std::string &data_dir)
{
    std::string projects = data_dir + "/projects";
    struct stat st;
    if (lstat(projects.c_str(), &st) == 0) return; /* projects/ already present => migrated or fresh; never touch */

    /* the six legacy STORE subtrees a pre-projects install kept directly under data_dir (NOT settings/roles.json,
     * which stay global) — `conductor_goals` rides along since it lived beside them */
    static const char *const kLegacy[] = {"sessions",   "artifacts",       "memory",
                                          "workspaces", "agendas",         "conductor_goals"};
    bool any = false;
    for (const char *name : kLegacy)
        if (dir_is_host_private(data_dir + "/" + name)) { any = true; break; }
    if (!any) return; /* a brand-new install — nothing to migrate (hc_projects_open will create projects/) */

    std::string def = projects + "/default";
    if (hc_fs_mkdirs(def.c_str()) != 0 || !dir_is_host_private(projects) || !dir_is_host_private(def)) {
        /* fail closed: a planted/loose/symlinked projects(/default) — leave the legacy data in place, untouched */
        return;
    }
    int moved = 0;
    for (const char *name : kLegacy) {
        std::string src = data_dir + "/" + name;
        std::string dst = def + "/" + name;
        if (!dir_is_host_private(src)) continue;          /* absent, or a planted symlink -> skip (never move) */
        if (lstat(dst.c_str(), &st) == 0) continue;       /* a partial prior migration already placed it -> skip */
        if (rename(src.c_str(), dst.c_str()) == 0) moved++; /* atomic within the one filesystem (both under data_dir) */
    }
    if (moved)
        std::fprintf(stderr, "host: migrated %d legacy store(s) into projects/default\n", moved);
}

} // namespace hcapp
