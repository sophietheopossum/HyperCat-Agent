/* hc_planner — the decompose call (the impure half: one LLM round-trip).
 *
 * Purpose:   prompt the model, accumulate the streamed text (bounded), extract + parse + validate the
 *            plan. Fails CLOSED to an empty plan so a garbled or hostile model response runs nothing.
 * Owns:      nothing — BORROWS the hc_llm for the duration of one call.
 * Threading: synchronous, single caller (the orchestrator's DRIVER thread, via set_decomposer); the
 *            borrowed hc_llm is used single-threaded. The caller frees the llm only after this returns.
 * Security:  the response is bounded (a flooding provider can't grow the buffer without limit); the pure
 *            parser/validator (hc_plan_validate.cpp) rejects an invalid plan before any task is built. */

#include "hc_planner.hpp"

#include "hc_llm.h"

#include <cstring>

namespace hc::planner {
namespace {

const char kPlanSystem[] =
    "You are a planning module for a multi-agent worker fleet. You turn a goal into a small, well-scoped "
    "task graph. You output ONLY valid JSON — never prose, never markdown, never code fences.";

/* Bound the accumulated response: any legitimate plan is far under this; the cap stops a flooding /
 * malfunctioning provider from growing host RAM for the whole 120 s call window. On overflow we abort the
 * stream (should_cancel) and fail closed. */
constexpr std::size_t kPlanRespMax = 4u * 1024 * 1024;

struct AccumCtx {
    std::string text;
    bool        overflow = false;
};

void accumulate(const char *delta, std::size_t n, void *user)
{
    AccumCtx *c = static_cast<AccumCtx *>(user);
    if (c->overflow) return;
    if (c->text.size() + n > kPlanRespMax) {
        c->overflow = true; /* the should_cancel below ends the stream */
        return;
    }
    c->text.append(delta, n);
}

bool stop_on_overflow(void *user) { return static_cast<AccumCtx *>(user)->overflow; }

} // namespace

std::vector<hc::orch::Task> decompose(hc_llm *llm, const std::string &goal,
                                      const std::vector<std::string> &allowed_caps, const PlanLimits &lim,
                                      std::string *why)
{
    auto fail = [&](const char *m) {
        if (why) *why = m;
        return std::vector<hc::orch::Task>();
    };
    if (!llm || goal.empty()) return fail("planner offline or empty goal");

    /* The vocabulary the planner picks from + the normalization target set. Empty (the operator pruned every role)
     * -> synthesize the generalist so decompose never reproduces the original "pick from []" failure. */
    std::vector<std::string> vocab = allowed_caps;
    if (vocab.empty()) vocab.push_back("generalist");

    std::string    user = build_plan_prompt(goal, vocab);
    hc_llm_message msgs[2];
    std::memset(msgs, 0, sizeof msgs);
    msgs[0].role = "system";
    msgs[0].content = kPlanSystem;
    msgs[1].role = "user";
    msgs[1].content = user.c_str();

    AccumCtx        acc;
    hc_llm_handlers h;
    std::memset(&h, 0, sizeof h);
    h.on_text = accumulate;
    h.should_cancel = stop_on_overflow;
    h.user = &acc;
    char fr[32] = {0};
    if (hc_llm_chat_stream(llm, msgs, 2, nullptr, &h, fr, sizeof fr) != HC_LLM_OK)
        return fail("the planner model call failed");
    if (acc.overflow) return fail("the plan response exceeded the size cap"); /* fail closed */

    /* the model may still wrap the object in ``` fences or add a stray sentence — extract the outermost
     * {...} before the strict parse (the parser itself stays strict for the offline tests). */
    std::size_t lb = acc.text.find('{');
    std::size_t rb = acc.text.rfind('}');
    std::string obj = (lb != std::string::npos && rb != std::string::npos && rb > lb)
                          ? acc.text.substr(lb, rb - lb + 1)
                          : acc.text;

    std::vector<hc::orch::Task> tasks;
    if (!parse_plan_json(obj, tasks)) return fail("the model did not return a valid plan (non-JSON)");
    normalize_capabilities(tasks, vocab);          /* A3: repair invented capabilities BEFORE validation */
    if (!validate_plan(tasks, vocab, lim, why)) return std::vector<hc::orch::Task>(); /* why already set */
    return tasks;
}

} // namespace hc::planner
