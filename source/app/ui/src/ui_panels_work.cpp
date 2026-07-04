/* ui_panels (work cluster) — the fleet roster, task-DAG view, activity timeline, agenda board, agenda
 * builder, and task detail. See ui_panels.hpp for the full contract. */

#include "ui_panels.hpp"
#include "ui_panels_internal.hpp"

#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hc::ui {

namespace {

/* Parse a "t1, t2" dependency string into task ids (whitespace-trimmed, empties dropped). */
std::vector<std::string> parse_deps(const std::string &csv)
{
    std::vector<std::string> out;
    for (size_t start = 0; start < csv.size();) {
        size_t      comma = csv.find(',', start);
        size_t      end = (comma == std::string::npos) ? csv.size() : comma;
        std::string tok = csv.substr(start, end - start);
        size_t      a = tok.find_first_not_of(" \t");
        if (a != std::string::npos) out.push_back(tok.substr(a, tok.find_last_not_of(" \t") - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

} // namespace

void draw_fleet_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Fleet", open)) {
        int ready = 0, dead = 0, other = 0;
        for (const auto &a : s.agents)
            (a.state == "ready") ? ready++ : (a.state == "dead" ? dead++ : other++);
        ImGui::Text("%zu agents", s.agents.size());
        ImGui::SameLine();
        ImGui::TextColored(muted_v4(), "  %d ready  %d busy  %d dead", ready, other, dead);
        ImGui::Spacing();
        if (s.agents.empty()) {
            ImGui::TextColored(muted_v4(), "no workers — add one below to build your fleet");
        } else if (ImGui::BeginTable("agents", 4,
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthStretch, 0.46f);
            ImGui::TableSetupColumn("role", ImGuiTableColumnFlags_WidthStretch, 0.28f);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.28f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
            ImGui::TableHeadersRow();
            std::string remove_id;
            for (const auto &a : s.agents) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                EllipsisText(a.id); /* O3: a long agent id ellipsizes (+ hover tooltip) instead of overflowing */
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "%s", a.role.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(state_color(a.state), "%s", a.state.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(a.id.c_str());
                if (ImGui::SmallButton("x")) remove_id = a.id; /* reap + de-authorize, drop from the fleet */
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!remove_id.empty())
                ctx.commands.push_back({UiCommand::Kind::RemoveWorker, remove_id, "", 0, {}});
        }

        ImGui::Spacing();
        PanelHeader("ADD WORKER");
        /* the role choices: the live fleet's roles, else the built-in defaults (mirrors the agenda builder) */
        static const char *const kFallbackRoles[] = {"dev", "qa", "research", "ops"};
        std::vector<std::string> roles;
        if (!s.roles.empty()) roles = s.roles;
        else
            for (const char *r : kFallbackRoles) roles.emplace_back(r);
        static int rsel = 0;
        if (rsel < 0 || rsel >= (int)roles.size()) rsel = 0;
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("##addrole", roles[rsel].c_str())) {
            for (int i = 0; i < (int)roles.size(); i++) {
                bool sel = (i == rsel);
                if (ImGui::Selectable(roles[i].c_str(), sel)) rsel = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("+ add worker"))
            ctx.commands.push_back({UiCommand::Kind::AddWorker, roles[rsel], "", 0, {}});
    }
    ImGui::End();
}

/* Pure layered layout (P10): depth = the longest dependency chain to a root (the column), assigned by
 * bounded relaxation — it converges in <= n passes for a DAG, and a dependency cycle simply stops
 * growing at the n-pass cap (so a cycle lays out without looping). row = the slot within a column. No
 * ImGui, no GL — unit-tested directly. */
std::vector<DagNode> dag_layout(const std::vector<TaskRow> &tasks)
{
    const size_t         n = tasks.size();
    std::vector<DagNode> nodes;
    if (n == 0) return nodes;

    std::unordered_map<std::string, size_t> idx;
    for (size_t i = 0; i < n; i++) idx[tasks[i].id] = i;

    std::vector<int> depth(n, 0);
    for (size_t pass = 0; pass < n; pass++) {
        bool changed = false;
        for (size_t i = 0; i < n; i++)
            for (const auto &d : tasks[i].deps) {
                auto it = idx.find(d);
                if (it == idx.end()) continue;       /* a missing dep is not an edge */
                int nd = depth[it->second] + 1;
                if (nd > (int)n - 1) nd = (int)n - 1; /* clamp at the DAG max so a cycle can't grow it
                                                       * unboundedly — depths stabilize, the loop ends */
                if (depth[i] < nd) {
                    depth[i] = nd;
                    changed = true;
                }
            }
        if (!changed) break;
    }

    std::unordered_map<int, int> next_row;
    nodes.reserve(n);
    for (size_t i = 0; i < n; i++) nodes.push_back({i, depth[i], next_row[depth[i]]++});
    return nodes;
}

/* The task-DAG view (P10): the agenda's dependency graph as a layered node graph — boxes via
 * state_color(), 1px dependency edges, the accent border on the selected node; click selects a task.
 * Drawn with ImDrawList primitives (no graph-layout dependency), within doc-10's restrained palette. */
/* Pixel-aware truncate-with-ellipsis: the longest prefix of `text` (cut on a UTF-8 boundary) whose width plus
 * a ".." fits in `max_px`. Replaces the old crude character count, so a label never overflows its box. */
void draw_dag_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Plan", open)) {
        if (s.tasks.empty()) {
            ImGui::TextColored(muted_v4(), "no tasks — build an agenda to see its graph");
            ImGui::End();
            return;
        }
        /* Own scroll region: wrap the absolute-positioned ImDrawList canvas in a child so it scrolls in its
         * OWN clean rect (V + H) and the drawlist clips to it. GetWindowDrawList()/GetCursorScreenPos() now
         * resolve against the child. */
        ImGui::BeginChild("dag_canvas", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        std::vector<DagNode> nodes = dag_layout(s.tasks);
        const float          boxW = 140.0f, boxH = 38.0f, gapX = 46.0f, gapY = 14.0f;
        const float          expW = 232.0f; /* the selected box grows to this width to show the full title */
        const ImVec2         origin = ImGui::GetCursorScreenPos();
        ImDrawList          *dl = ImGui::GetWindowDrawList();
        const float          lineH = ImGui::GetTextLineHeight();

        /* Per-box expand animation (0..1), persistent across frames — ImGui's idiom for in-place widget
         * state. The selected box eases toward 1 (expanded, full text); the rest ease toward 0. Keyed by task
         * id so it survives a reorder. */
        static std::unordered_map<std::string, float> anim;
        const float                                   dt = ImGui::GetIO().DeltaTime;

        std::vector<ImVec2> tl(s.tasks.size()); /* each task box's top-left, in screen space */
        int                 cols = 0, rows = 0;
        for (const auto &nd : nodes) {
            tl[nd.task] = ImVec2(origin.x + (float)nd.depth * (boxW + gapX),
                                 origin.y + (float)nd.row * (boxH + gapY));
            cols = std::max(cols, nd.depth + 1);
            rows = std::max(rows, nd.row + 1);
        }
        /* edges first (boxes draw over them): prerequisite right-edge -> dependent left-edge, 1px muted */
        const ImU32 edge = ImGui::GetColorU32(muted_v4());
        for (size_t i = 0; i < s.tasks.size(); i++)
            for (const auto &d : s.tasks[i].deps)
                for (size_t j = 0; j < s.tasks.size(); j++)
                    if (s.tasks[j].id == d) {
                        dl->AddLine(ImVec2(tl[j].x + boxW, tl[j].y + boxH * 0.5f),
                                    ImVec2(tl[i].x, tl[i].y + boxH * 0.5f), edge, 1.0f);
                        break;
                    }

        /* Draw one box at its animated size. CLICK toggles selection (re-click contracts); the content is
         * always clipped to the box, so it never overflows — collapsed shows a fit-to-width title, expanded
         * shows the full WRAPPED title + state + assignee. */
        auto draw_box = [&](size_t i, float t) {
            const TaskRow &task = s.tasks[i];
            const float    w = boxW + (expW - boxW) * t;
            float          eh = boxH;
            if (t > 0.001f) { /* expanded height fits the wrapped title + the state + the assignee lines */
                ImVec2 ts = ImGui::CalcTextSize(task.title.c_str(), nullptr, false, expW - 12.0f);
                float  exph = 6.0f + ts.y + 4.0f + lineH + 2.0f + lineH + 6.0f;
                if (exph < boxH) exph = boxH;
                if (exph > 180.0f) exph = 180.0f; /* a sane ceiling; the clip handles a runaway title */
                eh = boxH + (exph - boxH) * t;
            }
            const ImVec2 a = tl[i], b(a.x + w, a.y + eh);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID((int)i);
            bool clicked = ImGui::InvisibleButton("##node", ImVec2(w, eh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked) {
                if (ctx.selected_task == task.id) ctx.selected_task.clear(); /* toggle off -> contract */
                else {
                    ctx.selected_task = task.id;
                    ctx.show.task = true;
                }
            }
            if (hov && t < 0.5f) ImGui::SetTooltip("%s", task.title.c_str()); /* untrusted; only while collapsed */
            const bool  sel = (ctx.selected_task == task.id);
            const ImU32 border = ImGui::GetColorU32(sel ? accent_v4() : state_color(task.state));
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg));
            dl->AddRect(a, b, border, 0.0f, 0, 1.0f); /* the project's signature 1px border */
            dl->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true); /* never spill the box */
            const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text);
            if (t < 0.5f) { /* collapsed: a width-fitted title + the state */
                dl->AddText(ImVec2(a.x + 6, a.y + 4), txt, fit_ellipsis(task.title, boxW - 12.0f).c_str());
                dl->AddText(ImVec2(a.x + 6, a.y + 20), ImGui::GetColorU32(state_color(task.state)),
                            task.state.c_str());
            } else { /* expanded: the FULL title wrapped to the box, then state + assignee */
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 6, a.y + 6), txt,
                            task.title.c_str(), nullptr, w - 12.0f);
                ImVec2 ts = ImGui::CalcTextSize(task.title.c_str(), nullptr, false, w - 12.0f);
                float  yy = a.y + 6.0f + ts.y + 4.0f;
                dl->AddText(ImVec2(a.x + 6, yy), ImGui::GetColorU32(state_color(task.state)), task.state.c_str());
                if (!task.assignee.empty())
                    dl->AddText(ImVec2(a.x + 6, yy + lineH + 2.0f), ImGui::GetColorU32(muted_v4()),
                                task.assignee.c_str());
            }
            dl->PopClipRect();
        };

        /* advance each animation, then draw — the selected box is DEFERRED to the end so it overlays its
         * neighbours cleanly while expanded. */
        int sel_idx = -1;
        for (size_t i = 0; i < s.tasks.size(); i++) {
            const std::string &id = s.tasks[i].id;
            float              target = (ctx.selected_task == id) ? 1.0f : 0.0f;
            float              cur = anim.count(id) ? anim[id] : 0.0f;
            cur += (target - cur) * std::min(1.0f, dt * 12.0f); /* exponential ease -> the simple animation */
            if (cur < 0.001f) cur = 0.0f;
            anim[id] = cur;
            if (target >= 1.0f) {
                sel_idx = (int)i;
                continue;
            }
            draw_box(i, cur);
        }
        if (sel_idx >= 0) draw_box((size_t)sel_idx, anim[s.tasks[(size_t)sel_idx].id]);

        /* a left-click on empty canvas (no box hovered) contracts the selection */
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ctx.selected_task.clear();

        /* reserve the drawn extent (+ expanded-box headroom) so the child's scrollbars match the content. */
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2((float)(cols - 1) * (boxW + gapX) + expW + 4.0f,
                            (float)(rows - 1) * (boxH + gapY) + boxH + 4.0f));
        ImGui::EndChild();
    }
    ImGui::End();
}

