/* Tests for hc_policy — the SSRF egress classifier + allowlist + the C guard + the exec
 * allowlist, including a regression battery of v4-in-v6 embedding bypasses. Pure offline logic.
 * Exit non-zero on any failure (CTest reads the exit code). */

#include "hc_policy.hpp"

#include <cstdio>

using hc::EgressPolicy;
using hc::ExecPolicy;
using hc::IpClass;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                      \
            g_fails++;                                                                      \
        }                                                                                   \
    } while (0)

static IpClass c4(const char *s) { return hc::classify_ip(HC_HTTP_AF_INET, s); }
static IpClass c6(const char *s) { return hc::classify_ip(HC_HTTP_AF_INET6, s); }

int main()
{
    /* --- IPv4 classification --- */
    CHECK(c4("127.0.0.1") == IpClass::Loopback, "127.0.0.1 loopback");
    CHECK(c4("169.254.169.254") == IpClass::LinkLocal, "cloud metadata is link-local");
    CHECK(c4("10.0.0.5") == IpClass::Private, "10/8 private");
    CHECK(c4("172.16.0.1") == IpClass::Private, "172.16/12 private");
    CHECK(c4("172.32.0.1") == IpClass::Public, "172.32 is outside the /12 (public)");
    CHECK(c4("192.168.1.1") == IpClass::Private, "192.168/16 private");
    CHECK(c4("100.64.0.1") == IpClass::Private, "100.64/10 CGNAT private");
    CHECK(c4("0.0.0.0") == IpClass::Unspecified, "0.0.0.0 unspecified");
    CHECK(c4("224.0.0.1") == IpClass::Multicast, "224/4 multicast");
    CHECK(c4("255.255.255.255") == IpClass::Reserved, "broadcast reserved (240/4)");
    CHECK(c4("8.8.8.8") == IpClass::Public, "8.8.8.8 public");
    CHECK(c4("not-an-ip") == IpClass::Reserved, "unparseable -> reserved (deny)");

    /* --- IPv6 native classification --- */
    CHECK(c6("::1") == IpClass::Loopback, "::1 loopback");
    CHECK(c6("fe80::1") == IpClass::LinkLocal, "fe80::/10 link-local");
    CHECK(c6("fc00::1") == IpClass::Private, "fc00::/7 ULA private");
    CHECK(c6("ff02::1") == IpClass::Multicast, "ff00::/8 multicast");
    CHECK(c6("2606:4700:4700::1111") == IpClass::Public, "public v6 (2000::/3)");
    CHECK(c6("0100::1") != IpClass::Public, "non-global v6 tail is denied (allow-only-known-good)");

    /* --- v4-in-v6 embedding bypasses: every form must un-wrap to the embedded v4 and deny --- */
    CHECK(c6("::ffff:127.0.0.1") == IpClass::Loopback, "v4-mapped loopback");
    CHECK(c6("::ffff:169.254.169.254") == IpClass::LinkLocal, "v4-mapped metadata");
    CHECK(c6("::127.0.0.1") == IpClass::Loopback, "v4-compatible loopback (bypass closed)");
    CHECK(c6("::169.254.169.254") == IpClass::LinkLocal, "v4-compatible metadata (bypass closed)");
    CHECK(c6("64:ff9b::169.254.169.254") == IpClass::LinkLocal, "NAT64 metadata (bypass closed)");
    CHECK(c6("64:ff9b::127.0.0.1") == IpClass::Loopback, "NAT64 loopback (bypass closed)");
    CHECK(c6("2002:a9fe:a9fe::1") == IpClass::LinkLocal, "6to4 metadata 169.254.169.254 (bypass closed)");
    CHECK(c6("2002:7f00:0001::") == IpClass::Loopback, "6to4 loopback 127.0.0.1 (bypass closed)");
    CHECK(c6("2002:0a00:0001::") == IpClass::Private, "6to4 private 10.0.0.1 (bypass closed)");
    CHECK(c6("2001::1") == IpClass::Reserved, "Teredo tunnel denied");
    CHECK(c6("2001:db8::1") == IpClass::Reserved, "documentation range denied");

    /* --- egress decision (default deny-non-public) --- */
    EgressPolicy p;
    CHECK(p.allow_decision(HC_HTTP_AF_INET, "8.8.8.8"), "default allows public");
    CHECK(!p.allow_decision(HC_HTTP_AF_INET, "127.0.0.1"), "default denies loopback");
    CHECK(!p.allow_decision(HC_HTTP_AF_INET, "169.254.169.254"), "default denies metadata");
    CHECK(!p.allow_decision(HC_HTTP_AF_INET6, "64:ff9b::169.254.169.254"),
          "default denies NAT64-embedded metadata");
    CHECK(!p.allow_decision(HC_HTTP_AF_INET6, "2002:a9fe:a9fe::1"),
          "default denies 6to4-embedded metadata");

    p.allow.push_back("127.0.0.1");
    CHECK(p.allow_decision(HC_HTTP_AF_INET, "127.0.0.1"), "explicit allowlist re-permits");

    EgressPolicy strict;
    strict.strict_allowlist_only = true;
    strict.allow.push_back("8.8.8.8");
    CHECK(strict.allow_decision(HC_HTTP_AF_INET, "8.8.8.8"), "strict allows the listed host");
    CHECK(!strict.allow_decision(HC_HTTP_AF_INET, "1.1.1.1"), "strict denies an unlisted public host");

    /* --- the C-callable guard (what hc_http calls) --- */
    EgressPolicy gp;
    hc_http_peer peer_pub{HC_HTTP_AF_INET, "8.8.8.8", 443, "example.com"};
    hc_http_peer peer_meta{HC_HTTP_AF_INET, "169.254.169.254", 80, nullptr};
    hc_http_peer peer_nat64{HC_HTTP_AF_INET6, "64:ff9b::169.254.169.254", 80, nullptr};
    CHECK(hc_policy_http_guard(&peer_pub, &gp) == true, "guard allows public");
    CHECK(hc_policy_http_guard(&peer_meta, &gp) == false, "guard denies metadata");
    CHECK(hc_policy_http_guard(&peer_nat64, &gp) == false, "guard denies NAT64-embedded metadata");
    hc_http_peer peer_noip{HC_HTTP_AF_INET, "", 0, nullptr};
    CHECK(hc_policy_http_guard(&peer_noip, &gp) == false, "guard fail-closed on empty ip");
    CHECK(hc_policy_http_guard(&peer_pub, nullptr) == false, "guard fail-closed on null policy");

    /* --- P08: egress_decide — the exact-host allowlist layered OVER the UNCHANGED IP classifier --- */
    {
        hc::EgressDecision d;
        EgressPolicy       nohost; /* no host-allowlist -> identical to the bare classifier */
        d = hc::egress_decide(nohost, &peer_pub);
        CHECK(d.allow && d.verdict == hc::EgressVerdict::Allow, "no host-allowlist: a public peer is allowed");
        d = hc::egress_decide(nohost, &peer_meta);
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyIpClass, "no host-allowlist: metadata denied (IP class)");

        EgressPolicy hp;
        hp.host_allow = {"openrouter.ai"}; /* only this exact, named host may egress */
        hc_http_peer ok_host{HC_HTTP_AF_INET, "8.8.8.8", 443, "openrouter.ai"};
        hc_http_peer bad_host{HC_HTTP_AF_INET, "8.8.8.8", 443, "evil.example.com"};
        hc_http_peer sub_host{HC_HTTP_AF_INET, "8.8.8.8", 443, "api.openrouter.ai"}; /* NOT an exact match */
        CHECK(hc::egress_decide(hp, &ok_host).allow, "host-allowlist: the exact allowed host egresses");
        d = hc::egress_decide(hp, &bad_host);
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyHostList, "host-allowlist: an off-list host is denied");
        d = hc::egress_decide(hp, &sub_host);
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyHostList,
              "host-allowlist: a subdomain is NOT a wildcard match (the subdomain footgun)");
        d = hc::egress_decide(hp, &peer_meta); /* host==null + a configured allowlist -> denied before the IP check */
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyHostList, "host-allowlist: an IP-literal (no host) is denied");

        /* an empty allowlist ENTRY must never match an absent/empty host (the host[0] / !h.empty() guard) */
        EgressPolicy emptyentry;
        emptyentry.host_allow = {""};
        d = hc::egress_decide(emptyentry, &peer_meta); /* host==null */
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyHostList, "an empty allowlist entry does not match a null host");
        hc_http_peer empty_host{HC_HTTP_AF_INET, "8.8.8.8", 443, ""};
        d = hc::egress_decide(emptyentry, &empty_host);
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyHostList, "an empty allowlist entry does not match an empty host");

        /* the classifier STILL applies: an allowlisted host that resolves to a PRIVATE ip is denied */
        hc_http_peer ok_host_priv{HC_HTTP_AF_INET, "192.168.1.9", 443, "openrouter.ai"};
        d = hc::egress_decide(hp, &ok_host_priv);
        CHECK(!d.allow && d.verdict == hc::EgressVerdict::DenyIpClass,
              "host-allowlist does NOT bypass the IP classifier (a private resolve is still denied)");
        CHECK(hc_policy_http_guard(&ok_host, &hp) && !hc_policy_http_guard(&bad_host, &hp),
              "the C guard reflects egress_decide (allow the listed host, deny the off-list one)");
    }

    /* --- parse_egress_allow_csv (the shared HC_EGRESS_ALLOW / settings-field parser, WI-2 E0) --- */
    CHECK(hc::parse_egress_allow_csv(nullptr).empty(), "null csv -> empty");
    CHECK(hc::parse_egress_allow_csv("").empty(), "empty csv -> empty");
    CHECK(hc::parse_egress_allow_csv("   ,, ,").empty(), "only separators/ws -> empty");
    {
        auto one = hc::parse_egress_allow_csv("127.0.0.1");
        CHECK(one.size() == 1 && one[0] == "127.0.0.1", "single entry");
        auto many = hc::parse_egress_allow_csv("  127.0.0.1 , 10.0.0.5 ,,192.168.1.50,");
        CHECK(many.size() == 3 && many[0] == "127.0.0.1" && many[1] == "10.0.0.5" &&
                  many[2] == "192.168.1.50",
              "trims whitespace, drops empties + trailing comma");
    }
    /* end-to-end: a parsed CSV becomes the allow list that re-permits an otherwise-denied LAN peer
     * (the local-LLM-server case E0 exists for), while an unlisted private peer stays denied. */
    {
        EgressPolicy lan;
        lan.allow = hc::parse_egress_allow_csv("192.168.1.50");
        CHECK(lan.allow_decision(HC_HTTP_AF_INET, "192.168.1.50"), "parsed CSV re-permits the LAN host");
        CHECK(!lan.allow_decision(HC_HTTP_AF_INET, "192.168.1.51"), "an unlisted LAN host stays denied");
        CHECK(lan.allow_decision(HC_HTTP_AF_INET, "8.8.8.8"), "public still allowed (non-strict)");
    }

    /* --- egress_allow_edit (the WI-2 E2 editor's host-authoritative mutation) --- */
    {
        std::vector<std::string> al;
        /* add accepts only a numeric IP; hostnames / CIDR / garbage are rejected (no change) */
        CHECK(hc::egress_allow_edit(al, "10.0.0.5", true) && al.size() == 1, "add a numeric v4");
        CHECK(hc::egress_allow_edit(al, "::1", true) && al.size() == 2, "add a numeric v6");
        CHECK(!hc::egress_allow_edit(al, "evil.example.com", true) && al.size() == 2, "reject a hostname");
        CHECK(!hc::egress_allow_edit(al, "10.0.0.0/8", true) && al.size() == 2, "reject a CIDR");
        CHECK(!hc::egress_allow_edit(al, "not an ip", true) && al.size() == 2, "reject garbage");
        CHECK(!hc::egress_allow_edit(al, "10.0.0.5", true) && al.size() == 2, "reject a duplicate");
        /* remove erases a present entry; removing the last returns to the safe default-deny baseline */
        CHECK(hc::egress_allow_edit(al, "10.0.0.5", false) && al.size() == 1, "remove a present entry");
        CHECK(!hc::egress_allow_edit(al, "10.0.0.5", false) && al.size() == 1, "remove a missing entry = no-op");
        CHECK(hc::egress_allow_edit(al, "::1", false) && al.empty(), "remove the last -> empty (default-deny)");
        /* the cap bounds growth (no unbounded re-permit list) */
        std::vector<std::string> big;
        for (size_t i = 0; i < hc::kMaxEgressAllow; i++) {
            char ip[32];
            std::snprintf(ip, sizeof ip, "10.%zu.%zu.%zu", (i >> 16) & 255, (i >> 8) & 255, i & 255);
            hc::egress_allow_edit(big, ip, true);
        }
        CHECK(big.size() == hc::kMaxEgressAllow, "filled to the cap");
        CHECK(!hc::egress_allow_edit(big, "8.8.8.8", true) && big.size() == hc::kMaxEgressAllow,
              "the cap rejects one more");
    }

    /* --- exec allowlist --- */
    ExecPolicy ep;
    ep.allowed_binaries.push_back("/usr/bin/git");
    CHECK(ep.allow("/usr/bin/git"), "exec allows the listed binary");
    CHECK(!ep.allow("/bin/sh"), "exec denies an unlisted binary");

    /* --- exec_allow_edit (W4: the host-authoritative allowlist mutation) --- */
    {
        std::vector<std::string> xa;
        CHECK(hc::exec_allow_edit(xa, "/bin/sh", true) && xa.size() == 1, "add: an absolute existing file");
        CHECK(!hc::exec_allow_edit(xa, "/bin/sh", true), "add: a duplicate is rejected (no change)");
        CHECK(!hc::exec_allow_edit(xa, "relative/path", true), "add: a relative path is rejected");
        CHECK(!hc::exec_allow_edit(xa, "/usr/../bin/sh", true), "add: a path with .. is rejected");
        CHECK(!hc::exec_allow_edit(xa, "/no/such/binary/xyzzy", true), "add: a non-existent path is rejected");
        CHECK(!hc::exec_allow_edit(xa, "/etc", true), "add: a directory (non-regular) is rejected");
        CHECK(hc::exec_allow_edit(xa, "/bin/sh", false) && xa.empty(), "remove: present entry erased");
        CHECK(!hc::exec_allow_edit(xa, "/bin/sh", false), "remove: an absent entry is a no-op (false)");
        /* the cap */
        std::vector<std::string> capd(hc::kMaxExecAllow, "/x");
        CHECK(!hc::exec_allow_edit(capd, "/bin/sh", true), "add: rejected at the cap");
    }

    if (g_fails) {
        std::fprintf(stderr, "hc_policy: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("hc_policy: all checks passed\n");
    return 0;
}
