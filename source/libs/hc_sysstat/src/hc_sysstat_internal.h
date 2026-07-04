#ifndef HC_SYSSTAT_INTERNAL_H
#define HC_SYSSTAT_INTERNAL_H

#include <stdint.h>

/* hc_sysstat_internal.h — the layout shared between the OS-free core (hc_sysstat.c) and the per-OS reader
 * (hc_sysstat_linux.c / _mac.c). NOT installed and NOT part of the ABI: it exists only so the two
 * translation units of one static library agree on the handle shape without exposing it to consumers.
 *
 * Ownership: the handle is heap-allocated by the OS reader's hc_sysstat_open and freed by hc_sysstat_close.
 * Invariant: have_prev gates the cpu_pct delta — it is 0 until the first successful sample stores a prior,
 *            so the first reported cpu_pct is always 0 (no prior to subtract). Threading: single-owner
 *            (these fields mutate on every sample); one thread owns one handle. */

struct hc_sysstat {
    int      pid;          /* resolved pid (self pid when opened with pid <= 0) */
    long     clk_tck;      /* sysconf(_SC_CLK_TCK): jiffies per second, cached at open */
    int      have_prev;    /* 0 until the first sample stored a prior (gates the cpu_pct delta -> first = 0) */
    uint64_t prev_jiffies; /* utime+stime at the previous sample */
    uint64_t prev_wall_ms; /* CLOCK_REALTIME ms at the previous sample */
};

#endif /* HC_SYSSTAT_INTERNAL_H */
