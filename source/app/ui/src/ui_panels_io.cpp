/* ui_panels (io cluster) — the observability/stream panels: dashboard, log, console (token stream), reasoning
 * chain, session browser, transcript, and operator terminal. The file-CONTENT panels (file browser, Viewer,
 * IDE Editor) split into ui_panels_content.cpp at the ~500 LOC smell-test (W5 P5.3). See ui_panels.hpp. */

#include "ui_panels.hpp"

#include "ui_copy.hpp"
#include "ui_graph.hpp"
#include "ui_markdown.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <cstdio>
#include <ctime>
#include <string>

namespace hc::ui {

void draw_dashboard_panel(const UiSnapshot &s, bool *open)
{
    if (ImGui::Begin("Dashboard", open)) {
        int ready = 0, dead = 0, busy = 0;
        for (const auto &a : s.agents)
            (a.state == "ready") ? ready++ : (a.state == "dead" ? dead++ : busy++);
        int done = 0, running = 0, assigned = 0, pending = 0, failed = 0;
        for (const auto &t : s.tasks) {
            if (t.state == "done") done++;
            else if (t.state == "running") running++;
            else if (t.state == "assigned") assigned++;
            else if (t.state == "failed") failed++;
            else pending++;
        }
        char fleet[16], tasks[16], prog[16];
        std::snprintf(fleet, sizeof fleet, "%d/%zu", ready, s.agents.size());
        std::snprintf(tasks, sizeof tasks, "%d/%zu", done, s.tasks.size());
        std::snprintf(prog, sizeof prog, "%d%%", s.agenda_progress);
        /* a row of stat cards */
        if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_BordersInner)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            stat_cell("FLEET READY", fleet, ready > 0);
            ImGui::TableNextColumn();
            stat_cell("TASKS DONE", tasks);
            ImGui::TableNextColumn();
            stat_cell("PROGRESS", prog, true);
            ImGui::TableNextColumn();
            stat_cell("FAILED", failed ? std::to_string(failed).c_str() : "0");
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::ProgressBar(s.agenda_progress / 100.0f, ImVec2(-1.0f, 0.0f));
        ImGui::Spacing();
        /* USAGE sits directly under the headline stats: token/cost accounting is the primary metric of an
         * observability dashboard (P12), so it ranks above the task-state histogram (which only re-plots
         * the done/failed counts already shown in the stat cells, and so trails at the bottom). */
        PanelHeader("USAGE");
        if (s.provider == "offline") {
            ImGui::TextColored(muted_v4(), "offline - no usage");
        } else if (s.tokens_in == 0 && s.tokens_out == 0) {
            ImGui::TextColored(muted_v4(), "no usage yet (tokens appear as agents run)");
        } else {
            if (ImGui::BeginTable("usage_totals", 3, ImGuiTableFlags_BordersInner)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                stat_cell("TOKENS IN", std::to_string(s.tokens_in).c_str());
                ImGui::TableNextColumn();
                stat_cell("TOKENS OUT", std::to_string(s.tokens_out).c_str(), true);
                ImGui::TableNextColumn();
                /* cost stays honest: a number only when the model is priced (no default table yet) */
                if (s.cost_usd >= 0) {
                    char c[24];
                    std::snprintf(c, sizeof c, "$%.4f", s.cost_usd);
                    stat_cell("COST", c);
                } else {
                    stat_cell("COST", "n/a");
                }
                ImGui::EndTable();
            }
            if (ImGui::BeginTable("usage_by_agent", 3,
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("agent", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("in", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("out", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableHeadersRow();
                for (const auto &u : s.usage_by_agent) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(u.agent.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%ld", u.input_tokens);
                    ImGui::TableNextColumn();
                    ImGui::Text("%ld", u.output_tokens);
                }
                ImGui::EndTable();
            }
        }
        ImGui::Spacing();
        /* SYSTEM (WI-3): what the host process costs the machine — CPU%, resident memory, its own uptime,
         * the wall clock, and short CPU/MEM sparklines (hand-drawn, on-theme). present=false offline-or-
         * before-the-first-sample. */
        PanelHeader("SYSTEM");
        if (!s.sysstat.present) {
            ImGui::TextColored(muted_v4(), "sampling...");
        } else {
            const SysStat &ss = s.sysstat;
            char           cpu[24], mem[24], up[40], clk[24];
            std::snprintf(cpu, sizeof cpu, "%.1f%%", ss.cpu_pct);
            std::snprintf(mem, sizeof mem, "%.1f MB", (double)ss.rss_bytes / (1024.0 * 1024.0));
            unsigned long long us = (unsigned long long)(ss.uptime_ms / 1000);
            std::snprintf(up, sizeof up, "%lluh %02llum %02llus", us / 3600, (us % 3600) / 60, us % 60);
            time_t    wt = (time_t)(ss.wall_ms / 1000);
            struct tm tmv = {}; /* zero-init: a (theoretical) gmtime_r failure can't leave it uninitialized */
            gmtime_r(&wt, &tmv);
            std::snprintf(clk, sizeof clk, "%02d:%02d:%02d UTC", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            if (ImGui::BeginTable("sys_stats", 4, ImGuiTableFlags_BordersInner)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                stat_cell("CPU", cpu, ss.cpu_pct > 50.0);
                ImGui::TableNextColumn();
                stat_cell("MEM", mem);
                ImGui::TableNextColumn();
                stat_cell("UPTIME", up);
                ImGui::TableNextColumn();
                stat_cell("CLOCK", clk);
                ImGui::EndTable();
            }
            if (ss.cpu_history.size() >= 2) {
                ImGui::TextColored(muted_v4(), "cpu %%");
                float cmax = 100.0f;
                for (float v : ss.cpu_history)
                    if (v > cmax) cmax = v;
                AreaGraph(ss.cpu_history.data(), (int)ss.cpu_history.size(), ImVec2(-1.0f, 38.0f), 0.0f,
                          cmax);
            }
            if (ss.rss_mb_history.size() >= 2) {
                ImGui::TextColored(muted_v4(), "mem MB");
                float rmin = ss.rss_mb_history.front(), rmax = rmin;
                for (float v : ss.rss_mb_history) {
                    if (v < rmin) rmin = v;
                    if (v > rmax) rmax = v;
                }
                if (rmax - rmin < 1.0f) rmax = rmin + 1.0f; /* avoid a degenerate flat range */
                Sparkline(ss.rss_mb_history.data(), (int)ss.rss_mb_history.size(), ImVec2(-1.0f, 38.0f),
                          rmin, rmax);
            }
        }
        ImGui::Spacing();
        PanelHeader("TASK STATES");
        float       vals[5] = {(float)done, (float)running, (float)assigned, (float)pending,
                               (float)failed};
        const char *names[5] = {"done", "run", "asgn", "pend", "fail"};
        ImGui::PlotHistogram("##states", vals, 5, 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 60.0f));
        if (ImGui::BeginTable("legend", 5)) {
            for (int i = 0; i < 5; i++) {
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "%s %.0f", names[i], vals[i]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void draw_log_panel(const UiSnapshot &s, bool *open)
{
    if (ImGui::Begin("Log", open)) {
        if (ImGui::BeginChild("logscroll")) {
            /* logs stay plain + per-entry (discrete diagnostic lines, not prose), but WRAP at the panel edge
             * instead of scrolling sideways. TextUnformatted respects the wrap pos and never interprets the text. */
            constexpr size_t kMaxLines = 2000;
            const size_t     total = s.logs.size();
            const size_t     start = total > kMaxLines ? total - kMaxLines : 0;
            ImGui::PushTextWrapPos(0.0f);
            std::string all; /* the shown window, for right-click Copy all (the lines aren't per-block-copyable) */
            for (size_t i = start; i < total; i++) {
                ImGui::TextUnformatted(s.logs[i].c_str());
                all += s.logs[i];
                all += '\n';
            }
            ImGui::PopTextWrapPos();
            copy_region_menu("log_copy", all); /* right-click anywhere in the log -> Copy all */
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void draw_console_panel(const UiSnapshot &s, bool *open)
{
    if (ImGui::Begin("Console", open)) {
        if (ImGui::BeginChild("console_scroll")) {
            constexpr size_t kMaxLines = 2000;
            const size_t     total = s.chat.size();
            const size_t     start = total > kMaxLines ? total - kMaxLines : 0;
            std::string      all; /* the reconstructed flowing stream, for right-click Copy all */
            if (total == 0) {
                ImGui::TextColored(muted_v4(), "no token stream yet");
            } else {
                /* The fleet token stream arrives as per-delta lines "<agent>: <chunk>" (host_bridge). Rebuild
                 * flowing text by concatenating consecutive deltas from the SAME agent (LLM token deltas join
                 * directly — the model emits its own newlines), then render each agent block with the audited
                 * markdown renderer. So a streamed reply reads as prose instead of one-chunk-per-line, while the
                 * per-agent header keeps attribution for an interleaved multi-worker stream. */
                std::string cur_agent, block;
                auto flush = [&]() {
                    if (block.empty() && cur_agent.empty()) return;
                    if (!cur_agent.empty()) {
                        ImGui::TextColored(accent_v4(), "%s", cur_agent.c_str());
                        all += cur_agent;
                        all += ": ";
                    }
                    render_markdown_cached(block); /* settled blocks hit the content cache; only the live one re-parses */
                    all += block;
                    all += "\n\n";
                    ImGui::Spacing();
                };
                for (size_t i = start; i < total; i++) {
                    const std::string &ln    = s.chat[i];
                    const size_t       colon = ln.find(": ");
                    std::string        agent = (colon != std::string::npos) ? ln.substr(0, colon) : std::string();
                    const char        *body  = (colon != std::string::npos) ? ln.c_str() + colon + 2 : ln.c_str();
                    if (agent != cur_agent) {
                        flush();
                        cur_agent = std::move(agent);
                        block.clear();
                    }
                    block += body;
                }
                flush();
            }
            copy_region_menu("console_copy", all); /* right-click anywhere in the console -> Copy all */
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void draw_reasoning_panel(const UiSnapshot &s, bool *open)
{
    if (ImGui::Begin("Reasoning", open)) {
        if (s.reasoning.empty()) {
            ImGui::TextColored(muted_v4(), "no deep_reason chain yet");
        } else if (ImGui::BeginChild("reasoning_scroll")) {
            /* the deep_reason chain is settled prose — format it with the same audited markdown renderer the
             * conductor chat uses (TextUnformatted under the hood, bounded). The worker frame-bounds it to 240 KiB. */
            render_markdown_cached(s.reasoning);
            copy_region_menu("reasoning_copy", s.reasoning); /* right-click -> Copy all */
        }
        if (!s.reasoning.empty()) ImGui::EndChild();
    }
    ImGui::End();
}

void draw_sessions_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Sessions", open)) {
        ImGui::TextColored(muted_v4(), "%zu persisted", s.sessions.size());
        ImGui::Spacing();
        if (s.sessions.empty()) {
            ImGui::TextColored(muted_v4(), "no sessions yet");
        } else if (ImGui::BeginTable("sessions", 3,
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("turns", ImGuiTableColumnFlags_WidthStretch, 0.18f);
            ImGui::TableSetupColumn("updated", ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableHeadersRow();
            for (const auto &se : s.sessions) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool sel = (s.viewed_session == se.id);
                ImGui::PushID(se.id.c_str());
                if (ImGui::Selectable(se.title.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                    ctx.show.transcript = true;
                    ctx.commands.push_back({UiCommand::Kind::OpenSession, se.id, "", 0, {}});
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%d", se.turns);
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "%s", se.updated.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void draw_transcript_panel(const UiSnapshot &s, bool *open)
{
    if (ImGui::Begin("Transcript", open)) {
        if (s.viewed_session.empty()) {
            ImGui::TextColored(muted_v4(), "select a session to view its transcript");
        } else if (ImGui::BeginChild("transcript_scroll")) {
            /* role-labelled message blocks — untrusted text via TextWrapped("%s", ...). The role
             * prefix is rendered with %.*s (bounded, no per-line substring allocation). Each turn is
             * grouped so a right-click offers Copy (the message) / Copy all (the whole transcript). */
            std::string all;
            for (const auto &l : s.transcript) {
                all += l;
                all += '\n';
            }
            int idx = 0;
            for (const auto &line : s.transcript) {
                ImGui::PushID(idx++);
                ImGui::BeginGroup();
                size_t      colon = line.find(": ");
                std::string copy_text; /* "Copy" grabs the message content (no role prefix) */
                if (colon != std::string::npos) {
                    bool assistant = line.compare(0, colon, "assistant") == 0;
                    ImGui::TextColored(assistant ? accent_v4() : muted_v4(), "%.*s", (int)colon,
                                       line.c_str());
                    const char *content = line.c_str() + colon + 2;
                    copy_text = content;
                    if (assistant)
                        render_markdown_cached(content); /* WI-4: format the FINISHED assistant message */
                    else
                        WrappedText("%s", content); /* user text stays plain */
                } else {
                    WrappedText("%s", line.c_str());
                    copy_text = line;
                }
                ImGui::EndGroup();
                copy_block_menu("transcript_msg", copy_text, all);
                ImGui::PopID();
                ImGui::Spacing();
            }
            copy_region_menu("transcript_copy", all, true); /* empty space -> Copy all */
        }
        if (!s.viewed_session.empty()) ImGui::EndChild();
    }
    ImGui::End();
}

void draw_terminal_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Terminal", open)) {
        if (ImGui::BeginChild("term_out", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), 0,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::TextUnformatted(s.terminal.c_str()); /* raw shell bytes, never interpreted */
            copy_region_menu("terminal_copy", s.terminal); /* right-click -> Copy all */
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        static char cmd[1024] = ""; /* function-static: ImGui in-place input state, single UI thread */
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##terminput", cmd, sizeof cmd, ImGuiInputTextFlags_EnterReturnsTrue)) {
            ctx.commands.push_back({UiCommand::Kind::TermInput, cmd, "", 0, {}});
            cmd[0] = '\0';
        }
    }
    ImGui::End();
}

} // namespace hc::ui
