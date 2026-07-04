#ifndef HC_PROMPT_DEFANG_HPP
#define HC_PROMPT_DEFANG_HPP

/* prompt_defang — pure neutralizers for UNTRUSTED text injected into an LLM prompt (memory hits, skill
 * descriptions, skill bodies). The FENCE ("reference, not instructions") plus these defangs are the prompt-
 * injection mitigation; this is shared so the memory path (host_bridge) and the new skills path (agentd
 * worker) apply the SAME proven technique rather than two drifting copies. PURE: <string> only, no I/O — so
 * both the host and the separate agentd process link it (the plan's "factor the pure fn so agentd links it").
 *
 * The "neutralize markers" technique (from the proven memory fence): the caller wraps untrusted text between a
 * literal open/close marker (e.g. "[skill — reference]" ... "[end skill]"); defang replaces the OPENING '[' of
 * any occurrence of those markers INSIDE the untrusted text with '(', so the text cannot forge the delimiter to
 * escape the fence and smuggle an instruction line after a fake close.
 * Threading: reentrant. Lifetime: returned strings are caller-owned. */

#include <initializer_list>
#include <string>

namespace hcapp {

/* INLINE defang — for SHORT, single-line injected items (a memory hit, a skill description): collapse '\n'
 * '\r' '\t' to a space (so the text cannot inject a fresh directive line) AND neutralize each fence marker.
 * (This reproduces the proven host_bridge memory defang exactly.) */
std::string defang_inline(const std::string &raw, std::initializer_list<const char *> markers);

/* BLOCK defang — for a MULTI-LINE untrusted body (a skill's SKILL.md shown as reference): strip ASCII control
 * bytes EXCEPT '\n' and '\t' (preserve the document's structure; drop NUL/ESC/BEL/etc. and 0x7f) AND
 * neutralize each fence marker. Newlines are KEPT (unlike defang_inline) because the body is meant to be read
 * as a structured document — the fence + marker-neutralize is what contains it, not line-flattening. */
std::string defang_block(const std::string &raw, std::initializer_list<const char *> markers);

} // namespace hcapp

#endif /* HC_PROMPT_DEFANG_HPP */
