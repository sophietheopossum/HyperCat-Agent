/* hc_settings.cpp — parse / serialize / validate / env-merge / load / save the operator settings. See
 * hc_settings.hpp for the contract. Depends on hc::json + hc::fs ONLY.
 *
 * On-disk shape (canonical JSON, grouped by concern):
 *   {"version":1,
 *    "ui":{"accent":"white","mascot":false,"panels":[{"n":"fleet","v":true}, ...]},
 *    "poll":{"hz":2},
 *    "provider":{"model":"","base_url":"","embed_model":""},
 *    "paths":{"data_dir":"","ephemeral":false},
 *    "limits":{"llm_call_total_ms":120000,"llm_connect_ms":10000,"deep_reason_budget":4,"task_deadline_ms":300000},
 *    "security":{"egress_allow":["192.168.1.50", ...]},
 *    "automation":{"auto_approve_contained":false,"allow_all_approvals":false}}
 * panels is an ARRAY of {n,v} (not an object) so the codec needs no hardcoded panel names — a new panel
 * round-trips without touching this file. Unknown keys are ignored; absent keys keep the in/out default. */

#include "hc_settings.hpp"

#include "hc_fs.h"
#include "hc_json.h"

#include <climits>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace hcapp {
namespace {

constexpr size_t kMaxSettingsBytes = 256u * 1024u; /* a settings file is tiny; bound a planted/huge file */

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Split a comma-separated list into trimmed, non-empty, capped entries (HC_EGRESS_ALLOW). Local copy of
 * the policy parser's logic — this module deliberately does not depend on hc_policy. */
std::vector<std::string> split_csv(const char *csv)
{
    std::vector<std::string> out;
    if (!csv) return out;
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    const char *p = csv;
    while (*p && out.size() < kMaxEgress) {
        while (*p == ',' || is_ws(*p)) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && is_ws(end[-1])) end--;
        if (end > start) out.emplace_back(start, (size_t)(end - start));
    }
    return out;
}

/* Parse an int environment variable. Returns true only if present AND a clean integer (no trailing
 * garbage); present-but-malformed is treated as "no override" so a typo doesn't silently take effect. */
bool env_int(const char *name, int *out)
{
    const char *v = getenv(name);
    if (!v || !*v) return false;
    char *end = nullptr;
    long  parsed = std::strtol(v, &end, 10);
    if (*end != '\0') return false;
    if (parsed < INT_MIN) parsed = INT_MIN;
    if (parsed > INT_MAX) parsed = INT_MAX;
    *out = (int)parsed;
    return true;
}

} // namespace