/* The activity timeline (P10): per-agent swimlanes of spans over time (a task's Running window, a
 * crash/reassign). Time runs left→right [earliest .. now_ms]; each span is a near-black bar + 1px border
 * (accent if still open, the one dim red for a crash, muted else); click selects the task. Drawn with
 * ImDrawList — structure, not colour. */
void draw_timeline_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Activity", open)) {
        if (s.timeline.empty()) {
            ImGui::TextColored(muted_v4(), "no activity yet — run an agenda to see the fleet over time");
            ImGui::End();
            return;
        }
        /* Own scroll region (see draw_dag_panel): the swimlane canvas scrolls in its own clean child rect
         * so its 1px borders/labels stay aligned to the lanes while scrolling, not out-of-sync. */
        ImGui::BeginChild("activity_canvas", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        /* lanes = agents (in order), plus any span lane not in the fleet; id -> row */
        std::unordered_map<std::string, int> lane_row;
        int                                  lanes = 0;
        for (const auto &a : s.agents)
            if (lane_row.find(a.id) == lane_row.end()) lane_row[a.id] = lanes++;
        for (const auto &sp : s.timeline)
            if (!sp.lane.empty() && lane_row.find(sp.lane) == lane_row.end()) lane_row[sp.lane] = lanes++;

        uint64_t tmin = s.now_ms, tmax = s.now_ms;
        for (const auto &sp : s.timeline) {
            if (sp.start_ms < tmin) tmin = sp.start_ms;
            uint64_t e = sp.end_ms ? sp.end_ms : s.now_ms;
            if (e > tmax) tmax = e;
        }
        if (tmax <= tmin) tmax = tmin + 1; /* guard div-by-zero when everything is instantaneous */

        const float    gutter = 84.0f, laneH = 24.0f, pad = 3.0f, axisH = 16.0f;
        const ImVec2   origin = ImGui::GetCursorScreenPos();
        const float    top = origin.y + axisH; /* lanes start below the time axis row */
        const float    width = std::max(40.0f, ImGui::GetContentRegionAvail().x - gutter - 8.0f);
        const float    lanesH = (float)lanes * laneH;
        ImDrawList    *dl = ImGui::GetWindowDrawList();
        auto           px = [&](uint64_t t) {
            return origin.x + gutter + (float)(t - tmin) / (float)(tmax - tmin) * width;
        };
        /* the bar fill/edge colour for a span's state — restrained: the single accent for live work, a muted
         * grey for a settled span, the one err tint for a crash. The fill is a translucent wash of the edge
         * hue so a bar reads as FILLED (not a hollow outline) yet stays quiet on the near-black panel. */
        auto state_v4 = [&](const TimelineSpan &sp) -> ImVec4 {
            if (sp.end_ms == 0) return accent_v4();                       /* open / running */
            if (sp.kind == TimelineSpan::Kind::Crash) return err_v4();    /* crash          */
            return muted_v4();                                           /* done           */
        };

        /* time axis: evenly-spaced ticks (faint gridlines spanning the lanes) + a relative-second label. */
        const int kTicks = 5;
        for (int k = 0; k <= kTicks; k++) {
            float    fx = origin.x + gutter + (float)k / (float)kTicks * width;
            uint64_t t = tmin + (uint64_t)((float)k / (float)kTicks * (float)(tmax - tmin));
            dl->AddLine(ImVec2(fx, top), ImVec2(fx, top + lanesH),
                        ImGui::GetColorU32(ImGuiCol_Separator, 0.30f), 1.0f);
            char lab[24];
            std::snprintf(lab, sizeof lab, "%.1fs", (float)(t - tmin) / 1000.0f);
            dl->AddText(ImVec2(fx + 2, origin.y), ImGui::GetColorU32(muted_v4()), lab);
        }

        /* lanes: an alternating band so the swimlanes read, the agent id in the gutter (vertically centred). */
        const float textY = (laneH - ImGui::GetTextLineHeight()) * 0.5f;
        for (const auto &kv : lane_row) {
            float y = top + (float)kv.second * laneH;
            if (kv.second % 2 == 1)
                dl->AddRectFilled(ImVec2(origin.x + gutter, y), ImVec2(origin.x + gutter + width, y + laneH),
                                  ImGui::GetColorU32(ImGuiCol_FrameBg, 0.45f));
            dl->AddText(ImVec2(origin.x, y + textY), ImGui::GetColorU32(muted_v4()), kv.first.c_str());
        }

        /* spans */
        for (size_t i = 0; i < s.timeline.size(); i++) {
            const TimelineSpan &sp = s.timeline[i];
            auto                lr = lane_row.find(sp.lane);
            if (lr == lane_row.end()) continue;
            float        y = top + (float)lr->second * laneH + pad;
            /* end >= start always (guards an unsigned underflow in px() if a span ever carries
             * end_ms < start_ms — the renderer must not rely on the collector's invariant) */
            uint64_t     e = (sp.end_ms && sp.end_ms >= sp.start_ms) ? sp.end_ms : s.now_ms;
            float        x0 = px(sp.start_ms);
            float        x1 = px(e);
            if (x1 < x0 + 4.0f) x1 = x0 + 4.0f; /* a minimum visible width */
            const ImVec2  a(x0, y), b(x1, y + laneH - 2 * pad);
            const bool    is_open = (sp.end_ms == 0);
            const ImVec4  cv = state_v4(sp);
            const ImU32   fill = ImGui::ColorConvertFloat4ToU32(ImVec4(cv.x, cv.y, cv.z, is_open ? 0.55f : 0.38f));
            const ImU32   edge = ImGui::ColorConvertFloat4ToU32(ImVec4(cv.x, cv.y, cv.z, 1.0f));
            dl->AddRectFilled(a, b, fill);            /* FLAT, sharp corners (restrained-corporate) */
            dl->AddRect(a, b, edge, 0.0f, 0, 1.0f);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID((int)i);
            if (ImGui::InvisibleButton("##span", ImVec2(x1 - x0, laneH - 2 * pad)) && !sp.task_id.empty()) {
                ctx.selected_task = sp.task_id;
                ctx.show.task = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sp.label.c_str()); /* untrusted -> "%s" */
            ImGui::PopID();
            if (x1 - x0 > 24.0f) { /* a label, CLIPPED to the bar so it never spills into another lane */
                dl->PushClipRect(ImVec2(x0 + 2, a.y), ImVec2(x1 - 2, b.y), true);
                dl->AddText(ImVec2(x0 + 4, y + (laneH - 2 * pad - ImGui::GetTextLineHeight()) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), sp.label.c_str());
                dl->PopClipRect();
            }
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(gutter + width, axisH + lanesH + 4.0f));
        ImGui::EndChild();
    }
    ImGui::End();
}

