#ifndef HC_POLICY_HPP
#define HC_POLICY_HPP

/* hc_policy — egress allowlist (anti-SSRF) + exec allowlist (C++ host module).
 *
 * Purpose:   decide whether the agent may reach a resolved network peer (the SSRF control) and
 *            whether it may run a given binary (the exec control). The egress decision is exposed
 *            as a C-callable guard that plugs into hc_http_set_guard, so hc_http refuses a denied
 *            peer before connect / on every request. Pure logic — no I/O — fully offline-testable.
 *            See Docs/Plan_HyperCat/07-sandbox-and-policy.md.
 * Owns:      nothing beyond its config vectors; it borrows the hc_http_peer passed to the guard.
 * Threading: an EgressPolicy/ExecPolicy is read-only once configured and may be shared across
 *            threads for reads; mutate its vectors only during setup.
 * Lifetime:  EgressPolicy/ExecPolicy are value types — the caller constructs, owns, and destroys
 *            them. hc_policy_http_guard borrows `peer` and `user` only for the duration of the
 *            call and retains nothing.
 */

#include "hc_http.h" /* hc_http_peer, hc_http_addr_family */

#include <cstdint>
#include <string>
#include <vector>

namespace hc {

enum class IpClass {
    Public,      /* allowed by default                                                  */
    Loopback,    /* 127/8, ::1                                                           */
    LinkLocal,   /* 169.254/16 (incl. 169.254.169.254 metadata), fe80::/10              */
    Private,     /* RFC1918 10/8 172.16/12 192.168/16, CGNAT 100.64/10, ULA fc00::/7    */
    Multicast,   /* 224/4, ff00::/8                                                      */
    Unspecified, /* 0.0.0.0, ::                                                          */
    Reserved     /* 240/4, Teredo, doc ranges, unparseable, and the non-global v6 tail   */
};

const char *ip_class_name(IpClass c);

IpClass classify_ipv4(const uint8_t b[4]);
/* Un-wraps every v4-in-v6 embedding (::ffff:, ::a.b.c.d, 64:ff9b::/96, 2002::/16) into the v4
 * classifier, and allows ONLY provable global unicast (2000::/3) — the rest is denied. */
IpClass classify_ipv6(const uint8_t b[16]);
IpClass classify_ip(hc_http_addr_family family, const char *ip);

/* Egress allowlist (anti-SSRF). Default: deny everything that is not Public; an explicit `allow`
 * list re-permits exact IP strings; strict mode denies everything not explicitly allowed. */
struct EgressPolicy {
    bool                     strict_allowlist_only = false;
    std::vector<std::string> allow;
    /* P08: an OPTIONAL exact-hostname allowlist (NO wildcard — the attacker-controlled-subdomain footgun; the
     * match is byte-exact, so case + a trailing dot must match — a mismatch fails closed). When non-empty, a
     * request's host must be in it IN ADDITION to the IP-class check. EMPTY = check OFF (today's behaviour), so
     * this is opt-in and never breaks an unconfigured deployment (e.g. a LAN-LLM by IP).
     * CAVEAT (close before WIRING this to config + relying on it as a hostname boundary): hc_http does not
     * refresh the requested host across a 30x redirect, so on a redirect HOP this checks the ORIGINAL host, not
     * the target — the per-hop IP CLASSIFIER (which IS re-checked per hop) remains the binding anti-SSRF control;
     * the fix is to disable redirect-following on the LLM client (a provider should not 30x to a new origin). */
    std::vector<std::string> host_allow;
    bool allow_decision(hc_http_addr_family family, const char *ip) const;
};

/* P08: the verdict an egress decision yields — surfaced to the audit log + the Network panel (P08.2). */
enum class EgressVerdict { Allow, DenyIpClass, DenyHostList };
const char *egress_verdict_name(EgressVerdict v);

/* P08: the egress decision = the host-allowlist check layered OVER the UNTOUCHED IP classifier (allow_decision).
 * Returns {allow, verdict} so the caller can both enforce AND record WHY. A null/empty-ip peer fails closed.
 * The classifier (classify_ip / allow_decision) is unchanged; this is a thin pure layer on top. */
struct EgressDecision {
    bool          allow;
    EgressVerdict verdict;
};
EgressDecision egress_decide(const EgressPolicy &p, const hc_http_peer *peer);

/* Parse a comma-separated allowlist (e.g. HC_EGRESS_ALLOW or a saved settings field) into trimmed,
 * non-empty entries — the shared source the worker (HC_EGRESS_ALLOW/--egress-allow) and the host's
 * settings editor both build an EgressPolicy.allow from. PURE: splits on ',', trims surrounding ASCII
 * whitespace, drops empties; does NOT validate that each token is a real IP (allow_decision exact-matches
 * a resolved numeric peer, so a bad token simply never matches; the host editor validates via inet_pton
 * before persisting). A NULL/empty input yields an empty vector; capped at kMaxEgressAllow entries
 * (a typo'd giant input cannot bloat the per-request linear scan). */
constexpr size_t         kMaxEgressAllow = 256;
std::vector<std::string> parse_egress_allow_csv(const char *csv);

/* Apply ONE authoritative allowlist edit (WI-2 E2's host-side mutation; the UI's inet_pton + confirm modal
 * are advisory). add=true: append `ip` IFF it is a literal numeric IPv4/IPv6 (inet_pton — hostnames/CIDR
 * rejected), not already present, and the list is under kMaxEgressAllow. add=false: remove `ip` if present.
 * Returns true IFF `allow` changed. There is no "disable" here — an empty list is just default-deny. */
bool egress_allow_edit(std::vector<std::string> &allow, const char *ip, bool add);

/* Exec allowlist (argv-not-shell). Default-deny; only listed binaries are permitted. */
struct ExecPolicy {
    std::vector<std::string> allowed_binaries;
    bool allow(const std::string &argv0) const;
};

constexpr size_t kMaxExecAllow = 64;

/* Apply ONE authoritative exec-allowlist edit (W4.2; mirrors egress_allow_edit — the host is the authority,
 * the UI is advisory). add=true: append `path` IFF it is an ABSOLUTE path to an EXISTING regular file, not
 * already present, and the list is under kMaxExecAllow. add=false: remove `path` if present. Returns true IFF
 * `allow` changed. The deeper exec-time checks (realpath, reject setuid/non-absolute) live in the ExecGate
 * (W4.3); this is operator-list hygiene. An empty list is default-deny (exec disabled). */
bool exec_allow_edit(std::vector<std::string> &allow, const char *path, bool add);

} // namespace hc

/* C-callable guard for hc_http_set_guard. `user` MUST be a const hc::EgressPolicy*. Fail-closed:
 * returns false (deny) on a NULL/empty ip or NULL policy. */
extern "C" bool hc_policy_http_guard(const hc_http_peer *peer, void *user);

#endif /* HC_POLICY_HPP */
