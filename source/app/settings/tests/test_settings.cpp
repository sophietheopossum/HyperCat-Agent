/* test_settings — the offline gate for the WI-2 E1 settings core: serialize/parse round-trip (incl. the
 * sparse panels map + the egress list), malformed/missing -> defaults, validate clamps + the
 * task_deadline >= llm_call_total invariant, and env-precedence (explicit env WINS, marked in EnvOverrides).
 * The atomic-write primitive is hc_fs's (tested there); here we prove save->load survives a real file. */

#include "hc_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace hcapp;

static int g_fail = 0;
#define CHECK(cond)                                                                                        \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                    \
            g_fail++;                                                                                       \
        }                                                                                                  \
    } while (0)

static std::string tmp_path(const char *tag)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "/tmp/hc_settings_test_%ld_%s.json", (long)getpid(), tag);
    return buf;
}

static void test_round_trip()
{
    Settings s;
    s.accent = "violet";
    s.mascot = true;
    s.panels["fleet"] = true;
    s.panels["terminal"] = false;
    s.panels["a_new_panel_name"] = true; /* a name the codec never hardcodes */
    s.poll_hz = 5;
    s.model = "mistralai/mistral-medium-3-5";
    s.base_url = "http://192.168.1.50:8080/v1";
    s.embed_model = "google/gemini-embedding-2";
    s.data_dir = "/home/op/.local/share/hypercat";
    s.ephemeral = true;
    s.llm_call_total_ms = 90000;
    s.llm_connect_ms = 8000;
    s.deep_reason_budget = 6;
    s.task_deadline_ms = 240000;
    s.egress_allow = {"192.168.1.50", "::1"};
    s.exec_allow = {"/usr/bin/git", "/bin/sh"};
    s.models = {{"google/gemini-3.5-flash", "fast/cheap"}, {"anthropic/claude", "best for code"}};
    s.role_models = {{"dev", "anthropic/claude"}, {"research", "google/gemini-3.5-flash"}};
    /* automation (B3/B4): the persistence layer is FAITHFUL — both round-trip as set. The session-scoped disarm of
     * allow_all_approvals is a SEPARATE host-load step (settings_clear_session_arming), proven in test_session_arming. */
    s.auto_approve_contained = true;
    s.allow_all_approvals = true;
    s.conductor_persona = "Be terse and a touch playful. (=・ω・=)"; /* the global default persona slot */
    /* tools (Custom Tooling): the System Tools + third-party enable maps + the kill-switch + conductor opt-in */
    s.system_tools = {{"fs_write", false}, {"deep_reason", true}};
    s.thirdparty_tools = {{"web_search", true}};
    s.thirdparty_tools_enabled = false;
    s.thirdparty_tools_conductor = true;

    const std::string path = tmp_path("roundtrip");
    CHECK(settings_save(s, path.c_str()));

    Settings r; /* defaults */
    CHECK(settings_load(path.c_str(), r));
    CHECK(r.accent == "violet");
    CHECK(r.mascot == true);
    CHECK(r.panels.size() == 3 && r.panels["fleet"] == true && r.panels["terminal"] == false &&
          r.panels["a_new_panel_name"] == true);
    CHECK(r.poll_hz == 5);
    CHECK(r.model == s.model && r.base_url == s.base_url && r.embed_model == s.embed_model);
    CHECK(r.data_dir == s.data_dir && r.ephemeral == true);
    CHECK(r.llm_call_total_ms == 90000 && r.llm_connect_ms == 8000);
    CHECK(r.deep_reason_budget == 6 && r.task_deadline_ms == 240000);
    CHECK(r.egress_allow.size() == 2 && r.egress_allow[0] == "192.168.1.50" && r.egress_allow[1] == "::1");
    CHECK(r.exec_allow.size() == 2 && r.exec_allow[0] == "/usr/bin/git" && r.exec_allow[1] == "/bin/sh"); /* W4 */
    /* W4: validate drops a non-absolute / `..` exec entry, keeps the absolute one */
    {
        Settings v;
        v.exec_allow = {"/usr/bin/ok", "relative/bad", "/has/../dotdot"};
        settings_validate(v);
        CHECK(v.exec_allow.size() == 1 && v.exec_allow[0] == "/usr/bin/ok");
    }
    /* W2: the models catalog + the sparse per-role assignment round-trip */
    CHECK(r.models.size() == 2 && r.models[0].id == "google/gemini-3.5-flash" &&
          r.models[0].note == "fast/cheap" && r.models[1].id == "anthropic/claude");
    CHECK(r.role_models.size() == 2 && r.role_models["dev"] == "anthropic/claude" &&
          r.role_models["research"] == "google/gemini-3.5-flash");
    CHECK(r.auto_approve_contained == true && r.allow_all_approvals == true); /* B3/B4 persist faithfully */
    CHECK(r.conductor_persona == s.conductor_persona); /* the global default persona round-trips verbatim */
    /* tools (Custom Tooling) round-trip: the enable maps + the kill-switch + the conductor opt-in */
    CHECK(r.system_tools.size() == 2 && r.system_tools["fs_write"] == false && r.system_tools["deep_reason"] == true);
    CHECK(r.thirdparty_tools.size() == 1 && r.thirdparty_tools["web_search"] == true);
    CHECK(r.thirdparty_tools_enabled == false && r.thirdparty_tools_conductor == true);
    { /* conductor_persona is length-capped at load (hygiene; the authoritative sanitize is at assembly) */
        Settings v;
        v.conductor_persona = std::string(20000, 'p');
        settings_validate(v);
        CHECK(v.conductor_persona.size() <= hcapp::kMaxConductorPersonaBytes);
    }

    /* canonical serialization is stable: the same Settings serializes byte-identically */
    CHECK(settings_serialize(s) == settings_serialize(r));

    /* W2: validate keeps role assignments referentially consistent — a role mapped to a model NOT in the
     * catalog (and an empty-id catalog entry) is dropped. */
    {
        Settings v;
        v.models = {{"m1", ""}, {"", "no id -> dropped"}};
        v.role_models = {{"dev", "m1"}, {"qa", "ghost-model"}};
        settings_validate(v);
        CHECK(v.models.size() == 1 && v.models[0].id == "m1");
        CHECK(v.role_models.size() == 1 && v.role_models.count("dev") == 1 && v.role_models.count("qa") == 0);
    }

    std::remove(path.c_str());
}