bool settings_parse(const char *json, size_t len, Settings &out)
{
    if (!json) return false;
    hc_json *root = hc_json_parse(json, len);
    if (!root) return false;
    if (!hc_json_is_object(root)) {
        hc_json_free(root);
        return false;
    }

    out.version = (int)hc_json_get_int(root, "version", out.version);

    if (const hc_json *ui = hc_json_get(root, "ui")) {
        out.accent = hc_json_get_str(ui, "accent", out.accent.c_str());
        out.mascot = hc_json_get_bool(ui, "mascot", out.mascot);
        const hc_json *panels = hc_json_get(ui, "panels");
        if (panels && hc_json_is_array(panels)) {
            out.panels.clear();
            const size_t n = hc_json_arr_len(panels);
            for (size_t i = 0; i < n && out.panels.size() < kMaxPanels; i++) {
                const hc_json *e = hc_json_arr_at(panels, i);
                if (!e || !hc_json_is_object(e)) continue;
                const char *nm = hc_json_get_str(e, "n", "");
                if (nm && nm[0]) out.panels[nm] = hc_json_get_bool(e, "v", true);
            }
        }
    }

    if (const hc_json *poll = hc_json_get(root, "poll"))
        out.poll_hz = (int)hc_json_get_int(poll, "hz", out.poll_hz);

    if (const hc_json *prov = hc_json_get(root, "provider")) {
        out.model = hc_json_get_str(prov, "model", out.model.c_str());
        out.base_url = hc_json_get_str(prov, "base_url", out.base_url.c_str());
        out.embed_model = hc_json_get_str(prov, "embed_model", out.embed_model.c_str());
    }

    if (const hc_json *paths = hc_json_get(root, "paths")) {
        out.data_dir = hc_json_get_str(paths, "data_dir", out.data_dir.c_str());
        out.ephemeral = hc_json_get_bool(paths, "ephemeral", out.ephemeral);
    }

    if (const hc_json *lim = hc_json_get(root, "limits")) {
        out.llm_call_total_ms = (int)hc_json_get_int(lim, "llm_call_total_ms", out.llm_call_total_ms);
        out.llm_connect_ms = (int)hc_json_get_int(lim, "llm_connect_ms", out.llm_connect_ms);
        out.deep_reason_budget = (int)hc_json_get_int(lim, "deep_reason_budget", out.deep_reason_budget);
        out.task_deadline_ms = (int)hc_json_get_int(lim, "task_deadline_ms", out.task_deadline_ms);
    }

    if (const hc_json *sec = hc_json_get(root, "security")) {
        const hc_json *ea = hc_json_get(sec, "egress_allow");
        if (ea && hc_json_is_array(ea)) {
            out.egress_allow.clear();
            const size_t n = hc_json_arr_len(ea);
            for (size_t i = 0; i < n && out.egress_allow.size() < kMaxEgress; i++) {
                const char *s = hc_json_as_str(hc_json_arr_at(ea, i), "");
                if (s && s[0]) out.egress_allow.emplace_back(s);
            }
        }
        const hc_json *xa = hc_json_get(sec, "exec_allow"); /* W4: absolute paths for the brokered run tool */
        if (xa && hc_json_is_array(xa)) {
            out.exec_allow.clear();
            const size_t n = hc_json_arr_len(xa);
            for (size_t i = 0; i < n && out.exec_allow.size() < kMaxExecAllow; i++) {
                const char *s = hc_json_as_str(hc_json_arr_at(xa, i), "");
                if (s && s[0]) out.exec_allow.emplace_back(s);
            }
        }
    }

    /* models (W2): the available-models catalog + the sparse per-role assignment. Absent => empty => today. */
    if (const hc_json *m = hc_json_get(root, "models")) {
        if (const hc_json *list = hc_json_get(m, "list"); list && hc_json_is_array(list)) {
            out.models.clear();
            const size_t n = hc_json_arr_len(list);
            for (size_t i = 0; i < n && out.models.size() < kMaxModels; i++) {
                const hc_json *e = hc_json_arr_at(list, i);
                if (!e) continue;
                ModelEntry me;
                me.id = hc_json_get_str(e, "id", "");
                me.note = hc_json_get_str(e, "note", "");
                if (!me.id.empty()) out.models.push_back(std::move(me));
            }
        }
        if (const hc_json *assign = hc_json_get(m, "assign"); assign && hc_json_is_array(assign)) {
            out.role_models.clear();
            const size_t n = hc_json_arr_len(assign);
            for (size_t i = 0; i < n; i++) {
                const hc_json *e = hc_json_arr_at(assign, i);
                if (!e) continue;
                std::string role = hc_json_get_str(e, "role", "");
                std::string mid = hc_json_get_str(e, "model", "");
                if (!role.empty() && !mid.empty()) out.role_models[role] = mid;
            }
        }
    }

    /* audio (Music Player): the conductor-mood opt-in + the persisted volume/spectrum. Absent => the defaults. */
    if (const hc_json *au = hc_json_get(root, "audio")) {
        out.conductor_mood_enabled = hc_json_get_bool(au, "conductor_mood_enabled", out.conductor_mood_enabled);
        out.audio_volume = (int)hc_json_get_int(au, "volume", out.audio_volume);
        out.audio_spectrum = hc_json_get_bool(au, "spectrum", out.audio_spectrum);
    }

    /* conductor (personality): the operator's GLOBAL default persona slot. Absent => "" => canonical voice. */
    if (const hc_json *cd = hc_json_get(root, "conductor"))
        out.conductor_persona = hc_json_get_str(cd, "persona", out.conductor_persona.c_str());

    /* tools (Custom Tooling): the System Tools + third-party enable maps + kill-switch + conductor opt-in. The
     * maps are arrays of {n,v} (like panels), so a new tool round-trips without touching this codec. */
    if (const hc_json *tl = hc_json_get(root, "tools")) {
        if (const hc_json *st = hc_json_get(tl, "system"); st && hc_json_is_array(st)) {
            out.system_tools.clear();
            const size_t n = hc_json_arr_len(st);
            for (size_t i = 0; i < n && out.system_tools.size() < kMaxToolsMap; i++) {
                const hc_json *e = hc_json_arr_at(st, i);
                if (!e || !hc_json_is_object(e)) continue;
                const char *nm = hc_json_get_str(e, "n", "");
                if (nm && nm[0]) out.system_tools[nm] = hc_json_get_bool(e, "v", true); /* missing v => ON */
            }
        }
        if (const hc_json *tp = hc_json_get(tl, "thirdparty"); tp && hc_json_is_array(tp)) {
            out.thirdparty_tools.clear();
            const size_t n = hc_json_arr_len(tp);
            for (size_t i = 0; i < n && out.thirdparty_tools.size() < kMaxToolsMap; i++) {
                const hc_json *e = hc_json_arr_at(tp, i);
                if (!e || !hc_json_is_object(e)) continue;
                const char *nm = hc_json_get_str(e, "n", "");
                if (nm && nm[0]) out.thirdparty_tools[nm] = hc_json_get_bool(e, "v", false); /* missing v => OFF */
            }
        }
        out.thirdparty_tools_enabled = hc_json_get_bool(tl, "thirdparty_enabled", out.thirdparty_tools_enabled);
        out.thirdparty_tools_conductor = hc_json_get_bool(tl, "thirdparty_conductor", out.thirdparty_tools_conductor);
    }

    /* automation (B3/B4): the delegated-approval opt-ins. Absent => OFF (the human gate is the floor). */
    if (const hc_json *at = hc_json_get(root, "automation")) {
        out.auto_approve_contained = hc_json_get_bool(at, "auto_approve_contained", out.auto_approve_contained);
        out.auto_approve_readonly_egress =
            hc_json_get_bool(at, "auto_approve_readonly_egress", out.auto_approve_readonly_egress);
        out.allow_all_approvals = hc_json_get_bool(at, "allow_all_approvals", out.allow_all_approvals);
    }

    hc_json_free(root);
    return true;
}

