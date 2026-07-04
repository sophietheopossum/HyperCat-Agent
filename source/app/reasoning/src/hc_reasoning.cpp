/* hc_reasoning — the 5-stage deep_reason chain. See hc_reasoning.hpp.
 *
 * Each stage is one hc_llm_chat_stream call: system = the stage prompt + the shared grade rule;
 * user = the query plus EVERY prior stage's output, so the model sees earlier graded claims and
 * can carry/downgrade them (the threading that makes grade-carry possible — the kernel doc 11
 * salvages). The streamed text is accumulated per stage. A failure at any stage returns early with
 * complete==false and whatever ran so far (honest partial result). No tools, no bus, no threads.
 * Lifetime: borrows hc_llm* for the Reasoner's life (caller owns/frees it); the only heap is the
 * pimpl (freed in the destructor) — no allocation persists beyond a reason() call. */

#include "hc_reasoning.hpp"

#include "hc_reasoning_prompts.hpp"

#include "hc_llm.h"

#include <string>

namespace hc {

namespace {

/* Per-stage accumulation ceiling — bounds host memory if a hostile or buggy provider streams
 * without end (the transport caps the wire at 16 MiB; this caps what we RETAIN and amplify across
 * stages). A genuine reasoning stage is far smaller; exceeding it cancels the stream so the stage
 * fails and the chain returns an honest partial result rather than letting the provider grow us. */
constexpr size_t kMaxStageBytes = 256u * 1024;

struct Accum {
    std::string *out;
    bool         overflowed = false;
};

void accum_text(const char *delta, size_t n, void *user)
{
    Accum *a = static_cast<Accum *>(user);
    if (a->out->size() >= kMaxStageBytes) {
        a->overflowed = true;
        return;
    }
    size_t room = kMaxStageBytes - a->out->size();
    a->out->append(delta, n > room ? room : n);
    if (n > room) a->overflowed = true;
}
void no_tool(const char *, const char *, const char *, void *) {}
bool cancel_on_overflow(void *user) { return static_cast<Accum *>(user)->overflowed; }

} // namespace

struct Reasoner::Impl {
    ::hc_llm *llm = nullptr;

    /* One stage. system = stage prompt + grade rule; user = query + prior stage outputs. Accumulate
     * the streamed text into `out`. false on an LLM error or empty output. */
    bool run_stage(const char *stage_prompt, const std::string &query, const std::string &prior,
                   std::string &out)
    {
        std::string system = std::string(stage_prompt) + "\n\n" + reasoning::kGradeRule;
        std::string user = query;
        if (!prior.empty()) user += "\n\n--- prior stages (carry the grades) ---\n" + prior;

        hc_llm_message msgs[2];
        msgs[0].role = "system";
        msgs[0].content = system.c_str();
        msgs[0].tool_call_id = nullptr;
        msgs[0].tool_calls_json = nullptr;
        msgs[1].role = "user";
        msgs[1].content = user.c_str();
        msgs[1].tool_call_id = nullptr;
        msgs[1].tool_calls_json = nullptr;

        Accum           acc{&out, false};
        hc_llm_handlers h;
        h.on_text = accum_text;
        h.on_tool_call = no_tool;
        h.should_cancel = cancel_on_overflow;
        h.user = &acc;

        hc_llm_status st = hc_llm_chat_stream(llm, msgs, 2, nullptr, &h, nullptr, 0);
        return st == HC_LLM_OK && !acc.overflowed && !out.empty();
    }
};

Reasoner::Reasoner() : p_(new Impl) {}
Reasoner::~Reasoner() { delete p_; }

Reasoner *Reasoner::create(::hc_llm *llm)
{
    if (!llm) return nullptr;
    Reasoner *r = new Reasoner();
    r->p_->llm = llm;
    return r;
}

ReasonResult Reasoner::reason(const std::string &query)
{
    ReasonResult res;
    std::string  prior; /* accumulated prior-stage outputs, threaded into each next stage */

    struct Stage {
        const char  *prompt;
        const char  *name;
        std::string *out;
    };
    Stage stages[5] = {
        {reasoning::kDecompose, "DECOMPOSE", &res.decompose},
        {reasoning::kAnalyze, "ANALYZE", &res.analyze},
        {reasoning::kCritique, "CRITIQUE", &res.critique},
        {reasoning::kSynthesize, "SYNTHESIZE", &res.synthesize},
        {reasoning::kReflect, "REFLECT", &res.reflect},
    };

    for (auto &s : stages) {
        std::string out;
        if (!p_->run_stage(s.prompt, query, prior, out)) return res; /* complete stays false */
        *s.out = out;
        prior += std::string(s.name) + ":\n" + out + "\n\n";
    }
    res.answer = res.synthesize;  /* the synthesized answer */
    res.confidence = res.reflect; /* the calibrated reliability summary */
    res.complete = true;
    return res;
}

} // namespace hc
