/* hc_consolidation — see consolidation.hpp. The compaction + distillation host passes. Both are ONE-SHOT
 * hc_agent turns (no tools, max 1 iteration) driven through the P5 backend seam, so they run live (hosted
 * backend) or offline (scripted backend in tests). Compaction APPENDS an episode summary beside the raw
 * transcript; distillation emits provenance-tagged facts through a caller-supplied sink. */

#include "consolidation.hpp"

#include "hc_fs.h"
#include "hc_json.h"
#include "hc_store.h"

#include <cctype>
#include <cstring>
#include <unistd.h> /* access — the consolidated-marker existence check */

namespace hc::consolidation {
namespace {

/* Run ONE model turn with no tools (max 1 iteration) and return its assistant text ("" on failure). The
 * agent registers no tools, so the model cannot call anything — it just produces the summary/facts. */
std::string one_shot(const hc_agent_backend &backend, const std::string &system,
                     const std::string &prompt)
{
    hc_agent *ag = hc_agent_new_backend(&backend, system.c_str());
    if (!ag) return "";
    hc_agent_set_max_iterations(ag, 1); /* one LLM turn — a consolidation pass is not a tool loop */
    std::string out;
    if (hc_agent_run(ag, prompt.c_str(), nullptr, nullptr) == HC_AGENT_OK) {
        const char *t = hc_agent_last_text(ag);
        if (t) out = t;
    }
    hc_agent_free(ag);
    return out;
}

/* Render a session's messages [0, upto) as "role: content\n" lines, bounded so a hostile/huge transcript
 * cannot blow up the consolidation prompt. */
std::string render_messages(hc_session *sess, std::size_t upto, std::size_t byte_cap)
{
    std::string body;
    for (std::size_t i = 0; i < upto; i++) {
        const char *role = nullptr, *content = nullptr;
        if (!hc_session_message(sess, i, &role, &content)) continue;
        body += role ? role : "?";
        body += ": ";
        body += content ? content : "";
        body += "\n";
        if (body.size() >= byte_cap) {
            body.resize(byte_cap);
            /* don't cut mid-UTF-8: drop trailing continuation bytes + an orphaned lead byte */
            while (!body.empty() && (static_cast<unsigned char>(body.back()) & 0xC0) == 0x80)
                body.pop_back();
            if (!body.empty() && static_cast<unsigned char>(body.back()) >= 0xC0) body.pop_back();
            body += "\n…[truncated]\n";
            break;
        }
    }
    return body;
}

constexpr std::size_t kPromptByteCap = 24u * 1024; /* bound the transcript fed to a consolidation turn */
constexpr std::size_t kFactMaxBytes = 2048;        /* a single distilled fact is a short, durable note  */

/* Parse the distill turn's reply (one fact per line) into at most `max` bounded, non-empty facts; the
 * model is told to output "NONE" when nothing is durable. */
std::vector<std::string> parse_fact_lines(const std::string &resp, std::size_t max)
{
    std::vector<std::string> facts;
    std::size_t              start = 0;
    while (start <= resp.size() && facts.size() < max) {
        std::size_t nl = resp.find('\n', start);
        std::size_t end = (nl == std::string::npos) ? resp.size() : nl;
        std::string line = resp.substr(start, end - start);
        start = end + 1;
        /* trim surrounding whitespace + a leading list marker ("- ", "* ", "1. ") if the model adds one */
        std::size_t b = line.find_first_not_of(" \t\r-*•");
        std::size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos || e == std::string::npos) continue;
        std::string fact = line.substr(b, e - b + 1);
        /* skip the "nothing durable" sentinel robustly — trailing punctuation + any case ("NONE.", "none") */
        std::string probe = fact;
        while (!probe.empty() && std::ispunct(static_cast<unsigned char>(probe.back()))) probe.pop_back();
        for (char &c : probe) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (probe.empty() || probe == "NONE") continue;
        if (fact.size() > kFactMaxBytes) fact.resize(kFactMaxBytes);
        facts.push_back(std::move(fact));
        if (nl == std::string::npos) break;
    }
    return facts;
}

const char kCompactSystem[] =
    "You compress a finished work session. Summarize the given conversation excerpt into 2-4 sentences "
    "that preserve the decisions made, results produced, and any durable facts. Be factual and concise; "
    "do not invent. Output only the summary.";

const char kDistillSystem[] =
    "You distill DURABLE, reusable knowledge from a finished work session — conventions, decisions, and "
    "results that will matter for FUTURE tasks. Output only such facts, ONE per line, no numbering and no "
    "preamble. Exclude anything task-specific or ephemeral. If nothing is durable, output exactly: NONE";

} // namespace

