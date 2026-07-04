/* test_ui_sprite — the pure, offline gate for the WI-5 sprite engine + mascot classifier. Exercises the
 * math/state surface that needs no GL or ImGui context: sprite_uv (grid -> UVs), Animation construction,
 * the SpritePlayer state machine (loop modes, seek, speed, stills), and mascot_state_for precedence.
 * The texture upload + draw paths are covered by the headless ui_smoke screenshot, not here. */

#include "ui_conductor_chat.hpp"
#include "ui_mascot.hpp"
#include "ui_sprite.hpp"
#include "ui_sprite_registry.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace hc::ui;

static int g_fail = 0;
#define CHECK(cond)                                                                                        \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                    \
            g_fail++;                                                                                       \
        }                                                                                                  \
    } while (0)

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void test_sprite_uv()
{
    /* 1×1 sheet -> the whole texture. */
    FrameUV a = sprite_uv(128, 128, 1, 1, 128, 128, 0, 0, 0);
    CHECK(approx(a.uv0.x, 0.0f) && approx(a.uv0.y, 0.0f));
    CHECK(approx(a.uv1.x, 1.0f) && approx(a.uv1.y, 1.0f));

    /* 6×1 horizontal strip (768×128, frame 128) — the mascot "working" sheet. */
    FrameUV f0 = sprite_uv(768, 128, 6, 1, 128, 128, 0, 0, 0);
    CHECK(approx(f0.uv0.x, 0.0f) && approx(f0.uv1.x, 128.0f / 768.0f));
    CHECK(approx(f0.uv0.y, 0.0f) && approx(f0.uv1.y, 1.0f));
    FrameUV f3 = sprite_uv(768, 128, 6, 1, 128, 128, 0, 0, 3);
    CHECK(approx(f3.uv0.x, 384.0f / 768.0f) && approx(f3.uv1.x, 512.0f / 768.0f));
    FrameUV f5 = sprite_uv(768, 128, 6, 1, 128, 128, 0, 0, 5);
    CHECK(approx(f5.uv0.x, 640.0f / 768.0f) && approx(f5.uv1.x, 1.0f));

    /* Clamping: out-of-range high -> last frame; negative -> first frame. */
    FrameUV fhi = sprite_uv(768, 128, 6, 1, 128, 128, 0, 0, 99);
    CHECK(approx(fhi.uv0.x, f5.uv0.x) && approx(fhi.uv1.x, f5.uv1.x));
    FrameUV flo = sprite_uv(768, 128, 6, 1, 128, 128, 0, 0, -5);
    CHECK(approx(flo.uv0.x, f0.uv0.x) && approx(flo.uv1.x, f0.uv1.x));

    /* Row-major addressing on a 2D grid (4×2 of 32×32 in a 128×64 sheet): frame 5 = col 1, row 1. */
    FrameUV g = sprite_uv(128, 64, 4, 2, 32, 32, 0, 0, 5);
    CHECK(approx(g.uv0.x, 32.0f / 128.0f) && approx(g.uv0.y, 32.0f / 64.0f));
    CHECK(approx(g.uv1.x, 64.0f / 128.0f) && approx(g.uv1.y, 64.0f / 64.0f));

    /* margin + spacing: a 2×1 of 10px frames, 1px margin, 2px gap -> tex width 1+10+2+10+1 = 24.
     * Frame 1 starts at x = 1 + 1*(10+2) = 13. */
    FrameUV m = sprite_uv(24, 12, 2, 1, 10, 10, 1, 2, 1);
    CHECK(approx(m.uv0.x, 13.0f / 24.0f) && approx(m.uv1.x, 23.0f / 24.0f));

    /* Degenerate sheet -> full rect, no divide-by-zero. */
    FrameUV d = sprite_uv(0, 0, 1, 1, 0, 0, 0, 0, 0);
    CHECK(approx(d.uv0.x, 0.0f) && approx(d.uv1.x, 1.0f));
}

static void test_animation()
{
    Animation s = Animation::strip(6, 10.0f); /* 10 fps -> 100ms/frame */
    CHECK(s.steps.size() == 6);
    CHECK(approx(s.total_ms(), 600.0f));
    CHECK(s.steps[0].frame == 0 && s.steps[5].frame == 5);
    CHECK(s.loop == LoopMode::Loop);

    Animation still = Animation::still(2);
    CHECK(still.steps.size() == 1);
    CHECK(still.steps[0].frame == 2);
    CHECK(approx(still.total_ms(), 0.0f)); /* held -> zero total */

    Animation empty = Animation::strip(0, 10.0f);
    CHECK(empty.empty());
}

static void test_player_loop()
{
    SpritePlayer p;
    p.set_animation(Animation::strip(4, 10.0f, LoopMode::Loop)); /* 100ms/frame, total 400ms */
    CHECK(p.current_frame() == 0);
    p.advance(50.0f); /* t=50 -> step 0 */
    CHECK(p.current_frame() == 0);
    p.advance(100.0f); /* t=150 -> step 1 */
    CHECK(p.current_frame() == 1);
    p.advance(200.0f); /* t=350 -> step 3 */
    CHECK(p.current_frame() == 3);
    p.advance(100.0f); /* t=450 -> wraps to 50 -> step 0 */
    CHECK(p.current_frame() == 0);
}