static void test_missing_and_malformed()
{
    Settings def; /* untouched defaults */

    Settings a;
    CHECK(!settings_load("/tmp/hc_settings_does_not_exist_zzz.json", a)); /* missing -> false */
    CHECK(a.accent == def.accent && a.model == def.model);               /* left at defaults */

    /* a present but malformed file -> false, out unchanged */
    const std::string path = tmp_path("malformed");
    FILE             *f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        std::fputs("{ this is not valid json ::: ", f);
        std::fclose(f);
    }
    Settings b;
    CHECK(!settings_load(path.c_str(), b));
    CHECK(b.accent == def.accent);

    /* a valid JSON that is not an object (an array) -> false */
    Settings c;
    CHECK(!settings_parse("[1,2,3]", 7, c));

    /* a partial object: only what's present is read; the rest keeps defaults */
    Settings d;
    const char *partial = "{\"ui\":{\"accent\":\"cyan\"}}";
    CHECK(settings_parse(partial, std::strlen(partial), d));
    CHECK(d.accent == "cyan");
    CHECK(d.model == def.model && d.llm_call_total_ms == def.llm_call_total_ms);

    std::remove(path.c_str());
}

static void test_validate()
{
    Settings s;
    s.accent = "chartreuse"; /* not a real accent */
    s.poll_hz = 9999;
    s.llm_call_total_ms = 10;       /* below the floor */
    s.llm_connect_ms = 999999;      /* above the ceiling */
    s.deep_reason_budget = -3;      /* below 0 */
    s.task_deadline_ms = 1000;      /* below llm_call_total_ms -> must be floored up */
    s.egress_allow = {"10.0.0.1", "", "10.0.0.2"}; /* an empty entry must be dropped */
    settings_validate(s);
    CHECK(s.accent == "white");
    CHECK(s.poll_hz == 60);
    CHECK(s.llm_call_total_ms == 5000);
    CHECK(s.llm_connect_ms == 60000);
    CHECK(s.deep_reason_budget == 0);
    CHECK(s.task_deadline_ms >= s.llm_call_total_ms); /* the invariant holds */
    CHECK(s.task_deadline_ms == 5000);                /* floored to llm_call_total_ms */
    CHECK(s.egress_allow.size() == 2 && s.egress_allow[0] == "10.0.0.1" && s.egress_allow[1] == "10.0.0.2");
}

