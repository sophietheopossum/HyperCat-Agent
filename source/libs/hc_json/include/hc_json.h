#ifndef HC_JSON_H
#define HC_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_json — a small, opinionated wrapper over cJSON.
 *
 * Purpose:   one place for JSON parse/emit so no other HyperCat module depends on cJSON
 *            directly. Adds a canonical (sorted-key, compact) serializer used for
 *            reproducible manifest hashing and the IPC bus frame codec, plus typed
 *            accessors with defaults.
 * Owns:      values are heap trees. Every parse/constructor returns an OWNED tree, freed by
 *            hc_json_free. Children added via hc_json_obj_set / hc_json_arr_append are
 *            ADOPTED by the parent (and freed if the add fails — never double-free them).
 * Threading: no shared state; reentrant. A single tree must not be mutated from two threads.
 * Lifetime:  const char* from hc_json_get_str / hc_json_as_str points INTO the tree and is
 *            valid until the tree is freed or that member replaced — dup to outlive it.
 *            char* from hc_json_print* is malloc'd; the caller frees it with free().
 */

typedef struct cJSON hc_json; /* opaque to callers; the real type lives behind hc_json.c */

/* ---- parse / emit ---- */
hc_json *hc_json_parse(const char *text, size_t len); /* owned tree, or NULL on error */
void     hc_json_free(hc_json *v);
char    *hc_json_print(const hc_json *v, bool pretty); /* malloc'd; caller frees */
/* Canonical form: object keys sorted ascending, compact, identical across input key
 * orderings. Use for hashing (manifest) and bus frames. malloc'd; caller frees; NULL on error. */
char    *hc_json_print_canonical(const hc_json *v);

/* ---- construct ---- */
hc_json *hc_json_new_object(void);
hc_json *hc_json_new_array(void);
bool hc_json_obj_set_str   (hc_json *obj, const char *key, const char *val);
bool hc_json_obj_set_int   (hc_json *obj, const char *key, int64_t val);
bool hc_json_obj_set_double(hc_json *obj, const char *key, double val);
bool hc_json_obj_set_bool  (hc_json *obj, const char *key, bool val);
bool hc_json_obj_set_null  (hc_json *obj, const char *key);
bool hc_json_obj_set       (hc_json *obj, const char *key, hc_json *child); /* adopts child */
bool hc_json_arr_append    (hc_json *arr, hc_json *item);                   /* adopts item  */
bool hc_json_arr_append_str(hc_json *arr, const char *val);                 /* append a string node */

/* ---- typed access (returns the default when absent or of the wrong type) ---- */
const hc_json *hc_json_get       (const hc_json *obj, const char *key); /* borrowed or NULL */
const char    *hc_json_get_str   (const hc_json *obj, const char *key, const char *defv);
int64_t        hc_json_get_int   (const hc_json *obj, const char *key, int64_t defv);
double         hc_json_get_double(const hc_json *obj, const char *key, double defv);
bool           hc_json_get_bool  (const hc_json *obj, const char *key, bool defv);

/* ---- introspection ---- */
bool   hc_json_is_object(const hc_json *v);
bool   hc_json_is_array (const hc_json *v);
bool   hc_json_is_string(const hc_json *v);
bool   hc_json_is_number(const hc_json *v);
size_t hc_json_arr_len  (const hc_json *arr);
const hc_json *hc_json_arr_at(const hc_json *arr, size_t i); /* borrowed or NULL */
const char    *hc_json_as_str(const hc_json *v, const char *defv);
int64_t        hc_json_as_int(const hc_json *v, int64_t defv);
double         hc_json_as_double(const hc_json *v, double defv); /* a number node's value (e.g. an
                                                                  * embedding cell); defv if not a number */

#ifdef __cplusplus
}
#endif
#endif /* HC_JSON_H */
