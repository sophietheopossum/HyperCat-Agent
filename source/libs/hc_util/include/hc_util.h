#ifndef HC_UTIL_H
#define HC_UTIL_H

/* hc_util — tiny header-only low-level helpers shared across binaries (agentd worker, orchestrator, ...).
 * Header-only on purpose: no translation unit, no ABI surface, usable from C and C++. Each includer gets
 * its own internal-linkage copy (static inline) — no ODR concern. Keep this to leaf utilities with zero
 * dependencies beyond libc. */

#include <stdlib.h>

/* getenv as a bounded int: returns `defv` if `name` is unset / empty / non-numeric. strtol-based (so a
 * huge value saturates to INT_MIN/INT_MAX rather than wrapping like atoi). The CALLER clamps to its own
 * valid range — the clamp policy differs per knob (a timeout floor, a budget ceiling, ...) and belongs at
 * the call site, not here. */
static inline int hc_getenv_int(const char *name, int defv)
{
    const char *s = name ? getenv(name) : NULL;
    if (!s || !*s) return defv;
    char *end = NULL;
    long  v = strtol(s, &end, 10);
    if (end == s) return defv; /* no digits consumed -> not a number */
    if (v < (-2147483647 - 1)) v = (-2147483647 - 1);
    if (v > 2147483647) v = 2147483647;
    return (int)v;
}

#endif /* HC_UTIL_H */
