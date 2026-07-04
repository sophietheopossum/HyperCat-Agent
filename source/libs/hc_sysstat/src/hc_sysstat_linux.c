/* hc_sysstat_linux.c — the Linux /proc reader for the sampler. See hc_sysstat.h.
 *
 * Turns a pid into raw {jiffies, RSS, starttime} by reading three /proc files, then hands the jiffie deltas
 * to the OS-free hc_sysstat_compute_pct (hc_sysstat.c) — this file does NOT do the percentage math.
 *
 * The one trap this file owns is /proc/<pid>/stat field 2 (comm): it is wrapped in parentheses and the
 * executable name inside CAN contain spaces AND ')'  (e.g. a process named "(weird )(name)"). So we cannot
 * sscanf/strtok the whole line — a name with spaces would shift every later field. We instead find the LAST
 * ')' in the buffer and tokenize only the REMAINDER: after the last ')', token 1 is field 3 (state), so
 * field N is token (N-2). utime=field 14 -> token 12, stime=field 15 -> token 13, starttime=field 22 ->
 * token 20. (proc(5).) This is robust to any bytes inside comm because comm is entirely BEFORE that ')'.
 *
 * Safety: each /proc file is read once into a FIXED stack buffer with a single bounded read() after an
 * O_RDONLY|O_CLOEXEC open — no heap, no unbounded read, no overread; a short read is NUL-terminated within
 * the buffer. ENOENT / a read failure (a pid that exited) -> the call returns -1 and the caller drops it.
 * Read-only throughout; nothing but the target pid's own /proc entries is opened. */

#define _DEFAULT_SOURCE 1

#include "hc_sysstat.h"
#include "hc_sysstat_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROC_BUF 4096 /* one /proc file fits comfortably; we read at most this and NUL-terminate inside it */

/* Read a whole /proc file into `buf` (cap bytes) with a single bounded read(); NUL-terminate. Returns the
 * byte length read (>= 0) on success, or -1 on open/read failure (a dead pid -> ENOENT). Never overruns:
 * we read at most cap-1 bytes so the terminator always fits. */
static int read_proc_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do {
        n = read(fd, buf, cap - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

/* Read /proc/<pid>/uptime field 1 (system uptime, seconds, fractional) into *out_sec.
 * Returns 0 on success, -1 on failure. (No comm trap here — uptime is two plain space-separated floats.) */
static int read_system_uptime_sec(double *out_sec)
{
    char buf[PROC_BUF];
    if (read_proc_file("/proc/uptime", buf, sizeof buf) < 0) return -1;
    char *end = NULL;
    double up = strtod(buf, &end);
    if (end == buf || up < 0.0) return -1; /* no number parsed, or a nonsense negative */
    *out_sec = up;
    return 0;
}

/* Parse /proc/<pid>/stat for utime+stime (summed -> *out_jiffies) and starttime (-> *out_starttime).
 * Implements the last-')' rule described in the file banner. Returns 0 on success, -1 if the line is
 * malformed (no ')', or too few tokens after it). */
static int parse_stat(const char *buf, uint64_t *out_jiffies, uint64_t *out_starttime)
{
    /* find the LAST ')' — everything before it (pid + the parenthesized comm) is skipped wholesale, so a
     * comm containing spaces/')' cannot shift the field offsets. */
    const char *p = strrchr(buf, ')');
    if (!p) return -1;
    p++; /* now positioned just after field 2; the next token is field 3 (state) */

    /* Tokenize the remainder on runs of whitespace. We need token 12 (utime), 13 (stime), 20 (starttime),
     * counting from 1 where token 1 == field 3 (state). strtoull on each token; a token that is not a
     * number for the fields we use makes us fail. We never write into buf (read-only scan with a cursor). */
    uint64_t utime = 0, stime = 0, starttime = 0;
    int got_utime = 0, got_stime = 0, got_starttime = 0;

    int token = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++; /* skip leading whitespace */
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++; /* advance to token end */
        token++;

        if (token == 12 || token == 13 || token == 20) {
            char *end = NULL;
            unsigned long long v = strtoull(tok, &end, 10);
            if (end == tok) return -1; /* a numeric field we need was not a number -> malformed */
            if (token == 12) { utime = (uint64_t)v; got_utime = 1; }
            else if (token == 13) { stime = (uint64_t)v; got_stime = 1; }
            else { starttime = (uint64_t)v; got_starttime = 1; }
        }
        if (token >= 20) break; /* we have everything we need; stop scanning */
    }

    if (!got_utime || !got_stime || !got_starttime) return -1; /* line was truncated before field 22 */
    *out_jiffies   = utime + stime;
    *out_starttime = starttime;
    return 0;
}

/* Read /proc/<pid>/statm field 2 (resident pages) and convert to bytes via the page size.
 * Returns 0 on success, -1 on failure. statm is plain space-separated integers (no comm trap). */
static int read_rss_bytes(int pid, uint64_t *out_rss)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/statm", pid);
    char buf[PROC_BUF];
    if (read_proc_file(path, buf, sizeof buf) < 0) return -1;

    /* field 1 = total program size (pages), field 2 = resident pages. Parse the first two integers. */
    char *end = NULL;
    (void)strtoull(buf, &end, 10); /* field 1 (size) — skipped */
    if (end == buf) return -1;
    const char *p = end;
    unsigned long long resident = strtoull(p, &end, 10); /* field 2 (resident) */
    if (end == p) return -1;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return -1;
    *out_rss = (uint64_t)resident * (uint64_t)page;
    return 0;
}

