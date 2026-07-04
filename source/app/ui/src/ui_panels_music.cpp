/* ui_panels_music — the Music Player panel (audio feature, Phase C).
 *
 * Purpose:   render now-playing + an in-theme spectrum + transport + a seek scrubber + volume + the library list,
 *            and EMIT the Audio* UiCommands the host routes to the global hc_audio_player.
 * Owns:      nothing — a PURE RENDERER over UiSnapshot.audio_*; the engine, the jail, and ALL file I/O are
 *            host-side (this never touches the filesystem). Untrusted tag text renders via "%s" only.
 * Threading: UI thread only; the function-static seek-drag state is the established single-instance panel idiom.
 */

#include "ui_panels.hpp"
#include "ui_theme.hpp"

#include "hc_ui.hpp"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace hc::ui {

namespace {
const char *fmt_time(long ms, char *buf, size_t n)
{
    if (ms < 0) ms = 0;
    long s = ms / 1000;
    std::snprintf(buf, n, "%ld:%02ld", s / 60, s % 60);
    return buf;
}
} // namespace

void draw_music_player_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Music Player", open)) {
        ImGui::End();
        return;
    }

    if (!s.audio_present) {
        ImGui::TextColored(muted_v4(), "no audio backend on this host — playback unavailable");
        ImGui::End();
        return;
    }

    /* --- now playing (untrusted title/artist via "%s") --- */
    PanelHeader("NOW PLAYING");
    if (s.audio_state == 0 && s.audio_title.empty()) {
        ImGui::TextColored(muted_v4(), "nothing playing — double-click a track below");
    } else {
        ImGui::TextUnformatted(s.audio_title.empty() ? "(untitled)" : s.audio_title.c_str());
        if (!s.audio_artist.empty()) ImGui::TextColored(muted_v4(), "%s", s.audio_artist.c_str());
    }

    /* --- spectrum analyser: restrained accent bars (translucent fill + 1px edge) --- */
    if (s.audio_spectrum_on && !s.audio_spectrum.empty()) {
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        const float  h = 56.0f;
        const int    n = (int)s.audio_spectrum.size();
        const float  bw = n > 0 ? w / (float)n : w;
        const ImVec4 ac = accent_v4();
        const ImU32  fill = ImGui::ColorConvertFloat4ToU32(ImVec4(ac.x, ac.y, ac.z, 0.50f));
        const ImU32  edge = ImGui::ColorConvertFloat4ToU32(ImVec4(ac.x, ac.y, ac.z, 0.95f));
        for (int i = 0; i < n; i++) {
            float v = s.audio_spectrum[(size_t)i];
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            ImVec2 a(p0.x + (float)i * bw + 1.0f, p0.y + h - v * h);
            ImVec2 b(p0.x + (float)(i + 1) * bw - 1.0f, p0.y + h);
            if (b.x > a.x) {
                dl->AddRectFilled(a, b, fill);
                dl->AddRect(a, b, edge, 0.0f, 0, 1.0f);
            }
        }
        ImGui::Dummy(ImVec2(w, h));
    }

    /* --- seek scrubber (shows the live position; drag to seek, emit on release) --- */
    {
        char tb[16], tb2[16];
        long pos = s.audio_pos_ms, dur = s.audio_dur_ms;
        ImGui::TextColored(muted_v4(), "%s", fmt_time(pos, tb, sizeof tb));
        ImGui::SameLine();
        const char *dtxt = fmt_time(dur, tb2, sizeof tb2);
        float       rightx = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(dtxt).x;
        if (ImGui::GetCursorPosX() < rightx) ImGui::SetCursorPosX(rightx);
        ImGui::TextColored(muted_v4(), "%s", dtxt);

        static int held = -1; /* -1 = following the live position; >=0 = the value being dragged */
        int        maxv = dur > 0 ? (int)dur : 1;
        int        v = (held >= 0) ? held : (int)pos;
        if (v > maxv) v = maxv;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##seek", &v, 0, maxv, "");
        if (ImGui::IsItemActive()) held = v;
        else if (held >= 0 && ImGui::IsItemDeactivatedAfterEdit()) {
            ctx.commands.push_back({UiCommand::Kind::AudioSeek, "", "", v, {}});
            held = -1;
        } else if (held >= 0 && !ImGui::IsItemActive())
            held = -1; /* released without an edit (value unchanged) — discard the drag, emit nothing */
    }

    /* --- transport (text buttons — restrained, render in JetBrains Mono) --- */
    {
        bool playing = (s.audio_state == 1);
        if (ImGui::Button("Prev")) ctx.commands.push_back({UiCommand::Kind::AudioPrev, "", "", 0, {}});
        ImGui::SameLine();
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            if (playing) ctx.commands.push_back({UiCommand::Kind::AudioPause, "", "", 0, {}});
            else ctx.commands.push_back({UiCommand::Kind::AudioPlay, "", "", 0, {}}); /* resume the loaded track */
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) ctx.commands.push_back({UiCommand::Kind::AudioStop, "", "", 0, {}});
        ImGui::SameLine();
        if (ImGui::Button("Next")) ctx.commands.push_back({UiCommand::Kind::AudioNext, "", "", 0, {}});
    }

    /* --- volume --- DELIBERATELY emits LIVE on each change (audible feedback while dragging), UNLIKE the seek
     * scrubber which holds + emits once: a re-seek is expensive (stop + decode-reseek) whereas a volume change is
     * just AL_GAIN, and the live emits are bounded to <=100 per drag (one per integer step). The settings FILE
     * write is still release-gated (b == "save") so there is no disk thrash. */
    {
        int vol = s.audio_volume;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt("vol", &vol, 0, 100, "%d%%"))
            ctx.commands.push_back({UiCommand::Kind::AudioVolume, "", "", vol, {}});
        if (ImGui::IsItemDeactivatedAfterEdit())
            ctx.commands.push_back({UiCommand::Kind::AudioVolume, "", "save", vol, {}});
    }

    /* --- toggles (live-owned audio settings; persisted on flip) --- */
    {
        bool spec = s.audio_spectrum_on;
        if (ImGui::Checkbox("spectrum", &spec))
            ctx.commands.push_back({UiCommand::Kind::AudioToggle, "spectrum", "", spec ? 1 : 0, {}});
        ImGui::SameLine();
        bool mood = s.audio_enabled;
        if (ImGui::Checkbox("conductor may set the mood", &mood))
            ctx.commands.push_back({UiCommand::Kind::AudioToggle, "mood", "", mood ? 1 : 0, {}});
    }

    ImGui::Separator();

    /* --- library (double-click a row to play) --- */
    PanelHeader("LIBRARY");
    if (s.audio_library.empty()) {
        ImGui::TextColored(muted_v4(), "empty \xE2\x80\x94 drop audio files into the audio/ folder");
    } else if (ImGui::BeginTable("audio_lib", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto &t : s.audio_library) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(t.name.c_str());
            const std::string &label = t.title.empty() ? t.name : t.title;
            if (ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0))
                    ctx.commands.push_back({UiCommand::Kind::AudioPlay, t.name, "", 0, {}}); /* 0 = library jail */
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextColored(muted_v4(), "%s", t.artist.c_str());
            ImGui::TableNextColumn();
            if (t.duration_ms > 0) {
                char tb[16];
                ImGui::TextColored(muted_v4(), "%s", fmt_time(t.duration_ms, tb, sizeof tb));
            } else {
                ImGui::TextColored(muted_v4(), "\xE2\x80\x94");
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace hc::ui
