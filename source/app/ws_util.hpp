#ifndef HC_WS_UTIL_HPP
#define HC_WS_UTIL_HPP

/* ws_util — encode a bus id into ONE safe workspace path component. Shared by the host's workspace
 * PROVISIONING (app/main.cpp creates `<ws_root>/<ws_component(id)>`) and the diff READ path
 * (app/host_services.cpp opens that same dir): both MUST encode an id identically, so a single source of
 * truth prevents a divergence that would break workspace isolation / the diff lookup. Header-only — a
 * tiny pure function, no link dependency. */

#include <cctype>
#include <string>

namespace hcapp {

/* Percent-encode a bus id ("agent:A") into one path component ("agent%3AA"): alphanumerics pass through,
 * every other byte becomes %XX. INJECTIVE (distinct ids -> distinct dirs, so one agent is never jailed
 * into another's files) and free of '/' or '..', so a hostile id cannot escape the workspace root via its
 * own name. */
inline std::string ws_component(const std::string &id)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string       s;
    for (unsigned char c : id) {
        if (std::isalnum(c)) {
            s += (char)c;
        } else {
            s += '%';
            s += hex[c >> 4];
            s += hex[c & 0xF];
        }
    }
    return s;
}

/* The workspace SUBDIR an agent is jailed to under ws_root. Default: the per-agent dir (ws_component(id)) —
 * each worker isolated in its own tree (the locked per-agent isolation). When `shared` is on (Worker Revamp
 * W3 — opt-in, default-OFF), EVERY agent maps to one constant "shared" dir, so a dev->qa->polish pipeline
 * builds on the same files. This deliberately RELAXES per-agent isolation to session-shared; it is sound only
 * because the fleet is same-uid (the sandbox is a mediation jail, not a privilege boundary) and the shared
 * dir is still a jail (no worker escapes ws_root/shared; cross-SESSION isolation — a different ws_root per
 * host run — is untouched). Single source of truth for provisioning + the diff/verify read paths, exactly
 * like ws_component. */
inline std::string ws_subdir(const std::string &id, bool shared)
{
    return shared ? std::string("shared") : ws_component(id);
}

} // namespace hcapp

#endif /* HC_WS_UTIL_HPP */
