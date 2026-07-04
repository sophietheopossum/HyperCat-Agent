#ifndef HC_HOST_STORAGE_HPP
#define HC_HOST_STORAGE_HPP

/* host_storage — decide WHERE the host keeps its data and guard single-host access to it.
 *
 * Purpose:   the durable stores (sessions, artifacts, memory) + the agent workspace must survive a run
 *            so the operator can retrieve outputs, sessions can be replayed, and memory persists ACROSS
 *            runs (the point of the Memory wave). This module resolves a persistent, 0700 host-private
 *            data dir (HC_DATA_DIR > $XDG_DATA_HOME/hypercat > $HOME/.local/share/hypercat) and the four
 *            subroots under it, and acquires an advisory single-host lock — the stores are single-writer,
 *            so two hosts on one data dir would corrupt the shared logs. `--ephemeral` (or a lock-
 *            contended / unresolvable dir) puts the roots back under the run's throwaway temp dir.
 * Owns:      nothing durable — it returns paths + an open lock fd the caller (main.cpp) holds for the
 *            host's lifetime and closes on teardown. The bus socket stays in the caller's mkdtemp dir.
 * Threading: called once at startup on the host thread; not thread-safe (no shared state).
 * Security:  the data dir is created 0700 (host-private — the same bar mkdtemp gives the socket dir); the
 *            flock prevents two hosts from racing the single-writer stores. No secret is stored here. */

#include <string>
#include <utility>

namespace hcapp {

/* Owns the held single-host lock fd, so it is MOVE-ONLY: a copy would duplicate the fd and lead to a
 * double-close / leak. The move zeroes the source's lock_fd; the caller closes the final owner's fd once
 * at teardown. */
struct StorageRoots {
    std::string sessions;   /* hc_store root      */
    std::string artifacts;  /* hc_artifacts root  */
    std::string memory;     /* hc_memory root     */
    std::string workspaces; /* hc_sandbox jail root (per-agent subdirs are created under it by the host) */
    std::string wal;        /* hc_wal root for durable agendas (P14; outside every agent sandbox)         */
    std::string skills;     /* per-project skills root (Wave 6; RESERVED — no store opens it in P3.1)     */
    std::string data_dir;   /* the parent (persistent dir, or the temp dir when ephemeral)              */
    bool        ephemeral = false; /* true => roots are under the throwaway temp dir (cleaned with it)   */
    int         lock_fd = -1;      /* the held single-host lock (persistent only); close() to release    */

