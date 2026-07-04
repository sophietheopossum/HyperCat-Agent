/* hc_secrets.c — in-memory secret store. See hc_secrets.h for the contract.
 *
 * Values live in individually heap-allocated buffers so each can be scrubbed independently with
 * a non-elidable zero on replace / delete / close. No OS calls beyond getenv; no disk writes —
 * the keychain backend is a separate, dependency-gated seam.
 */

#define _DEFAULT_SOURCE 1

#include "hc_secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HC_WITH_KEYCHAIN_BACKEND
#include <libsecret/secret.h> /* pulls glib-2.0 / gobject-2.0; compiled ONLY in a keychain build */
#endif

#define HC_SECRETS_KEY_MAX 64

typedef struct {
    char   key[HC_SECRETS_KEY_MAX];
    char  *value;
    size_t len;
} hc_secret_entry;

struct hc_secrets {
    hc_secret_entry *items;
    size_t           n, cap;
};

void hc_secrets_zero(void *p, size_t n)
{
    if (!p) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

hc_secrets *hc_secrets_open(void) { return calloc(1, sizeof(hc_secrets)); }

void hc_secrets_close(hc_secrets *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) {
        hc_secrets_zero(s->items[i].value, s->items[i].len);
        free(s->items[i].value);
    }
    free(s->items);
    free(s);
}

static hc_secret_entry *find(hc_secrets *s, const char *key)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->items[i].key, key) == 0) return &s->items[i];
    return NULL;
}

hc_secrets_status hc_secrets_set(hc_secrets *s, const char *key, const char *value)
{
    if (!s || !key || !key[0] || !value) return HC_SECRETS_ERR_INVALID;
    if (strlen(key) >= HC_SECRETS_KEY_MAX) return HC_SECRETS_ERR_TOO_LONG;

    size_t vlen = strlen(value);
    char *v = malloc(vlen + 1);
    if (!v) return HC_SECRETS_ERR_NOMEM;
    memcpy(v, value, vlen + 1);

    hc_secret_entry *e = find(s, key);
    if (e) { /* replace: scrub the old value first */
        hc_secrets_zero(e->value, e->len);
        free(e->value);
        e->value = v;
        e->len = vlen;
        return HC_SECRETS_OK;
    }
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 8;
        hc_secret_entry *ni = realloc(s->items, ncap * sizeof *ni);
        if (!ni) {
            hc_secrets_zero(v, vlen);
            free(v);
            return HC_SECRETS_ERR_NOMEM;
        }
        s->items = ni;
        s->cap = ncap;
    }
    e = &s->items[s->n++];
    snprintf(e->key, sizeof e->key, "%s", key);
    e->value = v;
    e->len = vlen;
    return HC_SECRETS_OK;
}

hc_secrets_status hc_secrets_get(hc_secrets *s, const char *key, char *out, size_t out_cap)
{
    if (!s || !key || !out || out_cap == 0) return HC_SECRETS_ERR_INVALID;
    hc_secret_entry *e = find(s, key);
    if (!e) return HC_SECRETS_ERR_NOT_FOUND;
    if (e->len + 1 > out_cap) return HC_SECRETS_ERR_TOO_LONG;
    memcpy(out, e->value, e->len + 1);
    return HC_SECRETS_OK;
}

bool hc_secrets_has(hc_secrets *s, const char *key)
{
    return s && key && find(s, key) != NULL;
}

hc_secrets_status hc_secrets_delete(hc_secrets *s, const char *key)
{
    if (!s || !key) return HC_SECRETS_ERR_INVALID;
    hc_secret_entry *e = find(s, key);
    if (!e) return HC_SECRETS_ERR_NOT_FOUND;
    hc_secrets_zero(e->value, e->len);
    free(e->value);
    *e = s->items[--s->n]; /* move the last entry into the gap (order is irrelevant) */
    return HC_SECRETS_OK;
}

hc_secrets_status hc_secrets_load_env(hc_secrets *s, const char *key, const char *env_name)
{
    if (!s || !key || !env_name) return HC_SECRETS_ERR_INVALID;
    const char *v = getenv(env_name);
    if (!v || !v[0]) return HC_SECRETS_ERR_NOT_FOUND;
    return hc_secrets_set(s, key, v);
}

/* ---- OS keychain persistence (see hc_secrets.h) ----------------------------------------------------------
 * The real arms (libsecret) are walled behind HC_WITH_KEYCHAIN_BACKEND so a non-keychain build compiles the
 * in-memory store plus four UNSUPPORTED stubs — one file, no second TU, no vtable. */
#ifdef HC_WITH_KEYCHAIN_BACKEND

/* One schema for all HyperCat secrets; the sole attribute is the secret's logical name (e.g. the env-var name).
 * SECRET_SCHEMA_NONE keeps it portable across keyring implementations (no registered-schema match required).
 * Built by field assignment (not an aggregate initializer) so the type's reserved fields don't trip
 * -Wmissing-field-initializers; single-owner store, so the one-time init needs no lock. */
