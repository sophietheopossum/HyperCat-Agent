/* ui_preview — a headless screenshot tool for sighted UI iteration: build a representative UiSnapshot,
 * render it to a HIDDEN window (nothing appears on screen), and dump a binary PPM. Convert to PNG
 * (pnmtopng / convert) to view. Usage: ui_preview [out.ppm]. Dev tooling only (HC_BUILD_TESTS). */

#include "hc_ui.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

using namespace hc::ui;

/* A valid 2x2 RGB PNG (PIL-verified — the same fixture libs/hc_image's decode test uses) so the `viewer-image`
 * mode drives the REAL decode -> GPU texture -> ImGui::Image path headlessly (scaled up to fill the Viewer). */
static const unsigned char k_png_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00,
    0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x38, 0xa1, 0xa1, 0x01, 0x44, 0x0c, 0x10,
    0x0a, 0x00, 0x21, 0x2e, 0x04, 0x61, 0xf6, 0xe1, 0xc2, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82};

static UiSnapshot sample()
{
    UiSnapshot s;
    s.agenda_title = "release v0.1";
    s.agenda_progress = 67;
    s.provider = "openrouter";
    s.model = "google/gemini-3.5-flash";

    s.agents = {{"agent:A", "dev", "ready"},
                {"agent:B", "qa", "ready"},
                {"agent:C", "dev", "dead"},
                {"agent:D", "research", "spawned"}};

    s.tasks = {
        {"t1", "reverse a string", "done", "agent:A", "Implement a function that reverses a string.",
         "def reverse_string(s): return s[::-1]", {}},
        {"t2", "test the reverser", "done", "agent:B", "Write one test for the reverse function.",
         "assert reverse_string(\"hello\") == \"olleh\"", {"t1"}},
        {"t3", "is_prime", "running", "agent:A", "Implement an is_prime(n) function.", "", {}},
        {"t4", "prime edge case", "assigned", "agent:B", "Name one edge case for is_prime.", "", {"t3"}},
        {"t5", "save reverse.txt", "pending", "", "Use fs_write to persist the reverser.", "", {"t1"}},
        {"t6", "palindrome verdict", "failed", "agent:C", "Decide: is \"\" a palindrome?",
         "error: worker crashed", {"t3"}}};

    s.logs = {"12:00:01  orchestrator: agenda 'release v0.1' started (6 tasks)",
              "12:00:02  agent:A <- t1  (dev)",
              "12:00:02  agent:B <- t2  (qa, after t1)",
              "12:00:05  agent:A -> t1 done",
              "12:00:06  agent:C crashed mid-task; t6 reassigned to a survivor",
              "12:00:09  agent:B -> t2 done"};

    s.chat = {"agent:A: Implementing the ", "agent:A: string reverse using slicing...",
              "agent:A: def reverse_string(s): return s[::-1]"};

    s.reasoning =
        "agent:B:\n[decompose] Is the empty string a palindrome? Sub-questions: (1) the formal "
        "definition w == reverse(w); (2) does it hold vacuously for |w|=0?\n"
        "[analyze] reverse(\"\") == \"\" holds by the identity. [Fact]\n"
        "[critique] Some informal definitions require >= 1 character. [Inference]\n"
        "[synthesize] Under the algebraic definition, yes. [Fact]\n"
        "[reflect] Confidence high; the only dissent is colloquial. [Inference]\n"
        "[answer] Yes — the empty string is a palindrome.\n"
        "[confidence] 99% (mathematically clear; pragmatic edge only).";

    /* a nested workspace (depth 3, %3A-encoded agent dirs, a mix of kinds) for the split file-browser preview */
    s.files = {{"README.md", 1200, false},
               {"notes.txt", 84, false},
               {"agent%3AA", 0, true},
               {"agent%3AA/reverse_string.py", 412, false},
               {"agent%3AA/notes.md", 220, false},
               {"agent%3AA/portrait", 0, true},
               {"agent%3AA/portrait/portrait.qoi", 8192, false},
               {"agent%3AA/portrait/palette.json", 156, false},
               {"agent%3AB", 0, true},
               {"agent%3AB/test_reverse.py", 88, false},
               {"agent%3AB/meow-app.html", 3300, false},
               {"config", 0, true},
               {"config/settings.json", 64, false}};

    s.sessions = {{"sess-1-A", "reverse a string", "gemini-3.5-flash", "2026-06-16 12:00", 1},
                  {"sess-2-B", "test the reverser", "gemini-3.5-flash", "2026-06-16 12:00", 1},
                  {"sess-3-A", "is_prime", "gemini-3.5-flash", "2026-06-16 12:01", 2}};
    s.viewed_session = "sess-3-A";
    s.transcript = {"user: Implement an is_prime(n) function.",
                    "assistant: ## is_prime(n)\n"
                    "An **O(sqrt n)** primality test using the `6k +/- 1` optimization (n <= 1 is *not* "
                    "prime).\n\n"
                    "```\n"
                    "def is_prime(n):\n"
                    "    if n <= 3: return n > 1\n"
                    "    if n % 2 == 0 or n % 3 == 0: return False\n"
                    "    i = 5\n"
                    "    while i * i <= n:\n"
                    "        if n % i == 0 or n % (i + 2) == 0: return False\n"
                    "        i += 6\n"
                    "    return True\n"
                    "```\n\n"
                    "Key points:\n"
                    "- every prime > 3 is `6k +/- 1`\n"
                    "- we stop at `sqrt(n)`\n\n"
                    "> Note: returns a bool.\n\n"
                    "---",
                    "tool: deep_reason -> the 6k+/-1 skip is sound."};

    s.pending_auth = {{"auth-1", "agent:A", "fs_write",
                       "fs_write (create or replace) reverse.txt  (41 bytes)\ndef reverse_string(s): "
                       "return s[::-1]"}};

    s.terminal = "hypercat$ ls\nagent_A  agent_B\nhypercat$ cat agent_A/reverse.txt\n"
                 "def reverse_string(s): return s[::-1]\nhypercat$ ";
    s.notice = "agenda 'release v0.1' started — 6 tasks"; /* the transient action-status line */
    s.artifacts = {{"9af1c2e0b3d4556677889900aabbccddeeff00112233445566778899aabbccdd", "t1", "agent:A",
                    "reverse.txt", "2026-06-16 12:00", 41},
                   {"1b2c3d4e5f60718293a4b5c6d7e8f90112233445566778899aabbccddeeff001", "t1", "agent:A",
                    "notes.md", "2026-06-16 12:01", 220}};
    s.now_ms = 9000; /* the activity timeline (P10) — a few spans across the fleet's lanes */
    s.timeline = {{"agent:A", "reverse a string", "t1", 1000, 3200, TimelineSpan::Kind::Task},
                  {"agent:A", "is_prime", "t3", 4200, 0, TimelineSpan::Kind::Task}, /* open -> draws to now */
                  {"agent:B", "test the reverser", "t2", 3400, 5600, TimelineSpan::Kind::Task},
                  {"agent:B", "prime edge case", "t4", 6000, 0, TimelineSpan::Kind::Task},
                  {"agent:C", "palindrome verdict", "t6", 1200, 2400, TimelineSpan::Kind::Crash}};
    /* a pending fs_write rendered as a reviewable diff (P11) — keyed by the same id as pending_auth */
    s.pending_diff = {{"auth-1",
                       "agent:A",
                       "reverse.py",
                       false,
                       false,
                       {{10, 10,
                         {{' ', "def reverse(s):"},
                          {'-', "    return s[::-1]"},
                          {'+', "    if s is None:"},
                          {'+', "        return \"\""},
                          {'+', "    return s[::-1]"}}}},
                       3,
                       1}};
    return s;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/hypercat_ui.ppm";
    const char *mode = argc > 2 ? argv[2] : "";
    bool        empty = std::strcmp(mode, "empty") == 0;   /* the blank-slate startup            */
    bool        mascot = std::strcmp(mode, "mascot") == 0; /* WI-5: preview the opt-in mascot    */
    bool        conductor = std::strcmp(mode, "conductor") == 0; /* P5-S2: the chat avatar + tool cue */
    bool        chatattach = std::strcmp(mode, "chat-attach") == 0; /* A/C/D/E: attachments + chat-standard polish */
    bool        models = std::strcmp(mode, "models") == 0;       /* W2: the Models catalog + role assignment */
    bool        plan = std::strcmp(mode, "plan") == 0;           /* the task-DAG ("Plan") boxes + truncation */
    bool        builder = std::strcmp(mode, "builder") == 0;     /* the Agenda Builder form (artifact + verify)   */
    bool        roles = std::strcmp(mode, "roles") == 0;         /* P2.3b: the Worker Builder role-template editor */
    bool        projects = std::strcmp(mode, "projects") == 0;   /* W3 P3.2: the Projects panel (create + switch)  */
    UiApp      *app = UiApp::create("HyperCat preview", /*visible=*/false);
    if (!app) {
        std::fprintf(stderr, "ui_preview: skipped (no display/GL available)\n");
        return 0;
    }
    UiSnapshot s;
    if (empty) s.provider = "offline";
    else s = sample();
    if (mascot) { /* force the calm Idle mood (untinted white face) for the clearest art check */
        s.agents = {{"agent:A", "dev", "ready"}, {"agent:B", "qa", "ready"}};
        for (auto &t : s.tasks)
            if (t.state == "failed" || t.state == "running" || t.state == "assigned") t.state = "done";
    }
    if (conductor) { /* P5-S2: a representative conductor mid-conversation — the avatar (thinking) + a tool cue */
        s.conductor_online = true;
        s.conductor_chat = {
            {"user", "Plan a small release: reverse a string, an is_prime, and one test."},
            {"assistant",
             "On it — let me think through the shape first. (｀･ω･´)ゞ\n\n"
             "I'll **decompose** this into three tasks and dispatch the fleet, then leave the `agenda` hammer "
             "in the toolbox until you say go. We can iterate on the plan as much as you'd like before anything "
             "runs — and I'll keep you posted the *whole* way through, so nothing happens that you didn't "
             "approve first."}};
        s.conductor_streaming = "Checking what we already have in memory";
        s.conductor_busy = true;
        s.conductor_active_tool = "recall_memory"; /* -> the read tool cue */
        s.conductor_goals = {{"g1", "release v0.1", "active", 1}};
    }
    if (chatattach) { /* the chat-media feature: a staged-attachment tray, an inline attached image, per-message
                       * timestamps (D), and the Stop button (E, shown while busy). */
        s.conductor_online = true;
        s.conductor_chat = {
            {"user", "Here's the spec and a mockup — take a look.", 1718900000000ull,
             {{"mockup.png", std::make_shared<const std::string>((const char *)k_png_2x2, sizeof k_png_2x2),
               0x9e3779b97f4a7c15ull, false}}},
            {"assistant",
             "Got the spec via `read_artifact` — here's the shape:\n\n"
             "```c\nint reverse(char *s, size_t n);\n```\n\n"
             "The image I can't see (I'm text-only), but the spec reads clean.",
             1718900020000ull,
             {}}};
        s.conductor_busy = true; /* -> the Stop button + the thinking cue */
        s.conductor_streaming = "Reading the attached spec";
        s.conductor_staged = {{"spec.md", false}, {"diagram.png", true}}; /* the staged-attachment chip tray */
        s.conductor_goals = {{"g1", "release v0.1", "active", 1}};
    }
    if (models) { /* W2: a representative catalog + a per-role assignment so the panel shows populated */
        s.settings.model = "google/gemini-3.5-flash";
        s.settings.models = {{"google/gemini-3.5-flash", "fast/cheap"},
                             {"anthropic/claude-opus", "best for code"},
                             {"mistralai/mistral-medium-3-5", "balanced"}};
        s.settings.role_models = {{"dev", "anthropic/claude-opus"}, {"research", "google/gemini-3.5-flash"}};
    }
    if (plan) { /* stress the box truncation with a long title (must fit + ellipsize, not overflow) */
        if (!s.tasks.empty()) s.tasks[0].title = "implement the reverse-string helper with edge cases";
    }
    if (builder) s.roles = {"dev", "qa", "research", "ops", "docs"}; /* show the live-role combo (5, not the 4 defaults) */
    if (roles) { /* P2.3b: representative role templates + the tool catalog + a model catalog for the dropdown */
        s.tool_catalog = {"deep_reason", "memory_recall", "memory_write",
                          "fs_read",     "fs_list",       "fs_write",      "fs_update"};
        s.role_defs = {
            {"dev", "You are the fleet's DEVELOPER. Build working code; write your deliverable to the named file.",
             "anthropic/claude-opus",
             {"deep_reason", "memory_recall", "memory_write", "fs_read", "fs_list", "fs_write", "fs_update"}},
            {"qa", "You are the fleet's QA. Be skeptical: find the edge cases and write tests to files.", "",
             {"deep_reason", "memory_recall", "memory_write", "fs_read", "fs_list", "fs_write", "fs_update"}},
            {"research", "You are the fleet's RESEARCHER. Investigate + synthesize; you do not write the code.",
             "google/gemini-3.5-flash", {"deep_reason", "memory_recall", "memory_write", "fs_read", "fs_list"}}};
        s.settings.models = {{"google/gemini-3.5-flash", "fast/cheap"}, {"anthropic/claude-opus", "best for code"}};
    }
    if (projects) { /* W3 P3.2: a representative project list with the active one marked */
        s.projects = {{"default", "default", true},
                      {"reverse-string-release", "reverse string release", false},
                      {"research-spike", "research spike", false}};
        s.active_project = "default";
    }
    if (std::strcmp(mode, "skills") == 0) { /* W6 P6.3: the Skills authoring panel — list + the SKILL.md editor */
        s.skills = {{"pdf-extract", "pull text + tables out of a PDF"},
                    {"web-search", "search the web and summarize the top hits"},
                    {"sql-explain", "explain + optimize a SQL query"}};
        s.selected_skill = "pdf-extract";
        s.selected_skill_body = std::make_shared<const std::string>(
            "---\nname: pdf-extract\ndescription: pull text + tables out of a PDF\n---\n\n"
            "# pdf-extract\n\nUse `pdftotext -layout` first; for tables, fall back to camelot.\n");
        app->pin_window("Skills");
    }
    if (std::strcmp(mode, "settings") == 0) { /* Wave A: the persona fields are read from the snapshot (live-owned) */
        s.settings.conductor_persona = "Lead with the answer, then the why. Warm but brief.";
        s.settings.conductor_persona_per_project = true;
        s.settings.conductor_persona_project = "Be extra playful for this project. (=\xE3\x83\xBB\xCF\x89\xE3\x83\xBB=)";
        s.settings.conductor_spine_identity = "Who you are\n\nYou are HyperCat \xE2\x80\x94 a catgirl, and at home "
                                              "with it. (\xE2\x80\xA6 the locked identity preamble \xE2\x80\xA6)";
        s.settings.conductor_persona_default = "Quick, curious, warm, candid \xE2\x80\x94 the canonical voice.";
        s.settings.conductor_spine_floor = "How you hold yourself\n\nThis is the floor \xE2\x80\x94 the conduct you "
                                           "keep before anything else. (\xE2\x80\xA6 locked conduct + tools + gate \xE2\x80\xA6)";
        s.settings.persona_presets = {{"Canonical (default)", ""}, {"Neutral / minimal", "Be plain, precise, concise."}};
    }
    if (std::strcmp(mode, "tools") == 0) { /* Wave C: the Tools panel (System Tools + a sample third-party) */
        using TK = hc::ui::ToolRow::Kind;
        s.tools = {
            {"fs_write", TK::System, true, false, "create or replace a file in the workspace (operator-gated)", "",
             {}, "", ""},
            {"fs_read", TK::System, true, false, "read a file from the workspace (sandboxed)", "", {}, "", ""},
            {"deep_reason", TK::System, false, false, "staged multi-step reasoning for one hard question", "", {}, "",
             ""},
            {"memory_recall", TK::System, true, false, "search the semantic memory store (read-only)", "", {}, "", ""},
            {"web_search", TK::ThirdParty, false, false, "search the web (third-party)",
             "{\"type\":\"function\",\"function\":{\"name\":\"web_search\"}}",
             {{"~/cache/*"}, {"api.example.com:443"}, {}, "cpu 2s, mem 128MiB, 1 proc", "path:~/tools/web_search",
              "0.3"},
             "", ""},
        };
        s.third_party_tools_disabled = false;
        app->pin_window("Tools");
    }
    app->set_snapshot(std::move(s)); /* moved: s is unused after the handoff */
    if (mascot) app->show_mascot(true);
    if (conductor) app->pin_window("Conductor"); /* focus the chat tab so the capture shows the avatar */
    if (chatattach) app->pin_window("Conductor"); /* A: focus the chat to capture the attachments + chip tray */
    if (models) app->pin_window("Models");       /* focus the Models tab */
    if (plan) app->pin_window("Plan");           /* focus the task-DAG tab */
    if (builder) app->pin_window("Agenda Builder"); /* focus the builder so the capture shows artifact + verify */
    if (roles) app->pin_window("Roles");            /* P2.3b: focus the role editor tab */
    if (projects) app->pin_window("Projects");      /* W3 P3.2: focus the Projects panel */
    if (std::strcmp(mode, "files") == 0) app->pin_window("Files"); /* W4 P4.1: the file browser interactions */
    if (std::strcmp(mode, "music") == 0) { /* Phase C: the Music Player — now-playing + spectrum + library */
        hc::ui::UiSnapshot ms = sample();
        ms.audio_present = true;
        ms.audio_spectrum_on = true;
        ms.audio_enabled = false;
        ms.audio_title = "Aero Vista";
        ms.audio_artist = "HyperCat Sessions";
        ms.audio_state = 1; /* playing */
        ms.audio_pos_ms = 83000;
        ms.audio_dur_ms = 150047;
        ms.audio_volume = 70;
        for (int i = 0; i < 48; i++) { /* a plausible decaying spectrum (deterministic, no cmath) */
            int   k = (i * 37 + 11) % 100;
            float base = (float)(48 - i) / 48.0f;
            ms.audio_spectrum.push_back(base * (0.35f + 0.6f * (float)k / 100.0f));
        }
        ms.audio_library = {{"Aero_Vista.mp3", "Aero Vista", "HyperCat Sessions", 150047, "MP3"},
                            {"calm_rain.ogg", "Calm Rain", "Ambient Works", 240000, "OGG"},
                            {"focus_loop.flac", "Focus Loop", "", 600000, "FLAC"},
                            {"chime.wav", "Notify Chime", "", 1200, "WAV"}};
        app->set_snapshot(std::move(ms));
        app->show_music_player(true);
        app->pin_window("Music Player");
    }
    if (std::strcmp(mode, "viewer") == 0) {                        /* W4 P4.2: the opened-file Viewer (markdown) */
        hc::ui::UiSnapshot vs = sample();
        vs.open_path = "agent_A/notes.md";
        vs.open_bytes = std::make_shared<const std::string>(
            "# Reverse string\n\nThe **dev** worker wrote a `reverse_string(s)` helper.\n\n"
            "- handles the empty string\n- O(n)\n\n> verified by qa\n");
        vs.open_size = (long)vs.open_bytes->size();
        app->set_snapshot(std::move(vs));
        app->pin_window("Viewer");
    }
    if (std::strcmp(mode, "viewer-image") == 0) { /* W4 P4.2: the Viewer's IMAGE path (decode -> texture -> Image) */
        hc::ui::UiSnapshot vs = sample();
        vs.open_path = "agent_A/icon.png";
        vs.open_bytes = std::make_shared<const std::string>((const char *)k_png_2x2, sizeof k_png_2x2);
        vs.open_size = (long)vs.open_bytes->size();
        vs.open_hash = 0x9e3779b97f4a7c15ull; /* any stable non-zero content key (the cache keys on this) */
        app->set_snapshot(std::move(vs));
        app->pin_window("Viewer");
    }
    if (std::strcmp(mode, "editor") == 0) { /* W5 P5.1: the IDE view — gutter + syntax over a code file */
        hc::ui::UiSnapshot vs = sample();
        vs.open_path = "agent_A/reverse.c";
        vs.open_bytes = std::make_shared<const std::string>(
            "#include <stdio.h>\n"
            "\n"
            "/* reverse a C string in place\n"
            "   (a two-line block comment) */\n"
            "void reverse(char *s, size_t n) {\n"
            "    for (size_t i = 0; i < n / 2; i++) {\n"
            "        char t = s[i];        // swap the ends\n"
            "        s[i] = s[n - 1 - i];\n"
            "        s[n - 1 - i] = t;\n"
            "    }\n"
            "}\n"
            "\n"
            "const char *msg = \"reversed 0xFF bytes\\n\";\n");
        vs.open_size = (long)vs.open_bytes->size();
        vs.open_hash = 0xC0DEC0DEC0DEC0DEull;
        app->set_snapshot(std::move(vs));
        app->pin_window("Editor");
    }
    if (std::strcmp(mode, "editor-diff") == 0) { /* W5 P5.2: the change-on-disk banner + the reused diff view */
        hc::ui::UiSnapshot vs = sample();
        vs.open_path = "agent_A/reverse.c";
        vs.open_bytes = std::make_shared<const std::string>(
            "void reverse(char *s, size_t n) {\n    for (size_t i = 0; i < n / 2; i++) {\n");
        vs.open_size = (long)vs.open_bytes->size();
        vs.open_hash = 0xC0DEC0DEC0DEC0DEull;
        vs.open_disk_changed = true; /* an agent rewrote it -> the banner + diff */
        hc::ui::PendingDiff pd;
        pd.path = "agent_A/reverse.c";
        pd.added = 2;
        pd.removed = 1;
        hc::ui::DiffHunk h;
        h.old_start = 1;
        h.new_start = 1;
        h.lines = {{' ', "void reverse(char *s, size_t n) {"},
                   {'-', "    for (size_t i = 0; i < n / 2; i++) {"},
                   {'+', "    size_t lo = 0, hi = n - 1;"},
                   {'+', "    while (lo < hi) {"},
                   {' ', "        char t = s[lo];"}};
        pd.hunks = {h};
        vs.open_diff = pd;
        app->set_snapshot(std::move(vs));
        app->pin_window("Editor");
    }
    /* WI-2: a populated settings draft so a "settings" preview shows real fields (the host normally seeds
     * this via apply_settings; here we hand a representative POD straight in). */
    if (std::strcmp(mode, "settings") == 0) {
        hc::ui::UiSettings us;
        us.model = "mistralai/mistral-medium-3-5";
        us.base_url = "https://openrouter.ai/api/v1";
        us.embed_model = "google/gemini-embedding-2";
        us.poll_hz = 4;
        us.key_present = true;
        us.ov_model = true; /* show an env-locked field disabled */
        us.egress_allow = {"192.168.1.50"};
        us.exec_allow = {"/usr/bin/pytest", "/usr/bin/git"}; /* W4: the run allowlist editor populated */
        app->apply_settings(us); /* draft-read fields; the persona is seeded on the snapshot above (live-owned) */
        app->pin_window("Settings");
    }
    bool ok = app->screenshot(out, 8);
    delete app;
    std::printf("ui_preview: %s -> %s\n", ok ? "captured" : "FAILED", out);
    return ok ? 0 : 1;
}
