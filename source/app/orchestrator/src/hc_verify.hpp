#ifndef HC_VERIFY_HPP
#define HC_VERIFY_HPP

/* hc_verify — the PURE verification policy (P04). Tally sibling verdicts into an outcome + assign diverse
 * review lenses. No I/O, no state — the engine calls it; the unit test drives it directly.
 *
 * Scope: INTERNAL to hc_orchestrator (src/) — the engine's decision helper, not a public contract. */

#include "hc_orch_model.hpp" /* hc::orch::Verdict */

#include <string>
#include <vector>

namespace hc::orch {

/* The aggregate outcome of a verification round. */
enum class VerifyOutcome {
    Pending,  /* more verdicts expected (budget not yet spent, no quorum, no refute)   */
    Upheld,   /* >= quorum upholds, no refute -> ACCEPT                                */
    Refuted,  /* >= 1 credible refute -> REJECT (a single refute is decisive)           */
    Unproven  /* budget spent without a quorum or a refute -> accept-with-caveat        */
};

/* Tally the verdicts seen so far. ASYMMETRIC by design: a single Refute is decisive (Refuted); `quorum`
 * Upholds accept (Upheld); once `reported >= budget` with neither, the result is Unproven; else Pending. */
VerifyOutcome tally(int upholds, int refutes, int reported, int quorum, int budget);

/* Map a verifier's verdict word into a Verdict (lenient — anything not clearly uphold/refute is Uncertain,
 * which never accepts and never refutes on its own). */
Verdict     verdict_from_str(const std::string &s);
const char *verdict_str(Verdict);

/* Assign `n` distinct review lenses (correctness, completeness, edge-cases, efficiency) so multiple
 * verifiers check DIFFERENT failure modes rather than redundantly repeating one. */
std::vector<std::string> assign_lenses(int n);

} // namespace hc::orch

#endif /* HC_VERIFY_HPP */