static const SecretSchema *hc_secret_schema(void)
{
    static SecretSchema s;
    static int          init = 0;
    if (!init) {
        s.name             = "org.hypercat.Secret";
        s.flags            = SECRET_SCHEMA_NONE;
        s.attributes[0].name = "name";
        s.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
        init = 1; /* attributes[1].name stays NULL (the terminator) — static zero-init */
    }
    return &s;
}

bool hc_secrets_keychain_available(void)
{
    /* Report available only when the Secret Service is reachable AND the default collection is UNLOCKED — so a
     * caller's subsequent lookup/store can never surface a synchronous UNLOCK PROMPT that would BLOCK (notably
     * the startup auto-load on the boot thread, before any window is drawn). We connect + read the collection's
     * cached `Locked` property; we never force an unlock here. A locked keyring reads as not-available, so the
     * auto-load skips it (degrading to env); once the operator unlocks their keyring it lights up. */
    GError        *err = NULL;
    SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
    if (err) {
        g_error_free(err);
        return false;
    }
    if (!svc) return false;
    bool              ok  = false;
    SecretCollection *col = secret_collection_for_alias_sync(svc, SECRET_COLLECTION_DEFAULT,
                                                             SECRET_COLLECTION_NONE, NULL, &err);
    if (err)
        g_error_free(err); /* can't resolve the default collection -> treat as unavailable */
    else if (col)
        ok = !secret_collection_get_locked(col); /* a cached property read — no unlock, no prompt */
    if (col) g_object_unref(col);
    g_object_unref(svc);
    return ok;
}

hc_secrets_status hc_secrets_persist(hc_secrets *s, const char *key)
{
    if (!s || !key || !key[0]) return HC_SECRETS_ERR_INVALID;
    hc_secret_entry *e = find(s, key);
    if (!e) return HC_SECRETS_ERR_NOT_FOUND; /* nothing in memory to persist */
    char label[HC_SECRETS_KEY_MAX + 16];
    snprintf(label, sizeof label, "HyperCat: %s", key);
    GError  *err = NULL;
    gboolean ok  = secret_password_store_sync(hc_secret_schema(), SECRET_COLLECTION_DEFAULT, label, e->value,
                                              NULL, &err, "name", key, NULL);
    if (err) { /* no service / locked-or-absent collection -> graceful */
        g_error_free(err);
        return HC_SECRETS_ERR_UNSUPPORTED;
    }
    return ok ? HC_SECRETS_OK : HC_SECRETS_ERR_UNSUPPORTED;
}

hc_secrets_status hc_secrets_load_keychain(hc_secrets *s, const char *key)
{
    if (!s || !key || !key[0]) return HC_SECRETS_ERR_INVALID;
    GError *err = NULL;
    gchar  *pw  = secret_password_lookup_sync(hc_secret_schema(), NULL, &err, "name", key, NULL);
    if (err) {
        g_error_free(err);
        return HC_SECRETS_ERR_UNSUPPORTED;
    }
    if (!pw) return HC_SECRETS_ERR_NOT_FOUND;
    hc_secrets_status rc = hc_secrets_set(s, key, pw); /* copies into our own independently-scrubbed buffer */
    secret_password_free(pw);                          /* libsecret scrubs its page-locked buffer on free */
    return rc;
}

hc_secrets_status hc_secrets_forget_keychain(hc_secrets *s, const char *key)
{
    (void)s; /* clears by attribute — independent of the in-memory store */
    if (!key || !key[0]) return HC_SECRETS_ERR_INVALID;
    GError  *err     = NULL;
    gboolean removed = secret_password_clear_sync(hc_secret_schema(), NULL, &err, "name", key, NULL);
    if (err) {
        g_error_free(err);
        return HC_SECRETS_ERR_UNSUPPORTED;
    }
    return removed ? HC_SECRETS_OK : HC_SECRETS_ERR_NOT_FOUND;
}

#else /* !HC_WITH_KEYCHAIN_BACKEND — graceful stubs (the in-memory-only build) */

bool hc_secrets_keychain_available(void) { return false; }
hc_secrets_status hc_secrets_persist(hc_secrets *s, const char *key)
{
    (void)s;
    (void)key;
    return HC_SECRETS_ERR_UNSUPPORTED;
}
hc_secrets_status hc_secrets_load_keychain(hc_secrets *s, const char *key)
{
    (void)s;
    (void)key;
    return HC_SECRETS_ERR_UNSUPPORTED;
}
hc_secrets_status hc_secrets_forget_keychain(hc_secrets *s, const char *key)
{
    (void)s;
    (void)key;
    return HC_SECRETS_ERR_UNSUPPORTED;
}

#endif /* HC_WITH_KEYCHAIN_BACKEND */

const char *hc_secrets_status_str(hc_secrets_status s)
{
    switch (s) {
    case HC_SECRETS_OK:            return "ok";
    case HC_SECRETS_ERR_INVALID:   return "invalid argument";
    case HC_SECRETS_ERR_NOT_FOUND: return "secret not found";
    case HC_SECRETS_ERR_TOO_LONG:  return "key too long or buffer too small";
    case HC_SECRETS_ERR_NOMEM:     return "out of memory";
    case HC_SECRETS_ERR_UNSUPPORTED: return "keychain backend unavailable";
    }
    return "unknown";
}