/* CLOCK_REALTIME now, in ms since the epoch. */
static uint64_t wall_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

hc_sysstat *hc_sysstat_open(int pid)
{
    if (pid <= 0) pid = (int)getpid();

    /* Reject a bad/dead pid up front: its stat must be readable and parseable right now. (Also validates
     * the pid before we ever store it.) */
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char buf[PROC_BUF];
    if (read_proc_file(path, buf, sizeof buf) < 0) return NULL;
    uint64_t j = 0, st = 0;
    if (parse_stat(buf, &j, &st) != 0) return NULL;

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) return NULL; /* without a tick rate we cannot compute CPU% — refuse rather than lie */

    hc_sysstat *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pid       = pid;
    s->clk_tck   = clk;
    s->have_prev = 0; /* first sample reports cpu_pct 0 (no prior) */
    return s;
}

int hc_sysstat_sample(hc_sysstat *s, struct hc_sysstat_sample *out)
{
    if (!s || !out) return -1;

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", s->pid);
    char buf[PROC_BUF];
    if (read_proc_file(path, buf, sizeof buf) < 0) return -1; /* pid gone -> caller drops it */

    uint64_t jiffies = 0, starttime = 0;
    if (parse_stat(buf, &jiffies, &starttime) != 0) return -1;

    uint64_t rss = 0;
    if (read_rss_bytes(s->pid, &rss) != 0) return -1;

    double sys_up_sec = 0.0;
    if (read_system_uptime_sec(&sys_up_sec) != 0) return -1;

    uint64_t now_wall = wall_now_ms();

    /* Process uptime = system uptime - (starttime / clk_tck), in ms. Guard the subtraction against a
     * negative (a clock/boot-time edge or a stat parsed mid-update) -> clamp at 0. */
    double proc_start_sec = (double)starttime / (double)s->clk_tck;
    double proc_up_sec    = sys_up_sec - proc_start_sec;
    uint64_t uptime_ms    = (proc_up_sec > 0.0) ? (uint64_t)(proc_up_sec * 1000.0) : 0u;

    /* CPU% via the OS-free core, but only once we hold a prior interval: the first sample has no previous
     * jiffies/wall to delta against, so it reports 0 (the core would also return 0 for prev == now, but we
     * never fabricate a prior — we simply skip the call until have_prev). */
    double cpu_pct;
    if (s->have_prev) {
        cpu_pct = hc_sysstat_compute_pct(s->prev_jiffies, jiffies, s->prev_wall_ms, now_wall, s->clk_tck);
    } else {
        cpu_pct = 0.0; /* no prior interval */
    }

    out->cpu_pct   = cpu_pct;
    out->rss_bytes = rss;
    out->uptime_ms = uptime_ms;
    out->wall_ms   = now_wall;
    out->pid       = s->pid;

    /* roll the window forward for the next call's delta */
    s->prev_jiffies = jiffies;
    s->prev_wall_ms = now_wall;
    s->have_prev    = 1;
    return 0;
}

void hc_sysstat_close(hc_sysstat *s)
{
    free(s);
}
