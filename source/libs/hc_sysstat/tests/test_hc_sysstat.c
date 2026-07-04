/* test_hc_sysstat — the sampler gate (offline, deterministic for the math; live-but-self for the smoke).
 *
 * Two halves:
 *   1. The PURE core hc_sysstat_compute_pct, exercised with hand-chosen numbers and NO /proc — proving the
 *      first-sample / zero-delta / backwards-clock / backwards-jiffies / bad-clk_tck branches all yield 0,
 *      a known Δjiffies+Δwall yields the exact expected %, and nothing is ever NaN / Inf / negative.
 *   2. A self smoke: open(0) -> sample -> burn a few ms of CPU -> sample again, asserting the second sample
 *      has rss_bytes > 0, wall_ms > 0, a sane uptime, and a finite cpu_pct >= 0. (This is the only part that
 *      touches /proc, and only the test process's own entry — no external pid, no privilege.) */

#include "hc_sysstat.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

static int g_fails = 0;
#define CHECK(c, m)                                                                                    \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            fprintf(stderr, "FAIL: %s\n", (m));                                                        \
            g_fails++;                                                                                 \
        }                                                                                              \
    } while (0)

/* Portable finite test without linking libm: NaN is the only value != itself; +/-Inf survive a divide-by-2
 * unchanged (any finite value is strictly reduced in magnitude, or is 0). */
static int is_finite_d(double x)
{
    if (x != x) return 0;             /* NaN */
    if (x != 0.0 && x == x / 2.0) return 0; /* +/-Inf */
    return 1;
}

/* exact-ish equality for the clean test values (all powers of ten -> representable) */
static int approx(double a, double b)
{
    double d = a - b;
    if (d < 0) d = -d;
    return d < 1e-9;
}

static void test_compute_pct(void)
{
    /* first sample / zero interval: prev == now for both counters -> 0, not NaN (0/0 guarded) */
    double r = hc_sysstat_compute_pct(0, 0, 0, 0, 100);
    CHECK(approx(r, 0.0), "compute_pct: first sample (all zero) -> 0");
    CHECK(is_finite_d(r), "compute_pct: first sample is finite (no 0/0 NaN)");

    /* Δwall_ms == 0 with a real jiffie delta -> 0 (guarded divide-by-zero, would otherwise be +Inf) */
    r = hc_sysstat_compute_pct(0, 1000, 5000, 5000, 100);
    CHECK(approx(r, 0.0), "compute_pct: zero wall interval -> 0");
    CHECK(is_finite_d(r), "compute_pct: zero wall interval is finite (no +Inf)");

    /* known interval: Δjiffies=100, clk_tck=100 -> 1.0 cpu-sec; Δwall=1000ms -> 1.0 wall-sec => 100.0% */
    r = hc_sysstat_compute_pct(0, 100, 1000, 2000, 100);
    CHECK(approx(r, 100.0), "compute_pct: 100 jiffies / 1s @ 100Hz over 1s wall -> 100%");

    /* half a core: Δjiffies=50 @ 100Hz -> 0.5 cpu-sec over 1.0 wall-sec => 50.0% */
    r = hc_sysstat_compute_pct(0, 50, 1000, 2000, 100);
    CHECK(approx(r, 50.0), "compute_pct: 50 jiffies / 1s @ 100Hz over 1s wall -> 50%");

    /* multi-core busy: Δjiffies=200 @ 100Hz -> 2.0 cpu-sec over 1.0 wall-sec => 200% (NOT clamped to 100) */
    r = hc_sysstat_compute_pct(0, 200, 1000, 2000, 100);
    CHECK(approx(r, 200.0), "compute_pct: 200 jiffies over 1s -> 200% (multi-core, uncapped)");

    /* a sub-interval: Δjiffies=25 @ 100Hz = 0.25 cpu-sec over Δwall 500ms = 0.5s => 50% */
    r = hc_sysstat_compute_pct(100, 125, 10000, 10500, 100);
    CHECK(approx(r, 50.0), "compute_pct: 25 jiffies over 500ms @ 100Hz -> 50%");

    /* now_jiffies < prev_jiffies (counter went backwards) -> 0, never negative */
    r = hc_sysstat_compute_pct(500, 100, 1000, 2000, 100);
    CHECK(approx(r, 0.0), "compute_pct: backwards jiffies -> 0 (never negative)");
    CHECK(r >= 0.0, "compute_pct: backwards jiffies is non-negative");

    /* now_wall_ms < prev_wall_ms (realtime clock stepped back) -> 0 */
    r = hc_sysstat_compute_pct(0, 100, 5000, 4000, 100);
    CHECK(approx(r, 0.0), "compute_pct: backwards wall clock -> 0");

    /* clk_tck <= 0 -> 0 (no tick rate, would divide by zero) */
    r = hc_sysstat_compute_pct(0, 100, 1000, 2000, 0);
    CHECK(approx(r, 0.0), "compute_pct: clk_tck 0 -> 0");
    r = hc_sysstat_compute_pct(0, 100, 1000, 2000, -1);
    CHECK(approx(r, 0.0), "compute_pct: clk_tck negative -> 0");
}

