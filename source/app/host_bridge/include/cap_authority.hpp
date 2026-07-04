#ifndef HC_CAP_AUTHORITY_HPP
#define HC_CAP_AUTHORITY_HPP

/* cap_authority — the host CAPABILITY AUTHORITY (P09): mints unforgeable, scoped, budgeted capability tokens
 * (hc_caps) and answers a worker's PROMPT-FREE `cap.check` against a live registry. It is a SIBLING of the
 * host_bridge adapters (UiAdapter/AuthGate/MemoryBroker) — same one-reader/one-sender bus shape — but split into
 * its own TU because it alone depends on hc_caps + hc_secrets and alone holds a signing key, so co-locating it
 * would force the capability ABI + the secret dep onto every host_bridge.hpp includer.
 *
 * Purpose:   it sits BESIDE AuthGate: when a worker holds a capability covering a tool call, the call skips the
 *            human prompt; every miss falls back to AuthGate (the floor). The ONLY identity it trusts is the
 *            broker-stamped sender — a token minted for agent:A is rejected when presented by agent:B.
 * Owns:      one "capabilities" BusClient + its reader thread; the minted-token REGISTRY (cap_id -> {claims,
 *            budget_left, revoked}, the authoritative LIVENESS a token can't carry); and the per-process 256-bit
 *            HMAC signing key (from /dev/urandom at start, scrubbed at stop — never logged/bussed/on disk).
 * Threading: one-reader/one-sender — the reader handles cap.check + replies (sends serialized by send_mu_);
 *            grant/revoke/snapshot/set_known_agents run on the host/UI (or fleet-change) thread under reg_mu_.
 *            The budget test + decrement is ONE critical section, so a worker can't double-spend by racing two
 *            checks. reg_mu_ is never held across bus I/O or a blocking syscall.
 * Lifetime:  start() connects + returns null on failure (then caps are simply OFF — everything uses AuthGate);
 *            the caller owns + deletes it (dtor stop()s — idempotent). The "capabilities" id is authorized +
 *            confirmed by the host bootstrap before start() connects. */

#include "hc_caps.h" /* hc_cap_claims — the registry record + grant() carry one (a small pure C leaf) */

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hc {
class BusClient; /* forward — held only by pointer; hc_bus is included in the .cpp */
}

namespace hc::host {

/* A minted capability's live REGISTRY record: the signed claims + the mutable liveness the token can't carry
 * (remaining budget, revoked). The token proves AUTHENTICITY; this record proves the grant is still LIVE. */
struct CapRecord {
    hc_cap_claims claims;
    uint32_t      budget_left = 0;
    bool          revoked = false;
};

/* A host-facing view of one LIVE capability for the UI Capabilities panel (no token, no nonce — display only). */
struct CapView {
    uint64_t    cap_id = 0;
    std::string subject;     /* the bound agent id          */
    uint32_t    verb = 0;    /* an hc_cap_verb               */
    std::string scope;       /* the path prefix / arg        */
    uint32_t    budget_left = 0;
    uint64_t    not_after_ms = 0;
    bool        revoked = false;
};

class CapabilityAuthority {
public:
    static CapabilityAuthority *start(const std::string &sock, std::unordered_set<std::string> known_agents);
    ~CapabilityAuthority();

    /* Mint + register a capability from a claims TEMPLATE (the caller sets subject/verb/scope_kind/scope/budget/
     * not_after_ms/agenda; cap_id + a fresh random nonce are filled here). Returns the new cap_id (0 on failure)
     * and the base64url token via *out_token. Host/UI thread. The token is DELIVERED to the worker by the caller
     * (P09.3: it rides the AuthGate approval reply — there is no separate host->worker push). */
    uint64_t grant(const hc_cap_claims &tmpl, std::string &out_token);

    void revoke(uint64_t cap_id);                /* operator pull -> the next cap.check for it denies. Host/UI thread. */
    void revoke_agent(const std::string &agent); /* revoke ALL of an agent's caps (reap / fleet removal). Host/UI thread. */
    void snapshot(std::vector<CapView> &out);    /* live registry view for the UI. Host/UI thread.                     */
    /* W2 P2.3 mirror: track the LIVE fleet. A cap.check from a non-live id is denied, AND every cap whose subject
     * left the set is revoked (revoke-on-reap via the fleet-change push). Fleet-change push thread. */
    void set_known_agents(std::unordered_set<std::string>);
    void stop(); /* idempotent; unblocks + joins the reader */

private:
    CapabilityAuthority() = default;
    void reader_loop();
    bool check(const std::string &from, const std::string &token, uint32_t verb, const std::string &path);

    BusClient                              *bus_ = nullptr;
    std::thread                             reader_;
    std::mutex                              reg_mu_; /* guards reg_ + next_cap_id_ */
    std::unordered_map<uint64_t, CapRecord> reg_;
    uint64_t                                next_cap_id_ = 1;
    std::mutex                              known_mu_; /* guards known_agents_ (setter vs reader) */
    std::unordered_set<std::string>         known_agents_;
    std::mutex                              send_mu_;  /* serialize bus_ sends (only the reader sends today) */
    unsigned char                           key_[32];  /* the per-process signing key — scrubbed in stop() */
    bool                                    key_ready_ = false;
};

} // namespace hc::host

#endif /* HC_CAP_AUTHORITY_HPP */
