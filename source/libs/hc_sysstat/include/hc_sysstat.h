#ifndef HC_SYSSTAT_H
#define HC_SYSSTAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hc_sysstat — a per-process resource sampler (CPU% / RSS / uptime / wall clock) behind a narrow,
 * platform-neutral handle. The one concern: turn a pid into a periodic resource snapshot, with the
 * easy-to-get-wrong CPU%-delta arithmetic isolated in ONE tested place. The host's status bar polls
 * this for itself (and, later, per worker pid) instead of each caller re-deriving the /proc math.
 *
 * Purpose:   open a sampler for a pid; on each sample read CPU jiffies + RSS + start time and return a
 *            snapshot. cpu_pct is a DELTA the handle computes against the previous sample (so a caller
 *            gets a ready %, never raw jiffies). One OS source supplies the raw read; the % math is shared.
 * Owns:      hc_sysstat_open returns a small heap handle holding {pid, clk_tck, previous jiffies, previous
 *            wall_ms}; hc_sysstat_close frees it. No fd is held across samples (each sample reopens the
 *            /proc files and closes them — the sampler is poll-rate, so the reopen cost is irrelevant and
 *            there is no descriptor lifetime to leak).
 * Threading: a handle is single-owner / not reentrant — its stored previous-sample state mutates on every
 *            sample, so one thread must own one handle (no internal lock). Distinct handles are independent.
 * Security:  read-only. Every /proc read goes into a FIXED stack buffer with a single bounded read() — no
 *            heap, no unbounded growth, no overread on a truncated file. The handle never writes, execs, or
 *            opens anything but the target pid's own /proc entries; a pid that has exited (read fails /
 *            ENOENT) yields -1 so the caller simply drops it. The public header leaks NO /proc or platform
 *            types — only plain integers.
 * Portable:  Linux now (src/hc_sysstat_linux.c); macOS is a deferred one-file drop-in (src/hc_sysstat_mac.c,
 *            not yet written). The pure % math (src/hc_sysstat.c) is OS-free and unit-tested without any /proc.
 * Returns:   hc_sysstat_open => handle or NULL (bad/dead pid, alloc failure); hc_sysstat_sample => 0 on a
 *            filled *out, -1 on a gone/unreadable pid. */

typedef struct hc_sysstat hc_sysstat; /* opaque — no platform state leaks across the ABI */

/* The sample is a TAGGED struct (`struct hc_sysstat_sample`), deliberately NOT typedef'd to the bare name
 * `hc_sysstat_sample`: that name is the verb below (the sampling function), and in C a typedef and a
 * function cannot share one identifier. Tagging the struct keeps BOTH spelled exactly as intended — the
 * noun `struct hc_sysstat_sample` and the verb `hc_sysstat_sample(...)` — the same idiom as the C library's
 * `struct rusage` / `getrusage`. Callers write `struct hc_sysstat_sample s;`. */
struct hc_sysstat_sample {
    double   cpu_pct;   /* %% of ONE core consumed since the previous sample: 0 on the first sample (no
                         * prior) and may EXCEED 100 on a multi-core-busy process — this is intentionally
                         * NOT core-normalized (the caller divides by core count if it wants 0..100). */
    uint64_t rss_bytes; /* resident set size, bytes */
    uint64_t uptime_ms; /* the PROCESS's own uptime: now minus its start time, milliseconds */
    uint64_t wall_ms;   /* wall-clock now (CLOCK_REALTIME), milliseconds since the epoch — for the UI clock */
    int      pid;       /* the sampled pid (the resolved self pid when opened with pid <= 0) */
};

/* Open a sampler for `pid` (pid <= 0 means self, i.e. getpid()). Returns NULL on a bad/dead pid (its
 * /proc entry is unreadable at open) or on allocation failure. The first sample of the returned handle
 * reports cpu_pct == 0 (there is no previous sample to delta against). */
hc_sysstat *hc_sysstat_open(int pid);

/* Take one sample into *out. Returns 0 on success, or -1 (leaving *out untouched) if the pid has gone /
 * its /proc entry is unreadable — the caller drops a -1 pid. Updates the handle's stored previous sample
 * so the NEXT call's cpu_pct is the delta across this interval. */
int hc_sysstat_sample(hc_sysstat *s, struct hc_sysstat_sample *out);

void hc_sysstat_close(hc_sysstat *s);

/* The PURE CPU% core — the one place the delta arithmetic lives, OS-free so it is unit-tested without any
 * /proc. Returns % of one core over the interval:  100 * (Δjiffies / clk_tck) / (Δwall_ms / 1000).
 * Every division is guarded: returns 0.0 (never NaN, never Inf, never negative) when Δwall_ms == 0, when
 * now_jiffies < prev_jiffies, or when now_wall_ms < prev_wall_ms (a first sample, a zero interval, or a
 * clock that ran backwards). clk_tck <= 0 also yields 0.0. The Linux impl calls this with parsed jiffies. */
double hc_sysstat_compute_pct(uint64_t prev_jiffies, uint64_t now_jiffies,
                              uint64_t prev_wall_ms, uint64_t now_wall_ms, long clk_tck);

#ifdef __cplusplus
}
#endif
#endif /* HC_SYSSTAT_H */
