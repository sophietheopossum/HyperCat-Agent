/* worker_runtime_tools — implementation. See worker_runtime_tools.hpp. Mirrors the memory_recall bus round-trip:
 * a proxy tool's invoke sends `tool.invoke` to "toolhost" and returns the relayed result.
 *
 * Per-call gate (D5): a function the ToolHost flags `sensitive` (the manifest marked it, or the manifest grants
 * fs-write / egress / exec) routes through the SAME human approval path as fs_write — a `tool.authorize` to
 * "authgate", blocked on the verdict via await_reply_patient — BEFORE the invoke is forwarded. The gate logic
 * lives here in COMPILED worker code (not model output), so the model cannot make a sensitive call skip its
 * prompt, exactly as it cannot for fs_write. The hard floor is the tool's own hc_confine jail (workspace-only
 * fs, deny_network unless granted, seccomp) applied from the operator-reviewed manifest; this gate is the
 * per-call human control on top — the same two-layer model the System Tools use. */

#include "worker_runtime_tools.hpp"

#include "hc_agent.h"
#include "hc_bus.hpp"
#include "worker_protocol.hpp" /* await_reply, await_reply_patient, body_str, Message */

#include "hc_json.h"

#include <cstdlib>
#include <cstring>

namespace hc {

namespace {

char *dup_str(const std::string &s)
{
    char *r = static_cast<char *>(malloc(s.size() + 1));
    if (r) memcpy(r, s.c_str(), s.size() + 1);
    return r;
}

int body_bool(const std::string &body, const char *key)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    int      r = o ? (hc_json_get_bool(o, key, false) ? 1 : 0) : 0;
    if (o) hc_json_free(o);
    return r;
}

/* the human-gate wait budget (B1): unset / <=0 = PATIENT (wait until a verdict; a reaped/host-gone worker still
 * unblocks via bus.alive()); HC_APPROVAL_TIMEOUT_MS>0 caps it. Mirrors worker_tools.cpp:approval_timeout_ms. */
long approval_timeout_ms()
{
    const char *v = getenv("HC_APPROVAL_TIMEOUT_MS");
    if (!v || !*v) return 0;
    long ms = strtol(v, nullptr, 10);
    return ms > 0 ? ms : 0;
}

/* A bounded one-line summary for the operator's approval prompt: the function name + a short args preview. The
 * args are untrusted model output, but (like fs_write's summary) they only ever reach the gate as DISPLAY text —
 * the host never executes them, and the preview is length-capped well inside the bus frame. */
std::string gate_summary(const std::string &fn, const char *args_json)
{
    std::string preview = (args_json && args_json[0]) ? std::string(args_json) : std::string("{}");
    if (preview.size() > 200) preview.resize(200);
    return "third-party tool '" + fn + "' with args " + preview;
}

/* Block on the operator for a sensitive call. Returns true to proceed, false to deny (with *deny set to the
 * reason the model should see). Mirrors fs_write_invoke's tool.authorize round-trip exactly. */
bool gate_sensitive(ToolProxyCtx *ctx, const char *args_json, std::string *deny)
{
    uint64_t c = ++(*ctx->corr);
    hc_json *o = hc_json_new_object();
    if (!o) { *deny = "error: out of memory"; return false; }
    hc_json_obj_set_str(o, "cmd", "tool.authorize");
    hc_json_obj_set_str(o, "tool", ctx->fn.c_str());
    std::string summary = gate_summary(ctx->fn, args_json);
    hc_json_obj_set_str(o, "summary", summary.c_str());
    char *bs = hc_json_print(o, false);
    hc_json_free(o);
    std::string req = bs ? bs : "";
    free(bs);
    if (req.empty() || !ctx->bus->send_request("authgate", c, req)) {
        *deny = "denied: cannot reach the approval gate";
        return false;
    }
    Message reply;
    if (!await_reply_patient(*ctx->bus, c, approval_timeout_ms(), &reply)) {
        *deny = "denied: no operator approval (the gate is unreachable or a bounded timeout elapsed)";
        return false;
    }
    bool approved = false, dismissed = false;
    if (hc_json *r = hc_json_parse(reply.body.data(), reply.body.size())) {
        approved = hc_json_get_bool(r, "approved", false);
        dismissed = hc_json_get_bool(r, "dismissed", false); /* set aside — NOT a denial */
        hc_json_free(r);
    }
    if (dismissed) { *deny = "deferred: the operator set this aside — nothing ran; ask again if still needed"; return false; }
    if (!approved) { *deny = "denied: the operator declined this tool call"; return false; }
    return true;
}

/* the proxy: forward one call to the ToolHost and return its relayed result (or a clear error string). */
char *tool_proxy_invoke(const char *args_json, void *user)
{
    ToolProxyCtx *ctx = static_cast<ToolProxyCtx *>(user);
    /* sensitive functions are human-gated BEFORE we forward anything (D5) — the same floor as fs_write. */
    if (ctx->sensitive) {
        std::string deny;
        if (!gate_sensitive(ctx, args_json, &deny)) return dup_str(deny);
    }
    uint64_t c = ++(*ctx->corr);
    hc_json      *b = hc_json_new_object();
    if (!b) return dup_str("error: out of memory");
    hc_json_obj_set_str(b, "cmd", "tool.invoke");
    hc_json_obj_set_int(b, "v", 1);
    hc_json_obj_set_str(b, "tool", ctx->fn.c_str());
    hc_json_obj_set_str(b, "args", args_json && args_json[0] ? args_json : "{}");
    char *bs = hc_json_print(b, false);
    hc_json_free(b);
    if (!bs || !ctx->bus->send_request("toolhost", c, bs)) {
        free(bs);
        return dup_str("error: the tool host is unreachable");
    }
    free(bs);
    /* the ToolHost relays the tool's reply (or a timeout/deny) within the tool's timeout + the sweep slice;
     * wait a little past that so we always get a verdict rather than hanging. */
    Message r;
    if (!await_reply(*ctx->bus, c, ctx->timeout_ms + 3000, &r)) return dup_str("error: the tool did not respond");
    /* only "toolhost" relays our tool reply — reject a corr-matching frame from any other (confirmed) peer rather
     * than feed forged output to the model (parity with the conductor's from-check + the ToolHost's own). */
    if (r.from != "toolhost") return dup_str("error: the tool reply came from an unexpected source");
    if (body_bool(r.body, "ok")) {
        std::string result = body_str(r.body, "result", "");
        return dup_str(result.empty() ? "(the tool returned an empty result)" : result);
    }
    return dup_str("error: " + body_str(r.body, "error", "the tool failed"));
}

} // namespace

