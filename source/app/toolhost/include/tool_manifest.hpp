#ifndef HC_TOOL_MANIFEST_HPP
#define HC_TOOL_MANIFEST_HPP

/* tool_manifest — PURE parse + validate of a third-party tool's manifest.json into a bounded, sanitized value.
 * One responsibility: turn untrusted on-disk JSON into a ToolManifest the ToolHost can trust, or REJECT it. No
 * I/O (the caller reads the file, bounded), no bus, no exec, no LLM. Mirrors the worker_role / skill_store
 * pure-parser discipline: tolerant, bounded, never throws; unknown keys ignored; malformed => false + a reason.
 *
 * SECURITY — this is the install-time gate (defense-in-depth before a tool binary is ever launched):
 *   - REJECT if the manifest `id` != the install DIRECTORY name (a tool cannot claim an id it can't be loaded
 *     by — the same traversal-safe rule skills use: the directory is authoritative).
 *   - REJECT if `manifest_version` is unsupported (forward-compat tripwire).
 *   - REJECT if any declared tool function name is_reserved_tool_name() (anti-spoof: no shadowing fs_write etc.)
 *     or is empty / malformed.
 *   - CLAMP every limit to a host maximum; DEFAULT permissions to deny (fs=None, no egress, no exec).
 * The host then operator-reviews + hash-pins the (approved) manifest; this only decides structural validity.
 *
 * NOTE: distinct from hc::ui::ToolManifest (the DISPLAY-only POD in hc_ui.hpp) — this is the host-internal
 * PARSED on-disk manifest. Depends on hc::json + hc::tooling only. */

#include <cstddef>
#include <string>
#include <vector>

namespace hcapp {

/* host maxima (the limits are clamped to these; the SDK author docs reference them). */
constexpr int    kToolManifestVersion = 1; /* the only supported manifest_version */
constexpr size_t kToolMaxFns = 16;         /* a tool binary may expose at most this many functions */
constexpr int    kToolMaxTimeoutMs = 60000;
constexpr int    kToolMaxOutputBytes = 1024 * 1024;
constexpr int    kToolMaxCpuSeconds = 60;
constexpr long   kToolMaxMemBytes = 2L * 1024 * 1024 * 1024;
constexpr int    kToolMaxFiles = 1024;
constexpr size_t kToolMaxEgressHosts = 32;
constexpr size_t kToolMaxExecPaths = 32;

enum class ToolFsMode { None, Read, ReadWrite };

/* How the tool is run. Native = a compiled binary at bin/<id> that SELF-confines (the C SDK or the Rust crate;
 * strict floor). Managed = a non-self-confining program (an interpreter + an in-package entry script) that the
 * host LAUNCHER (hc_tool_launch) jails then exec's under the looser MANAGED_RUNTIME floor (the non-C path). */
enum class ToolRuntime { Native, Managed };

/* The interpreter-name allowlist for managed tools: a manifest may only name one of these (the host resolves the
 * name to a SYSTEM binary at launch — a package can never supply its own interpreter). Keeps the managed exec
 * target a known, operator-trusted runtime rather than arbitrary bytes. */
bool tool_interpreter_allowed(const std::string &name);

/* one function the tool binary exposes to the model */
struct ToolFn {
    std::string name;              /* the model-facing tool name (REJECTED if reserved / empty) */
    std::string spec_json;         /* the OpenAI-style function spec (a raw JSON object, kept as a string) */
    bool        sensitive = false; /* a CALL needs the per-call human gate (operator-declared) */
};

struct ToolManifest {
    int         manifest_version = 0;
    std::string id; /* MUST equal the install dir name */
    std::string name, description, author, version;
    std::vector<ToolFn> tools;
    /* runtime (the non-C path) — DEFAULT native (today's behaviour; bin/<id> self-confines). For managed, the
     * host launcher jails + exec's `interpreter` (an allowlisted system runtime) on the in-package `entry` script. */
    ToolRuntime runtime = ToolRuntime::Native;
    std::string interpreter; /* managed only: an allowlisted name (e.g. "python3"); "" for native */
    std::string entry;       /* managed only: the entry script, a single in-package path (no '/' or ".."); "" native */
    /* permissions — DEFAULT-DENY */
    ToolFsMode               fs_mode = ToolFsMode::None; /* workspace fs access the launcher grants */
    std::vector<std::string> egress_hosts;              /* exact hostnames; [] => no network */
    std::vector<std::string> exec_allow;                /* absolute binary paths; [] => no exec */
    /* limits — CLAMPED to the host maxima above */
    int  timeout_ms = 10000;
    int  max_output_bytes = 262144;
    int  cpu_seconds = 10;
    long mem_bytes = 536870912;
    int  max_files = 256;

    /* True if this tool's ENVELOPE is sensitive (grants fs-write/egress/exec): such a tool's calls route through
     * the per-call operator gate even when a function isn't individually flagged `sensitive`. */
    bool envelope_sensitive() const
    {
        return fs_mode == ToolFsMode::ReadWrite || !egress_hosts.empty() || !exec_allow.empty();
    }

    /* True if this tool's envelope grants network egress and NOTHING ELSE that can change the machine: no
     * workspace writes, no exec. Such a tool is still envelope_sensitive() -- it reaches the network, and its
     * calls still gate by default -- but it is a materially different risk class from one that can write files
     * or run binaries: the worst case is reading a page the operator did not sanction, not modifying anything.
     *
     * This exists so the operator can opt into auto-approving THAT class alone (settings
     * auto_approve_readonly_egress) instead of being forced to choose between prompting on every single fetch
     * and arming allow-all, which would also auto-approve exec. A research task is tens of fetches; without
     * this the only unattended option was the most dangerous switch in the system.
     *
     * Deliberately NOT satisfied by fs_mode == ReadOnly: read-only workspace access plus egress is an
     * exfiltration path, and this predicate must stay boring. */
    bool envelope_readonly_egress() const
    {
        return !egress_hosts.empty() && fs_mode == ToolFsMode::None && exec_allow.empty();
    }
};

const char *tool_fs_mode_str(ToolFsMode m);

/* Parse + validate `json` (`len` bytes) for a tool installed at directory `dir_id`. Returns true with `out`
 * populated + clamped on success; false with `err` set to a short reason on rejection. Bounded; never throws. */
bool tool_manifest_parse(const char *json, size_t len, const std::string &dir_id, ToolManifest &out,
                         std::string &err);

} // namespace hcapp

#endif /* HC_TOOL_MANIFEST_HPP */
