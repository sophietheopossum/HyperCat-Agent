#ifndef HC_BUS_HPP
#define HC_BUS_HPP

/* hc_bus — message broker + client over hc_transport (C++ host module).
 *
 * Purpose:   give endpoints (host, agents, ui) addressed messaging on top of the framed byte
 *            transport: request/reply (correlation id), publish/subscribe (topics), with bounded
 *            per-connection queues for backpressure. The Broker routes; a BusClient is one
 *            endpoint. Every message is serialized JSON — no pointer-passing (doc 04).
 * Owns:      the Broker owns its listener, per-connection threads, and routing tables; a
 *            BusClient owns its connection. Both are pimpl — no transport/thread types leak.
 * Threading: the Broker is internally multi-threaded (one reader + one writer thread per
 *            connection) and its public methods (start/stop) are called from one owner thread. A
 *            BusClient is single-owner; do not share one across threads. Per-connection invariant:
 *            a Conn's id and subscription set are touched only by ITS OWN reader thread (route and
 *            teardown both run there); the ONLY cross-thread state on a Conn is its outbound queue
 *            + closing flag, guarded by that conn's queue mutex. The shared routing tables are
 *            guarded by a single table mutex, never held together with a queue mutex.
 * Security:  the Broker treats every connected worker as untrusted and defends on six fronts:
 *            (1) peer-cred uid gate on every accept — a uid mismatch is dropped before any frame
 *            is read; (2) the sender's `from` is never trusted — `hello` binds an id to the
 *            connection and the broker stamps that authentic id onto every routed req/reply/pub,
 *            so a worker can only ever act as itself; (3) a `hello` for an already-bound (or
 *            empty) id is REFUSED, not overwritten — no id hijack; (4) per-connection bounded
 *            outbound queues bound memory (a stuck consumer yields a backpressure err, never
 *            unbounded growth); (5) subscriptions are capped per connection and per topic length,
 *            and a disconnecting worker's topic entries are reaped (no table growth via churn);
 *            (6) a fleet cap on concurrent connections bounds thread/fd growth from a same-uid
 *            connection flood (incl. a slow-loris that pins reader threads).
 *            (7) an AUTHORIZED-ID binding (id<->pid): authorize_id()/revoke_id() let the host bind a
 *            bus id to the one pid allowed to register it (the supervisor at spawn; the host for its
 *            own "host"/"orchestrator" ids) — a `hello` for an authorized id from any other pid is
 *            refused, and a squatter that registered first is evicted with its ROUTING AUTHORITY
 *            revoked synchronously (route() drops its in-flight/buffered frames). Peer pid is captured
 *            at accept (SO_PEERCRED). This raises the out-of-process same-uid id-squat from "trivially
 *            win the id" to "win a pid-reuse collision" — pid is a recyclable number, so a same-uid
 *            peer that obtains the bound pid value (pid reuse, e.g. in the reap window) is still
 *            accepted at HELLO. But ROUTING is now TOKEN-GATED (confirm_id), and this is the real
 *            closure: the broker withholds a managed `agent:<id>`'s task routing — req/reply AND
 *            sub/pub — until the supervisor verifies its spawn TOKEN at check-in (an unconfirmed
 *            managed id may only complete the check-in handshake with "host"). So a peer that registers
 *            a managed id but lacks the token never becomes routable — it can neither receive a
 *            task.assign, forge a task.result, nor subscribe to a stream. The token gate is
 *            PID-INDEPENDENT, so it closes the squat on macOS too (where the hello pid gate is a no-op);
 *            the pid binding + uid gate are an additional Linux hello-time filter. (Generic/unauthorized
 *            ids route freely — the gate protects only supervisor-managed ids; a new req handler must
 *            validate `from`, and the "host" id's req surface must stay token-guarded.)
 *            STILL DEFERRED (tracked in STATE.md / EXPANSIONS.md), hardening for a genuinely hostile
 *            same-uid peer: a per-frame recv timeout on the reader thread (a first-byte-then-deadline
 *            policy — a partial-frame slow-loris can still pin ONE reader thread, now bounded by the
 *            fleet cap); active reaping of a hung-but-not-closed connection; and an aggregate
 *            distinct-topic bound. (The host-private 0700 socket-dir obligation is enforced by the
 *            supervisor.)
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace hc {

struct Message {
    std::string type;  /* "hello" | "welcome" | "sub" | "subok" | "req" | "reply" | "pub" | "err" */
    std::string from;
    std::string to;
    std::string topic;
    uint64_t    corr = 0;
    std::string body;

    std::string        encode() const;                                  /* -> JSON frame */
    static bool        decode(const char *data, size_t len, Message &out);
};

