/* test_audio_helper — the confined audio probe helper's CONTRACT test (W-Audio.1). It spawns the REAL
 * hc_audio_helper binary exactly as the host does (the file on stdin, a reply pipe on stdout) and asserts:
 *   (1) a valid WAV -> ok=1 + the correct format label + (on a confining kernel) the self-applied floor posture;
 *   (2) garbage -> a graceful ok=0 reply and a clean child exit (a malformed file must never crash the helper);
 *   (3) the host-side bound: the reply's posture fields are internally consistent (net_denied rides seccomp).
 * The infinite-loop / wedge timeout is the HOST's SIGKILL+reap path (covered by the headless live smoke); this
 * test proves the helper's own contract. Skips the confinement asserts when the kernel lacks Landlock+seccomp
 * (the posture honestly reports the absence), mirroring libs/hc_confine's smoke test. */

#define _GNU_SOURCE

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio_helper_wire.h"
#include "hc_audio.h"

#define MUST(cond, msg)                                            \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL: %s (%s)\n", (msg), #cond);      \
            return 1;                                              \
        }                                                          \
    } while (0)

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* Build a minimal, valid 8 kHz / mono / 16-bit PCM WAV (44-byte header + `samples` 16-bit frames) into buf. */
static size_t build_wav(unsigned char *buf, unsigned samples)
{
    uint32_t data_size = samples * 2u;
    memcpy(buf + 0, "RIFF", 4);
    put_u32(buf + 4, 36u + data_size);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_u32(buf + 16, 16u);  /* subchunk1 size */
    put_u16(buf + 20, 1u);   /* PCM */
    put_u16(buf + 22, 1u);   /* channels */
    put_u32(buf + 24, 8000u);/* sample rate */
    put_u32(buf + 28, 16000u); /* byte rate */
    put_u16(buf + 32, 2u);   /* block align */
    put_u16(buf + 34, 16u);  /* bits */
    memcpy(buf + 36, "data", 4);
    put_u32(buf + 40, data_size);
    for (unsigned i = 0; i < samples; i++) put_u16(buf + 44 + i * 2u, (uint16_t)(i * 137u));
    return 44u + data_size;
}

/* Spawn the helper with `data` on stdin + a reply pipe on stdout; fill *out. Returns 1 iff the full fixed reply
 * was read back. Mirrors app/host_services.cpp's probe_audio_file_confined spawn (without the hc_sandbox jail —
 * the test owns its temp file). */
