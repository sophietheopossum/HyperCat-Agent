#ifndef HC_SETTINGS_HPP
#define HC_SETTINGS_HPP

/* hc_settings — the operator's persisted application settings (WI-2 E1). A C++ HOST module: a plain value
 * struct + a small set of pure-ish free functions to parse / serialize / validate / env-merge / load / save
 * it. It is the SOURCE of the runtime knobs the host applies (accent + panel visibility + poll cadence LIVE;
 * provider model/base_url/embed + data paths at respawn/restart; the four limits injected into the worker
 * env; the egress allowlist). It deliberately depends on hc::json + hc::fs ONLY — never hc_ui, hc_policy,
 * host_storage or the orchestrator — so it stays a leaf the host maps to/from those layers (the UI gets a
 * separate UiSettings POD; the API key is NEVER a field here — it lives in hc_secrets).
 *
 * Owns:      nothing persistent — Settings is a value type the caller owns. load/save touch one file via
 *            hc::fs (a capped read; a temp+fsync+rename atomic write). A MISSING file is NOT an error
 *            (all-defaults reproduce today's runtime); a MALFORMED file degrades to defaults, never throws.
 * Threading: the free functions are reentrant over caller-owned Settings; merge_env reads the ambient
 *            process environment. No shared mutable state.
 * Lifetime:  Settings + EnvOverrides are values; the (const char*) accent etc. are std::string members.
 */

#include <map>
#include <string>
#include <vector>

namespace hcapp {

/* One operator-designated available model: an id (the provider model string) + an optional free-text note
 * (e.g. "fast/cheap", "best for code"). The catalog the Models panel edits + the role assignments draw from. */
struct ModelEntry {
    std::string id;
    std::string note;
};

/* The persisted settings, grouped by concern (mirrored in the on-disk JSON: ui/poll/provider/paths/limits/
 * security/models). Every default reproduces the current runtime, so a missing settings file changes nothing. */
struct Settings {
    int version = 1; /* schema version, for a future migration */

    /* ui */
    std::string             accent = "white";           /* one of the six restrained accents          */
    bool                    mascot = false;             /* the opt-in WI-5 mascot (default-OFF)        */
    std::map<std::string, bool> panels;                 /* panel name -> visible; SPARSE (missing = the
                                                         * panel's built-in default) so a new panel needs
                                                         * no schema change here                       */
    /* poll */
    int poll_hz = 2; /* host store-listing cadence in Hz (1..60; the live loop re-lists at this rate) */

    /* provider */
    std::string model;       /* chat model (empty = offline echo)        */
    std::string base_url;    /* empty = the OpenRouter default           */
    std::string embed_model; /* embeddings model (empty = no embeddings) */
    /* Bounds how long a REASONING model thinks, as the OpenAI-compatible `reasoning_effort` request
     * field. Measured on one model with one prompt, "max" produced roughly 4x the tokens of "low", so
     * this is really a wall-clock dial -- and on a rate-limited or free model wall-clock is the only
     * budget that binds. Accepted values are listed in hc_llm.h; an unrecognised one is DROPPED at the
     * request builder rather than forwarded, so a typo costs the provider default, not a 400 on every
     * call.
     *
     * DEFAULT EMPTY = omit the field entirely, which is byte-identical to the behaviour before this
     * setting existed. Deliberate: a non-empty default would change the request body for every existing
     * install. OpenRouter and OpenAI ignore a field their model has no notion of, but that is not a
     * property of "OpenAI-compatible" in general -- stricter servers reject unknown or unsupported
     * parameters outright, and "Local LLM = Both" is a locked decision, so the local llama.cpp/vLLM path
     * has to keep working untouched. The operator opts in; nobody is opted in for. */
    std::string reasoning_effort;

    /* paths */
    std::string data_dir;        /* empty = the XDG/default resolution    */
    bool        ephemeral = false; /* throwaway temp data (CI/clean-slate) */

    /* limits — injected into the worker/orchestrator environment at (re)spawn */
    int llm_call_total_ms = 120000; /* HC_LLM_CALL_TOTAL_MS */
    int llm_connect_ms = 10000;     /* HC_LLM_CONNECT_MS    */
    int deep_reason_budget = 4;     /* HC_DEEP_REASON_BUDGET */
    int task_deadline_ms = 300000;  /* HC_TASK_DEADLINE_MS (kept >= llm_call_total_ms) */

    /* security */
    std::vector<std::string> egress_allow; /* exact numeric IPs re-permitted past the SSRF default-deny */
    std::vector<std::string> exec_allow;   /* absolute paths permitted for the brokered `run` tool ([] = deny-all,
                                            * exec disabled) — Worker Revamp W4 */

