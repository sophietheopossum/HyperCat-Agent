/* hc_policy.cpp — egress + exec policy. See hc_policy.hpp for the contract.
 *
 * Purpose:   classify a resolved peer IP for the SSRF egress control, and check argv0 for exec.
 * Owns:      nothing; operates on caller-owned config plus the borrowed IP string.
 * Threading: reentrant, no shared mutable state.
 * Lifetime:  returns by value / bool; retains no pointers.
 *
 * The egress classifier is the SSRF control, structured as ALLOW-ONLY-KNOWN-GOOD: every v4-in-v6
 * embedding (::ffff:, ::a.b.c.d, 64:ff9b::/96 NAT64, 2002::/16 6to4) is un-wrapped into the IPv4
 * classifier so an internal/metadata host cannot be reached through a v6 spelling, and anything
 * that is not provably global unicast (2000::/3) is DENIED rather than trusted as Public.
 */

#include "hc_policy.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

namespace hc {

static bool all_zero(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        if (b[i]) return false;
    return true;
}

IpClass classify_ipv4(const uint8_t b[4])
{
    if (b[0] == 127) return IpClass::Loopback;                             // 127.0.0.0/8
    if (b[0] == 169 && b[1] == 254) return IpClass::LinkLocal;             // 169.254.0.0/16
    if (b[0] == 10) return IpClass::Private;                               // 10.0.0.0/8
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return IpClass::Private;  // 172.16.0.0/12
    if (b[0] == 192 && b[1] == 168) return IpClass::Private;               // 192.168.0.0/16
    if (b[0] == 100 && b[1] >= 64 && b[1] <= 127) return IpClass::Private; // 100.64.0.0/10 CGNAT
    if (b[0] == 0) return IpClass::Unspecified;                            // 0.0.0.0/8
    if (b[0] >= 224 && b[0] <= 239) return IpClass::Multicast;             // 224.0.0.0/4
    if (b[0] >= 240) return IpClass::Reserved;                             // 240.0.0.0/4 (incl. 255.255.255.255)
    return IpClass::Public;
}

IpClass classify_ipv6(const uint8_t b[16])
{
    if (all_zero(b, 16)) return IpClass::Unspecified;            // ::
    if (all_zero(b, 15) && b[15] == 1) return IpClass::Loopback; // ::1

    // Un-wrap every v4-in-v6 embedding into the IPv4 classifier — the classic SSRF bypasses
    // (an internal/metadata IPv4 reached through an IPv6 spelling).
    if (all_zero(b, 10) && b[10] == 0xff && b[11] == 0xff)       // ::ffff:a.b.c.d  (v4-mapped)
        return classify_ipv4(b + 12);
    if (all_zero(b, 12))                                         // ::a.b.c.d  (v4-compatible)
        return classify_ipv4(b + 12);
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && all_zero(b + 4, 8))
        return classify_ipv4(b + 12);                           // 64:ff9b::/96  (NAT64 well-known)
    if (b[0] == 0x20 && b[1] == 0x02)                           // 2002:V4::/48  (6to4)
        return classify_ipv4(b + 2);

    // Native special ranges.
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return IpClass::LinkLocal; // fe80::/10
    if ((b[0] & 0xfe) == 0xfc) return IpClass::Private;                   // fc00::/7 ULA
    if (b[0] == 0xff) return IpClass::Multicast;                         // ff00::/8
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)
        return IpClass::Reserved;                                       // 2001:0000::/32 Teredo (tunnel) -> deny
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
        return IpClass::Reserved;                                       // 2001:db8::/32 documentation

    // Allow ONLY provable global unicast (2000::/3); deny the entire long tail.
    if ((b[0] & 0xe0) == 0x20) return IpClass::Public;                   // 2000::/3
    return IpClass::Reserved;
}

IpClass classify_ip(hc_http_addr_family family, const char *ip)
{
    if (!ip || !ip[0]) return IpClass::Reserved;
    if (family == HC_HTTP_AF_INET) {
        uint8_t b[4];
        if (::inet_pton(AF_INET, ip, b) != 1) return IpClass::Reserved; // unparseable -> deny
        return classify_ipv4(b);
    }
    if (family == HC_HTTP_AF_INET6) {
        uint8_t b[16];
        if (::inet_pton(AF_INET6, ip, b) != 1) return IpClass::Reserved;
        return classify_ipv6(b);
    }
    return IpClass::Reserved;
}

bool EgressPolicy::allow_decision(hc_http_addr_family family, const char *ip) const
{
    if (!ip || !ip[0]) return false; // fail closed
    for (const auto &a : allow)
        if (a == ip) return true; // explicit re-permit (exact match on the numeric IP)
    if (strict_allowlist_only) return false;
    return classify_ip(family, ip) == IpClass::Public;
}