CompactResult compact_session(hc_store *store, const hc_agent_backend &backend,
                              const std::string &session_id, const CompactOptions &opts)
{
    CompactResult r;
    if (!store) {
        r.error = "no store";
        return r;
    }
    hc_session *sess = hc_session_load(store, session_id.c_str());
    if (!sess) {
        r.error = "cannot load session: " + session_id;
        return r;
    }
    std::size_t n = hc_session_count(sess);
    if (n <= opts.min_messages) { /* below threshold — nothing to compact, not an error */
        hc_session_free(sess);
        return r;
    }
    std::size_t older = (n > opts.keep_recent) ? n - opts.keep_recent : 0;
    std::string body = render_messages(sess, older, kPromptByteCap);
    std::string dir = hc_session_dir(sess) ? hc_session_dir(sess) : "";

    std::string summary =
        one_shot(backend, kCompactSystem, "Conversation excerpt to summarize:\n\n" + body);
    if (summary.empty()) {
        hc_session_free(sess);
        r.error = "summary turn produced no text";
        return r;
    }

    /* append ONE episode record BESIDE the raw transcript — never modify the transcript (ground truth) */
    int      rc = -1;
    hc_json *o = hc_json_new_object();
    if (o && !dir.empty()) {
        hc_json_obj_set_str(o, "kind", "episode");
        hc_json_obj_set_str(o, "session", session_id.c_str());
        hc_json_obj_set_int(o, "messages_summarized", (long)older);
        hc_json_obj_set_str(o, "summary", summary.c_str());
        if (char *s = hc_json_print(o, false)) {
            std::string line = std::string(s) + "\n";
            std::string path = dir + "/episodes.jsonl";
            rc = hc_fs_append(path.c_str(), line.data(), line.size());
            free(s);
        }
    }
    hc_json_free(o);
    hc_session_free(sess);
    if (rc != 0) {
        r.error = "could not append the episode summary";
        return r;
    }
    r.compacted = true;
    r.summary = std::move(summary);
    return r;
}

DistillResult distill_session(hc_store *store, const hc_agent_backend &backend, const WriteFn &write,
                              const std::string &session_id, const DistillOptions &opts)
{
    DistillResult r;
    if (!store || !write) {
        r.error = "no store / sink";
        return r;
    }
    hc_session *sess = hc_session_load(store, session_id.c_str());
    if (!sess) {
        r.error = "cannot load session: " + session_id;
        return r;
    }
    std::string body = render_messages(sess, hc_session_count(sess), kPromptByteCap);
    hc_session_free(sess);
    if (body.empty()) {
        r.error = "empty session — nothing to distill";
        return r;
    }

    std::string resp = one_shot(backend, kDistillSystem,
                                "Session transcript:\n\n" + body
                                    + "\nDurable facts (one per line, or NONE):");
    r.ok = true; /* the turn ran; written==0 is a valid "nothing durable" outcome */
    std::vector<std::string> facts = parse_fact_lines(resp, opts.max_facts);
    for (const auto &f : facts)
        if (write(opts.scope, f, session_id)) r.written++; /* provenance = the session id */
    return r;
}

namespace {
/* the consolidated-marker path for a session ("" if it can't be loaded; the dir comes from the store) */
std::string marker_path(hc_store *store, const std::string &session_id)
{
    hc_session *s = hc_session_load(store, session_id.c_str());
    if (!s) return "";
    const char *d = hc_session_dir(s);
    std::string p = d ? std::string(d) + "/consolidated" : "";
    hc_session_free(s);
    return p;
}
} // namespace

bool is_consolidated(hc_store *store, const std::string &session_id)
{
    std::string p = marker_path(store, session_id);
    return !p.empty() && access(p.c_str(), F_OK) == 0;
}

void mark_consolidated(hc_store *store, const std::string &session_id)
{
    std::string p = marker_path(store, session_id);
    if (p.empty()) return;
    static const char kBody[] = "{\"consolidated\":true}\n";
    hc_fs_atomic_write(p.c_str(), kBody, sizeof kBody - 1);
}

} // namespace hc::consolidation
