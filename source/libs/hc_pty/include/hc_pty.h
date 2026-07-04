#ifndef HC_PTY_H
#define HC_PTY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_pty — spawn a child process under a pseudo-terminal (POSIX: Linux + macOS).
 *
 * Purpose:   give the host an OPERATOR terminal — a shell the human at the UI can drive. It is a local
 *            convenience for the operator (who already has a shell on this machine); it is NOT an agent
 *            capability and carries NO bus endpoint, so an agent can never reach it. Agent command
 *            execution stays gated by the exec allowlist (hc_policy); this is a separate, human-only
 *            surface. Pure POSIX (posix_openpt/grantpt/unlockpt + fork + execve) — no new dependency.
 * Security:  the child is exec'd with a MINIMAL, SCRUBBED environment (only HOME/PATH/TERM/PS1) — the
 *            parent's environment is NOT inherited, so a secret in it (notably OPENROUTER_API_KEY) can
 *            NEVER be read from the spawned shell. The child runs as the SAME uid (no escalation — the
 *            operator already has this shell) and starts in `cwd` (a contained workspace by default).
 *            The master side is non-blocking so the host can poll it from the render loop without a
 *            thread. There is no terminal EMULATION here — output is raw bytes (escape sequences are
 *            passed through, not interpreted); a v1 console for plain commands, not a vt100.
 * Owns:      one hc_pty owns the master fd + the child pid. Single-owner; drive it from one thread.
 * Lifetime:  hc_pty_spawn allocates; hc_pty_close terminates+reaps the child, closes the master, frees.
 * Returns:   the byte-count calls (read/write) use a plain `long` with an ssize_t-like three-way
 *            contract — >0 bytes, 0 = would-block / none-yet, <0 = the terminal ended / error — rather
 *            than the shared status enum, because they are a byte STREAM, not a discrete operation. */

typedef struct hc_pty hc_pty;

/* Spawn argv (NUL-terminated; argv[0] is an ABSOLUTE program path, e.g. "/bin/sh") under a fresh pty,
 * with cwd = `cwd` (NULL inherits). The child gets only a scrubbed env (see banner). The master is set
 * non-blocking. Returns NULL on failure (no pty, fork/openpt error). */
hc_pty *hc_pty_spawn(const char *const argv[], const char *cwd);

/* Read available output (non-blocking) into buf[cap]. Returns >0 bytes read, 0 if none is available
 * yet, or -1 if the child has closed the pty / a real error occurred (treat -1 as "terminal ended"). */
long hc_pty_read(hc_pty *, char *buf, size_t cap);

/* Write `len` input bytes to the child. Returns bytes written (may be < len), 0 if it would block, or
 * -1 on error. */
long hc_pty_write(hc_pty *, const char *data, size_t len);

/* Whether the child is still running (non-blocking reap; false once it has exited). */
bool hc_pty_alive(hc_pty *);

/* Terminate (SIGHUP then SIGKILL) + reap the child, close the master, free. NULL-safe. */
void hc_pty_close(hc_pty *);

#ifdef __cplusplus
}
#endif
#endif /* HC_PTY_H */