static void test_env_precedence()
{
    Settings base;
    base.model = "file-model";
    base.llm_call_total_ms = 90000;

    /* no env set -> nothing overridden */
    unsetenv("HC_MODEL");
    unsetenv("HC_LLM_CALL_TOTAL_MS");
    unsetenv("HC_EGRESS_ALLOW");
    unsetenv("HC_EPHEMERAL");
    {
        EnvOverrides ov;
        Settings     m = settings_merge_env(base, ov);
        CHECK(m.model == "file-model" && !ov.model);
        CHECK(m.llm_call_total_ms == 90000 && !ov.llm_call_total_ms);
    }

    /* explicit env WINS + is marked */
    setenv("HC_MODEL", "env-model", 1);
    setenv("HC_LLM_CALL_TOTAL_MS", "45000", 1);
    setenv("HC_EGRESS_ALLOW", "127.0.0.1, 10.0.0.5 ", 1);
    setenv("HC_EPHEMERAL", "1", 1);
    {
        EnvOverrides ov;
        Settings     m = settings_merge_env(base, ov);
        CHECK(m.model == "env-model" && ov.model);
        CHECK(m.llm_call_total_ms == 45000 && ov.llm_call_total_ms);
        CHECK(m.ephemeral == true && ov.ephemeral);
        CHECK(m.egress_allow.size() == 2 && m.egress_allow[0] == "127.0.0.1" &&
              m.egress_allow[1] == "10.0.0.5" && ov.egress_allow);
        /* base is unmodified (merge is non-mutating) */
        CHECK(base.model == "file-model");
    }

    /* a present-but-garbage int env is NOT treated as an override (no silent typo) */
    setenv("HC_LLM_CALL_TOTAL_MS", "not-a-number", 1);
    {
        EnvOverrides ov;
        Settings     m = settings_merge_env(base, ov);
        CHECK(m.llm_call_total_ms == 90000 && !ov.llm_call_total_ms);
    }

    unsetenv("HC_MODEL");
    unsetenv("HC_LLM_CALL_TOTAL_MS");
    unsetenv("HC_EGRESS_ALLOW");
    unsetenv("HC_EPHEMERAL");
}

static void test_session_arming()
{
    /* SECURITY regression (B4): the unbounded approval escape hatch must NOT survive a load — a stale or hand-edited
     * allow_all_approvals=true is disarmed so the gate can't silently open on a fresh launch / project switch. The
     * deterministic, sandbox-contained envelope (auto_approve_contained) must SURVIVE (it deliberately persists). */
    Settings s;
    s.allow_all_approvals = true;    /* as if just loaded from a persisted / hand-edited file */
    s.auto_approve_contained = true; /* the contained, reversible envelope */
    settings_clear_session_arming(s);
    CHECK(s.allow_all_approvals == false);   /* re-arm required in-session via the consent modal */
    CHECK(s.auto_approve_contained == true); /* untouched — persists */

    /* idempotent + safe on an already-disarmed value (a SwitchProject re-execs through the same boundary) */
    settings_clear_session_arming(s);
    CHECK(s.allow_all_approvals == false);
    CHECK(s.auto_approve_contained == true);
}

int main()
{
    test_round_trip();
    test_missing_and_malformed();
    test_validate();
    test_session_arming();
    test_env_precedence();
    if (g_fail) {
        std::printf("test_settings: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_settings: all checks passed\n");
    return 0;
}
