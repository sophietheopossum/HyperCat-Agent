/* hc_sysstat.c — the OS-FREE core of the sampler. See hc_sysstat.h.
 *
 * This translation unit holds ONLY the CPU%-delta arithmetic — the one calculation that is easy to get
 * wrong (units, a backwards clock, a counter wrap, a divide-by-zero on the first sample). Keeping it here,
 * free of any /proc or platform call, lets the unit test exercise every guarded branch with hand-chosen
 * numbers and no live process. The per-OS reader (hc_sysstat_linux.c) parses raw jiffies and calls in here;
 * it never re-derives the percentage. No state, no allocation, no I/O. */

#include "hc_sysstat.h"

double hc_sysstat_compute_pct(uint64_t prev_jiffies, uint64_t now_jiffies,
                              uint64_t prev_wall_ms, uint64_t now_wall_ms, long clk_tck)
{
    /* A non-positive clock tick would make the conversion meaningless (and divide by zero); a backwards or
     * zero wall interval means there is no usable interval to average over (first sample, or the realtime
     * clock was stepped back). A jiffie counter that went backwards (now < prev) cannot be a real CPU
     * delta — treat any of these as "no measurement" and report 0.0 rather than a NaN/Inf/negative. */
    if (clk_tck <= 0) return 0.0;
    if (now_wall_ms <= prev_wall_ms) return 0.0;
    if (now_jiffies < prev_jiffies) return 0.0;

    uint64_t d_jiffies = now_jiffies - prev_jiffies;
    uint64_t d_wall_ms = now_wall_ms - prev_wall_ms; /* > 0, checked above */

    /* % of one core = (cpu_seconds / wall_seconds) * 100
     *              = ((Δjiffies / clk_tck) / (Δwall_ms / 1000)) * 100
     * Computed in double; both denominators are guaranteed > 0 here, so no division can trap. */
    double cpu_seconds  = (double)d_jiffies / (double)clk_tck;
    double wall_seconds = (double)d_wall_ms / 1000.0;
    double pct          = (cpu_seconds / wall_seconds) * 100.0;

    if (pct < 0.0) return 0.0; /* belt-and-braces: the guards above make this unreachable */
    return pct;
}