    /* automation (B3/B4) — operator opt-in DELEGATED approval; BOTH default OFF, the human gate stays the floor.
     * auto_approve_contained: deterministically auto-approve sandbox-contained writes (fs_write/fs_update) ONLY —
     * exec / shared memory_write / anything novel still prompt; it never auto-denies. allow_all_approvals: the
     * power-user / stress-test escape hatch that auto-approves EVERYTHING — armed only behind a type-to-confirm
     * consent window and loud while live (B4). Neither is ever injected into a worker's env. */
    bool auto_approve_contained = false;
    /* B3b: auto-approve calls to third-party tools whose envelope grants egress ONLY (no fs-write, no exec) --
     * see ToolManifest::envelope_readonly_egress. Default OFF. Persisted like auto_approve_contained rather
     * than session-scoped like allow_all_approvals: the class is bounded and reversible (a read cannot modify
     * the machine), the tool is still inside its hc_confine jail, and the operator reviewed its manifest at
     * install. It never auto-denies, and an explicitly `sensitive` function still prompts. */
    bool auto_approve_readonly_egress = false;
    bool allow_all_approvals = false;
    /* Post a pending approval as a DESKTOP notification (with Approve/Deny actions), and mark the window
     * as wanting attention. DEFAULT OFF: it reaches outside the application to the user's desktop, which
     * is not a decision this program should make on an operator's behalf. When off, nothing connects to
     * the session bus at all. Applies at launch. */
    bool notify_approvals = false;

    /* models (Worker Revamp W2) — EXTENDS the single `model` above (which stays the global/fallback). With
     * both empty, behaviour is byte-identical to today. */
    std::vector<ModelEntry>            models;      /* the operator's available-models catalog ([] = none) */
    std::map<std::string, std::string> role_models; /* role -> model id; SPARSE (a missing role uses `model`) */

    /* role -> the OpenRouter provider-routing block, as the INNER object in canonical JSON
     * (e.g. `{"quantizations":["fp8","bf16","fp16"]}`). SPARSE: a missing role uses HC_OPENROUTER_PROVIDER,
     * and with that unset too the router picks freely — which is today's behaviour, byte-identical.
     *
     * Why a JSON blob and not a provider name: measured 27/8/2026, pinning ONE provider
     * (`{"only":["DeepInfra"]}`) breaks any role whose model that endpoint does not host, while a
     * QUANTISATION filter excludes 4-bit for every model and keeps fallbacks. The value has to be able to
     * express the latter, so it is the routing object itself.
     *
     * NOTE the deliberate asymmetry with role_models: settings_validate applies NO referential rule here.
     * role_models is pruned against the catalog, but there is no provider catalog and never will be — the
     * endpoint list is OpenRouter's, live, and never fetched. A pasted in_catalog check would silently
     * erase every assignment the operator makes. The structural check REPLACES it. */
    std::map<std::string, std::string> role_providers;

    /* audio (Music Player) — GLOBAL, like the rest of this struct. The conductor "set the mood" tool is OPT-IN
     * (default OFF): when off, the conductor cannot see or play the audio library. */
    bool conductor_mood_enabled = false; /* may the conductor browse + play the audio library to set the mood */
    int  audio_volume = 70;              /* 0..100 — the persisted player volume                              */
    bool audio_spectrum = true;          /* show the spectrum analyser in the Music Player panel              */

    /* tools (Custom Tooling program) — System Tools are the built-in worker tools (always installed, compiled-in);
     * `system_tools` toggles them GLOBALLY (SPARSE: a missing entry => ON, so a new System Tool needs no schema
     * change). The host enforces a disabled System Tool by subtracting it from every worker's spawn toolset.
     * `thirdparty_tools` toggles installed user tools (SPARSE: missing => OFF — install is not enable).
     * `thirdparty_tools_enabled` is the GLOBAL kill-switch (default ON; OFF disables ALL third-party tools at
     * once). `thirdparty_tools_conductor` exposes third-party tools to the CONDUCTOR too, not just workers
     * (default OFF — opt-in, since the conductor drives the fleet autonomously). The third-party fields are wired
     * here for the ToolHost (Wave D); until then no third-party tool can run. */
    std::map<std::string, bool> system_tools;     /* worker System Tool name -> enabled (missing => ON)        */
    std::map<std::string, bool> thirdparty_tools; /* third-party tool id -> enabled (missing => OFF)            */
    bool thirdparty_tools_enabled = true;         /* global kill-switch (OFF => every third-party tool off)     */
    bool thirdparty_tools_conductor = false;      /* also expose third-party tools to the conductor (opt-in)    */

