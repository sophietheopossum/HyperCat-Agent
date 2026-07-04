/* tool_manifest — implementation. See tool_manifest.hpp. PURE (hc::json + hc::tooling). */

#include "tool_manifest.hpp"

#include "system_tool_ids.hpp" /* is_reserved_tool_name — the anti-spoof check */

#include "hc_json.h"

#include <cstdlib> /* free */

namespace hcapp {

const char *tool_fs_mode_str(ToolFsMode m)
{
    switch (m) {
    case ToolFsMode::Read: return "read";
    case ToolFsMode::ReadWrite: return "readwrite";
    default: return "none";
    }
}

bool tool_interpreter_allowed(const std::string &name)
{
    /* a small, fixed allowlist of operator-trusted system runtimes; the host resolves the name to an absolute
     * system binary at launch (never a package-supplied one). Extend deliberately. */
    return name == "python3" || name == "python" || name == "node";
}

namespace {

int  clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
long clampl(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

ToolFsMode parse_fs_mode(const char *s)
{
    if (!s) return ToolFsMode::None;
    const std::string m = s;
    if (m == "read") return ToolFsMode::Read;
    if (m == "readwrite") return ToolFsMode::ReadWrite;
    return ToolFsMode::None; /* unknown / "none" => deny */
}

} // namespace

bool tool_manifest_parse(const char *json, size_t len, const std::string &dir_id, ToolManifest &out,
                         std::string &err)
{
    err.clear();
    out = ToolManifest{};
    if (!json) {
        err = "no manifest data";
        return false;
    }
    hc_json *root = hc_json_parse(json, len);
    if (!root) {
        err = "manifest is not valid JSON";
        return false;
    }
    if (!hc_json_is_object(root)) {
        hc_json_free(root);
        err = "manifest root is not an object";
        return false;
    }

    /* manifest_version — a forward-compat tripwire (reject unknown) */
    out.manifest_version = (int)hc_json_get_int(root, "manifest_version", 0);
    if (out.manifest_version != kToolManifestVersion) {
        hc_json_free(root);
        err = "unsupported manifest_version (expected " + std::to_string(kToolManifestVersion) + ")";
        return false;
    }

    /* id MUST equal the install directory name (the directory is authoritative; no claiming a foreign id) */
    out.id = hc_json_get_str(root, "id", "");
    if (out.id.empty()) {
        hc_json_free(root);
        err = "manifest id is empty";
        return false;
    }
    if (out.id != dir_id) {
        hc_json_free(root);
        err = "manifest id '" + out.id + "' does not match the install directory '" + dir_id + "'";
        return false;
    }
    out.name = hc_json_get_str(root, "name", out.id.c_str());
    out.description = hc_json_get_str(root, "description", "");
    out.author = hc_json_get_str(root, "author", "");
    out.version = hc_json_get_str(root, "version", "");

    /* tools[] — at least one; every function name must be non-empty + NOT a reserved built-in (anti-spoof) */
    const hc_json *tools = hc_json_get(root, "tools");
    if (!tools || !hc_json_is_array(tools) || hc_json_arr_len(tools) == 0) {
        hc_json_free(root);
        err = "manifest declares no tools";
        return false;
    }
    const size_t nt = hc_json_arr_len(tools);
    for (size_t i = 0; i < nt && out.tools.size() < kToolMaxFns; i++) {
        const hc_json *e = hc_json_arr_at(tools, i);
        if (!e || !hc_json_is_object(e)) continue;
        ToolFn fn;
        fn.name = hc_json_get_str(e, "name", "");
        if (fn.name.empty()) {
            hc_json_free(root);
            err = "a tool function has an empty name";
            return false;
        }
        if (is_reserved_tool_name(fn.name)) {
            hc_json_free(root);
            err = "tool name '" + fn.name + "' is reserved (a built-in tool) — choose another";
            return false;
        }
        fn.sensitive = hc_json_get_bool(e, "sensitive", false);
        if (const hc_json *spec = hc_json_get(e, "spec_json")) {
            if (char *txt = hc_json_print(spec, false)) { /* keep the function spec as a compact JSON string */
                fn.spec_json = txt;
                free(txt);
            }
        }
        out.tools.push_back(std::move(fn));
    }
    if (out.tools.empty()) {
        hc_json_free(root);
        err = "no valid tool functions";
        return false;
    }

    /* permissions — DEFAULT-DENY (absent block => fs None, no egress, no exec) */
    if (const hc_json *perms = hc_json_get(root, "permissions")) {
        if (const hc_json *fs = hc_json_get(perms, "fs"))
            out.fs_mode = parse_fs_mode(hc_json_get_str(fs, "mode", "none"));
        if (const hc_json *eg = hc_json_get(perms, "egress_hosts"); eg && hc_json_is_array(eg)) {
            const size_t n = hc_json_arr_len(eg);
            for (size_t i = 0; i < n && out.egress_hosts.size() < kToolMaxEgressHosts; i++) {
                const char *h = hc_json_as_str(hc_json_arr_at(eg, i), "");
                if (h && h[0]) out.egress_hosts.emplace_back(h);
            }
        }
        /* exec_allow is RESERVED but NOT functional in v1: the confine floor KILLs execve unconditionally and the
         * ToolHost passes no exec allowance to the tool, so a tool can never exec. Reject a non-empty grant rather
         * than advertise a capability that does not exist (fail-closed + honest). A future tool ExecGate would
         * wire this through; until then `out.exec_allow` stays empty. */
        if (const hc_json *xa = hc_json_get(perms, "exec_allow");
            xa && hc_json_is_array(xa) && hc_json_arr_len(xa) > 0) {
            hc_json_free(root);
            err = "permissions.exec_allow is not supported in v1 — third-party tools cannot exec";
            return false;
        }
    }

    /* limits — CLAMP every field to the host maximum (a planted huge value can't widen the jail) */
    if (const hc_json *lim = hc_json_get(root, "limits")) {
        out.timeout_ms = clampi((int)hc_json_get_int(lim, "timeout_ms", out.timeout_ms), 100, kToolMaxTimeoutMs);
        out.max_output_bytes =
            clampi((int)hc_json_get_int(lim, "max_output_bytes", out.max_output_bytes), 256, kToolMaxOutputBytes);
        out.cpu_seconds = clampi((int)hc_json_get_int(lim, "cpu_seconds", out.cpu_seconds), 1, kToolMaxCpuSeconds);
        out.mem_bytes =
            clampl((long)hc_json_get_int(lim, "mem_bytes", out.mem_bytes), 16L * 1024 * 1024, kToolMaxMemBytes);
        out.max_files = clampi((int)hc_json_get_int(lim, "max_files", out.max_files), 16, kToolMaxFiles);
    }

    /* runtime — the non-C path. DEFAULT native (today's behaviour: bin/<id> self-confines). For managed, the host
     * launcher (hc_tool_launch) jails + exec's an allowlisted SYSTEM interpreter on the in-package entry script.
     * An absent block => native, so every existing manifest keeps working unchanged (full back-compat). */
    if (const hc_json *rt = hc_json_get(root, "runtime")) {
        const std::string mode = hc_json_get_str(rt, "mode", "native");
        if (mode == "managed") {
            out.runtime = ToolRuntime::Managed;
            out.interpreter = hc_json_get_str(rt, "interpreter", "");
            out.entry = hc_json_get_str(rt, "entry", "");
            /* the interpreter MUST be an allowlisted name (the host resolves it to a SYSTEM binary at launch —
             * never a package-supplied one); reject anything else so a managed tool can't name an arbitrary exec
             * target. */
            if (!tool_interpreter_allowed(out.interpreter)) {
                hc_json_free(root);
                err = "runtime.interpreter '" + out.interpreter + "' is not an allowed managed runtime";
                return false;
            }
            /* the entry MUST be a single in-package file: non-empty, not the dir entries "."/"..", and no '/' (so
             * no subdir, no absolute path, and — since there is no separator — no traversal is even expressible).
             * The launcher resolves it strictly inside the package dir. */
            if (out.entry.empty() || out.entry == "." || out.entry == ".." ||
                out.entry.find('/') != std::string::npos) {
                hc_json_free(root);
                err = "runtime.entry must be a single in-package file (no '/' or '..')";
                return false;
            }
        } else if (mode != "native") {
            hc_json_free(root);
            err = "runtime.mode must be \"native\" or \"managed\"";
            return false;
        }
    }

    hc_json_free(root);
    return true;
}

} // namespace hcapp
