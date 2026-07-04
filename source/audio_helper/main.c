/* hc_audio_helper — the confined audio decode/probe helper (W-Audio.1). A short-lived child the HOST spawns per
 * file to keep the vendored audio decoders (dr_wav/dr_mp3/dr_flac/stb_vorbis) — which parse UNTRUSTED bytes — OUT
 * of the host's address space. It reads ONE complete audio file from stdin (a regular-file fd the host opened
 * jailed and handed us), drops itself to the hc_confine floor (NO new fs, NO network, NO exec / new process),
 * runs hc_audio_probe, writes a fixed-binary reply (posture + bounded metadata) to stdout, and exits.
 *
 * Containment: a decoder RCE on a hostile file is confined to THIS throwaway process — a separate address space
 * with NO host secret in it (the host spawns the helper with a SANITIZED/empty environment, so the env-borne API
 * key and every other host secret stay OUT of the decoder's memory; the capability signing key + project state
 * live only in the host), no filesystem to pivot through (Landlock grants zero subtrees here — only the auto
 * /dev/null), no socket to exfiltrate over (the network-deny seccomp filter), and no exec/clone to escalate (the
 * worker KILL set). The host treats the reply as untrusted, bounds every field, applies a hard timeout, and
 * SIGKILL+reaps so a wedged/looping decoder cannot hang it.
 *
 * Protocol:  stdin (fd 0) = the audio file (a seekable regular fd). stdout (fd 1) = the reply pipe. stderr (fd 2)
 *            = the posture log, emitted ONLY when the confinement is degraded (fail-open-but-loud). argv[1] = the
 *            job ("probe"). No other input — the filename is deliberately NOT passed (the magic bytes are
 *            authoritative for format detection, and the child needs to know nothing about the host).
 * Threading: single-threaded by construction — hc_confine denies clone/new-thread; apply runs on this thread.
 * Lifetime:  one file per process; _exit tears everything down. */

#define _GNU_SOURCE /* close_range / SYS_close_range */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "audio_helper_wire.h"
#include "hc_audio.h"
#include "hc_confine.h"

/* Drop every inherited fd except stdio (0,1,2). The host may not mark all of its long-lived fds close-on-exec, and
 * an inherited bus/store/socket fd would SURVIVE Landlock (which mediates open(), never an already-open fd) — so a
 * compromised decoder could read/write it. After this, a decoder has only the input file (0) and the reply pipe
 * (1). Done BEFORE confinement + before touching any untrusted byte. */
static void drop_inherited_fds(void)
{
    long rc = -1;
#if defined(SYS_close_range)
    rc = syscall(SYS_close_range, (unsigned)3, ~0U, 0u); /* Linux 5.9+: the clean primitive */
#endif
    if (rc != 0) {
        long maxfd = sysconf(_SC_OPEN_MAX); /* fallback: brute-close up to the soft limit */
        if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++) close(fd);
    }
}

/* read exactly `n` bytes of a regular fd into buf (handling short reads / EINTR); returns the count actually read. */
static size_t read_full(int fd, unsigned char *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, buf + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    return total;
}

/* write exactly `n` bytes to fd (handling short writes / EINTR); returns true iff all `n` were written. */
static int write_full(int fd, const unsigned char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (w == 0) return 0;
        off += (size_t)w;
    }
    return 1;
}

/* PROBE job: write the fixed metadata reply (posture + bounded meta) to stdout. Returns the process exit code.
 * net_denied rides the seccomp install (apply returns OK only when BOTH the worker + network-deny filters loaded),
 * so seccomp_active implies the network floor is live. */
static int do_probe(const unsigned char *buf, size_t total, const hc_confine_applied *posture, hc_confine_status cst)
{
    hc_audio_helper_reply reply;
    memset(&reply, 0, sizeof reply);
    reply.seccomp = (uint8_t)(posture->seccomp_active ? 1 : 0);
    reply.landlock_fs = (uint8_t)(posture->landlock_fs ? 1 : 0);
    reply.net_denied = (uint8_t)(posture->seccomp_active ? 1 : 0);
    reply.landlock_abi = (int32_t)posture->landlock_abi;
    reply.confine_status = (int32_t)cst;

    hc_audio_meta m;
    /* name_hint = NULL: the magic bytes are authoritative; the helper is told nothing about the file. */
    if (buf && total > 0 && hc_audio_probe(buf, total, NULL, &m) == 0) {
        reply.ok = 1;
        reply.meta = m;
    }
    (void)write_full(1, (const unsigned char *)&reply, sizeof reply);
    return reply.ok ? 0 : 1;
}

