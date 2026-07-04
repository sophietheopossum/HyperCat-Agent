/* hc_roles — see hc_roles.hpp. The worker role table: value types + pure JSON (de)serialize / validate /
 * load / save + the built-in defaults. Depends on hc::json + hc::fs only. */

#include "hc_roles.hpp"

#include "hc_fs.h"
#include "hc_json.h"

#include <algorithm>
#include <cstdlib>

namespace hcapp {

namespace {
constexpr size_t kMaxRoles = 64;             /* bound a hostile/typo'd role file              */
constexpr size_t kMaxRoleFileBytes = 256u * 1024; /* the load cap                              */
constexpr size_t kMaxRoleNameBytes = 64;          /* a role key is a short capability id; it rides --role argv */
constexpr size_t kMaxModelBytes = 256;            /* a model id is short; it rides --model argv — cap for symmetry */
constexpr size_t kMaxOverlayBytes = 8u * 1024;    /* bound a single role's prompt overlay (it rides --role-prompt
                                                   * argv + is APPENDED to every worker prompt of that role) */
constexpr size_t kMaxScopeEntries = 32;           /* P06: cap a role's exec/fs-path list (a hostile/typo'd file) */
constexpr size_t kMaxScopePathBytes = 512;        /* P06: cap one exec/fs-path entry (a path; rides argv later)  */

/* P06: sanitize a role's exec/fs scope list in place — cap the count, trim each entry to the byte cap, and DROP
 * empty entries + any entry containing ';' (the separator the paths will ride on as a spawn-arg csv), so the
 * record can never carry a value that would corrupt a ';'-joined arg. Idempotent (re-running changes nothing). */
void sanitize_scope_list(std::vector<std::string> &v)
{
    std::vector<std::string> kept;
    for (auto &s : v) {
        if (kept.size() >= kMaxScopeEntries) break;
        if (s.empty()) continue;
        if (s.size() > kMaxScopePathBytes) s.resize(kMaxScopePathBytes);
        if (s.find(';') != std::string::npos) continue;
        kept.push_back(std::move(s));
    }
    v = std::move(kept);
}

/* P06: parse a bounded JSON string array (exec_allow / fs_*_paths) into `out`, then sanitize it. */
void parse_scope_list(const hc_json *e, const char *key, std::vector<std::string> &out)
{
    const hc_json *arr = hc_json_get(e, key);
    if (!arr) return;
    size_t n = hc_json_arr_len(arr);
    for (size_t i = 0; i < n; i++) {
        const char *s = hc_json_as_str(hc_json_arr_at(arr, i), "");
        if (s && s[0]) out.emplace_back(s);
    }
    sanitize_scope_list(out);
}

/* The agentd tool ids, indexed by RoleTool. Keep in lockstep with the enum + register_agent_tools. */
const char *const kToolNames[] = {"deep_reason", "memory_recall", "memory_write", "fs_read",
                                  "fs_list",     "fs_write",      "fs_update",    "load_skill"};
constexpr size_t  kToolCount = sizeof kToolNames / sizeof kToolNames[0];
} // namespace

const char *role_tool_name(RoleTool t)
{
    size_t i = (size_t)t;
    return i < kToolCount ? kToolNames[i] : "";
}

bool role_tool_from_name(const std::string &name, RoleTool &out)
{
    for (size_t i = 0; i < kToolCount; i++)
        if (name == kToolNames[i]) {
            out = (RoleTool)i;
            return true;
        }
    return false;
}

size_t role_tool_count() { return kToolCount; }

std::string role_tools_csv(const RoleDef &d)
{
    std::string s;
    for (size_t i = 0; i < d.tools.size(); i++) {
        if (i) s += ",";
        s += role_tool_name(d.tools[i]);
    }
    return s; /* empty toolset (= all) -> "" */
}

const RoleDef *roletable_find(const RoleTable &t, const std::string &role)
{
    for (const auto &r : t.roles)
        if (r.role == role) return &r;
    return nullptr;
}

bool roletable_parse(const char *json, size_t len, RoleTable &out)
{
    if (!json || len == 0) return false;
    hc_json *root = hc_json_parse(json, len);
    if (!root) return false;
    RoleTable t;
    t.version = hc_json_get_int(root, "version", 1);
    if (const hc_json *roles = hc_json_get(root, "roles")) {
        size_t n = hc_json_arr_len(roles);
        for (size_t i = 0; i < n && t.roles.size() < kMaxRoles; i++) {
            const hc_json *e = hc_json_arr_at(roles, i);
            if (!e) continue;
            RoleDef d;
            d.role = hc_json_get_str(e, "role", "");
            d.prompt_overlay = hc_json_get_str(e, "prompt_overlay", "");
            d.model = hc_json_get_str(e, "model", "");
            if (const hc_json *tools = hc_json_get(e, "tools")) {
                size_t tn = hc_json_arr_len(tools);
                for (size_t j = 0; j < tn; j++) {
                    const char *nm = hc_json_as_str(hc_json_arr_at(tools, j), "");
                    RoleTool    rt;
                    if (nm && nm[0] && role_tool_from_name(nm, rt)) d.tools.push_back(rt); /* unknown -> skip */
                }
            }
            parse_scope_list(e, "exec_allow", d.exec_allow);         /* P06: the per-role exec intersection */
            parse_scope_list(e, "fs_write_paths", d.fs_write_paths); /* P06 record (enforced by P07/P09)    */
            parse_scope_list(e, "fs_read_paths", d.fs_read_paths);
            t.roles.push_back(std::move(d));
        }
    }
    hc_json_free(root);
    roletable_validate(t);
    out = std::move(t);
    return true;
}

std::string roletable_serialize(const RoleTable &t)
{
    hc_json *root = hc_json_new_object();
    if (!root) return "";
    hc_json_obj_set_int(root, "version", t.version);
    if (hc_json *roles = hc_json_new_array()) {
        for (const auto &d : t.roles) {
            hc_json *e = hc_json_new_object();
            if (!e) continue;
            hc_json_obj_set_str(e, "role", d.role.c_str());
            hc_json_obj_set_str(e, "prompt_overlay", d.prompt_overlay.c_str());
            hc_json_obj_set_str(e, "model", d.model.c_str());
            if (hc_json *tools = hc_json_new_array()) {
                for (RoleTool rt : d.tools) hc_json_arr_append_str(tools, role_tool_name(rt));
                hc_json_obj_set(e, "tools", tools); /* adopts */
            }
            /* P06: emit the scope lists ONLY when non-empty, so an unconfigured role serializes byte-identically
             * to before (no empty-array noise; an existing roles.json round-trips unchanged). */
            auto emit_scope = [&](const char *key, const std::vector<std::string> &v) {
                if (v.empty()) return;
                if (hc_json *a = hc_json_new_array()) {
                    for (const auto &s : v) hc_json_arr_append_str(a, s.c_str());
                    hc_json_obj_set(e, key, a); /* adopts */
                }
            };
            emit_scope("exec_allow", d.exec_allow);
            emit_scope("fs_write_paths", d.fs_write_paths);
            emit_scope("fs_read_paths", d.fs_read_paths);
            hc_json_arr_append(roles, e); /* adopts */
        }
        hc_json_obj_set(root, "roles", roles); /* adopts */
    }
    char       *txt = hc_json_print_canonical(root);
    std::string out = txt ? txt : "";
    free(txt);
    hc_json_free(root);
    return out;
}

void roletable_validate(RoleTable &t)
{
    if (t.version < 1) t.version = 1;
    std::vector<RoleDef> kept;
    for (auto &d : t.roles) {
        if (d.role.empty()) continue;              /* a role with no key is meaningless */
        if (d.role.size() > kMaxRoleNameBytes) d.role.resize(kMaxRoleNameBytes);    /* a short capability id */
        if (d.model.size() > kMaxModelBytes) d.model.resize(kMaxModelBytes);        /* a short model id (argv) */
        if (d.prompt_overlay.size() > kMaxOverlayBytes) {
            /* bound the overlay, but never split a UTF-8 sequence (it is re-serialized to JSON on save): back the
             * cut off any continuation bytes (10xxxxxx) so the kept prefix ends on a whole character. */
            size_t n = kMaxOverlayBytes;
            while (n > 0 && (static_cast<unsigned char>(d.prompt_overlay[n]) & 0xC0) == 0x80) n--;
            d.prompt_overlay.resize(n);
        }
        std::sort(d.tools.begin(), d.tools.end()); /* dedup the toolset (an enum set)   */
        d.tools.erase(std::unique(d.tools.begin(), d.tools.end()), d.tools.end());
        sanitize_scope_list(d.exec_allow); /* P06: bound + drop empty/';' entries (also for programmatic tables) */
        sanitize_scope_list(d.fs_write_paths);
        sanitize_scope_list(d.fs_read_paths);
        kept.push_back(std::move(d));
        if (kept.size() >= kMaxRoles) break;
    }
    t.roles = std::move(kept);
}

bool roletable_load(const char *path, RoleTable &out)
{
    if (!path || !*path) return false;
    size_t len = 0;
    char  *buf = hc_fs_read_file(path, kMaxRoleFileBytes, &len); /* NULL on missing/oversized */
    if (!buf) return false;
    bool ok = roletable_parse(buf, len, out);
    free(buf);
    return ok;
}

bool roletable_save(const RoleTable &t, const char *path)
{
    if (!path || !*path) return false;
    std::string txt = roletable_serialize(t);
    if (txt.empty()) return false;
    return hc_fs_atomic_write(path, txt.data(), txt.size()) == 0;
}

RoleTable roletable_builtin_defaults()
{
    using T = RoleTool;
    RoleTable t;
    /* dev — the coding brain: writes/edits files, structures the project, verifies its own work. ALL tools. */
    t.roles.push_back(
        {"dev",
         "You are the fleet's DEVELOPER. Build working code. Write your deliverable to the named file with "
         "fs_write (a new file) or fs_update (edit an existing one) — never paste code as the answer. Look at "
         "what's there first with fs_list/fs_read, and read the file back to confirm it landed. Prefer small, "
         "correct, runnable units over sprawling stubs, and lay the project out sensibly.",
         {T::DeepReason, T::MemoryRecall, T::MemoryWrite, T::FsRead, T::FsList, T::FsWrite, T::FsUpdate, T::LoadSkill},
         ""});
    /* qa — skeptical testing: reads the code, hunts edge cases, writes tests to files. ALL tools. */
    t.roles.push_back(
        {"qa",
         "You are the fleet's QA. Be skeptical: find the edge cases, the off-by-ones, the unhandled errors. "
         "Read the code under test with fs_read before you judge it; when you write a test, write it to a "
         "file with fs_write. Report what actually fails, not what should pass.",
         {T::DeepReason, T::MemoryRecall, T::MemoryWrite, T::FsRead, T::FsList, T::FsWrite, T::FsUpdate, T::LoadSkill},
         ""});
    /* research — investigate + synthesize; informs the build, does NOT write its code (no write tools). */
    t.roles.push_back(
        {"research",
         "You are the fleet's RESEARCHER. Investigate and gather: recall what the fleet already knows, reason "
         "carefully about the hard questions, and hand back a clear, well-grounded synthesis. You inform the "
         "build; you do not write its code.",
         {T::DeepReason, T::MemoryRecall, T::MemoryWrite, T::FsRead, T::FsList, T::LoadSkill},
         ""});
    /* ops — wiring + glue + keeping things running; writes the artifacts it produces. ALL tools. */
    t.roles.push_back(
        {"ops",
         "You are the fleet's OPS. Wire things together and keep them running: configuration, glue, scripts, "
         "and the connective tissue between components. Write the artifacts you produce to files with fs_write "
         "or fs_update.",
         {T::DeepReason, T::MemoryRecall, T::MemoryWrite, T::FsRead, T::FsList, T::FsWrite, T::FsUpdate, T::LoadSkill},
         ""});
    /* generalist — the flexible catch-all for work that doesn't fit a named specialist: the normalization landing for
     * a planner's invented capability AND the worker the conductor can add when a goal needs a role nobody set up.
     * Full dev-equivalent toolset — NO new privilege (dev/qa/ops already carry this exact set; tools stay a subset of
     * the global allowlists, and every irreversible action still passes the AuthGate + the sandbox). */
    t.roles.push_back(
        {"generalist",
         "You are a GENERALIST — a flexible worker for tasks that don't fit a named specialist. Use the right tool "
         "for the job: read what's there with fs_read/fs_list, write your deliverable to the named file with fs_write "
         "or fs_update (never paste it as the answer), and reason carefully when the task is hard. Hold your scope "
         "honestly: do the task you were handed, and if it genuinely needs something a sandboxed workspace agent "
         "can't do, say so plainly and hand back rather than guessing.",
         {T::DeepReason, T::MemoryRecall, T::MemoryWrite, T::FsRead, T::FsList, T::FsWrite, T::FsUpdate, T::LoadSkill},
         ""});
    return t;
}

} // namespace hcapp