/* burn a little CPU so the second sample sees a real jiffie delta (a sleep would NOT add CPU time). */
static volatile uint64_t g_sink;
static void burn_cpu_ms(unsigned ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t acc = 1;
    for (;;) {
        for (int i = 0; i < 100000; i++) acc = acc * 1103515245u + 12345u; /* keep the FPU/ALU busy */
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (double)(now.tv_sec - start.tv_sec) * 1000.0 +
                    (double)(now.tv_nsec - start.tv_nsec) / 1e6;
        if (el >= (double)ms) break;
    }
    g_sink = acc; /* defeat dead-code elimination */
}

static void test_self_smoke(void)
{
    hc_sysstat *s = hc_sysstat_open(0); /* self */
    CHECK(s != NULL, "smoke: open(self) succeeds");
    if (!s) return;

    struct hc_sysstat_sample first = {0};
    CHECK(hc_sysstat_sample(s, &first) == 0, "smoke: first sample succeeds");
    CHECK(approx(first.cpu_pct, 0.0), "smoke: first sample cpu_pct is 0 (no prior)");
    CHECK(first.rss_bytes > 0, "smoke: first sample rss_bytes > 0");
    CHECK(first.wall_ms > 0, "smoke: first sample wall_ms > 0");
    CHECK(first.pid > 0, "smoke: first sample reports a real pid");

    burn_cpu_ms(40); /* enough wall+CPU for a measurable, stable-ish delta */

    struct hc_sysstat_sample second = {0};
    CHECK(hc_sysstat_sample(s, &second) == 0, "smoke: second sample succeeds");
    CHECK(second.rss_bytes > 0, "smoke: second sample rss_bytes > 0");
    CHECK(second.wall_ms >= first.wall_ms, "smoke: wall_ms is monotonic non-decreasing");
    CHECK(second.uptime_ms > 0, "smoke: process uptime_ms is positive");
    CHECK(second.uptime_ms < 100ull * 365 * 24 * 3600 * 1000, "smoke: uptime_ms is sane (< 100y)");
    CHECK(second.cpu_pct >= 0.0, "smoke: second cpu_pct is non-negative");
    CHECK(is_finite_d(second.cpu_pct), "smoke: second cpu_pct is finite (no NaN/Inf)");
    CHECK(second.pid == first.pid, "smoke: pid is stable across samples");

    fprintf(stderr,
            "smoke numbers: pid=%d rss=%llu bytes (%.1f MiB) uptime=%llu ms wall=%llu ms "
            "cpu_pct[first]=%.3f cpu_pct[second]=%.3f\n",
            second.pid, (unsigned long long)second.rss_bytes,
            (double)second.rss_bytes / (1024.0 * 1024.0), (unsigned long long)second.uptime_ms,
            (unsigned long long)second.wall_ms, first.cpu_pct, second.cpu_pct);

    hc_sysstat_close(s);

    /* a clearly-dead pid: open should refuse it (its /proc entry is unreadable) */
    hc_sysstat *dead = hc_sysstat_open(2000000000); /* pid far above any live process */
    CHECK(dead == NULL, "smoke: open of a non-existent pid returns NULL");
    hc_sysstat_close(dead); /* close(NULL) must be a safe no-op */
}

int main(void)
{
    test_compute_pct();
    test_self_smoke();

    if (g_fails == 0) printf("test_hc_sysstat: OK\n");
    return g_fails ? 1 : 0;
}
