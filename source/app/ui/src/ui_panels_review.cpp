/* ui_panels (review cluster) — the human-in-the-loop review surfaces: the pending fs_write diff, the
 * memory panel, and the tool-authorization approvals. See ui_panels.hpp for the full contract. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp"

#include "ui_theme.hpp"

#include "imgui.h"

namespace hc::ui {

/* Render an fs_write as a reviewable unified diff (P11) in the restrained palette: the +/- gutter +
 * monospace alignment carry the meaning; added lines take the single accent, removed lines the one dim
 * red, context stays default — NO green, NO filled backgrounds (doc 10). The tint only reinforces what
 * the gutter already says. All line text is untrusted -> rendered via "%s". */
/* SHARED (declared in ui_panels_internal.hpp): the W5 P5.2 IDE editor reuses this for its change-on-disk diff. */
void draw_pending_diff(const PendingDiff &d)
{
    ImGui::TextUnformatted(d.path.c_str()); /* the file path (untrusted) */
    ImGui::SameLine();
    ImGui::TextColored(accent_v4(), "+%d", d.added);
    ImGui::SameLine();
    ImGui::TextColored(err_v4(), "-%d", d.removed);
    if (d.is_new) {
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "(new file)");
    }
    for (const auto &h : d.hunks) {
        ImGui::TextColored(muted_v4(), "@@ -%d +%d @@", h.old_start, h.new_start);
        for (const auto &l : h.lines) {
            ImVec4 col = (l.kind == '+')   ? accent_v4()
                         : (l.kind == '-') ? err_v4()
                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(col, "%c %s", l.kind, l.text.c_str()); /* untrusted text is the %s arg */
        }
    }
}

/* The Memory panel (P01): see + prune what the fleet remembers. Each record shows its scope (the recall
 * domain) + provenance + the text; a per-row Forget emits a ForgetMemory command. All text is untrusted
 * (model/operator-authored) — rendered via "%s"/TextWrapped, never interpreted. */