class BusClient {
public:
    /* Connect to the broker at `sock_path`, register as `id` (sends hello, waits for welcome).
     * Returns null on failure (connect, registration refused, no welcome). Caller owns the
     * returned pointer; `delete` it (or RAII-wrap) to free — the destructor closes the connection. */
    static BusClient *connect(const char *sock_path, const std::string &id);
    ~BusClient();

    bool subscribe(const std::string &topic);                            /* waits for subok */
    bool send_request(const std::string &to, uint64_t corr, const std::string &body);
    bool send_reply(const std::string &to, uint64_t corr, const std::string &body);
    bool publish(const std::string &topic, const std::string &body);
    bool recv(Message &out);                       /* blocking; false on disconnect or bad frame */
    /* NON-CONSUMING liveness probe (does not read a frame): true if still connected, false if the broker/peer has
     * gone away. Lets a patient bounded reply-wait tell "no verdict yet" from "the host went away" — see await_reply
     * callers. Single-owner: alive() reads the same fd as send/recv, so calling it concurrently with send()/recv()
     * from another thread is a data race — drive it from the one owning thread. */
    bool alive() const;
    const std::string &id() const;                 /* valid for the lifetime of this BusClient   */

    /* Bound the next recv()s with a deadline (ms; 0 restores indefinite blocking). On timeout
     * recv() returns false, same as a disconnect. Use ONLY around a bounded reply-wait where the
     * peer answers atomically or is silent (e.g. an agent pinging a peer that may have died) —
     * see hc_transport_set_recv_timeout for the mid-frame-desync caveat. Set it, do the wait,
     * then set 0 to return the client to blocking for its normal idle recv. */
    void set_recv_timeout(int timeout_ms);

    /* Unblock a thread blocked in recv() on this client by shutting the connection down — for
     * clean teardown when recv() runs on a background thread (e.g. the supervisor's check-in
     * thread). After this the client is dead: recv() returns false and sends fail. Safe to call
     * from a different thread than the one in recv(); idempotent. The destructor still frees it. */
    void shutdown();

private:
    BusClient();
    struct Impl;
    Impl *p_;
};

class Broker {
public:
    /* Listen on `sock_path`; accept only peers whose uid == expected_uid (pass -1 for geteuid()).
     * Returns null on failure (e.g. the path is taken). Caller owns the returned pointer; `delete`
     * it to free — the destructor calls stop() (joins all threads, closes connections). */
    static Broker *start(const char *sock_path, long expected_uid);
    ~Broker();
    void stop();

    /* Bind a bus id to the one pid permitted to register it (the supervisor calls this at spawn with
     * the worker's pid; the host binds its own "host"/"orchestrator" ids with getpid()). A `hello`
     * for an authorized id from any other pid is REFUSED, and a peer that squatted the id before the
     * binding landed is evicted — so the broker's ROUTING identity, not just the host's readiness
     * view, is gated. revoke_id clears the binding (at reap / before a respawn re-binds). Thread-safe.
     * LINUX-enforced via SO_PEERCRED; where the OS exposes no peer pid (macOS), this is a no-op and
     * the dup-id refusal + uid gate remain the floor. */
    void authorize_id(const std::string &id, long pid);
    void revoke_id(const std::string &id);

    /* Token-gated routing: mark an id ROUTING-confirmed. The supervisor calls this after it verifies a
     * worker's one-time spawn TOKEN at check-in; the host self-confirms its own "host"/"orchestrator"
     * ids. Until an `agent:<id>` is confirmed the broker withholds its task routing — req/reply, sub,
     * and pub (it may only complete the check-in handshake with "host") — so a peer that wins the pid
     * binding but lacks the
     * token can neither receive a task.assign nor forge a task.result. revoke_id clears confirmation
     * (a reaped id must re-check-in). Thread-safe; PID-INDEPENDENT (so it also closes the squat on
     * macOS, where the hello pid gate is a no-op). */
    void confirm_id(const std::string &id);

private:
    Broker();
    struct Impl;
    Impl *p_;
};

} // namespace hc

#endif /* HC_BUS_HPP */