std::string settings_serialize(const Settings &s)
{
    hc_json *root = hc_json_new_object();
    if (!root) return "";
    hc_json_obj_set_int(root, "version", s.version);

    if (hc_json *ui = hc_json_new_object()) {
        hc_json_obj_set_str(ui, "accent", s.accent.c_str());
        hc_json_obj_set_bool(ui, "mascot", s.mascot);
        if (hc_json *panels = hc_json_new_array()) {
            for (const auto &kv : s.panels) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "n", kv.first.c_str());
                hc_json_obj_set_bool(e, "v", kv.second);
                hc_json_arr_append(panels, e); /* adopts e (or frees it on failure) */
            }
            hc_json_obj_set(ui, "panels", panels); /* adopts */
        }
        hc_json_obj_set(root, "ui", ui); /* adopts */
    }
    if (hc_json *poll = hc_json_new_object()) {
        hc_json_obj_set_int(poll, "hz", s.poll_hz);
        hc_json_obj_set(root, "poll", poll);
    }
    if (hc_json *prov = hc_json_new_object()) {
        hc_json_obj_set_str(prov, "model", s.model.c_str());
        hc_json_obj_set_str(prov, "base_url", s.base_url.c_str());
        hc_json_obj_set_str(prov, "embed_model", s.embed_model.c_str());
        hc_json_obj_set(root, "provider", prov);
    }
    if (hc_json *paths = hc_json_new_object()) {
        hc_json_obj_set_str(paths, "data_dir", s.data_dir.c_str());
        hc_json_obj_set_bool(paths, "ephemeral", s.ephemeral);
        hc_json_obj_set(root, "paths", paths);
    }
    if (hc_json *lim = hc_json_new_object()) {
        hc_json_obj_set_int(lim, "llm_call_total_ms", s.llm_call_total_ms);
        hc_json_obj_set_int(lim, "llm_connect_ms", s.llm_connect_ms);
        hc_json_obj_set_int(lim, "deep_reason_budget", s.deep_reason_budget);
        hc_json_obj_set_int(lim, "task_deadline_ms", s.task_deadline_ms);
        hc_json_obj_set(root, "limits", lim);
    }
    if (hc_json *sec = hc_json_new_object()) {
        if (hc_json *ea = hc_json_new_array()) {
            for (const auto &ip : s.egress_allow) hc_json_arr_append_str(ea, ip.c_str());
            hc_json_obj_set(sec, "egress_allow", ea);
        }
        if (hc_json *xa = hc_json_new_array()) {
            for (const auto &p : s.exec_allow) hc_json_arr_append_str(xa, p.c_str());
            hc_json_obj_set(sec, "exec_allow", xa);
        }
        hc_json_obj_set(root, "security", sec);
    }
    if (hc_json *m = hc_json_new_object()) { /* models (W2) */
        if (hc_json *list = hc_json_new_array()) {
            for (const auto &me : s.models) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "id", me.id.c_str());
                hc_json_obj_set_str(e, "note", me.note.c_str());
                hc_json_arr_append(list, e); /* adopts */
            }
            hc_json_obj_set(m, "list", list); /* adopts */
        }
        if (hc_json *assign = hc_json_new_array()) {
            for (const auto &kv : s.role_models) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "role", kv.first.c_str());
                hc_json_obj_set_str(e, "model", kv.second.c_str());
                hc_json_arr_append(assign, e); /* adopts */
            }
            hc_json_obj_set(m, "assign", assign); /* adopts */
        }
        hc_json_obj_set(root, "models", m); /* adopts */
    }
    if (hc_json *au = hc_json_new_object()) { /* audio (Music Player) */
        hc_json_obj_set_bool(au, "conductor_mood_enabled", s.conductor_mood_enabled);
        hc_json_obj_set_int(au, "volume", s.audio_volume);
        hc_json_obj_set_bool(au, "spectrum", s.audio_spectrum);
        hc_json_obj_set(root, "audio", au);
    }
    if (hc_json *cd = hc_json_new_object()) { /* conductor (personality): the global default persona slot */
        hc_json_obj_set_str(cd, "persona", s.conductor_persona.c_str());
        hc_json_obj_set(root, "conductor", cd);
    }
    if (hc_json *tl = hc_json_new_object()) { /* tools (Custom Tooling) */
        if (hc_json *st = hc_json_new_array()) {
            for (const auto &kv : s.system_tools) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "n", kv.first.c_str());
                hc_json_obj_set_bool(e, "v", kv.second);
                hc_json_arr_append(st, e); /* adopts */
            }
            hc_json_obj_set(tl, "system", st); /* adopts */
        }
        if (hc_json *tp = hc_json_new_array()) {
            for (const auto &kv : s.thirdparty_tools) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "n", kv.first.c_str());
                hc_json_obj_set_bool(e, "v", kv.second);
                hc_json_arr_append(tp, e); /* adopts */
            }
            hc_json_obj_set(tl, "thirdparty", tp); /* adopts */
        }
        hc_json_obj_set_bool(tl, "thirdparty_enabled", s.thirdparty_tools_enabled);
        hc_json_obj_set_bool(tl, "thirdparty_conductor", s.thirdparty_tools_conductor);
        hc_json_obj_set(root, "tools", tl); /* adopts */
    }
    if (hc_json *at = hc_json_new_object()) { /* automation (B3/B4): the delegated-approval opt-ins */
        hc_json_obj_set_bool(at, "auto_approve_contained", s.auto_approve_contained);
        hc_json_obj_set_bool(at, "auto_approve_readonly_egress", s.auto_approve_readonly_egress);
        hc_json_obj_set_bool(at, "allow_all_approvals", s.allow_all_approvals);
        hc_json_obj_set(root, "automation", at);
    }

    char       *txt = hc_json_print_canonical(root);
    std::string out = txt ? txt : "";
    free(txt);
    hc_json_free(root);
    return out;
}

