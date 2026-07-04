/* test_tool_manifest — the PURE install-time manifest validator (the third-party tool security gate). Offline. */

#include "tool_manifest.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace hcapp;

static int g_fail = 0;
#define CHECK(c, m)                                                                                              \
    do {                                                                                                         \
        if (!(c)) {                                                                                              \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                            \
            ++g_fail;                                                                                            \
        }                                                                                                        \
    } while (0)

static bool parse(const char *json, const std::string &dir, ToolManifest &out, std::string &err)
{
    return tool_manifest_parse(json, std::strlen(json), dir, out, err);
}

int main()
{
    /* a well-formed manifest accepted; fields + clamps + default-deny permissions correct */
    {
        const char *j = R"({
          "manifest_version":1, "id":"csv-stats", "name":"CSV Stats", "version":"0.1.0",
          "tools":[{"name":"csv_stats","sensitive":false,
                    "spec_json":{"type":"function","function":{"name":"csv_stats"}}}],
          "permissions":{"fs":{"mode":"read"},"egress_hosts":["api.example.com"]},
          "limits":{"timeout_ms":999999,"cpu_seconds":3,"max_files":8}
        })";
        ToolManifest m;
        std::string  err;
        CHECK(parse(j, "csv-stats", m, err), "valid manifest accepted");
        CHECK(m.id == "csv-stats" && m.tools.size() == 1 && m.tools[0].name == "csv_stats", "fields parsed");
        CHECK(!m.tools[0].spec_json.empty() && m.tools[0].spec_json.find("csv_stats") != std::string::npos,
              "spec_json captured as a string");
        CHECK(m.fs_mode == ToolFsMode::Read, "fs mode parsed");
        CHECK(m.egress_hosts.size() == 1 && m.egress_hosts[0] == "api.example.com", "egress host parsed");
        CHECK(m.exec_allow.empty(), "exec_allow stays empty (exec is not a v1 capability)");
        CHECK(m.timeout_ms == kToolMaxTimeoutMs, "over-limit timeout clamped to the host max");
        CHECK(m.max_files == 16, "below-floor max_files clamped up to the floor"); /* 8 -> 16 floor */
        CHECK(m.envelope_sensitive(), "egress grant => envelope is sensitive (per-call gate)");
    }

    /* default-deny when permissions absent */
    {
        const char  *j = R"({"manifest_version":1,"id":"t","tools":[{"name":"t_fn","spec_json":{}}]})";
        ToolManifest m;
        std::string  err;
        CHECK(parse(j, "t", m, err), "minimal manifest accepted");
        CHECK(m.fs_mode == ToolFsMode::None && m.egress_hosts.empty() && m.exec_allow.empty(), "default-deny");
        CHECK(!m.envelope_sensitive(), "no grants => envelope not sensitive");
        CHECK(m.runtime == ToolRuntime::Native && m.interpreter.empty() && m.entry.empty(),
              "absent runtime block => native (full back-compat)");
    }

    /* a managed (non-C) manifest: an allowlisted interpreter + a single in-package entry script */
    {
        const char *j = R"({"manifest_version":1,"id":"py","tools":[{"name":"py_fn","spec_json":{}}],
                            "runtime":{"mode":"managed","interpreter":"python3","entry":"tool.py"}})";
        ToolManifest m;
        std::string  err;
        CHECK(parse(j, "py", m, err), "managed manifest accepted");
        CHECK(m.runtime == ToolRuntime::Managed, "runtime parsed as managed");
        CHECK(m.interpreter == "python3" && m.entry == "tool.py", "interpreter + entry parsed");
    }

    /* an explicit "native" runtime block is accepted and behaves like the default */
    {
        const char  *j = R"({"manifest_version":1,"id":"n","tools":[{"name":"n_fn","spec_json":{}}],
                             "runtime":{"mode":"native"}})";
        ToolManifest m;
        std::string  err;
        CHECK(parse(j, "n", m, err) && m.runtime == ToolRuntime::Native, "explicit native runtime accepted");
    }

    /* REJECTIONS */
    auto rejects = [&](const char *j, const char *dir, const char *why) {
        ToolManifest m;
        std::string  err;
        CHECK(!parse(j, dir, m, err) && !err.empty(), why);
    };
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}]})", "different-dir",
            "id != install dir is rejected");
    rejects(R"({"manifest_version":99,"id":"a","tools":[{"name":"x","spec_json":{}}]})", "a",
            "unknown manifest_version is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"fs_write","spec_json":{}}]})", "a",
            "a reserved (built-in) tool name is rejected (anti-spoof)");
    rejects(R"({"manifest_version":1,"id":"a","tools":[]})", "a", "no tools is rejected");
    rejects(R"({"manifest_version":1,"id":"a"})", "a", "missing tools is rejected");
    rejects("not json", "a", "non-JSON is rejected");
    rejects(R"(["array","root"])", "a", "non-object root is rejected");
    rejects(R"({"manifest_version":1,"id":"","tools":[{"name":"x","spec_json":{}}]})", "", "empty id is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],"permissions":{"exec_allow":["/bin/sh"]}})",
            "a", "a non-empty exec_allow is rejected (tool exec is not supported in v1)");
    /* runtime-block rejections (the managed non-C path) */
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],
                "runtime":{"mode":"managed","interpreter":"bash","entry":"t.sh"}})",
            "a", "a non-allowlisted interpreter is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],
                "runtime":{"mode":"managed","interpreter":"python3"}})",
            "a", "a managed manifest with no entry is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],
                "runtime":{"mode":"managed","interpreter":"python3","entry":"../escape.py"}})",
            "a", "a managed entry with '/' (traversal) is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],
                "runtime":{"mode":"managed","interpreter":"python3","entry":".."}})",
            "a", "a managed entry of '..' is rejected");
    rejects(R"({"manifest_version":1,"id":"a","tools":[{"name":"x","spec_json":{}}],
                "runtime":{"mode":"sideways","interpreter":"python3","entry":"t.py"}})",
            "a", "an unknown runtime.mode is rejected");

    if (g_fail == 0) std::printf("tool_manifest: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
