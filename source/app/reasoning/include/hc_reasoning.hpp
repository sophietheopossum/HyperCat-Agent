#ifndef HC_REASONING_HPP
#define HC_REASONING_HPP

/* hc_reasoning — the optional staged `deep_reason` tool (C++ host service).
 *
 * Purpose:   the genuinely-useful staged-reasoning kernel (doc 11), rebuilt small and
 *            grounded: a linear 5-stage prompt chain — Decompose -> Analyze -> Critique ->
 *            Synthesize -> Reflect — where each stage's output (with its evidence GRADES) is
 *            threaded into the next stage's prompt, so a [Fact]/[Inference]/[Speculation] grade is
 *            carried THROUGH the chain rather than emitted once and forgotten (the drift the
 *            original failed to enforce). It is prompt-chaining over hc_llm, ~a couple hundred LOC,
 *            NOT a framework. The drift (autonomous goals, awareness scores, multi-perspective
 *            theater, "value profundity") is deleted, not re-imported.
 * Owns:      nothing process-global. It BORROWS an hc_llm client (the caller creates and frees it).
 * Threading: single-owner, not thread-safe (it drives one hc_llm, which drives one hc_http). Runs
 *            on the caller's thread; it is OFF by default — nothing runs until reason() is called.
 *            The host wires `deep_reason` onto the bus elsewhere; this module knows nothing of it.
 */

#include <string>

struct hc_llm; /* opaque C handle, borrowed; created via hc_llm.h by the caller */

namespace hc {

struct ReasonResult {
    std::string decompose;       /* per-stage structured output */
    std::string analyze;
    std::string critique;
    std::string synthesize;
    std::string reflect;
    std::string answer;          /* the synthesized answer (from the Synthesize stage)      */
    std::string confidence;      /* the calibrated what-is-solid/uncertain summary (Reflect) */
    bool        complete = false; /* true iff all five stages ran and produced output        */
};

class Reasoner {
public:
    /* Borrow an hc_llm client (the caller owns and frees it). Returns null on a bad argument.
     * Caller owns the returned pointer; delete it to free. */
    static Reasoner *create(::hc_llm *llm);
    ~Reasoner();

    /* Run the 5-stage chain on `query`. Synchronous (a host service an agent calls over the bus);
     * returns the assembled result. On an LLM failure at any stage, returns with complete==false
     * and the stages completed so far. */
    ReasonResult reason(const std::string &query);

private:
    Reasoner();
    struct Impl;
    Impl *p_;
};

} // namespace hc

#endif /* HC_REASONING_HPP */