void settings_validate(Settings &s)
{
    static const char *const kAccents[] = {"white", "cyan", "amber", "emerald", "violet", "crimson"};
    bool                     accent_ok = false;
    for (const char *a : kAccents)
        if (s.accent == a) {
            accent_ok = true;
            break;
        }
    if (!accent_ok) s.accent = "white";

    s.poll_hz = clampi(s.poll_hz, 1, 60);
    /* mirror the worker/orchestrator clamps so the injected env values match what those binaries accept */
    s.llm_call_total_ms = clampi(s.llm_call_total_ms, 5000, 600000);
    s.llm_connect_ms = clampi(s.llm_connect_ms, 1000, 60000);
    s.deep_reason_budget = clampi(s.deep_reason_budget, 0, 16);
    /* the invariant: the per-task deadline must not be below the per-call HTTP cap */
    s.task_deadline_ms = clampi(s.task_deadline_ms, s.llm_call_total_ms, 3600000);
    s.audio_volume = clampi(s.audio_volume, 0, 100); /* Music Player volume */

    /* conductor_persona: hygiene length cap at load (UTF-8-safe back-off so we never keep half a codepoint).
     * The AUTHORITATIVE sanitize — control bytes, fence + chat-role markers — is the host's at prompt assembly
     * (conductor_prompt::validate_persona); this module stays hc::json + hc::fs only. */
    if (s.conductor_persona.size() > kMaxConductorPersonaBytes) {
        s.conductor_persona.resize(kMaxConductorPersonaBytes);
        while (!s.conductor_persona.empty() &&
               (static_cast<unsigned char>(s.conductor_persona.back()) & 0xC0) == 0x80)
            s.conductor_persona.pop_back();
        if (!s.conductor_persona.empty() && (static_cast<unsigned char>(s.conductor_persona.back()) & 0x80))
            s.conductor_persona.pop_back();
    }

    /* tools (Custom Tooling): cap the enable maps (defensive bound on a planted/huge file). */
    if (s.system_tools.size() > kMaxToolsMap) {
        auto it = s.system_tools.begin();
        std::advance(it, (std::ptrdiff_t)kMaxToolsMap);
        s.system_tools.erase(it, s.system_tools.end());
    }
    if (s.thirdparty_tools.size() > kMaxToolsMap) {
        auto it = s.thirdparty_tools.begin();
        std::advance(it, (std::ptrdiff_t)kMaxToolsMap);
        s.thirdparty_tools.erase(it, s.thirdparty_tools.end());
    }

    /* automation bools (auto_approve_contained / allow_all_approvals): deliberately NOT touched here — `true` is an
     * in-range value for both, so a range clamp is a no-op. The SESSION-LIFECYCLE rule that a persisted
     * allow_all_approvals=true must not silently re-arm across a launch/project-switch lives in
     * settings_clear_session_arming (called at the host load boundary, app/main.cpp), NOT here — this keeps
     * settings_validate a pure, idempotent range clamp with no coupling to gate/session policy. */

    if (s.panels.size() > kMaxPanels) {
        auto it = s.panels.begin();
        std::advance(it, (std::ptrdiff_t)kMaxPanels);
        s.panels.erase(it, s.panels.end());
    }

    std::vector<std::string> kept;
    for (auto &ip : s.egress_allow) {
        if (ip.empty()) continue;
        kept.push_back(ip);
        if (kept.size() >= kMaxEgress) break;
    }
    s.egress_allow = std::move(kept);

    /* exec_allow (W4): keep only ABSOLUTE paths (no `..`); cap. Existence isn't re-checked here (a file can
     * come and go between save + load) — the ExecGate (W4.3) re-validates at exec time; this is hygiene. */
    std::vector<std::string> kept_exec;
    for (auto &p : s.exec_allow) {
        if (p.empty() || p[0] != '/' || p.find("..") != std::string::npos) continue;
        kept_exec.push_back(p);
        if (kept_exec.size() >= kMaxExecAllow) break;
    }
    s.exec_allow = std::move(kept_exec);

    /* models (W2): drop empty ids, cap the catalog, and keep role assignments REFERENTIALLY CONSISTENT — a
     * role mapped to a model that isn't in the catalog is dropped (it would resolve to nothing). */
    std::vector<ModelEntry> mkept;
    for (auto &me : s.models) {
        if (me.id.empty()) continue;
        mkept.push_back(me);
        if (mkept.size() >= kMaxModels) break;
    }
    s.models = std::move(mkept);
    auto in_catalog = [&](const std::string &id) {
        for (const auto &me : s.models)
            if (me.id == id) return true;
        return false;
    };
    for (auto it = s.role_models.begin(); it != s.role_models.end();) {
        if (it->second.empty() || !in_catalog(it->second)) it = s.role_models.erase(it);
        else ++it;
    }
}

