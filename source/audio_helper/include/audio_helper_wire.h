#ifndef AUDIO_HELPER_WIRE_H
#define AUDIO_HELPER_WIRE_H

#include <assert.h> /* static_assert: C11 maps it to _Static_assert; a keyword in C++ — works from both ends */
#include <stdint.h>

#include "hc_audio.h" /* hc_audio_meta — the probed metadata carried in the reply */

/* audio_helper_wire — the fixed-binary contract between the HOST (app/host_services) and the confined audio
 * decode/probe helper (audio_helper/main.c). It is the WHOLE host<->helper protocol surface (W-Audio).
 *
 * Why a process boundary: the vendored audio decoders (dr_wav/dr_mp3/dr_flac/stb_vorbis) parse UNTRUSTED file
 * bytes. Running them in the host address space means a decoder RCE reads the host's secrets/state. The helper
 * runs them in a SEPARATE, hc_confine'd, throwaway process instead — the only design that truly contains a
 * decoder compromise (distinct address space + no fs/network/exec floor).
 *
 * Transport: the host hands the helper ONE audio file as its stdin (fd 0, a jailed regular-file fd) and a reply
 * pipe as its stdout (fd 1); stderr (fd 2) carries the helper's posture log (only when DEGRADED). The helper
 * writes EXACTLY one hc_audio_helper_reply to stdout, then exits. Same build both ends => identical struct ABI.
 *
 * TRUST: every field in the reply is UNTRUSTED — the helper produced it by parsing hostile bytes (and could be
 * compromised). The host MUST force-NUL-terminate the meta strings, clamp the numeric fields to the hc_audio
 * caps, and range-check `fmt` before use; tag strings reaching the LLM are defanged at their injection seam (the
 * conductor), and the UI renders them format-string-safe. */

/* argv[1] job selector. "probe" (W-Audio.1) returns metadata only; "decode" (W-Audio.2) returns the full track
 * decoded to interleaved s16 PCM. An unknown job => a no-op failure reply (ok=0). */
#define HC_AUDIO_HELPER_JOB_PROBE  "probe"
#define HC_AUDIO_HELPER_JOB_DECODE "decode"

typedef struct {
    uint8_t  ok;            /* 1 = `meta` is a successful probe; 0 = unrecognized / failed / wrong job          */
    uint8_t  seccomp;       /* posture: a seccomp syscall filter is live in the helper                          */
    uint8_t  landlock_fs;   /* posture: a Landlock fs ruleset is live in the helper                             */
    uint8_t  net_denied;    /* posture: the network-deny filter is live (rides the seccomp install)             */
    int32_t  landlock_abi;  /* posture: the Landlock ABI the kernel reported (0 = none)                         */
    int32_t  confine_status;/* the hc_confine_status apply() returned (0 = HC_CONFINE_OK = full floor)          */
    hc_audio_meta meta;     /* the probed metadata — valid IFF ok == 1                                          */
} hc_audio_helper_reply;

/* The DECODE reply (W-Audio.2): a fixed header FOLLOWED on the pipe by exactly `frames * channels *
 * sizeof(int16_t)` bytes of interleaved s16 PCM (iff ok==1). The host re-clamps `samplerate`/`channels`/`frames`
 * against the hc_audio caps + this transfer cap before reading/playing — the helper, having parsed hostile bytes,
 * is untrusted. The PCM itself is just samples (no structure to exploit); the host plays it via the raw-PCM
 * player entry, so NO decoder runs in the host. `truncated` => the track exceeded the cap and was clipped. */
#define HC_AUDIO_HELPER_MAX_PCM_BYTES HC_AUDIO_MAX_PCM_BYTES /* host<->helper PCM transfer cap (256 MiB) */

typedef struct {
    uint8_t  ok;            /* 1 = decode succeeded, the PCM payload follows; 0 = failed/unrecognized/wrong job  */
    uint8_t  seccomp;       /* posture (as in the probe reply)                                                  */
    uint8_t  landlock_fs;
    uint8_t  net_denied;
    int32_t  landlock_abi;
    int32_t  confine_status;
    uint32_t samplerate;    /* Hz                                                                               */
    uint32_t channels;      /* 1..HC_AUDIO_MAX_CHANNELS                                                         */
    uint64_t frames;        /* interleaved frames; the PCM payload = frames*channels*2 bytes                    */
    uint8_t  truncated;     /* 1 = the decode hit the cap (a track longer than HC_AUDIO_HELPER_MAX_PCM_BYTES)   */
    uint8_t  _pad[7];
    hc_audio_meta meta;     /* now-playing metadata (title/artist/fmt)                                          */
} hc_audio_helper_decode_reply;

/* Wire-ABI tripwires: a layout change to either reply (e.g. hc_audio_meta growing) must fail to COMPILE rather
 * than silently misparse the pipe. Sizes are the natural-aligned LP64 layout; a deliberate change updates these
 * AND both ends. */
static_assert(sizeof(hc_audio_helper_reply) == 1064,
              "hc_audio_helper_reply layout changed — bump the protocol and update both the host and the helper");
static_assert(sizeof(hc_audio_helper_decode_reply) == 1088,
              "hc_audio_helper_decode_reply layout changed — bump the protocol and update both ends");

#endif /* AUDIO_HELPER_WIRE_H */