void draw_tasks_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Agenda", open)) {
        PanelHeader(s.agenda_title.empty() ? "AGENDA" : s.agenda_title.c_str());
        ImGui::Text("%d%%", s.agenda_progress);
        ImGui::SameLine();
        ImGui::ProgressBar(s.agenda_progress / 100.0f, ImVec2(-1.0f, 0.0f), "");
        ImGui::Spacing();
        if (s.tasks.empty()) {
            ImGui::TextColored(muted_v4(), "no tasks — build an agenda below");
        } else if (ImGui::BeginTable("tasks", 3,
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("task", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.25f);
            ImGui::TableSetupColumn("by", ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableHeadersRow();
            for (const auto &t : s.tasks) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool sel = (ctx.selected_task == t.id);
                ImGui::PushID(t.id.c_str()); /* the row id is t.id, so ellipsizing the label can't collide */
                const float title_avail = ImGui::GetContentRegionAvail().x;
                if (ImGui::Selectable(fit_ellipsis(t.title, title_avail).c_str(), sel,
                                      ImGuiSelectableFlags_SpanAllColumns)) { /* O1: ellipsize the title */
                    ctx.selected_task = t.id;
                    ctx.show.task = true;
                }
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(t.title.c_str()).x > title_avail)
                    ImGui::SetTooltip("%s", t.title.c_str());
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextColored(state_color(t.state), "%s", t.state.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "%s", t.assignee.empty() ? "-" : t.assignee.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

/* The agenda-builder form: title/goal + a task adder + the draft table + Run/Clear. One cohesive form
 * (~120 LOC) — split reviewed and declined: the parts share the in-progress task fields + the commit_task
 * lambda, so separating them would only add coupling. The InputText buffers are function-static (ImGui's
 * idiomatic in-place widget state); single-instance + UI-thread-only, so the shared storage is safe. */
void draw_agenda_builder_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ctx.focus_builder) {
        ImGui::SetNextWindowFocus();
        ctx.focus_builder = false;
    }
    if (ImGui::Begin("Agenda Builder", open)) {
        static char               title[128] = "";
        static char               goal[512] = "";
        static char               ttitle[128] = "";
        static char               tdesc[256] = "";
        static char               tdeps[64] = "";
        static char               tartifact[256] = "";
        static int                tcap = 0;
        static bool               tverify = false;
        static int                tverifiers = 1;
        static int                tquorum = 1;

        /* The capability choices: the LIVE fleet's roles (s.roles), or the built-in defaults when offline /
         * the fleet is empty. Built each frame so the combo tracks a fleet that gains or loses roles. */
        static const char *const kFallbackCaps[] = {"dev", "qa", "research", "ops"};
        std::vector<std::string> caps;
        if (!s.roles.empty()) caps = s.roles;
        else
            for (const char *c : kFallbackCaps) caps.emplace_back(c);
        if (tcap < 0 || tcap >= (int)caps.size()) tcap = 0;

        /* Commit the in-progress task fields into the draft list (true if it added one). Shared by
         * "+ Add task", Enter in the title field, AND Run Agenda — so a single typed task + Run works
         * without an explicit Add (the forgiving flow that fixes "Run does nothing"). */
        auto commit_task = [&]() -> bool {
            if (ttitle[0] == '\0') return false;
            TaskSpec ts;
            ts.id = "t" + std::to_string(ctx.draft_tasks.size() + 1);
            ts.title = ttitle;
            ts.capability = caps[tcap];
            ts.description = tdesc;
            ts.deps = parse_deps(tdeps);
            ts.artifact_path = tartifact;
            ts.verify = tverify;
            ts.verifiers = tverifiers;
            ts.quorum = tquorum;
            ctx.draft_tasks.push_back(std::move(ts));
            ttitle[0] = tdesc[0] = tdeps[0] = tartifact[0] = '\0';
            tverify = false; /* reset the per-task verify toggle for the next task */
            tverifiers = tquorum = 1;
            return true;
        };

        ImGui::TextColored(muted_v4(), "agenda title");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##title", "e.g. release v0.1 (optional)", title, sizeof title);
        ImGui::TextColored(muted_v4(), "goal (planned into tasks if you add none below)");
        ImGui::InputTextMultiline("##goal", goal, sizeof goal, ImVec2(-1.0f, 40.0f));

        ImGui::Spacing();
        PanelHeader("ADD TASKS");
        ImGui::SetNextItemWidth(-1.0f);
        bool enter = ImGui::InputTextWithHint("##ttitle", "task title (Enter to add)", ttitle,
                                              sizeof ttitle, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##tcap", caps[tcap].c_str())) {
            for (int i = 0; i < (int)caps.size(); i++) {
                bool sel = (i == tcap);
                if (ImGui::Selectable(caps[i].c_str(), sel)) tcap = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##tdeps", "deps e.g. t1,t2 (optional)", tdeps, sizeof tdeps);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##tdesc", "description (optional)", tdesc, sizeof tdesc);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##tartifact", "deliverable file e.g. src/main.c (optional)", tartifact,
                                 sizeof tartifact);
        ImGui::Checkbox("verify", &tverify);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("require an independent sibling worker to check this task before it is accepted");
        if (tverify) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("verifiers", &tverifiers) && tverifiers < 1) tverifiers = 1;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("quorum", &tquorum) && tquorum < 1) tquorum = 1;
        }
        if (ImGui::Button("+ Add task") || enter) commit_task();

        ImGui::Spacing();
        if (ctx.draft_tasks.empty()) {
            ImGui::TextColored(muted_v4(),
                               "no tasks — add tasks above, or just give a goal and Run to let HyperCat plan it");
        } else if (ImGui::BeginTable("draft", 4,
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("cap", ImGuiTableColumnFlags_WidthStretch, 0.26f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
            ImGui::TableHeadersRow();
            int remove = -1;
            for (size_t i = 0; i < ctx.draft_tasks.size(); i++) {
                const TaskSpec &ts = ctx.draft_tasks[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(accent_v4(), "%s", ts.id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ts.title.c_str());
                if ((!ts.deps.empty() || !ts.artifact_path.empty()) && ImGui::IsItemHovered()) {
                    std::string tip;
                    if (!ts.deps.empty()) {
                        tip = "after: ";
                        for (const auto &x : ts.deps) tip += x + " ";
                    }
                    if (!ts.artifact_path.empty())
                        tip += (tip.empty() ? "" : "\n") + std::string("file: ") + ts.artifact_path;
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "%s", ts.capability.c_str());
                if (ts.verify) {
                    ImGui::SameLine();
                    ImGui::TextColored(accent_v4(), "· verify");
                }
                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("x")) remove = (int)i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove >= 0) ctx.draft_tasks.erase(ctx.draft_tasks.begin() + remove);
        }

        ImGui::Spacing();
        /* Forgiving Run that fixes "a goal does barely anything in the UI": it runs with EITHER explicit
         * tasks OR just a goal. With explicit tasks it runs them as given (a goal then rides along as
         * context); with ONLY a goal it sends a GOAL-ONLY agenda, which the live planner decomposes into a
         * multi-task DAG — the same fan-out the CLI/--agenda goal-only path gets, finally reachable from the
         * UI (the orchestrator only decomposes when agenda.tasks is empty). It still folds in a
         * typed-but-unadded task so the natural type-then-Run flow works without an explicit "+ Add". */
        bool goal_only = ctx.draft_tasks.empty() && ttitle[0] == '\0' && goal[0] != '\0';
        bool has_run = !ctx.draft_tasks.empty() || ttitle[0] != '\0' || goal[0] != '\0';
        ImGui::BeginDisabled(!has_run);
        if (ImGui::Button(goal_only ? "Plan & Run Goal" : "Run Agenda")) {
            commit_task(); /* fold in a typed-but-unadded task */
            if (!ctx.draft_tasks.empty() || goal[0] != '\0') {
                UiCommand c;
                c.kind = UiCommand::Kind::CreateAgenda;
                c.a = title[0] ? title : "agenda";
                c.b = goal;
                c.tasks = ctx.draft_tasks; /* empty => the live planner decomposes the goal into tasks */
                ctx.commands.push_back(std::move(c));
                ctx.draft_tasks.clear();
                title[0] = goal[0] = '\0';
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ctx.draft_tasks.empty() && ttitle[0] == '\0' && goal[0] == '\0');
        if (ImGui::Button("Clear")) {
            ctx.draft_tasks.clear();
            ttitle[0] = tdesc[0] = tdeps[0] = '\0';
            title[0] = goal[0] = '\0';
        }
        ImGui::EndDisabled();
        if (goal_only && s.provider == "offline") {
            ImGui::SameLine();
            ImGui::TextColored(muted_v4(), "offline — set a model to plan a goal into tasks");
        } else if (!has_run) {
            ImGui::SameLine();
            ImGui::TextColored(muted_v4(), "add a task or a goal to run");
        }
    }
    ImGui::End();
}

void draw_task_detail_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (ImGui::Begin("Task", open)) {
        const TaskRow *sel = nullptr;
        for (const auto &t : s.tasks)
            if (t.id == ctx.selected_task) {
                sel = &t;
                break;
            }
        if (!sel) {
            ImGui::TextColored(muted_v4(), "select a task in the agenda board");
        } else {
            ImGui::Text("%s", sel->title.c_str());
            ImGui::SameLine();
            ImGui::TextColored(state_color(sel->state), "  %s", sel->state.c_str());
            /* Act-vs-narrate cue: a terminal task with a result but ZERO artifacts only TALKED — surfaced
             * so a narrated non-result (the model describing a deliverable instead of producing it) is
             * visible, not disguised as success. Factual, not judgmental (some tasks are legitimately text). */
            int n_art = 0;
            for (const auto &art : s.artifacts)
                if (art.task == sel->id) n_art++;
            if (sel->state == "done" || sel->state == "failed") {
                ImGui::SameLine();
                if (n_art > 0)
                    ImGui::TextColored(muted_v4(), "  ·  wrote %d file%s", n_art, n_art == 1 ? "" : "s");
                else if (!sel->result.empty())
                    ImGui::TextColored(muted_v4(), "  ·  text only (no files written)");
            }
            if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "id");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sel->id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(muted_v4(), "assignee");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sel->assignee.empty() ? "-" : sel->assignee.c_str());
                ImGui::EndTable();
            }
            ImGui::Spacing();
            PanelHeader("DESCRIPTION");
            WrappedText("%s", sel->description.c_str()); /* untrusted -> "%s" */
            if (!sel->result.empty()) {
                ImGui::Spacing();
                PanelHeader("RESULT");
                WrappedText("%s", sel->result.c_str());
            }
            /* P02: the content-addressed artifacts this task produced (approved fs_writes) + provenance. */
            ImGui::Spacing();
            PanelHeader("ARTIFACTS");
            if (ImGui::BeginTable("artifacts", 3,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("meta", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                for (const auto &art : s.artifacts) {
                    if (art.task != sel->id) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(accent_v4(), "%.12s", art.id.c_str()); /* short content id */
                    ImGui::TableNextColumn();
                    EllipsisText(art.label); /* O2: untrusted write path — ellipsize + tooltip, never overflow */
                    ImGui::TableNextColumn();
                    ImGui::TextColored(muted_v4(), "%ld B  %s", art.size, art.created.c_str());
                }
                ImGui::EndTable();
            }
            if (!n_art) ImGui::TextColored(muted_v4(), "none yet (approved fs_writes appear here)");
        }
    }
    ImGui::End();
}

} // namespace hc::ui
