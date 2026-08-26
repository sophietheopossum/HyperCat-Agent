/* ui_panels (settings cluster) — the Settings panel (WI-2 E1): the live accent picker + the editable
 * provider / limits / paths draft + the masked API-key entry. It renders ctx.settings (the editable draft
 * the host seeds via UiApp::apply_settings) and emits two commands the host executes: SetSecret (the key,
 * straight into hc_secrets — NEVER a field, never disk) and SaveSettings (the edited draft -> hcapp::Settings
 * -> validated + persisted). accent + mascot apply LIVE on edit; provider/paths/limits are persisted and
 * apply on the next (re)spawn — the panel tags them. A field an explicit env var overrides is shown disabled
 * with an "env" note (the host marks it in UiSettings.ov_*). See ui_panels.hpp for the contract. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp"

#include "ui_theme.hpp"

#include "imgui.h"

#include <arpa/inet.h> /* inet_pton — E2 validates an egress entry is a literal numeric IP (POSIX) */

#include <cfloat> /* FLT_MIN — the ImGui "fill available width" idiom for the persona editor */
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc::ui {

namespace {

/* E2: a numeric IPv4/IPv6 only (reject hostnames / CIDR). The host RE-VALIDATES this authoritatively. */
bool valid_numeric_ip(const char *s)
{
    unsigned char b4[4], b16[16];
    return ::inet_pton(AF_INET, s, b4) == 1 || ::inet_pton(AF_INET6, s, b16) == 1;
}

/* E2 confirm-modal advisory ONLY (the host's hc::classify_ip is authoritative). Names the IP's class and
 * flags the cloud-metadata endpoint specially — the highest-risk thing an operator could re-permit. */
struct EgressClass {
    const char *desc;
    bool        danger;
};
EgressClass egress_class_advisory(const char *ip)
{
    /* substring (not exact) so a v4-in-v6 dotted spelling like ::ffff:169.254.169.254 still trips the
     * metadata warning. The host's hc::classify_ip un-wraps every embedding authoritatively. */
    if (std::strstr(ip, "169.254.169.254"))
        return {"link-local — the CLOUD METADATA endpoint (credential-theft risk)", true};
    if (std::strncmp(ip, "127.", 4) == 0 || std::strcmp(ip, "::1") == 0)
        return {"loopback (this host)", false};
    if (std::strncmp(ip, "169.254.", 8) == 0 || std::strncmp(ip, "fe80", 4) == 0)
        return {"link-local", false};
    if (std::strncmp(ip, "10.", 3) == 0 || std::strncmp(ip, "192.168.", 8) == 0)
        return {"private (LAN)", false};
    if (std::strncmp(ip, "172.", 4) == 0) {
        int o2 = std::atoi(ip + 4);
        if (o2 >= 16 && o2 <= 31) return {"private (LAN)", false};
    }
    return {"public", false};
}

/* A string row backed by the draft `field`. Copies the draft into a stack buffer each frame, lets ImGui
 * edit it, writes back on change. Disabled (with an "env" note) when `env_locked`. */
void settings_text_row(const char *label, std::string &field, bool env_locked)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", field.c_str());
    ImGui::BeginDisabled(env_locked);
    if (ImGui::InputText(label, buf, sizeof buf)) field = buf;
    ImGui::EndDisabled();
    if (env_locked) {
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "(env)");
    }
}

/* An int row backed by the draft `field`, disabled when env-locked. */
void settings_int_row(const char *label, int &field, bool env_locked)
{
    int v = field;
    ImGui::BeginDisabled(env_locked);
    if (ImGui::InputInt(label, &v, 0, 0)) field = v;
    ImGui::EndDisabled();
    if (env_locked) {
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "(env)");
    }
}

/* Muted explanatory prose that WRAPS to the panel width (so a narrow Settings panel never clips a line —
 * the restrained-corporate "no horizontal overflow" rule). */
void muted_wrapped(const char *s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
    WrappedTextUnformatted(s);
    ImGui::PopStyleColor();
}

/* A read-only LOCKED block in the persona preview: a muted label + the (long) literal text in a bordered,
 * scrolling, wrapped child — so the operator can see EXACTLY what frames the editable slot and cannot edit. */
