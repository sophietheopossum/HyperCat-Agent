/* worker_fs — see worker_fs.hpp. The sandbox-backed file-op core for the worker's fs_read / fs_list tools
 * (and the fs_update edit core in W1.2). No bus / llm / agent deps — only hc_sandbox + libc I/O. */

#include "worker_fs.hpp"

#include "hc_sandbox.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hc {

namespace {

/* read() the whole fd into `out`, retrying EINTR, stopping once `cap` bytes are buffered (then `truncated`
 * is set and the rest of the file is left unread). false on a read error. */
bool read_fd_capped(int fd, std::string &out, size_t cap, bool &truncated)
{
    char buf[8192];
    truncated = false;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) break;
        size_t room = out.size() < cap ? cap - out.size() : 0;
        if ((size_t)r > room) {
            out.append(buf, room);
            truncated = true;
            break;
        }
        out.append(buf, (size_t)r);
    }
    return true;
}

/* Open a workspace file O_RDONLY through the jail AND confirm it is a regular file — a FIFO/device/socket
 * could otherwise block the read up to the task deadline (a self-inflicted hang; not model-reachable since
 * the worker only creates regular files, but a cheap defense-in-depth). Returns the fd, or -1 + an
 * "error: …" in `e`. */
hc_sandbox_fd open_regular_rdonly(hc_sandbox *sb, const std::string &path, std::string &e)
{
    hc_sandbox_fd     fd = HC_SANDBOX_FD_INVALID;
    hc_sandbox_status ss = hc_sandbox_open_fd(sb, path.c_str(), O_RDONLY, 0, &fd);
    if (ss != HC_SANDBOX_OK) {
        e = std::string("error: ") + hc_sandbox_strerror(ss);
        return HC_SANDBOX_FD_INVALID;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        e = "error: not a regular file";
        return HC_SANDBOX_FD_INVALID;
    }
    return fd;
}

} // namespace

std::string fs_read_text(hc_sandbox *sb, const std::string &path)
{
    if (path.empty()) return "error: fs_read needs a non-empty path";
    std::string   e;
    hc_sandbox_fd fd = open_regular_rdonly(sb, path, e);
    if (fd == HC_SANDBOX_FD_INVALID) return e;
    std::string out;
    bool        truncated = false;
    bool        ok = read_fd_capped(fd, out, kFsReadMaxBytes, truncated);
    close(fd);
    if (!ok) return "error: read failed";
    if (truncated) out += "\n[truncated at " + std::to_string(kFsReadMaxBytes) + " bytes]";
    if (out.empty()) return "(empty file)";
    return out;
}

std::string fs_list_text(hc_sandbox *sb, const std::string &path)
{
    const std::string  p = path.empty() ? "." : path;
    hc_sandbox_dirent *ents = nullptr;
    size_t             n = 0;
    hc_sandbox_status  ss = hc_sandbox_list(sb, p.c_str(), &ents, &n);
    if (ss != HC_SANDBOX_OK) {
        hc_sandbox_list_free(ents);
        return std::string("error: ") + hc_sandbox_strerror(ss);
    }
    /* sort by name for a deterministic listing (the sandbox returns raw filesystem order). */
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b) { return std::strcmp(ents[a].name, ents[b].name) < 0; });
    std::string out;
    for (size_t k = 0; k < n; k++) {
        const hc_sandbox_dirent &e = ents[idx[k]];
        std::string              line = e.is_dir ? (std::string(e.name) + "/\n")
                                                 : (std::string(e.name) + "\t" + std::to_string(e.size) + "\n");
        if (out.size() + line.size() > kFsListMaxBytes) {
            out += "[list truncated]\n";
            break;
        }
        out += line;
    }
    hc_sandbox_list_free(ents);
    if (out.empty()) return "(empty)";
    return out;
}

bool fs_read_raw(hc_sandbox *sb, const std::string &path, std::string &out, std::string &err)
{
    if (path.empty()) {
        err = "error: fs_update needs a non-empty path";
        return false;
    }
    hc_sandbox_fd fd = open_regular_rdonly(sb, path, err);
    if (fd == HC_SANDBOX_FD_INVALID) {
        err += " (fs_update edits an existing file — use fs_write to create it)";
        return false;
    }
    out.clear();
    bool truncated = false;
    bool ok = read_fd_capped(fd, out, kFsReadMaxBytes, truncated);
    close(fd);
    if (!ok) {
        err = "error: read failed";
        return false;
    }
    if (truncated) {
        err = "error: file too large to edit (over " + std::to_string(kFsReadMaxBytes) + " bytes)";
        return false;
    }
    return true;
}

bool fs_ensure_parent_dirs(hc_sandbox *sb, const std::string &path, std::string &err)
{
    /* Reject an absolute path here (clear message) rather than silently no-op'ing and letting the later open
     * surface the sandbox's generic INVALID — the worker's paths are always workspace-relative. */
    if (!path.empty() && path.front() == '/') {
        err = "error: path must be workspace-relative (no leading /)";
        return false;
    }
    /* The parent is everything up to the last '/'. A flat path (no '/') or an empty parent leaves nothing to
     * create → a no-op success; the subsequent open reports any real error. */
    std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos) return true;
    std::string parent = path.substr(0, slash);
    if (parent.empty()) return true;
    hc_sandbox_status ss = hc_sandbox_mkdirs(sb, parent.c_str(), 0700);
    if (ss != HC_SANDBOX_OK) {
        err = std::string("error: could not create directory '") + parent + "': " + hc_sandbox_strerror(ss);
        return false;
    }
    return true;
}

bool fs_apply_edit(const std::string &current, const std::string &mode, const std::string &old_string,
                   const std::string &new_string, const std::string &append_text, std::string &out,
                   std::string &err)
{
    if (mode == "append") {
        if (append_text.empty()) {
            err = "error: fs_update append needs a non-empty append_text";
            return false;
        }
        out = current + append_text;
        return true;
    }
    if (mode == "replace") {
        if (old_string.empty()) {
            err = "error: fs_update replace needs a non-empty old_string";
            return false;
        }
        size_t pos = current.find(old_string);
        if (pos == std::string::npos) {
            err = "error: old_string not found in the file (read it with fs_read for the exact text)";
            return false;
        }
        if (current.find(old_string, pos + 1) != std::string::npos) {
            err = "error: old_string occurs more than once — add surrounding context to make it unique";
            return false;
        }
        out = current;
        out.replace(pos, old_string.size(), new_string);
        return true;
    }
    err = "error: fs_update mode must be \"replace\" or \"append\"";
    return false;
}

} // namespace hc
