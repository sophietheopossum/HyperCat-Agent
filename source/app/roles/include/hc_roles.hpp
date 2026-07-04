#ifndef HC_ROLES_HPP
#define HC_ROLES_HPP

/* hc_roles — the declarative WORKER ROLE TABLE (Worker Revamp, Wave 2): capability -> {system-prompt overlay,
 * toolset, model}. A C++ HOST leaf, mirroring hc_settings: a plain value struct + pure parse / serialize /
 * validate / load / save + the built-in defaults. The host resolves a RoleTable at startup (the built-in
 * overlays + toolsets, overlaid with the operator's per-role model from settings) and threads each worker its
 * OWN slice at spawn (--role / --role-prompt / --role-tools / --model). It deliberately depends on hc::json +
 * hc::fs ONLY — never hc_ui, the orchestrator, agentd, or settings — so it stays a leaf the host maps from.
 *
 * Owns:      nothing persistent — RoleTable is a caller-owned value. A MISSING role file => the built-in
 *            defaults (which reproduce a sane fleet); a MALFORMED file degrades to defaults, never throws.
 * Threading: the free functions are reentrant over caller-owned values; no shared mutable state. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hcapp {

/* A worker tool a role may carry (a subset registered on its agent). Names match the agentd tool ids. */
enum class RoleTool : uint8_t {
    DeepReason,
    MemoryRecall,
    MemoryWrite,
    FsRead,
    FsList,
    FsWrite,
    FsUpdate,
    LoadSkill, /* W6 P6.2/P6.3: read a per-project skill body on demand (gated additionally by a skills dir) */
};

/* One role's definition. `role` is the spawn-time capability key ("dev"/"qa"/"research"/"ops"). The
 * `prompt_overlay` is APPENDED after the worker base prompt (empty = base only). `tools` is the registered
 * subset — EMPTY means "all tools" (the safe default, so an unconfigured role keeps full capability). `model`
 * is the chat model id for this role ("" = the global/fallback model).
 *
 * Wave 5 (P06) — the declarative fs/exec SCOPE record, the single contract the least-privilege layers read:
 *   `exec_allow`     — ENFORCED NOW (host-side): when non-empty, this role may run ONLY commands whose
 *                      canonical path is in BOTH the global operator exec allowlist AND this list (an
 *                      intersection — a role can only SUBTRACT, never widen). EMPTY = inherit the global
 *                      allowlist unchanged (today's behaviour). Entries are absolute binary paths.
 *   `fs_write_paths` / `fs_read_paths` — RECORDED here as the forward contract for OS confinement (P07) +
 *                      capability scope (P09) to enforce; workspace-relative path prefixes, EMPTY = no
 *                      narrowing (the whole jail, today's behaviour). Not yet hard-enforced (their consumer
 *                      threads + enforces them), so they are not surfaced as a UI control until then.
 * All three are OPTIONAL and fail-safe (empty = no narrowing) — an unconfigured role behaves exactly as before. */
struct RoleDef {
    std::string              role;
    std::string              prompt_overlay;
    std::vector<RoleTool>    tools;
    std::string              model;
    /* Default-initialized (C++17 aggregates allow this) so a 4-field brace-init of the older fields stays valid
     * AND warning-free under -Wmissing-field-initializers — an unconfigured role keeps all three empty. */
    std::vector<std::string> exec_allow{};     /* P06: per-role exec command intersection ([] = inherit global)   */
    std::vector<std::string> fs_write_paths{}; /* P06 record: write-allow prefixes (enforced by P07/P09; [] = all) */
    std::vector<std::string> fs_read_paths{};  /* P06 record: read-allow prefixes  (enforced by P07/P09; [] = all) */
};

/* The whole declarative map (operator-editable via a role file in a later landing; built-in defaults now). */
struct RoleTable {
    int                  version = 1;
    std::vector<RoleDef> roles;
};

/* The built-in defaults: dev / qa / research / ops with tailored overlays + curated toolsets and no model
 * override (the host overlays the operator's per-role model). Reproduces a sensible fleet out of the box. */
RoleTable roletable_builtin_defaults();

/* Find a role by key (nullptr if absent). */
const RoleDef *roletable_find(const RoleTable &t, const std::string &role);

/* Pure (de)serialization + validation, mirroring hc_settings. parse: a malformed/empty blob => false, `out`
 * untouched. serialize: the canonical JSON. validate: clamp the role count, drop empty-role entries, dedup a
 * role's tools. load/save: one file via hc::fs (a missing/oversized file => false; the caller falls back to
 * the built-in defaults). */
bool        roletable_parse(const char *json, size_t len, RoleTable &out);
std::string roletable_serialize(const RoleTable &t);
void        roletable_validate(RoleTable &t);
bool        roletable_load(const char *path, RoleTable &out);
bool        roletable_save(const RoleTable &t, const char *path);

/* RoleTool <-> the agentd tool-id string (for serialize + the spawn --role-tools csv). An unknown name =>
 * false (skipped by the parser — an unknown tool can never be granted). */
const char *role_tool_name(RoleTool t);
bool        role_tool_from_name(const std::string &name, RoleTool &out);

/* The number of RoleTool enumerators — lets a UI enumerate every tool as `role_tool_name((RoleTool)i)` for
 * i in [0, role_tool_count()) without hardcoding the count or reaching the private name table. */
size_t role_tool_count();

/* The role's tools as a comma-separated agentd-id list (for the --role-tools spawn arg). An EMPTY toolset
 * (= all) serializes to "" — the worker treats "" as all-tools, preserving today's behavior. */
std::string role_tools_csv(const RoleDef &d);

} // namespace hcapp

#endif /* HC_ROLES_HPP */