void draw_memory_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Memory", open)) {
        const MemoryStatus &st = s.memory_status;
        using MState = MemoryStatus::State;
        if (st.state == MState::ModelClash) {
            /* The dangerous state, and the reason this branch exists at all: the store HOLDS records
             * but will not serve them, because scoring them against another model's vectors returns
             * confident nonsense. Falling through to "no memories yet" would describe a refusing store
             * as an empty one — reassuring, and wrong. All ids are format ARGUMENTS, never the format. */
            ImGui::TextColored(err_v4(), "memory unavailable — the store is refusing");
            ImGui::Spacing();
            WrappedText("These records were embedded by %s%s, but the model configured now is %s. "
                        "Vectors from two different models are not comparable, so recall and writes are "
                        "refused rather than answered with nonsense.",
                        st.store_model.empty() ? "(unknown)" : st.store_model.c_str(),
                        st.store_model_assumed ? " (assumed — inferred later, not recorded at the time)" : "",
                        st.config_model.empty() ? "(none)" : st.config_model.c_str());
            ImGui::Spacing();
            WrappedText("Set the embedding model back to %s in Settings > Provider, or delete %s to start "
                        "a fresh store on the new model.",
                        st.store_model.empty() ? "the original one" : st.store_model.c_str(),
                        st.store_path.empty() ? "this project's memory directory" : st.store_path.c_str());
        } else if (st.state == MState::NoEmbedModel) {
            ImGui::TextColored(muted_v4(), "memory is off — no embedding model configured");
            ImGui::Spacing();
            WrappedText("Storing a memory and recalling one both need an embedding model: the text is "
                        "turned into a vector so it can be found by meaning later. Without one there is "
                        "nowhere for a memory to go, so writes are refused as well as recall — not just "
                        "the search. Set one in Settings > Provider.");
        } else if (s.memory.empty()) {
            ImGui::TextColored(muted_v4(), "no memories yet — the fleet learns as it works");
        } else {
            ImGui::TextColored(muted_v4(), "%zu memories (reference, not instruction)", s.memory.size());
            ImGui::Separator();
            for (const auto &m : s.memory) {
                ImGui::PushID(m.id.c_str());
                ImGui::TextColored(accent_v4(), "%s", m.scope.c_str());
                ImGui::SameLine();
                /* O7: ellipsize the (variable-length) source so the forget button stays on-screen on a narrow
                 * panel; full source on hover. scope is short and stays accent. */
                const float       fbtn = ImGui::CalcTextSize("forget").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                                   ImGui::GetStyle().ItemSpacing.x;
                const float       src_avail = ImGui::GetContentRegionAvail().x - fbtn;
                const std::string src = std::string("\xC2\xB7 ") + (m.source.empty() ? "?" : m.source);
                ImGui::PushStyleColor(ImGuiCol_Text, muted_v4());
                ImGui::TextUnformatted(fit_ellipsis(src, src_avail > 24.0f ? src_avail : 24.0f).c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && !m.source.empty() && ImGui::CalcTextSize(src.c_str()).x > src_avail)
                    ImGui::SetTooltip("%s", m.source.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("forget"))
                    ctx.commands.push_back({UiCommand::Kind::ForgetMemory, m.id, "", 0, {}});
                WrappedText("%s", m.text.c_str());
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

void draw_approvals_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Approvals", open)) {
        if (s.pending_auth.empty()) {
            ImGui::TextColored(muted_v4(), "no pending tool requests");
        } else {
            for (const auto &p : s.pending_auth) {
                ImGui::PushID(p.id.c_str());
                ImGui::TextColored(accent_v4(), "%s", p.agent.empty() ? "?" : p.agent.c_str());
                ImGui::SameLine();
                ImGui::TextColored(muted_v4(), "wants %s", p.tool.c_str());
                /* B1: PATIENT approvals never auto-expire, so show how long it has waited — accented once it's been
                 * a while, so a long-pending request draws the eye instead of being silently lost. */
                {
                    long secs = p.age_ms / 1000;
                    ImGui::SameLine();
                    if (secs >= 60)
                        ImGui::TextColored(secs >= 120 ? accent_v4() : muted_v4(), "· pending %ldm %lds", secs / 60,
                                           secs % 60);
                    else
                        ImGui::TextColored(muted_v4(), "· pending %lds", secs);
                }
                /* an fs_write shows the diff; everything else (and a too-large/binary/no-op diff) shows
                 * the bounded blob summary — the human gate becomes a real review, not a rubber-stamp. */
                const PendingDiff *diff = nullptr;
                for (const auto &d : s.pending_diff)
                    if (d.id == p.id) {
                        diff = &d;
                        break;
                    }
                if (diff && !diff->too_large && !diff->hunks.empty())
                    draw_pending_diff(*diff);
                else
                    WrappedText("%s", p.summary.c_str()); /* model-influenced -> "%s" */
                if (ImGui::Button("Allow"))
                    ctx.commands.push_back({UiCommand::Kind::ToolVerdict, p.id, "", 1, {}});
                ImGui::SameLine();
                if (ImGui::Button("Deny"))
                    ctx.commands.push_back({UiCommand::Kind::ToolVerdict, p.id, "", 0, {}});
                ImGui::SameLine();
                if (ImGui::Button("Dismiss")) /* B1: defer — clears the prompt WITHOUT denying (the agent may re-ask) */
                    ctx.commands.push_back({UiCommand::Kind::ToolDismiss, p.id, "", 0, {}});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Set aside without deciding — not a denial. The request is cleared and the "
                                      "agent is told it was deferred (nothing is written or run).");
                if (p.tool == "fs_write") { /* P09.3: approve + grant a scoped capability (prompt-free repeats) */
                    std::string dir; /* the EXACT prefix the grant covers — shown so the operator's consent is explicit */
                    if (diff) {
                        size_t slash = diff->path.find_last_of('/');
                        if (slash != std::string::npos) dir = diff->path.substr(0, slash + 1);
                    }
                    ImGui::SameLine();
                    std::string label = dir.empty() ? "Allow 10x here" : ("Allow 10x in " + dir);
                    if (ImGui::Button(label.c_str()))
                        ctx.commands.push_back({UiCommand::Kind::ToolGrantScoped, p.id, "", 10, {}});
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Approve this write AND allow 10 more under %s without prompting — "
                                          "scoped, budgeted, revocable, bound to this agent.",
                                          dir.empty() ? "the file's directory" : dir.c_str());
                }
                ImGui::Separator();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

/* P08.2: the egress-audit Network panel — a read-only table of recent egress decisions (broker-stamped agent +
 * the requested peer + the verdict). Default-OFF (opt-in via the View menu). A DENY row carries the single
 * functional accent. Host strings are model-influenced -> rendered "%s" only. */
void draw_network_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    (void)ctx;
    if (ImGui::Begin("Network", open)) {
        if (s.egress.empty()) {
            ImGui::TextColored(muted_v4(), "no egress recorded yet");
        } else {
            ImGui::TextColored(muted_v4(), "%zu recent egress decision%s (newest last)", s.egress.size(),
                               s.egress.size() == 1 ? "" : "s");
            if (ImGui::BeginTable("egress", 4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("agent");
                ImGui::TableSetupColumn("host:port");
                ImGui::TableSetupColumn("ip");
                ImGui::TableSetupColumn("verdict");
                ImGui::TableHeadersRow();
                for (const auto &e : s.egress) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.agent.c_str()); /* broker-stamped -> authentic */
                    ImGui::TableNextColumn();
                    ImGui::Text("%s:%u", e.host.c_str(), (unsigned)e.port); /* host is model-influenced -> %s */
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.ip.c_str());
                    ImGui::TableNextColumn();
                    bool deny = e.verdict.rfind("deny", 0) == 0;
                    ImGui::TextColored(deny ? accent_v4() : muted_v4(), "%s", e.verdict.c_str());
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

} // namespace hc::ui
