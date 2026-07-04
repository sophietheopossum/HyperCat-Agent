#ifndef HC_EXE_PATH_HPP
#define HC_EXE_PATH_HPP

/* exe_path — resolve a sibling helper binary relative to the host's OWN executable, so a built bundle is
 * RELOCATABLE. The host spawns two helpers (agentd, hc_audio_helper) whose locations are baked at compile time as
 * absolute build-tree paths ($<TARGET_FILE:...>); on another machine those are dead. This resolves each helper next
 * to the running host binary instead, falling back to the compile-time path for the dev build tree (where the three
 * binaries live in separate sub-dirs).
 *
 * Security: the directory comes ONLY from the kernel-maintained /proc/self/exe symlink (NOT argv[0], which is
 * spoofable; NOT $CWD; NOT an env var) and the sibling name is a fixed caller literal, so the resolved path can only
 * ever be a binary in the same trusted directory as the host — the same trust level as the compile-time constant it
 * replaces, with no new attacker-controllable input.
 * Threading: stateless + reentrant (no shared state).
 * Lifetime: both functions return std::string BY VALUE — the caller owns the result; no returned pointer/reference,
 * no shared internal state.
 * Platform: Linux-only (relies on /proc/self/exe); elsewhere resolution yields "" and the caller uses the fallback. */

#include <string>

namespace hc {

/* PURE (offline-testable): if `dir` is non-empty and `dir/name` exists + is executable (access X_OK), return that
 * absolute path; otherwise return `fallback` (the compile-time constant) verbatim. The only syscall is access() on
 * the joined path — no readlink — so a test can drive every branch with a fixture dir. */
std::string resolve_sibling(const std::string &dir, const char *name, const char *fallback);

/* Resolve `name` next to the host's own binary (/proc/self/exe dir + resolve_sibling), else the compile-time
 * `fallback`. This is the form callers use. */
std::string resolve_sibling_exe(const char *name, const char *fallback);

} // namespace hc

#endif /* HC_EXE_PATH_HPP */
