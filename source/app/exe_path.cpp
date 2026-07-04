/* exe_path — see exe_path.hpp. Relocatable sibling-helper resolution via /proc/self/exe. */

#include "exe_path.hpp"

#include <limits.h>
#include <unistd.h>

namespace hc {
namespace {

/* The directory of the running executable, or "" if it can't be determined (readlink failure, a possibly-truncated
 * path, or a slash-less result). Internal: callers want the composed resolve_sibling_exe, not the raw dir. */
std::string exe_dir()
{
    char    buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    /* n<=0: unreadable (non-Linux / restricted). n>=sizeof-1: readlink can't signal truncation, so treat a full
     * buffer as possibly-truncated and fall back rather than spawn from a wrong, silently-cut path. */
    if (n <= 0 || (size_t)n >= sizeof buf - 1) return std::string();
    buf[n] = '\0';
    std::string p(buf);
    std::string::size_type slash = p.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return p.substr(0, slash); /* the directory, no trailing slash */
}

} // namespace

std::string resolve_sibling(const std::string &dir, const char *name, const char *fallback)
{
    if (!dir.empty() && name && *name) {
        std::string cand = dir + "/" + name;
        if (::access(cand.c_str(), X_OK) == 0) return cand; /* a runnable sibling next to the host -> the bundle case */
    }
    return fallback ? std::string(fallback) : std::string(); /* the compile-time constant -> the dev-tree case */
}

std::string resolve_sibling_exe(const char *name, const char *fallback)
{
    return resolve_sibling(exe_dir(), name, fallback);
}

} // namespace hc