void persona_locked_block(const char *id, const char *label, const char *text, float height)
{
    ImGui::TextColored(muted_v4(), "%s", label);
    if (ImGui::BeginChild(id, ImVec2(0, height), true)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

/* CONDUCTOR PERSONALITY — the operator restyles HyperCat's VOICE only; the host re-asserts the locked
 * identity + conduct floor around it (see conductor_prompt), so this can never change who she is or weaken
 * the approval gate. LIVE-OWNED: Save emits SetConductorPersona (persisted at once; preserved across the
 * [Apply] draft). Self-contained so the panel can place it prominently; the function-statics are fine for
 * the single Settings panel instance. */
void draw_persona_section(const UiSettings &live, DrawCtx &ctx)
{
    PanelHeader("CONDUCTOR PERSONALITY");
    muted_wrapped("Restyle HyperCat's VOICE. Her identity (\"You are HyperCat\") and conduct floor are locked and "
                  "re-asserted around your text \xE2\x80\x94 this cannot change them.");

    /* scope: the global default vs the per-project override (offered only when a persistent project is open). */
    static int  persona_scope = 0;       /* 0 = global default, 1 = this project */
    static int  persona_last_scope = -1; /* re-seed the editor buffer when the scope changes */
    static char persona_buf[8192] = {};  /* == conductor_prompt::kMaxPersonaBytes; the editor caps input here */
    if (!live.conductor_persona_per_project) persona_scope = 0; /* no persistent project => global only */

    ImGui::RadioButton("global default", &persona_scope, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(!live.conductor_persona_per_project);
    ImGui::RadioButton("this project", &persona_scope, 1);
    ImGui::EndDisabled();
    if (!live.conductor_persona_per_project) muted_wrapped("(no persistent project \xE2\x80\x94 global only)");

    /* (re)seed the editor from the live value for the selected scope whenever the scope changes (Save first to
     * keep an in-progress edit before switching). */
    if (persona_scope != persona_last_scope) {
        const std::string &src = (persona_scope == 1) ? live.conductor_persona_project : live.conductor_persona;
        std::snprintf(persona_buf, sizeof persona_buf, "%s", src.c_str());
        persona_last_scope = persona_scope;
    }

    /* preset picker — drops a built-in voice into the editor (Canonical clears it => the default voice). */
    if (ImGui::BeginCombo("preset", "apply a preset\xE2\x80\xA6", ImGuiComboFlags_NoPreview)) {
        for (const auto &pr : live.persona_presets)
            if (ImGui::Selectable(pr.first.c_str()))
                std::snprintf(persona_buf, sizeof persona_buf, "%s", pr.second.c_str());
        ImGui::EndCombo();
    }

    ImGui::InputTextMultiline("##persona_editor", persona_buf, sizeof persona_buf, ImVec2(-FLT_MIN, 120));
    if (persona_buf[0] == '\0')
        ImGui::TextColored(muted_v4(), "empty \xE2\x80\x94 using the canonical HyperCat voice");

    if (ImGui::Button("Save persona"))
        ctx.commands.push_back({UiCommand::Kind::SetConductorPersona, persona_scope == 1 ? "project" : "global",
                                std::string(persona_buf), 0, {}, {}});
    ImGui::SameLine();
    if (ImGui::Button("Reset to canonical")) persona_buf[0] = '\0'; /* clear; Save persists the canonical default */
    muted_wrapped("applies to the next chat (New Chat / Resume)");

    /* preview: the LOCKED identity + your editable voice + the LOCKED floor, in assembly order. */
    if (ImGui::CollapsingHeader("Preview assembled prompt")) {
        persona_locked_block("##persona_id", "LOCKED \xE2\x80\x94 identity (always first):",
                             live.conductor_spine_identity.c_str(), 90.0f);
        ImGui::TextColored(accent_v4(), "EDITABLE \xE2\x80\x94 voice:");
        const char *voice = persona_buf[0] ? persona_buf : live.conductor_persona_default.c_str();
        if (ImGui::BeginChild("##persona_voice", ImVec2(0, 90.0f), true)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(voice);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        persona_locked_block("##persona_floor", "LOCKED \xE2\x80\x94 conduct & tools (always last):",
                             live.conductor_spine_floor.c_str(), 120.0f);
    }
}

} // namespace

void draw_settings_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Settings", open)) {
        ImGui::End();
        return;
    }

    /* The editable draft (ctx.settings) is seeded by the host's apply_settings; defensively seed from the
     * snapshot the first time if that never happened (e.g. a headless smoke). key_present + the ov_* locks
     * are host-authoritative and read live from the snapshot, never edited here. */
    if (!ctx.settings_primed) {
        ctx.settings = s.settings;
        ctx.settings_primed = true;
    }
    UiSettings       &d = ctx.settings;
    const UiSettings &live = s.settings;

    PanelHeader("ACCENT");
    for (const AccentOpt &o : k_accents) {
        bool active = (ctx.accent == o.val);
        if (ImGui::Selectable(o.name, active) && !active) { /* LIVE on edit */
            ctx.accent = o.val;
            d.accent = o.val;
            apply_theme(o.val);
        }
    }

    ImGui::Spacing();
    PanelHeader("DISPLAY");
    settings_int_row("listing poll Hz", d.poll_hz, false); /* LIVE on Apply: the store re-list cadence */

    ImGui::Spacing();
    draw_persona_section(live, ctx); /* headline feature, placed early; LIVE-owned (its own Save, not [Apply]) */

    ImGui::Spacing();
    PanelHeader("PROVIDER");
    ImGui::TextColored(muted_v4(), "applies on restart");
    settings_text_row("model", d.model, live.ov_model);
    settings_text_row("base url", d.base_url, live.ov_base_url);
    settings_text_row("embed model", d.embed_model, live.ov_embed_model);

    ImGui::Spacing();
    PanelHeader("API KEY");
    ImGui::TextColored(live.key_present ? accent_v4() : muted_v4(),
                       live.key_present ? (live.keychain_available ? "a provider key is set (in the OS keychain)"
                                                                   : "a provider key is set (this session)")
                                        : "no key set");
    /* The key is TRANSIENT: a function-static buffer the operator types into, sent once via SetSecret then
     * scrubbed here (the host zeroizes its own copy after hc_secrets_set). It is NEVER a settings field and
     * never written to disk. */
    static char keybuf[512] = {};
    ImGui::InputText("##apikey", keybuf, sizeof keybuf, ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button("Set key") && keybuf[0]) {
        /* n carries the per-session "export to worker env" opt-in so the host knows whether to setenv it. */
        ctx.commands.push_back(
            {UiCommand::Kind::SetSecret, std::string(keybuf), "", d.export_key_to_env ? 1 : 0, {}, {}});
        std::memset(keybuf, 0, sizeof keybuf); /* scrub the UI buffer immediately */
    }
    if (live.key_present) { /* clear the persisted key from the OS keychain + the in-memory store */
        ImGui::SameLine();
        if (ImGui::Button("Forget stored key"))
            ctx.commands.push_back({UiCommand::Kind::ForgetSecret, "", "", 0, {}, {}});
    }
    /* SECURITY (default-OFF): re-expose the key to worker process environments (/proc/<pid>/environ,
     * same-uid). OFF keeps it host-only. Persisted with the settings; takes effect on the next spawn. */
    ImGui::Checkbox("export key to worker env (re-exposes it; default off)", &d.export_key_to_env);

    ImGui::Spacing();
    PanelHeader("LIMITS");
    ImGui::TextColored(muted_v4(), "injected into workers on restart");
    settings_int_row("llm call total ms", d.llm_call_total_ms, live.ov_llm_call_total_ms);
    settings_int_row("llm connect ms", d.llm_connect_ms, live.ov_llm_connect_ms);
    settings_int_row("deep reason budget", d.deep_reason_budget, live.ov_deep_reason_budget);
    settings_int_row("task deadline ms", d.task_deadline_ms, live.ov_task_deadline_ms);

    ImGui::Spacing();
    PanelHeader("PATHS");
    settings_text_row("data dir", d.data_dir, live.ov_data_dir);
    {
        bool eph = d.ephemeral;
        ImGui::BeginDisabled(live.ov_ephemeral);
        if (ImGui::Checkbox("ephemeral (throwaway data)", &eph)) d.ephemeral = eph;
        ImGui::EndDisabled();
        if (live.ov_ephemeral) {
            ImGui::SameLine();
            ImGui::TextColored(muted_v4(), "(env)");
        }
    }

    ImGui::Spacing();
    PanelHeader("EGRESS ALLOWLIST (advanced)");
    ImGui::TextColored(muted_v4(), "default-deny SSRF guard; each entry re-permits ONE numeric peer.");
    ImGui::TextColored(muted_v4(), "applies to workers started after the change (restart to apply).");
    /* Render the LIVE, host-authoritative list (the editor persists immediately — NOT the draft / [Apply]).
     * There is deliberately NO "disable guard" / "clear all" affordance: removing entries one at a time
     * returns to the safe default-deny baseline; the guard itself is never removed (the worker always
     * installs it). Remove narrows (no confirm); Add WIDENS and is confirm-gated. */
    for (size_t i = 0; i < live.egress_allow.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::TextUnformatted(live.egress_allow[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            ctx.commands.push_back({UiCommand::Kind::EditAllowlist, live.egress_allow[i], "remove", 0, {}, {}});
        ImGui::PopID();
    }
    if (live.egress_allow.empty()) ImGui::TextColored(muted_v4(), "(none — public egress only)");

    static char ipbuf[64] = {};
    static char pending[64] = {};
    static char err[96] = {};
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##egress_add", ipbuf, sizeof ipbuf);
    ImGui::SameLine();
    if (ImGui::Button("Add\xE2\x80\xA6")) { /* "Add…" */
        err[0] = '\0';
        if (ipbuf[0] && !valid_numeric_ip(ipbuf))
            std::snprintf(err, sizeof err, "not a numeric IP (hostnames / CIDR are rejected)");
        else if (ipbuf[0]) {
            std::snprintf(pending, sizeof pending, "%s", ipbuf);
            ImGui::OpenPopup("Confirm egress entry");
        }
    }
    if (err[0]) ImGui::TextColored(err_v4(), "%s", err);

    if (ImGui::BeginPopupModal("Confirm egress entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EgressClass ec = egress_class_advisory(pending);
        ImGui::TextUnformatted("Re-permit egress to:");
        ImGui::TextColored(accent_v4(), "%s", pending);
        ImGui::TextColored(ec.danger ? err_v4() : muted_v4(), "class: %s", ec.desc);
        ImGui::TextColored(muted_v4(), "This WIDENS the SSRF guard — the fleet may then reach this peer.");
        if (ec.danger) {
            ImGui::Spacing();
            ImGui::TextColored(err_v4(), "WARNING: the cloud metadata endpoint. Allowing it can expose");
            ImGui::TextColored(err_v4(), "instance credentials. Only proceed if you are certain.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Confirm \xE2\x80\x94 re-permit")) { /* "Confirm — re-permit" */
            ctx.commands.push_back({UiCommand::Kind::EditAllowlist, std::string(pending), "add", 0, {}, {}});
            ipbuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    PanelHeader("RUN ALLOWLIST (advanced)");
    ImGui::TextColored(muted_v4(), "default-deny; each entry permits ONE absolute binary the brokered run tool");
    ImGui::TextColored(muted_v4(), "may execute. Empty => exec disabled. Every run is operator-gated + kernel-jailed.");
    /* The LIVE list (EditExecAllowlist persists immediately, like the egress editor). Remove narrows; Add WIDENS
     * and is confirm-gated. The host re-validates (absolute + exists); the ExecGate re-checks again at run time. */
    for (size_t i = 0; i < live.exec_allow.size(); i++) {
        ImGui::PushID((int)(10000 + i));
        ImGui::TextUnformatted(live.exec_allow[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            ctx.commands.push_back({UiCommand::Kind::EditExecAllowlist, live.exec_allow[i], "remove", 0, {}, {}});
        ImGui::PopID();
    }
    if (live.exec_allow.empty()) ImGui::TextColored(muted_v4(), "(none — exec disabled)");

    static char execbuf[512] = {};
    static char execpending[512] = {};
    static char execerr[96] = {};
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("##exec_add", execbuf, sizeof execbuf);
    ImGui::SameLine();
    if (ImGui::Button("Add\xE2\x80\xA6##exec")) { /* "Add…" */
        execerr[0] = '\0';
        if (execbuf[0] && execbuf[0] != '/')
            std::snprintf(execerr, sizeof execerr, "must be an absolute path (e.g. /usr/bin/pytest)");
        else if (execbuf[0]) {
            std::snprintf(execpending, sizeof execpending, "%s", execbuf);
            ImGui::OpenPopup("Confirm run-allow entry");
        }
    }
    if (execerr[0]) ImGui::TextColored(err_v4(), "%s", execerr);

    if (ImGui::BeginPopupModal("Confirm run-allow entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Allow the fleet to RUN:");
        ImGui::TextColored(accent_v4(), "%s", execpending);
        ImGui::TextColored(muted_v4(), "Each run stays operator-gated + kernel-jailed (no network, workspace only).");
        ImGui::TextColored(err_v4(), "Only allow binaries you trust the fleet to execute.");
        ImGui::Spacing();
        if (ImGui::Button("Confirm \xE2\x80\x94 allow")) { /* "Confirm — allow" */
            ctx.commands.push_back({UiCommand::Kind::EditExecAllowlist, std::string(execpending), "add", 0, {}, {}});
            execbuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##exec")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("AUTOMATION");
    ImGui::TextColored(muted_v4(), "Delegated approval — the human gate is the default and stays the floor.");
    /* B3: deterministic auto-approve of sandbox-contained writes only; everything else still prompts; never denies. */
    ImGui::Checkbox("auto-approve sandboxed writes (fs_write/fs_update) \xE2\x80\x94 exec/memory still prompt",
                    &d.auto_approve_contained);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When on, file writes inside an agent's workspace jail are approved without a prompt — "
                          "they're sandbox-contained + recorded as artifacts. Exec, shared-memory writes, and "
                          "anything new ALWAYS come to you. It never auto-denies. Default off.");

    /* B3b: the read-only-egress class — tools that can reach the network but cannot write files or run binaries. */
    ImGui::Checkbox("auto-approve read-only web tools (egress, no fs-write, no exec)",
                    &d.auto_approve_readonly_egress);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When on, calls to third-party tools whose manifest grants network access but NO file "
                          "writes and NO exec are approved without a prompt — the worst case is a page fetched "
                          "you didn't sanction, not a changed machine. A tool that also writes or execs still "
                          "prompts, as does any function its author marked sensitive. Meant for research runs, "
                          "where one task is tens of fetches. It never auto-denies. Default off.");

    /* B4: the allow-all escape hatch — armed ONLY behind a type-to-confirm consent window, loud while live, and
     * disarmable in one click (arming/disarming is committed immediately, bypassing [Apply], for an unambiguous
     * footgun). */
    ImGui::Spacing();
    if (d.allow_all_approvals) {
        ImGui::TextColored(err_v4(), "ALLOW-ALL is ARMED \xE2\x80\x94 EVERY tool request is auto-approved, no prompts.");
        if (ImGui::Button("Disarm allow-all")) { /* narrowing is always frictionless */
            d.allow_all_approvals = false;
            ctx.commands.push_back({UiCommand::Kind::SaveSettings, "", "", 0, {}, d});
        }
    } else if (ImGui::Button("Arm ALLOW-ALL (disables the human approval gate)")) {
        ImGui::OpenPopup("Arm allow-all");
    }
    if (consent_modal("Arm allow-all",
                      "ALLOW-ALL disables the human approval gate ENTIRELY: every fs_write, fs_update, shared memory "
                      "write, and allowlisted command runs WITHOUT asking you. Intended only for power users and "
                      "controlled stress tests. The sandbox jail and the exec allowlist still apply; the human floor "
                      "does not. It stays loudly visible while armed and can be disarmed in one click.",
                      "ALLOW ALL", /*danger=*/true)) {
        d.allow_all_approvals = true;
        ctx.commands.push_back({UiCommand::Kind::SaveSettings, "", "", 0, {}, d});
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Apply")) /* persist the draft + apply (LIVE now; provider/limits on restart) */
        ctx.commands.push_back({UiCommand::Kind::SaveSettings, "", "", 0, {}, d});
    ImGui::SameLine();
    ImGui::TextColored(muted_v4(), "accent applies now; provider/limits on restart");

    if (!s.notice.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(accent_v4(), "%s", s.notice.c_str());
    }

    ImGui::End();
}

} // namespace hc::ui