std::vector<std::string> parse_egress_allow_csv(const char *csv)
{
    std::vector<std::string> out;
    if (!csv) return out;
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    const char *p = csv;
    while (*p && out.size() < kMaxEgressAllow) {
        while (*p == ',' || is_ws(*p)) p++; /* skip separators + leading whitespace */
        const char *start = p;
        while (*p && *p != ',') p++; /* a token runs to the next comma */
        const char *end = p;
        while (end > start && is_ws(end[-1])) end--; /* trim trailing whitespace */
        if (end > start) out.emplace_back(start, (size_t)(end - start));
    }
    return out;
}

bool egress_allow_edit(std::vector<std::string> &allow, const char *ip, bool add)
{
    if (!ip || !*ip) return false;
    if (add) {
        unsigned char b4[4], b16[16];
        if (::inet_pton(AF_INET, ip, b4) != 1 && ::inet_pton(AF_INET6, ip, b16) != 1)
            return false; /* numeric IPv4/IPv6 only — reject hostnames / CIDR / garbage */
        for (const auto &e : allow)
            if (e == ip) return false; /* already present */
        if (allow.size() >= kMaxEgressAllow) return false; /* bounded */
        allow.emplace_back(ip);
        return true;
    }
    for (auto it = allow.begin(); it != allow.end(); ++it)
        if (*it == ip) {
            allow.erase(it);
            return true;
        }
    return false;
}

bool ExecPolicy::allow(const std::string &argv0) const
{
    for (const auto &b : allowed_binaries)
        if (b == argv0) return true;
    return false; // default deny
}

bool exec_allow_edit(std::vector<std::string> &allow, const char *path, bool add)
{
    if (!path || !*path) return false;
    if (add) {
        if (path[0] != '/') return false; /* absolute only — a relative argv0 is never allowlisted */
        if (std::string(path).find("..") != std::string::npos) return false; /* no traversal in the stored entry */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false; /* must EXIST + be a regular file */
        for (const auto &e : allow)
            if (e == path) return false; /* dedup */
        if (allow.size() >= kMaxExecAllow) return false; /* bounded */
        allow.emplace_back(path);
        return true;
    }
    for (auto it = allow.begin(); it != allow.end(); ++it)
        if (*it == path) {
            allow.erase(it);
            return true;
        }
    return false;
}

const char *ip_class_name(IpClass c)
{
    switch (c) {
    case IpClass::Public:      return "public";
    case IpClass::Loopback:    return "loopback";
    case IpClass::LinkLocal:   return "link-local";
    case IpClass::Private:     return "private";
    case IpClass::Multicast:   return "multicast";
    case IpClass::Unspecified: return "unspecified";
    case IpClass::Reserved:    return "reserved";
    }
    return "unknown";
}

const char *egress_verdict_name(EgressVerdict v)
{
    switch (v) {
    case EgressVerdict::Allow: return "allow";
    case EgressVerdict::DenyIpClass: return "deny-ip-class";
    case EgressVerdict::DenyHostList: return "deny-host-list";
    }
    return "deny";
}

EgressDecision egress_decide(const EgressPolicy &p, const hc_http_peer *peer)
{
    if (!peer || !peer->ip || !peer->ip[0]) return {false, EgressVerdict::DenyIpClass}; /* fail closed */
    /* P08: when a host-allowlist is configured, the REQUESTED host must be in it (exact match, no wildcard) —
     * checked IN ADDITION to the IP class, never instead. An IP-literal request (peer->host == NULL) has no
     * host to match, so a non-empty host-allowlist denies it (only named, allowlisted hosts may egress). */
    if (!p.host_allow.empty()) {
        const char *host = peer->host ? peer->host : "";
        bool        ok = false;
        /* EXACT, byte-for-byte match (case- + trailing-dot-sensitive — a mismatch fails CLOSED, the safe
         * direction). An empty allowlist entry is skipped so it can never match an absent/empty host. */
        for (const auto &h : p.host_allow)
            if (!h.empty() && host[0] && h == host) {
                ok = true;
                break;
            }
        if (!ok) return {false, EgressVerdict::DenyHostList};
    }
    /* fall through to the UNCHANGED IP classifier (the strong anti-SSRF default-deny-non-Public) */
    if (!p.allow_decision(peer->family, peer->ip)) return {false, EgressVerdict::DenyIpClass};
    return {true, EgressVerdict::Allow};
}

} // namespace hc

extern "C" bool hc_policy_http_guard(const hc_http_peer *peer, void *user)
{
    if (!user) return false; // fail closed (egress_decide also fail-closes a null/empty-ip peer)
    const auto *pol = static_cast<const hc::EgressPolicy *>(user);
    return hc::egress_decide(*pol, peer).allow; /* P08: host-allowlist + the IP classifier */
}