void settings_clear_session_arming(Settings &s)
{
    /* allow_all_approvals is the ONLY session-scoped arming flag: the most dangerous switch in the system must be
     * re-armed in-session via the type-to-confirm consent modal, never inherited from disk. auto_approve_contained
     * (the deterministic, sandbox-contained, reversible envelope) is intentionally left untouched — it persists. If a
     * future unbounded escape hatch is ever added, disarm it here too so it can't silently survive a launch/switch. */
    s.allow_all_approvals = false;
}

Settings settings_merge_env(const Settings &base, EnvOverrides &ov)
{
    Settings s = base;
    ov = EnvOverrides{};
    if (const char *v = getenv("HC_MODEL")) {
        s.model = v;
        ov.model = true;
    }
    if (const char *v = getenv("HC_BASE_URL")) {
        s.base_url = v;
        ov.base_url = true;
    }
    if (const char *v = getenv("HC_EMBED_MODEL")) {
        s.embed_model = v;
        ov.embed_model = true;
    }
    if (const char *v = getenv("HC_DATA_DIR")) {
        s.data_dir = v;
        ov.data_dir = true;
    }
    if (getenv("HC_EPHEMERAL")) { /* presence = true (matches main.cpp's getenv != nullptr) */
        s.ephemeral = true;
        ov.ephemeral = true;
    }
    if (getenv("HC_CONDUCTOR_MOOD")) s.conductor_mood_enabled = true; /* dev/test: force-enable the mood tool */
    /* dev/test: override the GLOBAL default persona (inline text wins over a file). Per-project overrides still
     * win over this at assembly. The value is sanitized at assembly like any persona, so a malformed env is safe. */
    if (const char *v = getenv("HC_CONDUCTOR_PERSONA")) {
        s.conductor_persona = v;
    } else if (const char *pf = getenv("HC_CONDUCTOR_PERSONA_FILE")) {
        size_t plen = 0;
        if (char *pd = hc_fs_read_file(pf, 64u * 1024, &plen)) {
            s.conductor_persona.assign(pd, plen);
            free(pd);
        }
    }
    int x = 0;
    if (env_int("HC_LLM_CALL_TOTAL_MS", &x)) {
        s.llm_call_total_ms = x;
        ov.llm_call_total_ms = true;
    }
    if (env_int("HC_LLM_CONNECT_MS", &x)) {
        s.llm_connect_ms = x;
        ov.llm_connect_ms = true;
    }
    if (env_int("HC_DEEP_REASON_BUDGET", &x)) {
        s.deep_reason_budget = x;
        ov.deep_reason_budget = true;
    }
    if (env_int("HC_TASK_DEADLINE_MS", &x)) {
        s.task_deadline_ms = x;
        ov.task_deadline_ms = true;
    }
    if (const char *v = getenv("HC_EGRESS_ALLOW")) {
        s.egress_allow = split_csv(v);
        ov.egress_allow = true;
    }
    return s;
}

bool settings_load(const char *path, Settings &out)
{
    if (!path || !*path) return false;
    size_t len = 0;
    char  *buf = hc_fs_read_file(path, kMaxSettingsBytes, &len); /* NULL on missing/oversized */
    if (!buf) return false;
    bool ok = settings_parse(buf, len, out);
    free(buf);
    return ok;
}

bool settings_save(const Settings &s, const char *path)
{
    if (!path || !*path) return false;
    std::string txt = settings_serialize(s);
    if (txt.empty()) return false;
    return hc_fs_atomic_write(path, txt.data(), txt.size()) == 0;
}

} // namespace hcapp