    StorageRoots() = default;
    StorageRoots(const StorageRoots &) = delete;
    StorageRoots &operator=(const StorageRoots &) = delete;
    StorageRoots(StorageRoots &&o) noexcept { *this = std::move(o); }
    StorageRoots &operator=(StorageRoots &&o) noexcept
    {
        if (this == &o) return *this; /* self-move: don't zero our own fd */
        sessions = std::move(o.sessions);
        artifacts = std::move(o.artifacts);
        memory = std::move(o.memory);
        workspaces = std::move(o.workspaces);
        wal = std::move(o.wal);
        skills = std::move(o.skills);
        data_dir = std::move(o.data_dir);
        ephemeral = o.ephemeral;
        lock_fd = o.lock_fd;
        o.lock_fd = -1; /* the moved-from no longer owns the fd */
        return *this;
    }
};

/* Resolve the persistent data dir: HC_DATA_DIR > $XDG_DATA_HOME/hypercat > $HOME/.local/share/hypercat.
 * Returns "" if none can be resolved (HOME unset + no override). Does NOT create it. Exposed for tests. */
std::string resolve_data_dir();

/* True iff `path` is a real directory we OWN, 0700 (no group/other bits), and NOT a symlink. The host-
 * private bar for a predictable-path dir (vs mkdtemp's atomic 0700). Used to re-validate the data dir AND
 * any durable subdir provisioned under it (e.g. the WAL dir) before trusting it — a same-uid process could
 * have pre-created it loose or as a symlink. A trailing slash is trimmed first (else lstat follows a link). */
bool dir_is_host_private(const std::string &path);

/* Exclusive, non-blocking advisory lock on `<data_dir>/.lock`. Returns an open fd that HOLDS the lock
 * (close to release), or -1 if another host already holds it / on error. Exposed for tests. */
int acquire_host_lock(const std::string &data_dir);

/* P1 conductor conversations: the per-project ACTIVE-conversation pointer is a tiny host-private file
 * `<project_dir>/conductor_session` (one line = the session id) so a launch auto-resumes the last conversation.
 * `conductor_session_id_ok` validates an id as a single path component (a whitelist => no '/', '.', '..'; bounded)
 * BEFORE it reaches hc_session_load — defense in depth (hc_session_load also rejects '/'+'..'). `read_` returns ""
 * (=> fresh) on an absent/oversized/invalid file; `write_` is atomic (temp+fsync+rename). An empty project_dir
 * (ephemeral) => read "" / write no-op. Exposed for tests. */
bool        conductor_session_id_ok(const std::string &id);
std::string read_conductor_session(const std::string &project_dir);
void        write_conductor_session(const std::string &project_dir, const std::string &id);

/* The per-project conductor PERSONA override (voice/demeanour text) — a tiny host-private file
 * `<project_dir>/conductor_persona` (raw UTF-8, possibly multi-line). `read_` returns the raw file content
 * (capped; NOT validated here — the caller sanitizes it via validate_persona at prompt assembly, the trust
 * boundary), or "" on an absent/oversized file => fall back to the GLOBAL default persona. `write_` replaces
 * it atomically; an EMPTY text REMOVES the override (so the project falls back to the global default). An
 * empty project_dir (ephemeral) => read "" / write no-op. The CALLER owns the guarantee that project_dir is a
 * host-private project root (it is — every call site passes an id_ok'd hc_projects_dir result); the write is
 * hc_fs_atomic_write (temp O_NOFOLLOW + rename-replace), so a same-uid attacker pre-planting the file as a
 * symlink has it replaced, not followed. Exposed for tests. */
std::string read_project_persona(const std::string &project_dir);
void        write_project_persona(const std::string &project_dir, const std::string &text);

/* Decide + provision the host's storage roots. `want_ephemeral` (the --ephemeral flag), OR a data dir
 * that cannot be resolved/created, OR a lock already held by another host => roots under
 * `socket_temp_dir` with .ephemeral=true and .lock_fd=-1 (the caller warns on a lock-contended
 * fallback). Otherwise the persistent tree under the resolved data dir (created 0700) with .lock_fd set.
 * The caller closes .lock_fd on teardown; the per-store subdirs are created by each store's *_open. */
StorageRoots open_host_storage(bool want_ephemeral, const std::string &socket_temp_dir);

/* Re-root the storage layout under a single PROJECT subtree (Wave 3): the same subroots as open_host_storage
 * but one level deeper, at `project_dir` (= `<data_dir>/projects/<id>`), PLUS a per-project `skills/` root. The
 * project dir was already created + validated host-private by hc_projects (its open/create enforce that); each
 * store's *_open creates its own subdir 0700 as usual. `ephemeral` is the GLOBAL flag (it cannot be inferred
 * from the path) — it drives the WAL/conductor "no journaling" branches. lock_fd is ALWAYS -1: the single-host
 * lock is the DATA-DIR's, held once globally by open_host_storage; a project subtree takes no second lock. */
StorageRoots open_project_storage(const std::string &project_dir, bool ephemeral);

/* One-time legacy migration (Wave 3): if `<data_dir>/projects/` is ABSENT but legacy store subtrees exist
 * directly under data_dir (a pre-projects install), move {sessions,artifacts,memory,workspaces,agendas,
 * conductor_goals} into `<data_dir>/projects/default/` so existing data isn't stranded by the re-root. Atomic
 * per-subtree (rename within one filesystem); each source is `dir_is_host_private`-gated (a planted symlink is
 * skipped, never moved); best-effort + idempotent (projects/ present => no-op). Persistent mode only; the
 * caller invokes it AFTER open_host_storage (lock held) and BEFORE hc_projects_open (which creates projects/). */
void migrate_legacy_to_default(const std::string &data_dir);

} // namespace hcapp

#endif /* HC_HOST_STORAGE_HPP */
