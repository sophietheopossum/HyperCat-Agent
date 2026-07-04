#ifndef HC_SUPERVISOR_SPAWN_HPP
#define HC_SUPERVISOR_SPAWN_HPP

/* hc_spawn — launch one worker process with a private token channel. INTERNAL to the supervisor
 * (lives under src/, not a public module header).
 *
 * Responsibility (one thing): process creation + one-time-token handoff. Lifecycle, monitoring,
 * and restart policy live in hc_supervisor — this file does not know about them.
 *
 * The token is written to a pipe whose read end the child inherits; the child learns the fd
 * number from an appended "--token-fd <n>" argv pair. The secret never touches argv or the
 * environment, so it is invisible in `ps` and /proc/<pid>/environ. The read end is left
 * inheritable for the brief spawn window, so callers MUST serialize spawns (the supervisor holds
 * its table mutex across the call) to avoid leaking the fd into an unrelated concurrent child.
 *
 * Threading: NOT thread-safe; call serialized. Lifetime: returns an OS pid the caller waitpid()s.
 */

#include <string>
#include <vector>

namespace hc {

/* Spawn `exe` with the given argv arguments (NOT including argv[0]; `exe` is prepended as argv[0]).
 * Writes `token` down a private pipe the child reads via the appended "--token-fd <n>". Returns
 * the child pid (> 0) on success, or -1 on failure. */
long spawn_worker(const std::string &exe, const std::vector<std::string> &args,
                  const std::string &token);

} // namespace hc

#endif /* HC_SUPERVISOR_SPAWN_HPP */
