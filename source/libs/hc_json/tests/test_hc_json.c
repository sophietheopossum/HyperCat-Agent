/* Unit tests for hc_json. Exit non-zero on any failure (CTest reads the exit code). */

#include "hc_json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL: %s\n", (msg));                                           \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

static hc_json *parse(const char *s) { return hc_json_parse(s, strlen(s)); }

static void test_typed_access(void)
{
    hc_json *v = parse("{\"name\":\"hypercat\",\"n\":42,\"on\":true,\"sub\":{\"x\":1}}");
    CHECK(v != NULL, "parse object");
    CHECK(strcmp(hc_json_get_str(v, "name", ""), "hypercat") == 0, "get_str");
    CHECK(hc_json_get_int(v, "n", 0) == 42, "get_int");
    CHECK(hc_json_get_bool(v, "on", false) == true, "get_bool");
    CHECK(hc_json_get_int(v, "missing", -1) == -1, "get_int default");
    CHECK(hc_json_get_str(v, "name", NULL) != NULL, "present key non-default");
    const hc_json *sub = hc_json_get(v, "sub");
    CHECK(sub && hc_json_get_int(sub, "x", 0) == 1, "nested access");
    hc_json_free(v);
}

static void test_canonical(void)
{
    hc_json *a = parse("{\"b\":1,\"a\":2,\"c\":{\"z\":1,\"y\":2}}");
    hc_json *b = parse("{\"c\":{\"y\":2,\"z\":1},\"a\":2,\"b\":1}");
    char *ca = hc_json_print_canonical(a);
    char *cb = hc_json_print_canonical(b);
    CHECK(ca && cb, "canonical print non-null");
    CHECK(ca && strcmp(ca, "{\"a\":2,\"b\":1,\"c\":{\"y\":2,\"z\":1}}") == 0,
          "canonical sorted (incl. nested)");
    CHECK(ca && cb && strcmp(ca, cb) == 0, "canonical is order-independent");
    free(ca);
    free(cb);
    hc_json_free(a);
    hc_json_free(b);
}

static void test_build_roundtrip(void)
{
    hc_json *o = hc_json_new_object();
    CHECK(hc_json_obj_set_str(o, "k", "v"), "set_str");
    CHECK(hc_json_obj_set_int(o, "i", 7), "set_int");
    hc_json *arr = hc_json_new_array();
    CHECK(hc_json_arr_append(arr, hc_json_new_object()), "arr_append");
    CHECK(hc_json_obj_set(o, "list", arr), "obj_set adopts array");
    CHECK(hc_json_arr_len(hc_json_get(o, "list")) == 1, "array length");

    char *s = hc_json_print(o, false);
    CHECK(s != NULL, "print built object");
    hc_json *rt = parse(s);
    CHECK(rt && strcmp(hc_json_get_str(rt, "k", ""), "v") == 0, "round-trip string");
    CHECK(rt && hc_json_get_int(rt, "i", 0) == 7, "round-trip int");
    free(s);
    hc_json_free(o);
    hc_json_free(rt);
}

static void test_errors(void)
{
    CHECK(hc_json_parse("{not valid", 10) == NULL, "invalid parse returns NULL");
    CHECK(hc_json_get_int(NULL, "x", 99) == 99, "NULL object yields default");
    /* a failed adopt must free the child, not leak — exercised under ASan */
    CHECK(hc_json_obj_set(NULL, "k", hc_json_new_object()) == false, "set on NULL obj fails+frees");
}

/* Untrusted JSON: cJSON parses every number into a double, and casting an out-of-range or non-finite
 * double to int64 is UB (C11 6.3.1.4). A hostile provider can put any magnitude in a numeric field (e.g.
 * P12 token counts), so hc_json_get_int must saturate instead of invoking UB. */
static void test_int_saturation(void)
{
    hc_json *v = parse("{\"huge\":1e308,\"neg\":-1e308,\"big\":9000000000,\"frac\":3.9}");
    CHECK(v != NULL, "parse numbers");
    CHECK(hc_json_get_int(v, "huge", 0) == INT64_MAX, "overflow saturates to INT64_MAX (no UB)");
    CHECK(hc_json_get_int(v, "neg", 0) == INT64_MIN, "underflow saturates to INT64_MIN (no UB)");
    CHECK(hc_json_get_int(v, "big", 0) == 9000000000LL, "in-range large value preserved");
    CHECK(hc_json_get_int(v, "frac", -1) == 3, "fractional truncates toward zero");
    hc_json_free(v);
}

/* The array-of-strings constructor + the number-node accessor (added for the P01 embeddings parse):
 * as_int would truncate an embedding cell to 0, so as_double is the one that must read it. */
static void test_array_and_double(void)
{
    hc_json *arr = hc_json_new_array();
    CHECK(arr && hc_json_arr_append_str(arr, "alpha") && hc_json_arr_append_str(arr, "beta"),
          "append string nodes to an array");
    CHECK(hc_json_arr_len(arr) == 2 && strcmp(hc_json_as_str(hc_json_arr_at(arr, 1), "") , "beta") == 0,
          "array string round-trips via as_str");
    hc_json_free(arr);

    hc_json *v = parse("{\"vec\":[0.25,-0.5,1e400]}");
    const hc_json *vec = v ? hc_json_get(v, "vec") : NULL;
    CHECK(vec && hc_json_arr_len(vec) == 3, "parse a numeric array");
    CHECK(hc_json_as_double(hc_json_arr_at(vec, 0), 9.0) == 0.25, "as_double reads a fractional cell");
    CHECK(hc_json_as_int(hc_json_arr_at(vec, 0), 9) == 0, "as_int would truncate the same cell (why as_double)");
    CHECK(!isfinite(hc_json_as_double(hc_json_arr_at(vec, 2), 0.0)), "as_double surfaces a non-finite (1e400) cell");
    CHECK(hc_json_as_double(hc_json_arr_at(vec, 0), 9.0) != 9.0, "present cell is not the default");
    hc_json_free(v);
}

int main(void)
{
    test_typed_access();
    test_canonical();
    test_build_roundtrip();
    test_errors();
    test_int_saturation();
    test_array_and_double();

    if (g_fails) {
        fprintf(stderr, "hc_json: %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("hc_json: all checks passed\n");
    return 0;
}