static void test_player_once()
{
    SpritePlayer p;
    p.set_animation(Animation::strip(3, 10.0f, LoopMode::Once)); /* total 300ms */
    p.advance(1000.0f);                                          /* well past the end */
    CHECK(p.current_frame() == 2);                              /* holds the final frame */
    /* Once stops playing at the end — a further advance does nothing. */
    p.advance(1000.0f);
    CHECK(p.current_frame() == 2);
}

static void test_player_pingpong()
{
    SpritePlayer p;
    p.set_animation(Animation::strip(3, 10.0f, LoopMode::PingPong)); /* frames 0,1,2 @100ms, total 300 */
    p.advance(50.0f);              /* t=50 -> 0 */
    CHECK(p.current_frame() == 0);
    p.advance(200.0f);             /* t=250 -> 2 */
    CHECK(p.current_frame() == 2);
    p.advance(100.0f);             /* t=350 -> mirror: eff=600-350=250 -> 2 */
    CHECK(p.current_frame() == 2);
    p.advance(150.0f);             /* t=500 -> mirror: eff=600-500=100 -> 1 */
    CHECK(p.current_frame() == 1);
    p.advance(150.0f);             /* t=650 -> fold to 50 -> 0 (forward again) */
    CHECK(p.current_frame() == 0);
}

static void test_player_controls()
{
    SpritePlayer p;
    p.set_animation(Animation::strip(5, 10.0f, LoopMode::Loop));

    p.seek_step(3);
    CHECK(p.current_frame() == 3);
    p.seek_time(0.0f);
    CHECK(p.current_frame() == 0);
    p.seek_time(250.0f); /* step 2 */
    CHECK(p.current_frame() == 2);

    /* speed 0 freezes the playhead. */
    p.seek_step(1);
    p.set_speed(0.0f);
    p.advance(10000.0f);
    CHECK(p.current_frame() == 1);

    /* pause freezes; play resumes. */
    p.set_speed(1.0f);
    p.pause();
    p.advance(1000.0f);
    CHECK(p.current_frame() == 1);

    /* stop -> back to t=0. (the playhead is at t=100 from seek_step(1) above; +250 -> t=350 -> step 3) */
    p.play();
    p.advance(250.0f);
    CHECK(p.current_frame() == 3);
    p.stop();
    CHECK(p.current_frame() == 0);

    /* A still ignores advance entirely. */
    SpritePlayer h;
    h.set_animation(Animation::still(4));
    h.advance(99999.0f);
    CHECK(h.current_frame() == 4);
}

static void test_mascot_state()
{
    UiSnapshot s;
    CHECK(mascot_state_for(s) == MascotState::Idle); /* empty fleet */

    s.tasks.push_back(TaskRow{"t1", "do", "running", "A", "", "", {}});
    CHECK(mascot_state_for(s) == MascotState::Working);

    s.tasks.push_back(TaskRow{"t2", "do", "assigned", "B", "", "", {}});
    CHECK(mascot_state_for(s) == MascotState::Working);

    /* A failed task takes precedence over a running one. */
    s.tasks.push_back(TaskRow{"t3", "do", "failed", "C", "", "err", {}});
    CHECK(mascot_state_for(s) == MascotState::Error);

    /* A dead agent alone -> Error. */
    UiSnapshot d;
    d.agents.push_back(AgentRow{"A", "dev", "dead"});
    CHECK(mascot_state_for(d) == MascotState::Error);

    /* Ready agents with no active tasks -> Idle. */
    UiSnapshot r;
    r.agents.push_back(AgentRow{"A", "dev", "ready"});
    r.tasks.push_back(TaskRow{"t1", "do", "done", "A", "", "ok", {}});
    CHECK(mascot_state_for(r) == MascotState::Idle);
}