std::size_t register_runtime_tools(hc_agent *ag, BusClient *bus, uint64_t *corr,
                                   std::vector<std::unique_ptr<ToolProxyCtx>> &out_ctxs)
{
    if (!ag || !bus || !corr) return 0;
    uint64_t c = ++(*corr);
    if (!bus->send_request("toolhost", c, "{\"cmd\":\"tool.list\",\"v\":1}")) return 0; /* no ToolHost */
    Message r;
    if (!await_reply(*bus, c, 3000, &r)) return 0; /* ToolHost absent / no third-party tools => graceful no-op */
    hc_json *o = hc_json_parse(r.body.data(), r.body.size());
    if (!o) return 0;
    const hc_json *arr = hc_json_get(o, "tools");
    const size_t   n = (arr && hc_json_is_array(arr)) ? hc_json_arr_len(arr) : 0;
    std::size_t    count = 0;
    for (size_t i = 0; i < n; i++) {
        const hc_json *e = hc_json_arr_at(arr, i);
        if (!e) continue;
        std::string name = hc_json_get_str(e, "name", "");
        std::string spec = hc_json_get_str(e, "spec_json", "");
        if (name.empty() || spec.empty()) continue;
        auto ctx = std::make_unique<ToolProxyCtx>();
        ctx->bus = bus;
        ctx->corr = corr;
        ctx->fn = std::move(name);
        ctx->spec = std::move(spec);
        ctx->timeout_ms = (int)hc_json_get_int(e, "timeout_ms", 10000);
        ctx->sensitive = hc_json_get_bool(e, "sensitive", false); /* gate this call (D5) when the host flags it */
        hc_agent_tool t = {};
        t.name = ctx->fn.c_str();    /* points into ctx (stable heap address; survives the move into out_ctxs) */
        t.spec_json = ctx->spec.c_str();
        t.invoke = tool_proxy_invoke;
        t.user = ctx.get();
        if (hc_agent_add_tool(ag, &t)) {
            out_ctxs.push_back(std::move(ctx));
            count++;
        }
    }
    hc_json_free(o);
    return count;
}

} // namespace hc
