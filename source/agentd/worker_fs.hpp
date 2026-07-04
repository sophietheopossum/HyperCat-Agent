#ifndef HC_AGENTD_WORKER_FS_HPP
#define HC_AGENTD_WORKER_FS_HPP

/* worker_fs — the sandbox-backed file operations behind the worker's fs_* tools (read / list, and the
 * fs_update edit core in W1.2), separated from worker_tools so the I/O + formatting logic is testable in
 * isolation against a real hc_sandbox jail with NO bus / llm / agent deps. One responsibility: turn a
 * jailed path + an operation into the bounded result string the tool hands the model. ALL path confinement
 * is hc_sandbox's (O_NOFOLLOW per component; `..` / symlink / absolute rejected) — these helpers add only
 * the bounded read, the directory formatting, and (W1.2) the in-memory edit. agentd-internal (no ABI).
 * Threading: synchronous, single-threaded (called inside one hc_agent_run tool dispatch). */

#include <cstddef>
#include <string>

struct hc_sandbox; /* opaque — hc_sandbox.h */

namespace hc {

constexpr size_t kFsReadMaxBytes = 256u * 1024; /* read cap — symmetric with the fs_write cap         */
constexpr size_t kFsListMaxBytes = 240u * 1024; /* rendered-listing cap (under the worker bus frame cap) */

/* Read a workspace file (O_RDONLY, jailed) into a bounded string. Returns the file contents (plus a
 * truncation sentinel if it exceeded the cap), "(empty file)" for a 0-byte file, or "error: <reason>" on
 * any sandbox failure (not-found / escape / symlink / read error). Never throws; safe with an untrusted
 * path (the sandbox confines it). */
std::string fs_read_text(hc_sandbox *sb, const std::string &path);

/* List a workspace directory: one entry per line, sorted by name, bounded — a directory renders as
 * "<name>/", a file as "<name>\t<size>". "." lists the workspace root. "(empty)" for an empty dir, or
 * "error: <reason>" on a sandbox failure. */
std::string fs_list_text(hc_sandbox *sb, const std::string &path);

/* Read the RAW bytes of a workspace file (NO "(empty file)"/truncation sentinels) for the fs_update
 * read-modify-write path. true + `out` on success; false + an "error: <reason>" in `err` on a sandbox
 * failure, a missing file (fs_update edits an EXISTING file — use fs_write to create), or a file larger
 * than the read cap (an edit must see the whole file, never a truncated view). */
bool fs_read_raw(hc_sandbox *sb, const std::string &path, std::string &out, std::string &err);

/* Ensure the PARENT directories of a workspace file `path` exist, creating any missing ones inside the jail
 * (hc_sandbox_mkdirs — mkdir -p, O_NOFOLLOW per component, no escape). A flat path (no '/') has no parent →
 * a no-op success. This lets fs_write materialize a deliverable into a not-yet-existing subdirectory (e.g.
 * "src/app.py"): without it the write fails with "path component does not exist" and the model loops retrying.
 * true on success (or no parent needed); false + an "error: <reason>" in `err` on a sandbox failure. */
bool fs_ensure_parent_dirs(hc_sandbox *sb, const std::string &path, std::string &err);

/* Apply the fs_update edit in memory (PURE — no I/O; the unit-test surface). mode "replace": `old_string`
 * must occur EXACTLY ONCE in `current` (count 0 → not found; >1 → ambiguous; empty → rejected). mode
 * "append": `append_text` (non-empty) is concatenated. true + `out` (the new content) on success; false +
 * an "error: <reason>" in `err` otherwise. The size policy (the write cap) is the caller's. */
bool fs_apply_edit(const std::string &current, const std::string &mode, const std::string &old_string,
                   const std::string &new_string, const std::string &append_text, std::string &out,
                   std::string &err);

} // namespace hc

#endif /* HC_AGENTD_WORKER_FS_HPP */
