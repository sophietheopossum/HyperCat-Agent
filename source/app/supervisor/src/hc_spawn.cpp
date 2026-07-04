/* hc_spawn — see hc_spawn.hpp. posix_spawn (Linux + macOS) a worker with a token pipe.
 *
 * The read end is deliberately left WITHOUT close-on-exec so the child inherits it at the same fd
 * number, which we pass on argv; the write end is close-on-exec so it never reaches the child.
 * posix_spawn (vs raw fork+exec) keeps us off the async-signal-safety tightrope between fork and
 * exec. The brief window where the read end is inheritable is why callers must serialize spawns.
 */

#include "hc_spawn.hpp"

#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

namespace hc {

namespace {

/* write exactly len bytes to fd; false on error. The token is tiny (well under PIPE_BUF), so this
 * never blocks on a full pipe. */
bool write_all_fd(int fd, const char *p, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

} // namespace

long spawn_worker(const std::string &exe, const std::vector<std::string> &args,
                  const std::string &token)
{
    int p[2];
    if (pipe(p) != 0) return -1; /* pipe() (not Linux-only pipe2) for macOS portability */
    fcntl(p[1], F_SETFD, FD_CLOEXEC); /* keep the write end out of the child */

    char fdbuf[16];
    std::snprintf(fdbuf, sizeof fdbuf, "%d", p[0]);

    /* argv: exe, args..., "--token-fd", "<p0>", NULL. Own the strings so the char* stay valid. */
    std::vector<std::string> store;
    store.reserve(args.size() + 4);
    store.push_back(exe);
    for (const auto &a : args) store.push_back(a);
    store.push_back("--token-fd");
    store.push_back(fdbuf);

    std::vector<char *> argv;
    argv.reserve(store.size() + 1);
    for (auto &s : store) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    close(p[0]); /* the child has inherited its own copy of the read end */
    if (rc != 0) {
        close(p[1]);
        errno = rc;
        return -1;
    }
    /* Hand the child its token, then close so the child sees EOF after it. A short write here just
     * makes the child's check-in fail (wrong/empty token) — the supervisor reaps + retries. */
    write_all_fd(p[1], token.data(), token.size());
    close(p[1]);
    return (long)pid;
}

} // namespace hc
