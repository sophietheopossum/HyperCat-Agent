#ifndef HC_CONDUCTOR_PROMPT_HPP
#define HC_CONDUCTOR_PROMPT_HPP

/* conductor_prompt — assemble + validate the conductor's system prompt as an IMMUTABLE SPINE + a
 * MUTABLE VOICE SLOT. One responsibility: turn an operator-authored "persona" string into the finished
 * system prompt the conductor runs, while guaranteeing the things that must never be editable stay fixed.
 *
 * The canonical prompt (HyperCat's authored identity + voice + conduct floor + operational body; SOUL.md is
 * the source wording) is split into three slices of ONE literal:
 *   - the IDENTITY spine ("Who you are" — "You are HyperCat ..."), always prepended FIRST,
 *   - the default VOICE slot ("Your voice ..."), the value used when the operator left the persona empty,
 *   - the conduct/operational FLOOR ("How you hold yourself" ... the DATA-not-instructions + approval-gate
 *     boundary), always RE-ASSERTED LAST so a persona can never weaken it.
 * A custom persona replaces only the voice slot, fenced + wrapped so the model treats it as voice-only
 * configuration it cannot escape; identity and floor are untouched. By construction
 * assemble_conductor_prompt("") reproduces the historical kPersona byte-for-byte (the pieces are slices of
 * the same literal), so the default behaviour is provably unchanged.
 *
 * SECURITY: the slot is operator-authored configuration (lower threat than runtime tool/memory data), but
 * validate_persona still applies defence-in-depth — length cap, control-byte strip, fence-marker and
 * chat-role-marker neutralization — and assemble fences + wraps it. The host is the trust boundary: it
 * validates at assembly, so even a hand-edited or planted persona value is sanitized before it reaches the
 * model. No agent tool can set the persona (it is operator-only config; see the threat table in doc 08).
 *
 * PURE: <string> + hc::prompt_defang only — no I/O, no settings, no LLM. The caller owns the returned
 * strings. Reentrant. */

#include <cstddef>
#include <string>

namespace hcapp {

/* The custom-persona byte cap. Generous for a voice description; bounds a planted/huge value. The settings
 * layer also caps at load (length only) so the field is bounded even before it reaches assembly. */
constexpr size_t kMaxPersonaBytes = 8u * 1024;

/* A built-in persona preset the UI offers. `text` is the slot value the preset selects: the empty string
 * for `canonical` (empty slot => the canonical default voice) and for the `custom` sentinel (leave the
 * operator's own text). Presets only ever fill the SLOT — none can alter identity or the floor. */
struct PersonaPreset {
    const char *key;   /* stable id (e.g. "canonical", "neutral", "custom") */
    const char *label; /* display label */
    const char *text;  /* the slot text this preset applies ("" => canonical default / custom) */
};

/* The static preset table (count via `n`). [0] is always the canonical default. */
const PersonaPreset *persona_presets(std::size_t *n);

/* The immutable spine pieces + the canonical default voice — for the UI's locked-vs-editable preview and
 * for seeding the editor. DISPLAY ONLY: the authoritative assembly is assemble_conductor_prompt's, server
 * side. The returned pointers are valid for the process lifetime. */
const char *conductor_spine_identity();  /* the locked identity preamble ("You are HyperCat ...")  */
const char *conductor_persona_default(); /* the canonical default voice (the empty-slot value)      */
const char *conductor_spine_floor();     /* the locked conduct + operational body (always last)     */

/* Validate + sanitize an operator-authored persona slot into a value safe to splice into the fence.
 * Whitespace-only => "" (the canonical-default sentinel; NOT an error). Caps to kMaxPersonaBytes on a
 * UTF-8 boundary, strips ASCII control bytes (keeps \n and \t), neutralizes the fence markers and the
 * chat-role turn-boundary markers. Idempotent: validate_persona(validate_persona(x)) == validate_persona(x). */
std::string validate_persona(const std::string &raw);

/* Assemble the conductor system prompt from a (validated) persona slot. Empty => identity + canonical
 * default voice + floor, byte-identical to the historical kPersona. Non-empty => identity + a fenced,
 * wrapped voice slot + the re-asserted floor. The result always contains the identity anchor and the
 * conduct floor, with identity before the slot and the floor after it, for ANY slot value. */
std::string assemble_conductor_prompt(const std::string &persona_slot);

} // namespace hcapp

#endif /* HC_CONDUCTOR_PROMPT_HPP */
