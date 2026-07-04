/* cap_authority — see cap_authority.hpp. The host capability authority: mint (grant) + verify (cap.check) +
 * a budget/revocation registry, over an "capabilities" BusClient. Pure policy/host logic on top of the hc_caps
 * primitive; the only secret it holds is the per-process signing key (scrubbed at stop). */

#include "cap_authority.hpp"

#include "hc_bus.hpp"
#include "hc_json.h"
#include "hc_secrets.h" /* hc_secrets_zero — scrub the signing key on teardown */

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>  /* open(/dev/urandom) for the signing key + per-grant nonce */
#include <unistd.h> /* read/close /dev/urandom */

namespace hc::host {

namespace {

/* Wall-clock ms since the epoch — for expiry evaluation (a 4-line helper, also present in host_bridge.cpp's
 * MemoryBroker section; not worth a shared header for two call sites in the same library). */
uint64_t now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/* Fill `buf` with `n` bytes from the OS CSPRNG (/dev/urandom). false on any failure — the caller then runs
 * WITHOUT a capability authority, so every tool call falls back to the human AuthGate (the safe floor). */
bool fill_urandom(unsigned char *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r > 0) got += (size_t)r;
        else if (r < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    return got == n;
}

std::string cap_reply_body(bool allow)
{
    hc_json *o = hc_json_new_object();
    if (!o) return "{\"ok\":true,\"allow\":false}";
    hc_json_obj_set_bool(o, "ok", true);
    hc_json_obj_set_bool(o, "allow", allow);
    char       *s = hc_json_print(o, false);
    std::string out = s ? s : "{\"ok\":true,\"allow\":false}";
    free(s);
    hc_json_free(o);
    return out;
}

/* Reject a path the capability scope cannot safely vet lexically: absolute, empty, or with a ".." component.
 * (The actual write is ALSO jailed by hc_sandbox; this is the cap layer's own fail-closed pre-check, meeting
 * hc_caps's "canonicalize before authorizes" caller obligation by refusing anything not already canonical —
 * an unsafe path simply denies the cap, falling back to the human gate.) */
bool path_lexically_safe(const std::string &p)
{
    if (p.empty() || p[0] == '/') return false;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        size_t end = (j == std::string::npos) ? p.size() : j;
        if (end - i == 2 && p[i] == '.' && p[i + 1] == '.') return false;
        i = (j == std::string::npos) ? p.size() : j + 1;
    }
    return true;
}

} // namespace

CapabilityAuthority *CapabilityAuthority::start(const std::string             &sock,
                                                std::unordered_set<std::string> known_agents)
{
    BusClient *b = BusClient::connect(sock.c_str(), "capabilities");
    if (!b) return nullptr;
    CapabilityAuthority *a = new CapabilityAuthority();
    if (!fill_urandom(a->key_, sizeof a->key_)) { /* no key -> no authority (everything falls back to the gate) */
        delete b;
        delete a;
        return nullptr;
    }
    a->key_ready_ = true;
    a->bus_ = b;
    a->known_agents_ = std::move(known_agents);
    a->reader_ = std::thread([a] { a->reader_loop(); });
    return a;
}

CapabilityAuthority::~CapabilityAuthority() { stop(); }

void CapabilityAuthority::stop()
{
    if (bus_) bus_->shutdown();
    if (reader_.joinable()) reader_.join();
    delete bus_;
    bus_ = nullptr;
    if (key_ready_) {
        hc_secrets_zero(key_, sizeof key_); /* scrub the signing key — never logged/bussed/disk, gone on teardown */
        key_ready_ = false;
    }
}

void CapabilityAuthority::set_known_agents(std::unordered_set<std::string> a)
{
    {
        std::lock_guard<std::mutex> lk(known_mu_);
        known_agents_ = a;
    }
    /* Revoke-on-reap: a cap whose subject is no longer a live fleet id is revoked here (the operator/conductor
     * remove + fleet-change push runs through this). The token itself already died with the worker's RAM; this
     * also clears the registry + the UI so a respawned same-id worker inherits NO live grant.
     * KNOWN RESIDUAL (Low, closed in P09.3): the supervisor's CRASH auto-reap does NOT fire the fleet on_change,
     * so a crashed id lingers here until an operator fleet change / shutdown. Not exploitable — the token died
     * with the worker's RAM and is never persisted, and cap.check verifies the token (key-gated) BEFORE touching
     * the registry — but P09.3 (which delivers tokens to workers) should also drive this off the reap event. */
    std::lock_guard<std::mutex> rk(reg_mu_);
    for (auto &kv : reg_)
        if (a.find(kv.second.claims.subject) == a.end()) kv.second.revoked = true;
}

