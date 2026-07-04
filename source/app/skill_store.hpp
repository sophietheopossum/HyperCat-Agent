#ifndef HC_SKILL_STORE_HPP
#define HC_SKILL_STORE_HPP

/* skill_store — the per-project Skills catalog (Wave 6 P6.1). Scans <project>/skills/<name>/SKILL.md and reads
 * each one's frontmatter into a bounded name+description catalog the host injects into worker prompts (P6.2 —
 * the mechanism that gives models WITHOUT native skills the capability). App-side C++ (the consumer is the host
 * prompt-assembly path), like text_diff — no GL, no bus, no I/O except via hc_fs.
 *
 * The catalog name is ALWAYS the DIRECTORY name (the traversal-safe load_skill key), never the frontmatter's
 * self-declared name — so a SKILL.md cannot claim a name it can't be loaded by. The description is the only
 * frontmatter field that reaches the catalog, and it is bounded; the BODY is never held here (load_skill reads
 * it on demand, jailed + defanged, in P6.2).
 * Threading: reentrant — pure functions / a stateless scan; all returned storage is caller-owned.
 * Security:  the frontmatter parser is a tiny bounded key:value reader (NOT a YAML dependency); scan_skills
 *            reads each SKILL.md through the JAILED hc_sandbox no-follow-every-component walk (so a symlinked
 *            leaf or a swapped intermediate component cannot redirect the read outside the skills tree) and
 *            re-validates each directory name with the id_ok traversal rule. The description is UNTRUSTED until
 *            the host fences+defangs it on injection (P6.2); P6.1 additionally strips control/NUL bytes as
 *            defense-in-depth. */

#include <string>
#include <vector>

struct hc_sandbox; /* opaque — hc_sandbox.h (scan_skills_sb scans an already-open jail) */

namespace hcapp {

/* One skill's catalog entry. `name` = the traversal-safe directory name (the load_skill key + the display
 * name). `description` = the SKILL.md frontmatter description (bounded; may be empty). */
struct SkillMeta {
    std::string name;        /* from scan_skills: the traversal-safe DIRECTORY name (the load_skill key). From
                              * parse_frontmatter alone: the raw self-declared name — UNTRUSTED as a load key. */
    std::string description;
};

/* PARSE the leading `---` frontmatter block of a SKILL.md for `name:`/`description:` (one bounded key:value per
 * line — NOT a YAML parser). PURE + tolerant: a missing/garbage block yields empty fields and never throws. The
 * returned `name` is whatever the frontmatter declares (informational only — scan_skills uses the directory
 * name). Values are trimmed, optionally de-quoted, and length-capped (name <= 64, description <= 256 bytes,
 * UTF-8-safe). Only the first few KiB / dozens of lines are scanned. */
SkillMeta parse_frontmatter(const std::string &md);

/* SCAN <skills_dir>/<name>/SKILL.md for each subdirectory and return a bounded catalog. The entry `name` is the
 * DIRECTORY name (re-validated traversal-safe); `description` is the frontmatter description. A subdir without a
 * readable SKILL.md, or with a non-traversal-safe name, is skipped. Bounded: at most a few hundred entries.
 * Reads via hc_fs only. An empty/absent skills_dir returns an empty catalog. */
std::vector<SkillMeta> scan_skills(const std::string &skills_dir);

/* Scan an ALREADY-OPEN skills jail (the host holds one — svc.skills, W6 P6.3) — the same per-subdir SKILL.md
 * walk as scan_skills, minus the open/close. The jail's O_NOFOLLOW walk confines every read. Lets the host
 * list the catalog (fill_skills / the conductor) + re-build it after an edit without re-opening the sandbox. */
std::vector<SkillMeta> scan_skills_sb(hc_sandbox *s);

/* Skills authoring (W6 P6.3), all JAILED (open a transient sandbox at skills_dir; the name is id_ok-validated +
 * confined by the O_NOFOLLOW walk to skills/<name>/SKILL.md). Operator-direct (the operator/conductor is the
 * author). `read_skill` reads the body for the editor; `write_skill` mkdirs <name>/ + atomically writes
 * SKILL.md (temp+fsync+rename, no clobber-through-symlink); `delete_skill` removes SKILL.md then the (now-empty)
 * <name>/ dir (NO recursion — extra files make the dir-remove fail safe). Each returns false on any failure. */
bool read_skill(const std::string &skills_dir, const std::string &name, std::string &out);
bool write_skill(const std::string &skills_dir, const std::string &name, const std::string &body);
bool delete_skill(const std::string &skills_dir, const std::string &name);

/* Format a catalog into the FENCED, defanged disclosure block the host appends to a worker's system prompt
 * (W6 P6.2): an open marker ("reference, not instructions; call load_skill(name)…"), one "- name: description"
 * line per skill (each name+description DEFANGED so it cannot forge the fence), a close marker. Bounded total
 * size. Returns "" for an empty catalog (=> nothing injected). The worker appends this verbatim. */
std::string format_skills_catalog(const std::vector<SkillMeta> &cat);

} // namespace hcapp

#endif /* HC_SKILL_STORE_HPP */