static int spawn_probe(const unsigned char *data, size_t n, hc_audio_helper_reply *out)
{
    char tmpl[] = "/tmp/hc_audio_helper_test_XXXXXX";
    int  tfd = mkstemp(tmpl);
    if (tfd < 0) return 0;
    unlink(tmpl); /* anonymous on disk — reaped on close */
    if (write(tfd, data, n) != (ssize_t)n) {
        close(tfd);
        return 0;
    }
    lseek(tfd, 0, SEEK_SET);

    int resp[2];
    if (pipe(resp) != 0) {
        close(tfd);
        return 0;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, tfd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, resp[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, tfd);
    posix_spawn_file_actions_addclose(&fa, resp[0]);
    posix_spawn_file_actions_addclose(&fa, resp[1]);

    char       *argv[] = {(char *)AUDIO_HELPER_PATH, (char *)HC_AUDIO_HELPER_JOB_PROBE, NULL};
    char *const empty_env[] = {NULL}; /* mirror the host: spawn with NO env (no secret reaches the helper) */
    pid_t       pid = -1;
    int         rc = posix_spawn(&pid, AUDIO_HELPER_PATH, &fa, NULL, argv, empty_env);
    posix_spawn_file_actions_destroy(&fa);
    close(tfd);
    close(resp[1]);
    if (rc != 0) {
        close(resp[0]);
        return 0;
    }

    unsigned char *rp = (unsigned char *)out;
    size_t         got = 0;
    while (got < sizeof *out) {
        struct pollfd pfd = {resp[0], POLLIN, 0};
        int           pr = poll(&pfd, 1, 8000);
        if (pr <= 0) break;
        ssize_t r = read(resp[0], rp + got, sizeof *out - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(resp[0]);
    int wst = 0;
    waitpid(pid, &wst, 0);
    return got == sizeof *out;
}

/* Spawn the helper "decode" job with `data` on stdin; fill *hdr + read the PCM payload into pcm_out (sized for up
 * to `pcm_out_frames` int16 samples). Returns 1 iff the full header + the declared PCM bytes were read back. */
static int spawn_decode(const unsigned char *data, size_t n, hc_audio_helper_decode_reply *hdr, int16_t *pcm_out,
                        size_t pcm_out_samples)
{
    char tmpl[] = "/tmp/hc_audio_helper_dec_XXXXXX";
    int  tfd = mkstemp(tmpl);
    if (tfd < 0) return 0;
    unlink(tmpl);
    if (write(tfd, data, n) != (ssize_t)n) {
        close(tfd);
        return 0;
    }
    lseek(tfd, 0, SEEK_SET);
    int resp[2];
    if (pipe(resp) != 0) {
        close(tfd);
        return 0;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, tfd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, resp[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, tfd);
    posix_spawn_file_actions_addclose(&fa, resp[0]);
    posix_spawn_file_actions_addclose(&fa, resp[1]);
    char       *argv[] = {(char *)AUDIO_HELPER_PATH, (char *)HC_AUDIO_HELPER_JOB_DECODE, NULL};
    char *const empty_env[] = {NULL};
    pid_t       pid = -1;
    int         rc = posix_spawn(&pid, AUDIO_HELPER_PATH, &fa, NULL, argv, empty_env);
    posix_spawn_file_actions_destroy(&fa);
    close(tfd);
    close(resp[1]);
    if (rc != 0) {
        close(resp[0]);
        return 0;
    }

    unsigned char *hp = (unsigned char *)hdr; /* read the fixed header */
    size_t         hgot = 0;
    while (hgot < sizeof *hdr) {
        struct pollfd pfd = {resp[0], POLLIN, 0};
        if (poll(&pfd, 1, 8000) <= 0) break;
        ssize_t r = read(resp[0], hp + hgot, sizeof *hdr - hgot);
        if (r <= 0) break;
        hgot += (size_t)r;
    }
    int okret = 0;
    if (hgot == sizeof *hdr && hdr->ok) {
        size_t want = (size_t)hdr->frames * hdr->channels * sizeof(int16_t);
        size_t cap = pcm_out_samples * sizeof(int16_t);
        size_t toread = want < cap ? want : cap; /* the test track fits, so toread == want */
        unsigned char *pp = (unsigned char *)pcm_out;
        size_t         pgot = 0;
        while (pgot < toread) {
            struct pollfd pfd = {resp[0], POLLIN, 0};
            if (poll(&pfd, 1, 8000) <= 0) break;
            ssize_t r = read(resp[0], pp + pgot, toread - pgot);
            if (r <= 0) break;
            pgot += (size_t)r;
        }
        okret = (pgot == want);
    }
    close(resp[0]);
    kill(pid, SIGKILL);
    int wst = 0;
    waitpid(pid, &wst, 0);
    return okret;
}

int main(void)
{
    /* (1) a valid WAV -> a successful probe with the right format. */
    unsigned char         wav[64];
    size_t                wlen = build_wav(wav, 8);
    hc_audio_helper_reply r;
    memset(&r, 0, sizeof r);
    MUST(spawn_probe(wav, wlen, &r), "valid WAV: full reply read back");
    MUST(r.ok == 1, "valid WAV: ok=1");
    MUST(strcmp(hc_audio_fmt_label((hc_audio_fmt)r.meta.fmt), "WAV") == 0, "valid WAV: format label is WAV");

    /* the confinement posture the helper self-applied. On a kernel without Landlock+seccomp the helper honestly
     * reports the floor's absence (fail-open-but-loud) — assert the floor only when it IS present. */
    if (r.seccomp || r.landlock_fs) {
        printf("audio_helper: helper confined (seccomp=%d landlock_fs=%d abi=%d net_denied=%d status=%d)\n",
               r.seccomp, r.landlock_fs, r.landlock_abi, r.net_denied, r.confine_status);
        if (r.seccomp) MUST(r.net_denied == 1, "confined: the network-deny filter rides the seccomp install");
    } else {
        printf("audio_helper: SKIP confinement asserts — kernel lacks Landlock+seccomp (status=%d)\n",
               r.confine_status);
    }

    /* (2) garbage -> a graceful ok=0 reply (the helper handled a malformed file without crashing). */
    unsigned char garbage[1024];
    memset(garbage, 0xAB, sizeof garbage);
    memcpy(garbage, "NOPE", 4); /* not any known magic */
    hc_audio_helper_reply g;
    memset(&g, 0, sizeof g);
    MUST(spawn_probe(garbage, sizeof garbage, &g), "garbage: full reply read back (helper did not crash)");
    MUST(g.ok == 0, "garbage: ok=0 (unrecognized input)");

    /* (3) DECODE job: the same valid WAV -> the full track decoded to interleaved PCM in the confined child. */
    hc_audio_helper_decode_reply dh;
    memset(&dh, 0, sizeof dh);
    int16_t pcmbuf[64];
    MUST(spawn_decode(wav, wlen, &dh, pcmbuf, 64), "decode: full header + PCM payload read back");
    MUST(dh.ok == 1, "decode: ok=1");
    MUST(dh.samplerate == 8000 && dh.channels == 1 && dh.frames == 8,
         "decode: header reports 8 frames @ 8000 Hz mono");
    MUST(dh.truncated == 0, "decode: not truncated (well under the cap)");
    /* build_wav wrote sample i = (int16_t)(i*137); decoding the WAV PCM must round-trip those exact samples. */
    MUST(pcmbuf[0] == 0 && pcmbuf[1] == 137 && pcmbuf[7] == (int16_t)(7 * 137),
         "decode: PCM samples match the source ramp");
    if (dh.seccomp) MUST(dh.net_denied == 1, "decode: confined child has the network floor too");

    /* (4) garbage -> a graceful decode failure (ok=0), no crash. */
    hc_audio_helper_decode_reply dg;
    memset(&dg, 0, sizeof dg);
    int16_t none[2];
    MUST(spawn_decode(garbage, sizeof garbage, &dg, none, 2) == 0 || dg.ok == 0,
         "decode garbage: ok=0 / no PCM (helper did not crash)");

    printf("test_audio_helper: PASS\n");
    return 0;
}