uint64_t CapabilityAuthority::grant(const hc_cap_claims &tmpl, std::string &out_token)
{
    out_token.clear();
    if (!key_ready_) return 0;
    hc_cap_claims c = tmpl;
    /* Fill the nonce BEFORE locking reg_mu_: /dev/urandom is a (potentially blocking) syscall and must not hold
     * the registry lock (which the reader's cap.check path needs). Refuse rather than mint a guessable nonce. */
    if (!fill_urandom(c.nonce, sizeof c.nonce)) return 0;
    char                        tok[HC_CAP_TOKEN_MAX];
    size_t                      tl = 0;
    std::lock_guard<std::mutex> lk(reg_mu_);
    uint64_t                    id = next_cap_id_++;
    c.cap_id = id;
    if (hc_caps_mint(&c, key_, sizeof key_, tok, sizeof tok, &tl) != HC_CAP_OK) return 0;
    reg_[id] = CapRecord{c, c.budget, false};
    out_token.assign(tok, tl);
    return id;
}

void CapabilityAuthority::revoke(uint64_t cap_id)
{
    std::lock_guard<std::mutex> lk(reg_mu_);
    auto                        it = reg_.find(cap_id);
    if (it != reg_.end()) it->second.revoked = true;
}

void CapabilityAuthority::revoke_agent(const std::string &agent)
{
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto &kv : reg_)
        if (agent == kv.second.claims.subject) kv.second.revoked = true;
}

void CapabilityAuthority::snapshot(std::vector<CapView> &out)
{
    out.clear();
    std::lock_guard<std::mutex> lk(reg_mu_);
    out.reserve(reg_.size());
    for (auto &kv : reg_) {
        const CapRecord &r = kv.second;
        out.push_back({kv.first, r.claims.subject, r.claims.verb, r.claims.scope, r.budget_left,
                       r.claims.not_after_ms, r.revoked});
    }
}

bool CapabilityAuthority::check(const std::string &from, const std::string &token, uint32_t verb,
                               const std::string &path)
{
    if (!key_ready_) return false;
    if (!path_lexically_safe(path)) return false; /* fail-closed on a non-canonical path -> the human gate */
    hc_cap_claims c;
    if (hc_caps_verify(token.c_str(), token.size(), key_, sizeof key_, &c) != HC_CAP_OK) return false; /* forged */
    if (!hc_caps_authorizes(&c, from.c_str(), verb, path.c_str(), now_ms())) return false; /* subj/verb/scope/exp */
    /* registry liveness — the budget test + spend is ONE critical section, so a worker cannot double-spend by
     * racing two concurrent cap.checks. */
    std::lock_guard<std::mutex> lk(reg_mu_);
    auto                        it = reg_.find(c.cap_id);
    if (it == reg_.end() || it->second.revoked || it->second.budget_left == 0) return false;
    it->second.budget_left--;
    return true;
}

void CapabilityAuthority::reader_loop()
{
    for (;;) {
        Message m;
        if (!bus_->recv(m)) break;
        if (m.type != "req") continue;
        hc_json *o = hc_json_parse(m.body.data(), m.body.size());
        if (!o) continue;
        std::string cmd = hc_json_get_str(o, "cmd", "");
        if (cmd != "cap.check") {
            hc_json_free(o);
            continue;
        }
        std::string token = hc_json_get_str(o, "token", "");
        std::string path = hc_json_get_str(o, "path", "");
        uint32_t    verb = (uint32_t)hc_json_get_int(o, "verb", 0);
        hc_json_free(o);
        /* Only a LIVE fleet id may exercise a capability (same filter AuthGate applies); an unknown / departed id
         * is DENIED with a reply, so a real worker falls back to the gate promptly rather than timing out. */
        bool known;
        {
            std::lock_guard<std::mutex> lk(known_mu_);
            known = known_agents_.find(m.from) != known_agents_.end();
        }
        bool allow = known && check(m.from, token, verb, path);
        std::lock_guard<std::mutex> sl(send_mu_);
        bus_->send_reply(m.from, m.corr, cap_reply_body(allow));
    }
}

} // namespace hc::host