    /* conductor (personality) — the operator-authored VOICE/demeanour slot spliced into the conductor's system
     * prompt between the immutable identity spine and the immutable conduct floor (see app/conductor_prompt).
     * "" => the canonical HyperCat voice (today's behaviour, byte-identical). This is the GLOBAL DEFAULT; a
     * per-project override lives in projects/<id>/conductor_persona (host_storage) and WINS when present. The
     * host fences + sanitizes it at prompt assembly (validate_persona), so it styles voice ONLY and can never
     * alter identity, conduct, the tools, or the approval gate. Bounded at load (length only — the full
     * sanitize is the host's at assembly, to keep this module hc::json + hc::fs only). */
    std::string conductor_persona;
};

/* Which fields an explicit environment variable overrode at host startup (explicit env WINS over the file).
 * The UI renders an overridden field disabled with an "overridden by $VAR" note; the host must not persist
 * an env-derived value back as if the operator chose it. */
struct EnvOverrides {
    bool model = false, base_url = false, embed_model = false, reasoning_effort = false, data_dir = false,
         ephemeral = false;
    bool llm_call_total_ms = false, llm_connect_ms = false, deep_reason_budget = false,
         task_deadline_ms = false, egress_allow = false;
};

/* Caps (defensive bounds; also used by the UI). egress entries reuse the policy's intent without depending
 * on hc_policy (this module stays hc::json + hc::fs only). */
constexpr size_t kMaxPanels = 64;
constexpr size_t kMaxEgress = 256;
constexpr size_t kMaxModels = 64;    /* the available-models catalog cap (W2) */
constexpr size_t kMaxRoleProviders = 64;         /* cap on the per-role provider-routing map */
constexpr size_t kMaxProviderRoutingBytes = 512; /* cap on ONE canonical routing block (the UI edits in a
                                                  * 512-byte buffer; keep the two in step) */
constexpr size_t kMaxExecAllow = 64; /* the exec allowlist cap (W4; matches hc::kMaxExecAllow) */
constexpr size_t kMaxToolsMap = 128; /* cap on the system_tools / thirdparty_tools enable maps (Custom Tooling) */
constexpr size_t kMaxConductorPersonaBytes =
    8u * 1024; /* global-persona hygiene bound at load; mirrors conductor_prompt::kMaxPersonaBytes (the
                * authoritative, UTF-8-safe cap applied at prompt assembly) */

/* PURE: validate and CANONICALISE one provider-routing block in place. True iff `json` parses as a JSON
 * OBJECT whose canonical form fits kMaxProviderRoutingBytes; on true `json` is replaced by that canonical
 * form, on false it is left untouched and the caller drops the entry.
 *
 * The re-emit is the mechanism, not cosmetics. A bare parse-and-check gate is provably insufficient: cJSON
 * accepts trailing content after the root value, so `{"a":1} JUNK` is "a valid object" to any such gate.
 * Printing back from the PARSED TREE is what discards the tail. Exported so a caller can REJECT an
 * operator's edit with a message instead of silently pruning it later. */
bool settings_normalize_provider_routing(std::string &json);

/* PURE: parse a settings JSON document (`json`, `len` bytes) into `out`. Unknown keys are ignored; absent
 * keys keep `out`'s current value (so call on a default-constructed Settings). Returns false on a parse
 * failure or a non-object root (with `out` left as passed in) — the caller then keeps defaults. Does NOT
 * validate ranges (call settings_validate after). */
bool settings_parse(const char *json, size_t len, Settings &out);

/* PURE: serialize `s` to a canonical (sorted-key, compact) JSON string. Empty string on failure. */
std::string settings_serialize(const Settings &s);

/* Clamp every field to its valid range, in place (idempotent): an unknown accent -> "white"; poll_hz, the
 * four limits, and the list/map sizes bounded; task_deadline_ms floored at llm_call_total_ms (the invariant);
 * empty egress entries dropped. Does NOT touch the automation bools (`true` is in-range for both) — the
 * session-scoped disarm of allow_all_approvals is a SEPARATE step (settings_clear_session_arming). */
void settings_validate(Settings &s);

/* SECURITY (B4): clear the persisted-but-SESSION-SCOPED arming flags to their safe (disarmed) default. Currently
 * that is allow_all_approvals ONLY — the unbounded approval escape hatch must never survive a process boundary: a
 * stale or hand-edited allow_all_approvals=true must NOT silently re-arm the gate on a fresh launch or a project
 * switch (which re-execs the host). The operator must re-arm in-session via the type-to-confirm consent modal (the
 * sole arm path). auto_approve_contained (the deterministic, sandbox-contained envelope) deliberately PERSISTS and
 * is left untouched. Call once at the host load boundary, after load+merge+validate, before the value reaches the
 * live gate. Idempotent; does NOT touch the on-disk file (the next save rewrites the disarmed value). */
void settings_clear_session_arming(Settings &s);

/* Return a copy of `base` with each field that has an explicit process-environment override REPLACED by the
 * env value (explicit env WINS), marking each in `ov`. Reads the ambient environment (HC_MODEL, HC_BASE_URL,
 * HC_EMBED_MODEL, HC_DATA_DIR, HC_EPHEMERAL, the four HC_*_MS / HC_DEEP_REASON_BUDGET, HC_EGRESS_ALLOW).
 * Non-mutating w.r.t. `base`. Does not validate (call settings_validate after if you need the clamp). */
Settings settings_merge_env(const Settings &base, EnvOverrides &ov);

/* Read settings from `path`. A MISSING file is not an error: `out` is left at its defaults and false is
 * returned. A present-but-malformed/oversized file also returns false with `out` at defaults. On a clean
 * read returns true with `out` populated (NOT yet validated — caller calls settings_validate). */
bool settings_load(const char *path, Settings &out);

/* Serialize `s` canonically and atomically write it to `path` (temp + fsync + rename via hc::fs). Returns
 * false on a serialize or write failure. Does NOT create the parent directory (the caller ensures it). */
bool settings_save(const Settings &s, const char *path);

} // namespace hcapp

#endif /* HC_SETTINGS_HPP */
