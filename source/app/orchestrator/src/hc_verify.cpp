/* hc_verify — see hc_verify.hpp. Pure tally + lens assignment; no I/O, no state. */

#include "hc_verify.hpp"

namespace hc::orch {

VerifyOutcome tally(int upholds, int refutes, int reported, int quorum, int budget)
{
    if (refutes >= 1) return VerifyOutcome::Refuted;        /* a single credible refute is decisive    */
    if (upholds >= quorum) return VerifyOutcome::Upheld;    /* enough independent upholds -> accept     */
    if (reported >= budget) return VerifyOutcome::Unproven; /* verifiers exhausted, inconclusive        */
    return VerifyOutcome::Pending;                          /* wait for more verdicts                   */
}

Verdict verdict_from_str(const std::string &s)
{
    if (s == "uphold" || s == "upheld" || s == "pass" || s == "correct" || s == "valid")
        return Verdict::Uphold;
    if (s == "refute" || s == "refuted" || s == "fail" || s == "incorrect" || s == "reject"
        || s == "invalid")
        return Verdict::Refute;
    return Verdict::Uncertain;
}

const char *verdict_str(Verdict v)
{
    switch (v) {
    case Verdict::Uphold:    return "uphold";
    case Verdict::Refute:    return "refute";
    case Verdict::Uncertain: return "uncertain";
    }
    return "uncertain";
}

std::vector<std::string> assign_lenses(int n)
{
    static const char *const kLenses[] = {"correctness", "completeness", "edge cases", "efficiency"};
    std::vector<std::string>  out;
    for (int i = 0; i < n; i++) out.emplace_back(kLenses[i % 4]);
    return out;
}

} // namespace hc::orch
