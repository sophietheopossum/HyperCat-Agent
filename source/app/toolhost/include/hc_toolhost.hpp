#ifndef HC_TOOLHOST_HPP
#define HC_TOOLHOST_HPP

/* hc_toolhost — the host-side supervisor + router for third-party tool subprocesses (Custom Tooling, Wave D).
 * It is to tool binaries what hc::Supervisor is to agentd workers: it launches them, authenticates each one's
 * bus check-in with a one-time spawn token (broker authorize-at-spawn -> confirm-at-checkin -> revoke-at-reap),
 * and reaps them. ON TOP of that lifecycle it ROUTES invocations: a worker/conductor sends a `tool.invoke` to
 * the "toolhost" bus id, the ToolHost forwards it to the owning tool process and relays the result back, so the
 * invoke path is a single funnel where the kill-switch, the per-call gate (D5), and the timeout apply.
 *
 * Owns:      a host BusClient (id "toolhost"), a registry of {manifest, token, pid, confirmed} per tool id, and
 *            one recv-loop thread (an ASYNC router: it never blocks awaiting a tool reply — it tracks pending
 *            invokes and relays replies as they arrive, sweeping timed-out ones). BORROWS the Broker (must
 *            outlive it) for id<->pid authorization + token-gated routing confirmation; the caller must have
 *            authorized + confirmed the "toolhost" id before start() (mirrors main's host-id block).
 * Threading: public methods are called from one owner thread; the recv loop runs for the ToolHost's life;
 *            the registry is mutex-guarded. ~ToolHost stops the loop, SIGTERM/SIGKILL-reaps every tool, joins.
 * Security:  each tool runs in its OWN confined process (it self-applies hc_confine via the SDK); a tool that
 *            does not check in with its exact one-time token is never confirmed and the broker withholds its
 *            routing. macOS/non-Linux: confinement is unavailable, so a tool fail-closes (refuses to run) and
 *            the ToolHost simply never confirms it -> third-party tools are effectively disabled, honestly.
 * Lifetime:  start() returns null on failure; the caller owns the unique_ptr. */

#include "tool_manifest.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hc {
class Broker; /* borrowed: id<->pid authorize + token-gated confirm/revoke (see hc_bus.hpp) */
}

namespace hcapp {

class ToolHost {
public:
    /* Start the ToolHost on the broker already listening at `sock`. Discovers tool packages under
     * `tools_root` (`<tools_root>/<id>/manifest.json` + either a native `bin/<id>` or a managed interpreter +
     * entry script) and LAUNCHES each whose id is in `enabled_ids` when `kill_switch_on` is false. A managed
     * (non-C) tool is jailed+exec'd by `launcher_path` (hc_tool_launch, resolved by the caller next to the host
     * binary); pass "" if no launcher is available, and managed tools then fail-closed (native tools still run).
     * `broker` is borrowed; the caller MUST have already authorized+confirmed the "toolhost" id on it. Null on
     * failure (can't connect / OOM). */
    static std::unique_ptr<ToolHost> start(const std::string &sock, hc::Broker *broker,
                                           const std::string &tools_root, const std::string &launcher_path,
                                           bool kill_switch_on, const std::vector<std::string> &enabled_ids);
    ~ToolHost();

    ToolHost(const ToolHost &) = delete;
    ToolHost &operator=(const ToolHost &) = delete;

    /* One tool function the host exposes (to the Tools panel snapshot + the worker proxy that registers a shim
     * per function). A mutex-guarded snapshot copy — safe from the host thread while the loop mutates state. */
    struct FnView {
        std::string tool_id;     /* the bus id "tool:<id>" */
        std::string name;        /* the model-facing function name */
        std::string description; /* from the manifest */
        std::string spec_json;   /* the function spec */
        bool        sensitive = false;
        /* The package grants egress but no fs-write and no exec, AND this function is not individually flagged
         * sensitive. The HOST derives this from the manifest it parsed itself -- never from a worker's claim
         * about its own risk -- and hands the gate the resulting name set. */
        bool        readonly_egress = false;
        bool        running = false; /* the tool process is launched + token-confirmed */
    };
    std::vector<FnView> functions() const;

    /* True once at least one launched tool has completed its token check-in (for tests / readiness polling). */
    bool any_ready() const;

    /* The kill-switch's IMMEDIATE action (D5): SIGTERM/SIGKILL every running tool, revoke their bus ids, clear
     * the registry, and refuse all further check-ins/invokes/list. One-way for this process's lifetime — the
     * host persists the setting, so re-enabling takes effect on the next launch. Called from the owner thread
     * when the operator arms "disable all third-party tools" (the security "stop everything now" affordance). */
    void disable_all_live();

    /* Live enable/disable of ONE installed tool package (Wave E — the management UX). launch_one validates +
     * pin-verifies + spawns + authorizes the package under `<tools_root>/<id>/` and returns true if it spawned
     * (it still token-checks-in before it routes); a no-op if already running or the kill-switch is armed.
     * reap_one SIGTERM/SIGKILL-stops + revokes + unregisters it. Both run on the owner (host) thread; the
     * registry is mutex-guarded against the recv loop. The host persists the enable state + writes the
     * manifest.lock at operator approval; these just bring the process up/down to match. */
    bool launch_one(const std::string &id);
    void reap_one(const std::string &id);

private:
    ToolHost();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

/* The supply-chain pin (D5): the lowercase-hex sha256 TREE hash of a tool package — the validated
 * `<dir>/manifest.json` bytes (passed in, since the caller has them) folded with every other file under `<dir>`
 * in a canonical, symlink-safe order, EXCLUDING the runtime workspace `work/` and the pin file `manifest.lock`.
 * One hash covers both runtimes: a native tool's `bin/<id>` and a managed tool's interpreter script(s)/modules
 * are all pinned. The operator-approved install (Wave E) writes this to `<dir>/manifest.lock`; ToolHost::start
 * recomputes + compares at every launch and refuses a tool whose bytes changed (or that has no lock). Returns ""
 * on an unreadable/oversized/anomalous tree. Local integrity only — NOT a signing authority (the
 * network-distribution / signing path is a documented v1 deferral). */
std::string tool_lock_hex(const std::string &dir, const char *manifest_bytes, size_t manifest_len);

/* Read `<dir>/manifest.lock` (the stored approval hex), trailing whitespace trimmed; "" if absent. The single
 * reader of the lock-file wire format — shared by the launch-time pin verify (hc_toolhost) and the panel's
 * "approved?" check (host_services), so the format lives in one place. */
std::string read_tool_lock(const std::string &dir);

} // namespace hcapp

#endif /* HC_TOOLHOST_HPP */
