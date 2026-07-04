#ifndef HC_SECRETS_H
#define HC_SECRETS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_secrets — process-local secret store with non-elidable zeroization (POSIX).
 *
 * Purpose:   hold sensitive strings (provider API keys, tokens) for the process, keyed by name,
 *            and scrub them from memory when removed or on close — so a key is never left in a
 *            freed heap block, and never written to disk by this module.
 * Owns:      the stored secret values (heap); zeroized before free on set-replace, delete, and
 *            close. The caller owns any buffer passed to hc_secrets_get and should zeroize it.
 * Threading: a store is single-owner, not thread-safe.
 * Backend:   the in-memory store is ALWAYS the working set (zeroized on replace/delete/close). When built
 *            with HC_WITH_KEYCHAIN (Linux: libsecret / the freedesktop Secret Service) AND a keyring is
 *            reachable at runtime, the persistence verbs below (persist / load_keychain / forget_keychain)
 *            mirror a secret into the OS-encrypted keychain so a GUI-entered key survives a restart — meeting
 *            the "secrets in the OS keychain" non-negotiable. Everything DEGRADES GRACEFULLY: a build without
 *            libsecret, or a host with no Secret Service (headless / CI / bare SSH), runs the in-memory path
 *            and the persistence verbs return HC_SECRETS_ERR_UNSUPPORTED — never blocking, never a hard fail.
 *            The HOST owns precedence (the env var wins; the keychain is consulted only when no env key is
 *            set). macOS Keychain Services is a ready seam (no code yet — there is no macOS build).
 *            See Docs/Plan_HyperCat/08-secrets-and-security.md.
 */

typedef enum {
    HC_SECRETS_OK = 0,
    HC_SECRETS_ERR_INVALID,
    HC_SECRETS_ERR_NOT_FOUND,
    HC_SECRETS_ERR_TOO_LONG,   /* key too long, or the get buffer is too small */
    HC_SECRETS_ERR_NOMEM,
    HC_SECRETS_ERR_UNSUPPORTED /* keychain backend not built, or no Secret Service reachable (graceful) */
} hc_secrets_status;

const char *hc_secrets_status_str(hc_secrets_status);

/* Non-elidable memory scrub (volatile writes; the compiler may not drop it). Exposed so callers
 * can scrub their own get buffers with the same guarantee. */
void hc_secrets_zero(void *p, size_t n);

typedef struct hc_secrets hc_secrets;

hc_secrets *hc_secrets_open(void); /* v1 in-memory backend; NULL on OOM */
void        hc_secrets_close(hc_secrets *); /* zeroizes every stored secret first; NULL-safe */

/* Store/replace `value` under `key` (both NUL-terminated). A replaced value is zeroized. */
hc_secrets_status hc_secrets_set(hc_secrets *, const char *key, const char *value);

/* Copy the secret for `key` into `out` (NUL-terminated) if it fits in `out_cap`. The caller
 * should zeroize `out` when done. Returns HC_SECRETS_ERR_NOT_FOUND if absent; on any non-OK
 * return `out` is not written. */
hc_secrets_status hc_secrets_get(hc_secrets *, const char *key, char *out, size_t out_cap);

bool              hc_secrets_has(hc_secrets *, const char *key);
hc_secrets_status hc_secrets_delete(hc_secrets *, const char *key); /* zeroizes the value */

/* Convenience: if environment variable `env_name` is set and non-empty, store it under `key`.
 * The dev/bootstrap path for provider keys until the keychain backend lands. */
hc_secrets_status hc_secrets_load_env(hc_secrets *, const char *key, const char *env_name);

/* --- OS keychain persistence (libsecret on Linux; the macOS Security.framework arm is a future seam) -------
 * The in-memory store above is ALWAYS the working set; these verbs are a PERSISTENCE SIDECAR that moves a value
 * between it and the OS-encrypted keychain. get/set/has/delete are unchanged (in-memory only). ALL of these
 * RUNTIME-DEGRADE: with no keychain backend built, or no Secret Service / unlocked keyring reachable (headless,
 * CI, bare SSH), they return HC_SECRETS_ERR_UNSUPPORTED — never blocking, never a hard failure. The HOST
 * orchestrates precedence (the env var wins; the keychain is consulted only when no env key is set). */

/* True iff a keychain backend was BUILT and a Secret Service is reachable RIGHT NOW (a short sync probe — the
 * UI/host gate persistence affordances on it). Always false in a non-keychain build. */
bool hc_secrets_keychain_available(void);

/* Write the in-memory value for `key` into the OS keychain (persist across restarts). `key` must already be in
 * the in-memory store (call hc_secrets_set first). OK; NOT_FOUND if absent from memory; UNSUPPORTED if no
 * keychain. The in-memory copy is unchanged. */
hc_secrets_status hc_secrets_persist(hc_secrets *, const char *key);

/* Read `key` from the OS keychain INTO the in-memory store (as if hc_secrets_set had been called with the stored
 * value). OK; NOT_FOUND if absent from the keychain; UNSUPPORTED if no keychain. The keychain buffer is copied
 * into hc_secrets' own scrubbed buffer and freed (scrubbed) at once. */
hc_secrets_status hc_secrets_load_keychain(hc_secrets *, const char *key);

/* Delete `key` from the OS keychain (does NOT touch the in-memory copy — use hc_secrets_delete for that). OK if
 * removed; NOT_FOUND if it was not stored; UNSUPPORTED if no keychain. */
hc_secrets_status hc_secrets_forget_keychain(hc_secrets *, const char *key);

#ifdef __cplusplus
}
#endif
#endif /* HC_SECRETS_H */
