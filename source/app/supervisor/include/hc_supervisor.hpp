#ifndef HC_SUPERVISOR_HPP
#define HC_SUPERVISOR_HPP

/* hc_supervisor — host-side lifecycle manager for agent worker processes (doc 02/04, C++ host).
 *
 * Purpose:   spawn agentd worker processes; authenticate each one's bus check-in with a one-time
 *            spawn token (binding a bus id to the process the supervisor actually launched — the
 *            id<->spawn authorization the bus deferred); monitor them (reap exits, optional
 *            restart); and stop them. The single owner of "which pids are my workers."
 * Owns:      a worker registry (id -> {pid, token, ready, alive}); a host BusClient (id "host")
 *            used ONLY to answer worker check-ins; a monitor thread (waitpid) and a check-in
 *            thread (bus recv). Each worker is a separate OS process the supervisor reaps. It does
 *            NOT own the bus broker — the host starts that and passes its socket path here, so
 *            routing stays the bus's responsibility (no God Object).
 * Threading: public methods are called from one owner thread. Two internal threads run for the
 *            supervisor's life; the registry is mutex-guarded. shutdown()/dtor join them.
 * Security:  workers are authenticated at check-in by a one-time CSPRNG token handed down a private
 *            pipe (off argv/env). create() borrows the Broker, and spawn()/reap call
 *            Broker::authorize_id()/revoke_id() (an id registers only from the pid we launched — the
 *            LINUX hello-time floor) AND checkin_loop calls Broker::confirm_id() on a verified token,
 *            which TOKEN-GATES ROUTING: the broker withholds an id's task routing (req/reply, sub, pub)
 *            until confirmed,
 *            so a same-uid peer that wins the pid binding but lacks the token can't route. This CLOSES
 *            the id-squat — the token, not the recyclable pid, is the routing authority (portable: it
 *            holds on macOS too, where the hello pid gate is a no-op). reap() (DELIBERATE removal) calls
 *            revoke_id() SYNCHRONOUSLY, and revoke_id also cuts any still-live connection, so the freed id
 *            can no longer route the moment reap() returns — the remove-path squat closure, symmetric with
 *            the spawn-path authorize. create() refuses a socket dir
 *            that is not 0700 and owned by us. Other residual same-uid items (broker per-frame
 *            slow-loris deadline; a pid-reuse window in signal(); waitpid(-1) specificity once the
 *            host has non-worker children) are logged, not yet closed.
 */

#include <string>
#include <utility>
#include <vector>

namespace hc {

class Broker; /* borrowed at create() for the id<->pid authorization (see hc_bus.hpp) */

class Supervisor {
public:
    /* Build a supervisor for the broker already listening at `sock_path`; spawn `worker_exe`
     * (the agentd binary) for each worker. Connects the host BusClient. `broker` is BORROWED (must
     * outlive the supervisor) and binds each spawned id to its pid (authorize at spawn, revoke at
     * reap); pass nullptr to skip the binding (e.g. a lower-level test). Null on failure. */
    static Supervisor *create(const std::string &sock_path, const std::string &worker_exe,
                              Broker *broker = nullptr);
    ~Supervisor();

    /* Spawn a worker that will register as `id` and check in with a fresh token. `extra_args` are
     * appended to the agentd command line (e.g. a per-worker mode) and are remembered, so an
     * auto-respawn re-applies them. False if the worker is already alive or the fork/exec failed.
     * Returns before the worker checks in — poll is_ready(). */
    bool spawn(const std::string &id, const std::vector<std::string> &extra_args = {});

    /* Snapshot the LIVE workers' (id, pid), for the host's per-worker resource sampler (WI-3). A mutex-
     * guarded copy — safe from the host/UI thread while the monitor thread mutates the registry; skips
     * reaped/dead workers. Returns by value. */
    std::vector<std::pair<std::string, long>> get_worker_pids();

    bool is_ready(const std::string &id); /* connected AND passed token check-in */
    bool is_alive(const std::string &id); /* process spawned and not yet reaped   */

    /* Send `sig` to `id`'s process (SIGKILL to simulate a crash, SIGTERM to ask it to stop).
     * False if the id is unknown or already reaped. */
    bool signal(const std::string &id, int sig);

    /* DELIBERATELY retire a worker (the inverse of spawn): SIGTERM it, let the monitor reap it WITHOUT
     * respawning (even if autorestart is on), erase it from the registry, and REVOKE its broker id<->pid +
     * token routing authority — so a same-uid peer cannot squat the freed id. A SIGTERM the worker ignores
     * is escalated to SIGKILL after a bounded wait. The revoke is synchronous + idempotent: when reap()
     * returns, the id can no longer route. False if `id` is unknown. Distinct from signal(SIGKILL), which
     * leaves the id dead-but-known (and respawnable). Safe to call from any owner thread. */
    bool reap(const std::string &id);

    void set_autorestart(bool on); /* respawn a worker that exits unexpectedly (default off) */

    void shutdown(); /* SIGKILL all workers, join threads, reap. Idempotent. */

private:
    Supervisor();
    struct Impl;
    Impl *p_;
};

} // namespace hc

#endif /* HC_SUPERVISOR_HPP */