static void test_chat_avatar()
{
    ChatAvatarIn prev; /* all default: offline, not busy, no tool, 0 msgs */

    /* at rest -> Base. */
    CHECK(chat_avatar_for(prev, ChatAvatarIn{}) == ChatAvatar::Base);

    /* run_agenda dominates -> Dispatch (even while busy). */
    ChatAvatarIn disp;
    disp.active_tool = "run_agenda";
    disp.busy = true;
    CHECK(chat_avatar_for(prev, disp) == ChatAvatar::Dispatch);

    /* busy or streaming (a turn in flight) -> Received. */
    ChatAvatarIn think;
    think.busy = true;
    CHECK(chat_avatar_for(prev, think) == ChatAvatar::Received);
    ChatAvatarIn stream;
    stream.streaming = true;
    CHECK(chat_avatar_for(prev, stream) == ChatAvatar::Received);

    /* a reply just settled (the assistant count rose, and not busy) -> Sent. */
    ChatAvatarIn before;
    before.assistant_msgs = 2;
    ChatAvatarIn after;
    after.assistant_msgs = 3;
    CHECK(chat_avatar_for(before, after) == ChatAvatar::Sent);
    /* the same count, idle -> Base (the edge is gone). */
    CHECK(chat_avatar_for(after, after) == ChatAvatar::Base);
    /* precedence: a turn in flight beats the fresh-reply edge. */
    ChatAvatarIn after_busy = after;
    after_busy.busy = true;
    CHECK(chat_avatar_for(before, after_busy) == ChatAvatar::Received);

    /* the sheet keys (the manifest names). */
    CHECK(std::string(chat_avatar_sheet(ChatAvatar::Base)) == "conductor.base");
    CHECK(std::string(chat_avatar_sheet(ChatAvatar::Received)) == "conductor.messagereceived");
    CHECK(std::string(chat_avatar_sheet(ChatAvatar::Sent)) == "conductor.messagesent");
    CHECK(std::string(chat_avatar_sheet(ChatAvatar::Dispatch)) == "conductor.taskagendadispatch");
}

static void test_chat_tool_cue()
{
    CHECK(chat_tool_cue_for("") == ToolCue::None); /* no tool -> hidden */
    CHECK(chat_tool_cue_for("recall_memory") == ToolCue::Read);
    CHECK(chat_tool_cue_for("agenda_status") == ToolCue::Read);
    CHECK(chat_tool_cue_for("plan_goal") == ToolCue::Read);
    CHECK(chat_tool_cue_for("deep_reason") == ToolCue::Read);
    CHECK(chat_tool_cue_for("write_memory") == ToolCue::Write);
    /* everything else -> Generic (the placeholder for a tool without dedicated art). */
    CHECK(chat_tool_cue_for("run_agenda") == ToolCue::Generic);
    CHECK(chat_tool_cue_for("set_goal") == ToolCue::Generic);
    CHECK(chat_tool_cue_for("ask_user") == ToolCue::Generic);
    CHECK(chat_tool_cue_for("totally_unknown_tool") == ToolCue::Generic);

    CHECK(chat_tool_sheet(ToolCue::None) == nullptr); /* nothing to draw */
    CHECK(std::string(chat_tool_sheet(ToolCue::Read)) == "conductor.tool.read");
    CHECK(std::string(chat_tool_sheet(ToolCue::Write)) == "conductor.tool.write");
    CHECK(std::string(chat_tool_sheet(ToolCue::Generic)) == "conductor.tool.generic");
}

static void test_registry()
{
    /* The registry registers the catalog's named Animations in its CTOR (pure — no GL), so animation() is
     * valid offline. The sheets need a live ImGui context (ensure_loaded), so sheet() is nullptr here and the
     * decode/upload/draw/teardown path is covered by the headless ui_smoke. */
    SpriteRegistry r;
    CHECK(!r.loaded()); /* nothing decoded yet */

    /* mascot.think -> a 6-frame strip, looped (the manifest: 6 cols, 9 fps, loop). */
    Animation think = r.animation("mascot.think");
    CHECK(think.steps.size() == 6);
    CHECK(think.loop == LoopMode::Loop);

    /* mascot.face -> a single still (the manifest: 1 frame, fps 0). */
    Animation face = r.animation("mascot.face");
    CHECK(face.steps.size() == 1);
    CHECK(approx(face.total_ms(), 0.0f));

    /* An unknown name -> a safe still, never a crash. */
    Animation none = r.animation("does.not.exist");
    CHECK(none.steps.size() == 1);

    /* The conductor S2 sheets resolve to the right default animations (the manifest grid/fps/loop). */
    Animation cbase = r.animation("conductor.base");
    CHECK(cbase.steps.size() == 1); /* a still — 1 frame, fps 0 */
    Animation csent = r.animation("conductor.messagesent");
    CHECK(csent.steps.size() == 10 && csent.loop == LoopMode::Once); /* the one-shot flourish */
    Animation cdisp = r.animation("conductor.taskagendadispatch");
    CHECK(cdisp.steps.size() == 10 && cdisp.loop == LoopMode::Loop);
    Animation cread = r.animation("conductor.tool.read");
    CHECK(cread.steps.size() == 10 && cread.loop == LoopMode::Loop);

    /* sheet() before ensure_loaded (and for any unknown name) is nullptr -> consumers fall back to text. */
    CHECK(r.sheet("mascot.think") == nullptr);
    CHECK(r.sheet("conductor.base") == nullptr);
    CHECK(r.sheet("does.not.exist") == nullptr);
}

int main()
{
    test_sprite_uv();
    test_animation();
    test_player_loop();
    test_player_once();
    test_player_pingpong();
    test_player_controls();
    test_mascot_state();
    test_chat_avatar();
    test_chat_tool_cue();
    test_registry();
    if (g_fail) {
        std::printf("test_ui_sprite: %d CHECK(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_ui_sprite: all checks passed\n");
    return 0;
}
