/* Headless smoke for hc_ui: create a HIDDEN window (no visible window appears), render a few frames
 * of the dockspace + themed panels from a sample snapshot, and tear down cleanly. Proves the whole
 * GLFW + GL3 + ImGui + theme + panel path builds and runs. On a host with no display/GL, create()
 * returns null and the test SKIPS (passes) — the UI inherently needs a display. Exit non-zero only
 * on a genuine failure. */

#include "hc_ui.hpp"

#include "ui_panels.hpp" /* the pure dag_layout (P10) — tested headless below */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace hc::ui;

/* A valid 2x2 RGB PNG (PIL-verified; the same fixture libs/hc_image uses) — drives the Viewer's image path
 * (decode -> GPU texture -> ImGui::Image) and, opened under many distinct keys, the cache's evict/reap cycle. */
static const unsigned char k_png_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00,
    0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x38, 0xa1, 0xa1, 0x01, 0x44, 0x0c, 0x10,
    0x0a, 0x00, 0x21, 0x2e, 0x04, 0x61, 0xf6, 0xe1, 0xc2, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82};

/* Unit-test the pure layered DAG layout (no GL needed) — runs even on a headless host. */
static int test_dag_layout()
{
    auto mk = [](const char *id, std::vector<std::string> deps) {
        TaskRow t;
        t.id = id;
        t.deps = std::move(deps);
        return t;
    };
    /* a chain t1->t2->t3: strictly increasing depths, one per column */
    auto chain = dag_layout({mk("t1", {}), mk("t2", {"t1"}), mk("t3", {"t2"})});
    if (chain.size() != 3 || chain[0].depth != 0 || chain[1].depth != 1 || chain[2].depth != 2) {
        std::fprintf(stderr, "ui_smoke: dag_layout chain depths wrong\n");
        return 1;
    }
    /* a fan: t2 and t3 both depend on t1 -> same depth, distinct rows */
    auto fan = dag_layout({mk("t1", {}), mk("t2", {"t1"}), mk("t3", {"t1"})});
    if (fan[1].depth != 1 || fan[2].depth != 1 || fan[1].row == fan[2].row) {
        std::fprintf(stderr, "ui_smoke: dag_layout fan layout wrong\n");
        return 1;
    }
    /* a missing dep is not an edge: t2 deps a non-existent t9 -> depth 0 */
    auto miss = dag_layout({mk("t1", {}), mk("t2", {"t9"})});
    if (miss[1].depth != 0) {
        std::fprintf(stderr, "ui_smoke: dag_layout missing-dep should be depth 0\n");
        return 1;
    }
    /* a cycle t1<->t2 must terminate (cycle-tolerant) with depths bounded by the task count */
    auto cyc = dag_layout({mk("t1", {"t2"}), mk("t2", {"t1"})});
    if (cyc.size() != 2 || cyc[0].depth > 2 || cyc[1].depth > 2) {
        std::fprintf(stderr, "ui_smoke: dag_layout cycle not bounded\n");
        return 1;
    }
    return 0;
}

