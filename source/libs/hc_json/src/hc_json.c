/* hc_json.c — wrapper over cJSON. See hc_json.h for the contract.
 *
 * cJSON is confined to this translation unit: the public header forward-declares
 * `struct cJSON` as the opaque `hc_json`, so callers link this library but never include
 * cJSON.h. `hc_json` and `cJSON` are the same struct type, so no casts are needed between them.
 */

#include "hc_json.h"

#include <cjson/cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- parse / emit ---- */

hc_json *hc_json_parse(const char *text, size_t len)
{
    if (!text) return NULL;
    return cJSON_ParseWithLength(text, len);
}

void hc_json_free(hc_json *v)
{
    cJSON_Delete(v);
}

char *hc_json_print(const hc_json *v, bool pretty)
{
    return pretty ? cJSON_Print(v) : cJSON_PrintUnformatted(v);
}

/* ---- canonical serializer (sorted keys, compact, stable) ----
 * Reuses cJSON's correctly-escaped compact form for scalars and keys, and sorts object
 * members by key so the output is identical regardless of input ordering. This is what the
 * manifest hashes and the bus frames; do not change its output without bumping consumers. */

typedef struct {
    char  *p;
    size_t len, cap;
} strbuf;

static bool sb_ensure(strbuf *b, size_t extra)
{
    if (b->p && b->len + extra + 1 <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char *np = realloc(b->p, ncap);
    if (!np) return false;
    b->p = np;
    b->cap = ncap;
    return true;
}

static bool sb_put(strbuf *b, const char *s, size_t n)
{
    if (!sb_ensure(b, n)) return false;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

static bool sb_putc(strbuf *b, char c) { return sb_put(b, &c, 1); }
static bool sb_puts(strbuf *b, const char *s) { return sb_put(b, s, strlen(s)); }

/* Emit a string token (key or value) with cJSON's escaping. */
static bool emit_escaped(strbuf *b, const char *s)
{
    cJSON *node = cJSON_CreateString(s ? s : "");
    if (!node) return false;
    char *text = cJSON_PrintUnformatted(node);
    cJSON_Delete(node);
    if (!text) return false;
    bool ok = sb_puts(b, text);
    free(text);
    return ok;
}

static int key_cmp(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON *const *)a;
    const cJSON *y = *(const cJSON *const *)b;
    return strcmp(x->string ? x->string : "", y->string ? y->string : "");
}

static bool canon(strbuf *b, const cJSON *v)
{
    if (!v) return sb_puts(b, "null");

    if (cJSON_IsObject(v)) {
        size_t n = (size_t)cJSON_GetArraySize(v);
        const cJSON **items = NULL;
        if (n) {
            items = malloc(n * sizeof *items);
            if (!items) return false;
            size_t i = 0;
            for (const cJSON *c = v->child; c; c = c->next) items[i++] = c;
            qsort(items, n, sizeof *items, key_cmp);
        }
        bool ok = sb_putc(b, '{');
        for (size_t i = 0; ok && i < n; i++) {
            if (i) ok = ok && sb_putc(b, ',');
            ok = ok && emit_escaped(b, items[i]->string) && sb_putc(b, ':')
                 && canon(b, items[i]);
        }
        free(items);
        return ok && sb_putc(b, '}');
    }

    if (cJSON_IsArray(v)) {
        bool ok = sb_putc(b, '[');
        size_t i = 0;
        for (const cJSON *c = v->child; ok && c; c = c->next, i++) {
            if (i) ok = ok && sb_putc(b, ',');
            ok = ok && canon(b, c);
        }
        return ok && sb_putc(b, ']');
    }

    /* scalar: cJSON already emits a correctly-escaped compact token */
    char *text = cJSON_PrintUnformatted(v);
    if (!text) return false;
    bool ok = sb_puts(b, text);
    free(text);
    return ok;
}

char *hc_json_print_canonical(const hc_json *v)
{
    strbuf b = {0};
    if (!sb_ensure(&b, 0) || !canon(&b, v)) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* ---- construct ---- */

hc_json *hc_json_new_object(void) { return cJSON_CreateObject(); }
hc_json *hc_json_new_array(void)  { return cJSON_CreateArray(); }

static bool obj_add(hc_json *obj, const char *key, cJSON *item)
{
    if (!obj || !key || !item) {
        cJSON_Delete(item);
        return false;
    }
    if (!cJSON_AddItemToObject(obj, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

bool hc_json_obj_set_str(hc_json *obj, const char *key, const char *val)
{
    return obj_add(obj, key, cJSON_CreateString(val ? val : ""));
}

bool hc_json_obj_set_int(hc_json *obj, const char *key, int64_t val)
{
    return obj_add(obj, key, cJSON_CreateNumber((double)val));
}

bool hc_json_obj_set_double(hc_json *obj, const char *key, double val)
{
    return obj_add(obj, key, cJSON_CreateNumber(val));
}

bool hc_json_obj_set_bool(hc_json *obj, const char *key, bool val)
{
    return obj_add(obj, key, cJSON_CreateBool(val));
}

bool hc_json_obj_set_null(hc_json *obj, const char *key)
{
    return obj_add(obj, key, cJSON_CreateNull());
}

bool hc_json_obj_set(hc_json *obj, const char *key, hc_json *child)
{
    return obj_add(obj, key, child);
}

bool hc_json_arr_append(hc_json *arr, hc_json *item)
{
    if (!arr || !item) {
        cJSON_Delete(item);
        return false;
    }
    if (!cJSON_AddItemToArray(arr, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

bool hc_json_arr_append_str(hc_json *arr, const char *val)
{
    /* the array analogue of hc_json_obj_set_str; append adopts/frees the node on failure */
    return hc_json_arr_append(arr, cJSON_CreateString(val ? val : ""));
}

/* ---- typed access ---- */

const hc_json *hc_json_get(const hc_json *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

const char *hc_json_get_str(const hc_json *obj, const char *key, const char *defv)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : defv;
}

int64_t hc_json_get_int(const hc_json *obj, const char *key, int64_t defv)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || !cJSON_IsNumber(it)) return defv;
    /* cJSON parses every number into `valuedouble`; casting an out-of-range or NaN/Inf double to an
     * integer is UB (C11 6.3.1.4). Untrusted JSON (e.g. a hostile provider's token counts) can hold any
     * value, so reject non-finite and saturate to the int64 range before narrowing. 2^63 is exact in
     * double, so `>=` against it is the correct upper guard. */
    double d = it->valuedouble;
    if (!isfinite(d)) return defv;
    if (d >= 9223372036854775808.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

double hc_json_get_double(const hc_json *obj, const char *key, double defv)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : defv;
}

bool hc_json_get_bool(const hc_json *obj, const char *key, bool defv)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) return defv;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    return defv;
}

/* ---- introspection ---- */

bool hc_json_is_object(const hc_json *v) { return cJSON_IsObject(v); }
bool hc_json_is_array(const hc_json *v)  { return cJSON_IsArray(v); }
bool hc_json_is_string(const hc_json *v) { return cJSON_IsString(v); }
bool hc_json_is_number(const hc_json *v) { return cJSON_IsNumber(v); }

size_t hc_json_arr_len(const hc_json *arr)
{
    int n = cJSON_GetArraySize(arr);
    return n > 0 ? (size_t)n : 0;
}

const hc_json *hc_json_arr_at(const hc_json *arr, size_t i)
{
    return cJSON_GetArrayItem(arr, (int)i);
}

const char *hc_json_as_str(const hc_json *v, const char *defv)
{
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : defv;
}

int64_t hc_json_as_int(const hc_json *v, int64_t defv)
{
    return (v && cJSON_IsNumber(v)) ? (int64_t)v->valuedouble : defv;
}

double hc_json_as_double(const hc_json *v, double defv)
{
    /* valuedouble holds the parsed number directly (no narrowing cast, unlike as_int); a caller that
     * must reject non-finite provider values (e.g. "1e400" -> Inf) checks isfinite() on the result. */
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : defv;
}