/* DECODE job: decode the WHOLE track to interleaved s16 PCM (bounded by the transfer cap) and write the fixed
 * decode-reply HEADER + (iff ok) the PCM payload to stdout. The host plays the raw samples via the player's PCM
 * entry, so no decoder runs in the host. Returns the process exit code. */
static int do_decode(const unsigned char *buf, size_t total, const hc_confine_applied *posture, hc_confine_status cst)
{
    hc_audio_helper_decode_reply reply;
    memset(&reply, 0, sizeof reply);
    reply.seccomp = (uint8_t)(posture->seccomp_active ? 1 : 0);
    reply.landlock_fs = (uint8_t)(posture->landlock_fs ? 1 : 0);
    reply.net_denied = (uint8_t)(posture->seccomp_active ? 1 : 0);
    reply.landlock_abi = (int32_t)posture->landlock_abi;
    reply.confine_status = (int32_t)cst;

    int16_t      *pcm = NULL;
    uint64_t      frames = 0;
    unsigned      rate = 0, channels = 0;
    int           truncated = 0;
    hc_audio_meta m;
    if (buf && total > 0 &&
        hc_audio_decode_pcm(buf, total, NULL, HC_AUDIO_HELPER_MAX_PCM_BYTES, &pcm, &frames, &rate, &channels,
                            &truncated, &m) == 0) {
        reply.ok = 1;
        reply.samplerate = rate;
        reply.channels = channels;
        reply.frames = frames;
        reply.truncated = (uint8_t)(truncated ? 1 : 0);
        reply.meta = m;
    }
    /* header first, then the PCM payload (iff ok). A short write just yields a host-side fallback. */
    if (!write_full(1, (const unsigned char *)&reply, sizeof reply)) {
        free(pcm);
        return 1;
    }
    if (reply.ok) (void)write_full(1, (const unsigned char *)pcm, (size_t)frames * channels * sizeof(int16_t));
    free(pcm);
    return reply.ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *job = (argc >= 2) ? argv[1] : "";
    int         is_probe = strcmp(job, HC_AUDIO_HELPER_JOB_PROBE) == 0;
    int         is_decode = strcmp(job, HC_AUDIO_HELPER_JOB_DECODE) == 0;
    if (!is_probe && !is_decode) _exit(2); /* unknown/missing job — the host only ever requests probe/decode */

    drop_inherited_fds();

    /* size the input from the (regular, seekable) stdin fd the host handed us; reject non-regular / empty /
     * oversized before allocating. */
    struct stat st;
    long long   sz = -1;
    if (fstat(0, &st) == 0 && S_ISREG(st.st_mode)) sz = (long long)st.st_size;

    /* Build + apply the confinement floor BEFORE reading or parsing the untrusted bytes: NO fs subtree (only the
     * auto /dev/null hc_confine always grants), network DENIED, default ERRNO (a missed syscall degrades, never
     * crashes). All shared libraries are already mapped (the loader ran before main), so zero fs grants is correct.
     * Fail-open-but-loud: on a kernel without Landlock+seccomp the job still runs (the decoders' own fuzz+bounds
     * hardening is the remaining mitigation) and we bark on stderr. */
    hc_confine_applied posture = {0};
    hc_confine_status  cst = HC_CONFINE_ERR_INVALID;
    hc_confine_spec   *spec = hc_confine_spec_new();
    if (spec) {
        hc_confine_deny_network(spec);
        hc_confine_set_syscall_policy(spec, HC_CONFINE_SYSCALL_WORKER_DEFAULT);
        cst = hc_confine_apply(spec, &posture);
        hc_confine_spec_free(spec);
    }
    if (cst != HC_CONFINE_OK)
        fprintf(stderr,
                "hc_audio_helper: DEGRADED confine (%s) — parsing untrusted audio with reduced isolation\n",
                hc_confine_strerror(cst));

    /* read the whole file into memory (trusted I/O on the host-handed fd) — both jobs need the complete bytes. */
    unsigned char *buf = NULL;
    size_t         total = 0;
    if (sz > 0 && (size_t)sz <= HC_AUDIO_MAX_BYTES) {
        buf = (unsigned char *)malloc((size_t)sz);
        if (buf) total = read_full(0, buf, (size_t)sz);
    }

    int rc = is_probe ? do_probe(buf, total, &posture, cst) : do_decode(buf, total, &posture, cst);
    free(buf);
    _exit(rc);
}