int main()
{
    if (test_dag_layout() != 0) return 1;

    UiApp *app = UiApp::create("HyperCat (smoke)", /*visible=*/false);
    if (!app) {
        std::printf("ui_smoke: skipped (no display/GL available)\n");
        return 0; /* headless host — skip, pass */
    }

    UiSnapshot s;
    s.agenda_title = "release";
    s.agenda_progress = 50;
    s.agents = {{"agent:A", "dev", "ready"}, {"agent:B", "qa", "ready"}, {"agent:C", "dev", "dead"}};
    s.tasks = {{"t1", "build A", "done", "agent:A", "compile module A", "ok: built", {}},
               {"t2", "verify A", "running", "agent:B", "run the test for A", "", {"t1"}},
               {"t3", "build B", "pending", "", "compile module B", "", {"t1"}}};
    s.logs = {"orchestrator: agenda started", "agent:C crashed; t? reassigned to a survivor",
              "t1 done"};
    s.provider = "openrouter";
    s.model = "google/gemini-3.5-flash";
    s.chat = {"agent:A: reversing the ", "agent:A: string now"}; /* live token-stream console */
    /* a pending tool-authorization request — exercises the approvals panel (untrusted summary) */
    s.pending_auth = {{"auth-1", "agent:A", "fs_write", "fs_write out.txt  (12 bytes)\nhello world!"}};
    s.reasoning = "agent:A:\n[decompose] ...\n[answer] 42\n[confidence] high"; /* deep_reason panel */
    /* file browser: a NESTED tree (depth 3) + a %3A-encoded dir + mixed kinds so the smoke run drives the
     * split explorer's tree build, the sortable contents table, the right-aligned size, and the Kind labels;
     * files_truncated exercises the truncation-note path. */
    s.files = {{"README.md", 1200, false},      {"agent%3AA", 0, true},
               {"agent%3AA/reverse.py", 412, false}, {"agent%3AA/portrait", 0, true},
               {"agent%3AA/portrait/icon.qoi", 8192, false}, {"agent%3AB", 0, true},
               {"agent%3AB/test_reverse.py", 88, false}};
    s.files_truncated = true;
    /* Music Player (audio feature): now-playing + a spectrum + a library so the smoke run drives the panel's
     * spectrum-draw + the sortable library table + the transport/seek/volume widgets under ASan/UBSan. */
    s.audio_present = true;
    s.audio_spectrum_on = true;
    s.audio_title = "Aero Vista";
    s.audio_artist = "HyperCat Sessions";
    s.audio_state = 1;
    s.audio_pos_ms = 83000;
    s.audio_dur_ms = 150047;
    s.audio_volume = 70;
    s.audio_spectrum.assign(48, 0.4f);
    s.audio_library = {{"Aero_Vista.mp3", "Aero Vista", "HyperCat Sessions", 150047, "MP3"},
                       {"calm_rain.ogg", "Calm Rain", "Ambient", 240000, "OGG"}};
    s.sessions = {{"s1", "reverse a string", "google/gemini-3.5-flash", "12:00", 1}}; /* session browser */
    s.viewed_session = "s1";
    /* a markdown-rich assistant message so the smoke run drives every WI-4 render path (heading, inline
     * bold/italic/code, fenced code block, bullets, quote, rule) under ASan/UBSan — incl. a literal %s. */
    s.transcript = {"user: reverse a string",
                    "assistant: # Result\n"
                    "Here is the **reversed** string via `s[::-1]` (a literal %s stays text).\n\n"
                    "```\n"
                    "def rev(s):\n"
                    "    return s[::-1]\n"
                    "```\n\n"
                    "- handles *unicode*\n"
                    "- O(n)\n\n"
                    "> note: it returns a copy\n\n"
                    "---\n"
                    "Done."};
    s.terminal = "hypercat$ pwd\n/home/op/workspace\nhypercat$ "; /* operator terminal output */
    /* B2: a couple of toasts so the headless run drives draw_toasts (the floating overlay + the clickable
     * ApprovalPending card + a Warning) under ASan/UBSan. */
    s.toasts = {{"auth-1", "agent:A wants fs_write \xE2\x80\x94 click to review", Toast::Kind::ApprovalPending},
                {"w-1", "ALLOW-ALL ARMED \xE2\x80\x94 every tool request is auto-approved", Toast::Kind::Warning}};
    /* the Reasoning panel now renders markdown (settled prose) — drive its heading/bullet/code paths under ASan. */
    s.reasoning = "agent:A:\n## Stage 1 — frame\n"
                  "The ask is to **reverse a string**; the crux is `s[::-1]` (a literal %s stays text).\n\n"
                  "- consider unicode\n- O(n)\n\n> verdict: a slice is correct";
    /* the Console panel reconstructs flowing text from per-delta \"<agent>: <chunk>\" lines (two agents interleaved,
     * one with markdown) — exercises the join-by-agent + markdown path + the literal-%s safety. */
    s.chat = {"agent:A: Here is the ", "agent:A: **plan**:\n- step one\n", "agent:B: (worker B) computing\xE2\x80\xA6",
              "agent:A: - step two `done`"};
    /* the Log panel stays plain but now WRAPS — give it a long line so the wrap path runs (no horizontal scroll). */
    s.logs = {"[host] supervisor up; bus bound at /tmp/hypercat_XXXXXX/bus.sock",
              "[host] a deliberately long diagnostic line that must wrap at the panel edge instead of scrolling "
              "sideways forever and ever and ever so the PushTextWrapPos path is exercised under ASan/UBSan"};
    /* P10/Wave0: a few activity spans (a done bar, an OPEN bar drawn to now, and a Crash) so the headless run
     * exercises the timeline's filled-bar + clipped-label + lane-band + axis path under ASan/UBSan. */
    s.now_ms = 9000;
    s.timeline = {{"agent:A", "reverse a string", "t1", 1000, 3200, TimelineSpan::Kind::Task},
                  {"agent:A", "is_prime", "t3", 4200, 0, TimelineSpan::Kind::Task}, /* open -> draws to now */
                  {"agent:B", "test the reverser", "t2", 3400, 5600, TimelineSpan::Kind::Task},
                  {"agent:C", "palindrome verdict", "t6", 1200, 2400, TimelineSpan::Kind::Crash}};
    /* P08.2: an allow + a deny egress decision so the Network panel renders both row styles under ASan/UBSan */
    s.egress = {{"agent:A", "openrouter.ai", "203.0.113.7", "allow", 443, 1000},
                {"agent:B", "169.254.169.254", "169.254.169.254", "deny-ip-class", 80, 1100}};
    /* Custom Tooling: System Tools + a third-party row so the Tools panel exercises the tree + detail + toggle. */
    s.tools = {{"fs_write", ToolRow::Kind::System, true, false, "create or replace a file (gated)", "", {}, "", ""},
               {"deep_reason", ToolRow::Kind::System, false, false, "staged reasoning", "", {}, "", ""},
               {"web_search", ToolRow::Kind::ThirdParty, false, false, "search the web (third-party)", "{...}",
                {{"~/cache/*"}, {"api.example.com:443"}, {}, "cpu 2s", "path:x", "0.3"}, "", ""}};
    s.third_party_tools_disabled = false;
    app->set_snapshot(s);
    /* WI-5: enable the opt-in mascot so the headless run also exercises its full render path (QOI decode
     * -> backend texture upload -> Image with the NEAREST sampler -> teardown) under ASan/UBSan. The
     * snapshot above has a dead agent:C -> the mascot's Error mood (the err-tinted idle face). */
    app->show_mascot(true);
    app->show_music_player(true); /* exercise the opt-in Music Player panel under ASan/UBSan */
    app->show_network(true);      /* P08.2: exercise the egress-audit Network panel (both row styles) */

    /* exercise the runtime accent (re-themes in place) — render a frame on each to prove no crash */
    int n = 0;
    for (Accent a : {Accent::White, Accent::Cyan, Accent::Amber, Accent::Emerald, Accent::Violet,
                     Accent::Crimson}) {
        app->set_accent(a);
        n += app->render_frames(1);
    }
    if (app->accent() != Accent::Crimson) {
        std::fprintf(stderr, "ui_smoke: set_accent did not stick\n");
        delete app;
        return 1;
    }
    /* the command channel: no interaction happened headless, so the host drains nothing */
    if (!app->drain_commands().empty()) {
        std::fprintf(stderr, "ui_smoke: unexpected UI command from a non-interactive render\n");
        delete app;
        return 1;
    }

    /* WI-5: switch to a healthy, running fleet so the mascot enters its Working mood — this drives the
     * 6-frame "think" spritesheet (the looped Image draw + the per-frame advance) which the Error mood
     * above does not, giving the working animation + strip UVs headless coverage too. */
    UiSnapshot working = s;
    working.agents = {{"agent:A", "dev", "ready"}};
    working.tasks = {{"t1", "verify A", "running", "agent:A", "run the test for A", "", {}}};
    app->set_snapshot(working);
    n += app->render_frames(3);

    /* Conductor P5-S2: drive the chat avatar + tool cue through their phases so the headless run exercises the
     * live decode -> upload -> advance -> draw of all 7 conductor sheets (the 96x96 avatar states + the 128x128
     * tool cues) under ASan/UBSan, the coverage the mascot gets above. The chat panel is default-ON. */
    UiSnapshot cond = s;
    cond.conductor_online = true;
    /* P3b: populate the conversations picker so its header button + label-snprintf path render under ASan (the
     * popup ROWS open only on a click, which the headless harness can't inject; the row code is a bounded-snprintf
     * mirror of the Sessions panel). An untrusted title with a quote proves the "%s"-as-label path is exercised. */
    cond.conductor_current_session_id = "sess-aaa";
    cond.conductor_conversations = {
        {"sess-aaa", "Chat 2026-06-19 10:00", "2026-06-19T10:05:00Z", 4}, /* the active one (highlighted) */
        {"sess-bbb", "Renamed \"chat\"", "2026-06-19T09:00:00Z", 2},
    };
    cond.conductor_chat = {{"user", "reverse a string"}, {"assistant", "On it"}};
    cond.conductor_busy = true; /* a turn in flight -> the messagereceived loop */
    app->set_snapshot(cond);
    n += app->render_frames(2);
    cond.conductor_active_tool = "recall_memory"; /* a read tool -> the read cue */
    app->set_snapshot(cond);
    n += app->render_frames(2);
    cond.conductor_active_tool = "write_memory"; /* a write tool -> the write cue */
    app->set_snapshot(cond);
    n += app->render_frames(2);
    cond.conductor_active_tool = "run_agenda"; /* dispatch -> the taskagendadispatch avatar + the generic cue */
    app->set_snapshot(cond);
    n += app->render_frames(2);
    cond.conductor_busy = false; /* the reply settles: a NEW assistant msg, no tool -> the one-shot messagesent */
    cond.conductor_active_tool = "";
    cond.conductor_chat.push_back({"assistant", "Done"});
    app->set_snapshot(cond);
    n += app->render_frames(3);

    /* W4 P4.2: drive the OperatorTextureCache through its WHOLE lifecycle under ASan/UBSan. The Viewer panel is
     * default-ON, so each frame with an opened image calls cache.get() (decode -> backend texture upload ->
     * ImGui::Image). Opening the valid PNG under MANY distinct content keys (varying open_hash) past the
     * cache's entry cap forces the mid-run evict -> WantDestroy -> backend glDeleteTextures -> reap handshake to
     * fire repeatedly; render_frames(2) per open lets the 2-frame handshake complete. A final settle drains the
     * last retiring textures before teardown frees the rest (the post-backend CPU-side sweep). */
    UiSnapshot img = s;
    img.open_path = "agent_A/icon.png";
    img.open_bytes = std::make_shared<const std::string>((const char *)k_png_2x2, sizeof k_png_2x2);
    img.open_size = (long)img.open_bytes->size();
    for (int i = 0; i < 80; i++) {              /* > the 64-entry cap -> count-based eviction + reaping */
        img.open_hash = 0x100u + (uint64_t)i;   /* a fresh content key each open -> a miss + a new texture */
        app->set_snapshot(img);
        n += app->render_frames(2);
    }
    /* one truncated/garbage "image" so the negative-cache (decode-fail) branch is exercised too */
    img.open_bytes = std::make_shared<const std::string>((const char *)k_png_2x2, 12); /* header-only -> fails */
    img.open_hash = 0xDEAD;
    app->set_snapshot(img);
    n += app->render_frames(2);
    n += app->render_frames(3); /* settle: let the last batch of retiring textures reap before teardown */

    /* W5 P5.1: drive the Editor (default-ON) over a REAL code file so the lexer's keyword/type/number/string/
     * comment/preproc paths + the multi-line block-comment carry + the clipper/gutter render all run under
     * ASan/UBSan (the image opens above already drove it with arbitrary binary bytes). */
    UiSnapshot code = s;
    code.open_path = "agent_A/snippet.c";
    code.open_bytes = std::make_shared<const std::string>(
        "#include <stdio.h>\n/* a\n   two-line block */\nint main(void) {\n"
        "    char *p = \"hi\\n\"; // greet\n    return 0xFF;\n}\n");
    code.open_size = (long)code.open_bytes->size();
    code.open_hash = 0x5151;
    app->set_snapshot(code);
    n += app->render_frames(2);

    /* W5 P5.2: drive the editor's change-on-disk banner + the reused diff renderer under ASan/UBSan. */
    code.open_disk_changed = true;
    code.open_diff.path = "agent_A/snippet.c";
    code.open_diff.added = 1;
    code.open_diff.removed = 1;
    {
        DiffHunk h;
        h.old_start = 1;
        h.new_start = 1;
        h.lines = {{' ', "int main(void) {"}, {'-', "    return 0xFF;"}, {'+', "    return 0;"}};
        code.open_diff.hunks = {h};
    }
    app->set_snapshot(code);
    n += app->render_frames(2);

    delete app;

    if (n < 1) {
        std::fprintf(stderr, "ui_smoke: rendered no frames\n");
        return 1;
    }
    std::printf("ui_smoke: ok (rendered %d frames, hidden window)\n", n);
    return 0;
}
