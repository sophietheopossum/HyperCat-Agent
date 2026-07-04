/* hc_bus.cpp — message broker + client. See hc_bus.hpp for the contract.
 *
 * Broker model: thread-per-connection. A reader thread blocks on hc_transport_recv, decodes the
 * JSON envelope, and routes it (req/reply by `to`, pub by topic) by enqueuing the frame onto the
 * target connection's bounded outbound queue; a writer thread drains that queue to the socket.
 * A bounded queue gives backpressure (enqueue fails when full → an `err` is returned to a `req`'s
 * sender). A peer that dies surfaces as a recv error, tearing down only that connection (the
 * crash-isolation the multi-process model needs). Shutdown shuts down each endpoint (unblocking
 * its threads) before joining. Routing tables are mutex-guarded; queue and table locks are never
 * held together. Broker and BusClient share the Message codec + the send_msg helper and are
 * co-located in this TU by design — splitting them would need a third TU for the shared-only codec.
 */

#include "hc_bus.hpp"

#include "hc_json.h"
#include "hc_transport.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

namespace hc {

/* ---------- Message codec ---------- */

std::string Message::encode() const
{
    hc_json *o = hc_json_new_object();
    if (!o) return std::string();
    hc_json_obj_set_str(o, "t", type.c_str());
    if (!from.empty()) hc_json_obj_set_str(o, "from", from.c_str());
    if (!to.empty()) hc_json_obj_set_str(o, "to", to.c_str());
    if (!topic.empty()) hc_json_obj_set_str(o, "topic", topic.c_str());
    if (corr) hc_json_obj_set_int(o, "corr", (int64_t)corr);
    if (!body.empty()) hc_json_obj_set_str(o, "body", body.c_str());
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

bool Message::decode(const char *data, size_t len, Message &out)
{
    hc_json *o = hc_json_parse(data, len);
    if (!o) return false;
    out.type = hc_json_get_str(o, "t", "");
    out.from = hc_json_get_str(o, "from", "");
    out.to = hc_json_get_str(o, "to", "");
    out.topic = hc_json_get_str(o, "topic", "");
    out.corr = (uint64_t)hc_json_get_int(o, "corr", 0);
    out.body = hc_json_get_str(o, "body", "");
    hc_json_free(o);
    return !out.type.empty();
}

static bool send_msg(hc_transport *ep, const Message &m)
{
    std::string s = m.encode();
    return hc_transport_send(ep, s.data(), s.size()) == HC_TRANSPORT_OK;
}

/* ---------- Broker connection ---------- */

namespace {

constexpr size_t kQueueMax = 256;       /* per-connection outbound queue bound (backpressure) */
constexpr size_t kMaxSubsPerConn = 64;  /* bound a worker's subscriptions (anti-DoS)          */
constexpr size_t kMaxTopicLen = 128;    /* bound a topic-string length                        */
constexpr size_t kMaxConns = 256;       /* fleet cap: bound concurrent connections — a same-uid
                                         * peer flood (incl. partial-frame slow-loris that pins a
                                         * reader thread) can pin at most this many threads/fds   */

struct Conn {
    hc_transport *ep = nullptr;
    long          peer_pid = -1;           /* connecting peer's pid (SO_PEERCRED), -1 if none    */
    std::atomic<bool> authority_revoked{false}; /* set when evicted; route() then drops its frames */
    std::string   id;                      /* the AUTHENTIC registered id (set at hello)        */
    std::unordered_set<std::string> subs;  /* this conn's topics, reader-thread-owned            */

    std::mutex              qmu;
    std::condition_variable qcv;
    std::deque<std::string> q;
    bool                    closing = false;

    std::thread reader;
    std::thread writer;

    /* Enqueue an encoded frame; false == backpressure (queue full) or closing. */
    bool enqueue(const std::string &frame)
    {
        std::lock_guard<std::mutex> lk(qmu);
        if (closing || q.size() >= kQueueMax) return false;
        q.push_back(frame);
        qcv.notify_one();
        return true;
    }
};

} // namespace

struct Broker::Impl {
    hc_transport_listener *listener = nullptr;
    long                   expected_uid = -1;
    std::atomic<bool>      stopping{false};
    std::thread            accept_thread;

    std::mutex                                          table_mu;
    std::vector<std::shared_ptr<Conn>>                  conns;     /* all live conns           */
    std::unordered_map<std::string, std::shared_ptr<Conn>> by_id;  /* registered endpoints     */
    std::unordered_map<std::string, std::unordered_set<std::string>> topic_subs;
    std::unordered_map<std::string, long>               authorized_pids; /* id -> the only pid that
                                                                          * may register it (Linux) */
    std::unordered_set<std::string>                     confirmed_ids;   /* token-confirmed ids (the
                                                                          * supervisor verified a spawn
                                                                          * token, or the host self-
                                                                          * confirmed) — routable      */

    /* Reaper: a connection whose reader has finished (peer gone) hands itself here so a SEPARATE
     * thread can join its now-finished reader+writer and drop it from conns[]. A thread cannot
     * join itself, so without this, finished Conns (and their OS thread handles) would pile up
     * until stop(). This is steady-state cleanup for CLEAN disconnects; detecting a hung-but-
     * never-closed peer still needs a supervisor-side timeout (deferred, see hc_bus.hpp). */
    std::mutex                        reap_mu;
    std::condition_variable           reap_cv;
    std::deque<std::shared_ptr<Conn>> reap_queue;
    bool                              reap_stop = false;
    std::thread                       reaper_thread;

    void accept_loop();
    void reaper_loop();
    void reader_loop(std::shared_ptr<Conn> c);
    void writer_loop(std::shared_ptr<Conn> c);
    void route(const std::shared_ptr<Conn> &c, const Message &m);
    void handle_hello(const std::shared_ptr<Conn> &c, const Message &m);
    void handle_sub(const std::shared_ptr<Conn> &c, const Message &m);
    void handle_pub(const std::shared_ptr<Conn> &c, const Message &m);
    void handle_req_reply(const std::shared_ptr<Conn> &c, const Message &m);
    void teardown(const std::shared_ptr<Conn> &c);

    /* True for an id the supervisor AUTHORIZED (a managed worker id) that has NOT yet token-confirmed
     * — withheld from ALL task routing (req/reply, sub, pub); it may only complete the check-in
     * handshake with "host". CALL UNDER table_mu. */
    bool is_unconfirmed_managed(const std::string &id) const
    {
        return authorized_pids.count(id) != 0 && confirmed_ids.count(id) == 0;
    }
};

void Broker::Impl::writer_loop(std::shared_ptr<Conn> c)
{
    for (;;) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lk(c->qmu);
            c->qcv.wait(lk, [&] { return c->closing || !c->q.empty(); });
            if (c->q.empty()) return; /* closing with nothing left */
            frame = std::move(c->q.front());
            c->q.pop_front();
        }
        if (hc_transport_send(c->ep, frame.data(), frame.size()) != HC_TRANSPORT_OK) return;
    }
}

/* hello binds an id to THIS connection. The id is untrusted, so a duplicate (already-bound) id is
 * REFUSED rather than overwritten — a worker cannot hijack another agent's id. (Full id<->peer
 * authorization, e.g. a spawn token, lands with the supervisor.) */
void Broker::Impl::handle_hello(const std::shared_ptr<Conn> &c, const Message &m)
{
    bool refused;
    {
        std::lock_guard<std::mutex> lk(table_mu);
        refused = !c->id.empty() || m.from.empty() || by_id.count(m.from) != 0;
        if (!refused) {
            /* If this id is authorized to a specific pid (the supervisor bound it at spawn, or the
             * host bound its own ids), ONLY that pid may register it — this raises the same-uid
             * id-squat from "trivially win the id" to "win a pid-reuse collision" (the documented
             * residual; the spawn token is the cryptographic authority — see the banner). Enforced
             * only where the OS gives a USABLE peer pid (c->peer_pid > 0; Linux). On macOS or a
             * cross-namespace peer (pid <= 0) the dup-refusal above stays the floor. */
            auto ap = authorized_pids.find(m.from);
            if (ap != authorized_pids.end() && c->peer_pid > 0 && ap->second != c->peer_pid)
                refused = true;
        }
        if (!refused) {
            c->id = m.from;
            by_id[m.from] = c;
        }
    }
    Message r;
    r.type = refused ? "err" : "welcome";
    r.to = m.from;
    if (refused) r.body = "registration refused";
    c->enqueue(r.encode());
}

void Broker::Impl::handle_sub(const std::shared_ptr<Conn> &c, const Message &m)
{
    if (m.topic.empty() || m.topic.size() > kMaxTopicLen) return;
    bool ok_sub;
    bool unconfirmed = false;
    {
        std::lock_guard<std::mutex> lk(table_mu);
        if (is_unconfirmed_managed(c->id)) { /* an unconfirmed managed id can't subscribe (e.g. it must
                                              * not steal a peer's token stream) until it is confirmed */
            ok_sub = false;
            unconfirmed = true;
        } else {
            ok_sub = c->subs.size() < kMaxSubsPerConn;
            if (ok_sub) {
                c->subs.insert(m.topic);
                topic_subs[m.topic].insert(c->id);
            }
        }
    }
    Message r;
    r.type = ok_sub ? "subok" : "err";
    r.to = c->id;
    r.topic = m.topic;
    if (!ok_sub) r.body = unconfirmed ? "not confirmed" : "subscription limit";
    c->enqueue(r.encode());
}

void Broker::Impl::handle_pub(const std::shared_ptr<Conn> &c, const Message &m)
{
    std::vector<std::shared_ptr<Conn>> targets;
    {
        std::lock_guard<std::mutex> lk(table_mu);
        if (is_unconfirmed_managed(c->id)) return; /* an unconfirmed managed id can't publish either */
        auto it = topic_subs.find(m.topic);
        if (it != topic_subs.end())
            for (const auto &sid : it->second) {
                auto j = by_id.find(sid);
                if (j != by_id.end()) targets.push_back(j->second);
            }
    }
    Message out = m;
    out.from = c->id; /* stamp authentic sender */
    std::string frame = out.encode();
    for (auto &t : targets) t->enqueue(frame); /* best-effort: drop on backpressure */
}

void Broker::Impl::handle_req_reply(const std::shared_ptr<Conn> &c, const Message &m)
{
    std::shared_ptr<Conn> target;
    bool                  withheld = false;
    {
        std::lock_guard<std::mutex> lk(table_mu);
        /* Token-gated routing: an id the supervisor AUTHORIZED (a managed worker id) but has NOT yet
         * CONFIRMED (token-verified at check-in) is withheld from task routing — it may only complete
         * the check-in handshake with "host". So a same-uid peer that wins the pid binding for a worker
         * id but lacks the spawn token can neither receive a task.assign nor forge a task.result. The
         * gate protects ONLY supervisor-managed ids; a generic (unauthorized) id routes freely — its
         * containment rests on the orchestrator's assignee==from check + the worker's controller gate,
         * so every NEW req/reply handler MUST validate `from`. The "host" exception passes the check-in
         * handshake (worker->host req + host->worker reply); the host's "host"-id req surface must stay
         * token-guarded (today only checkin_loop, which validates the token) — do not add an unguarded
         * verb there. "toolhost" is the SYMMETRIC exception for third-party tool processes (Custom Tooling):
         * it is a host-side, self-confirmed service that validates the same one-time spawn token at the tool's
         * check-in (ToolHost::handle_checkin), so an unconfirmed `tool:<id>` may complete its handshake with
         * "toolhost" exactly as a worker does with "host" — and nothing else until confirmed. */
        bool checkin_channel =
            (c->id == "host" || m.to == "host" || c->id == "toolhost" || m.to == "toolhost");
        if (!checkin_channel && (is_unconfirmed_managed(c->id) || is_unconfirmed_managed(m.to)))
            withheld = true;
        else {
            auto it = by_id.find(m.to);
            if (it != by_id.end()) target = it->second;
        }
    }
    Message out = m;
    out.from = c->id; /* stamp authentic sender */
    bool delivered = !withheld && target && target->enqueue(out.encode());
    if (!delivered && m.type == "req") {
        Message e;
        e.type = "err";
        e.to = c->id; /* report to the authentic sender, not the claimed from */
        e.corr = m.corr;
        e.body = withheld ? "not confirmed" : (target ? "backpressure" : "no such endpoint");
        c->enqueue(e.encode());
    }
}

/* Route one decoded frame. hello registers; everything else requires a registered id (an
 * unregistered/forged sender is silently dropped) and NEVER trusts m.from — the handlers stamp the
 * authentic c->id onto routed frames, so a worker can only ever act as itself. */
void Broker::Impl::route(const std::shared_ptr<Conn> &c, const Message &m)
{
    if (m.type == "hello") {
        handle_hello(c, m);
        return;
    }
    if (c->id.empty()) return;
    /* An evicted squatter's reader may still drain frames already buffered in its socket; once its
     * routing authority is revoked (authorize_id evicted it) drop them, so it cannot deliver a frame
     * stamped with the authentic id after eviction. */
    if (c->authority_revoked.load(std::memory_order_acquire)) return;
    if (m.type == "sub")
        handle_sub(c, m);
    else if (m.type == "pub")
        handle_pub(c, m);
    else if (m.type == "req" || m.type == "reply")
        handle_req_reply(c, m);
    /* unknown type: ignore */
}

void Broker::Impl::teardown(const std::shared_ptr<Conn> &c)
{
    {
        std::lock_guard<std::mutex> lk(table_mu);
        if (!c->id.empty()) {
            auto it = by_id.find(c->id);
            if (it != by_id.end() && it->second == c) by_id.erase(it);
        }
        /* Reap only THIS conn's own subscriptions — not a full topic_subs scan — and drop any
         * topic whose subscriber set becomes empty, so a subscribe/disconnect churn cannot grow
         * topic_subs without bound. c->subs is reader-thread-owned (route + teardown run on the
         * same reader thread); table_mu guards only the shared topic_subs writes. */
        for (const auto &t : c->subs) {
            auto it = topic_subs.find(t);
            if (it != topic_subs.end()) {
                it->second.erase(c->id);
                if (it->second.empty()) topic_subs.erase(it);
            }
        }
        c->subs.clear();
    }
    std::lock_guard<std::mutex> lk(c->qmu);
    c->closing = true;
    c->qcv.notify_all();
}

void Broker::Impl::reader_loop(std::shared_ptr<Conn> c)
{
    for (;;) {
        void *buf = nullptr;
        size_t n = 0;
        if (hc_transport_recv(c->ep, &buf, &n) != HC_TRANSPORT_OK) {
            free(buf);
            break;
        }
        Message m;
        bool ok = Message::decode((const char *)buf, n, m);
        free(buf);
        if (ok) route(c, m);
    }
    teardown(c); /* peer gone — unregister id + reap this conn's subscriptions */
    /* Hand ourselves to the reaper to join this (about-to-return) reader + the writer and drop the
     * conn from conns[]; a thread cannot join itself. If the broker is already stopping, stop()
     * sweeps conns[] instead and this enqueue is harmlessly ignored. */
    {
        std::lock_guard<std::mutex> lk(reap_mu);
        reap_queue.push_back(c);
    }
    reap_cv.notify_one();
}

/* Reaper thread: join the threads of conns whose reader has finished, then forget them. Runs until
 * stop() sets reap_stop AND the queue is drained. */
void Broker::Impl::reaper_loop()
{
    for (;;) {
        std::shared_ptr<Conn> c;
        {
            std::unique_lock<std::mutex> lk(reap_mu);
            reap_cv.wait(lk, [&] { return reap_stop || !reap_queue.empty(); });
            if (reap_queue.empty()) return; /* stop requested and nothing left to reap */
            c = std::move(reap_queue.front());
            reap_queue.pop_front();
        }
        /* The reader that enqueued c is returning; teardown() already set closing so the writer
         * drains and exits. shutdown() also unblocks a writer stuck mid-send to a half-closed peer.
         * We are a DIFFERENT thread, so joining c's reader/writer is safe. */
        if (c->ep) hc_transport_shutdown(c->ep);
        if (c->reader.joinable()) c->reader.join();
        if (c->writer.joinable()) c->writer.join();
        /* INVARIANT (keep): a Conn reaches the reaper only AFTER its reader ran teardown(), which
         * erased it from by_id under table_mu. authorize_id touches a Conn's ep ONLY while that Conn
         * is still in by_id (also under table_mu). The two sets are therefore disjoint, so this close
         * can never race authorize_id's ep access. Do not move the by_id erase out of teardown. */
        if (c->ep) {
            hc_transport_close(c->ep);
            c->ep = nullptr;
        }
        std::lock_guard<std::mutex> lk(table_mu);
        for (auto it = conns.begin(); it != conns.end(); ++it)
            if (*it == c) {
                conns.erase(it);
                break;
            }
    }
}

void Broker::Impl::accept_loop()
{
    for (;;) {
        hc_transport *ep = hc_transport_accept(listener);
        if (!ep) break; /* listener closed */
        long uid = -1, pid = -1;
        if (!hc_transport_peer_cred(ep, &uid, &pid) || uid != expected_uid) {
            hc_transport_close(ep); /* peer-cred uid gate: drop unauthorized */
            continue;
        }
        auto c = std::make_shared<Conn>();
        c->ep = ep;
        c->peer_pid = pid; /* for the id<->pid check at hello; <=0 = no usable pid (macOS / cross-ns) */
        {
            std::lock_guard<std::mutex> lk(table_mu);
            if (conns.size() >= kMaxConns) {
                hc_transport_close(ep); /* fleet cap reached — refuse rather than exhaust */
                c->ep = nullptr;
                continue;
            }
            conns.push_back(c);
        }
        c->writer = std::thread([this, c] { writer_loop(c); });
        c->reader = std::thread([this, c] { reader_loop(c); });
    }
}

Broker::Broker() : p_(new Impl) {}

Broker *Broker::start(const char *sock_path, long expected_uid)
{
    Broker *b = new Broker();
    b->p_->expected_uid = (expected_uid < 0) ? (long)geteuid() : expected_uid;
    b->p_->listener = hc_transport_listen(sock_path);
    if (!b->p_->listener) {
        delete b;
        return nullptr;
    }
    b->p_->reaper_thread = std::thread([b] { b->p_->reaper_loop(); });
    b->p_->accept_thread = std::thread([b] { b->p_->accept_loop(); });
    return b;
}

void Broker::stop()
{
    Impl *p = p_;
    if (p->stopping.exchange(true)) return;

    hc_transport_listener *L = p->listener;
    if (L) hc_transport_listener_shutdown(L); /* wake the blocked accept() WITHOUT freeing it */
    if (p->accept_thread.joinable()) p->accept_thread.join();
    if (L) {
        hc_transport_listener_close(L);
        p->listener = nullptr;
    }

    /* Stop the reaper and let it drain any already-finished conns (joining + removing them from
     * conns[]). Once it has exited, NO other thread touches conns[], so this thread can sweep
     * whatever remains (still-live conns, plus any that finished after the reaper exited) without
     * racing the reaper — no Conn is ever joined twice. */
    {
        std::lock_guard<std::mutex> lk(p->reap_mu);
        p->reap_stop = true;
    }
    p->reap_cv.notify_all();
    if (p->reaper_thread.joinable()) p->reaper_thread.join();

    std::vector<std::shared_ptr<Conn>> all;
    {
        std::lock_guard<std::mutex> lk(p->table_mu);
        all = p->conns;
    }
    for (auto &c : all) {
        {
            std::lock_guard<std::mutex> lk(c->qmu);
            c->closing = true;
            c->qcv.notify_all();
        }
        hc_transport_shutdown(c->ep); /* unblock a reader/writer blocked in recv/send */
    }
    for (auto &c : all) {
        if (c->reader.joinable()) c->reader.join();
        if (c->writer.joinable()) c->writer.join();
    }
    for (auto &c : all)
        if (c->ep) {
            hc_transport_close(c->ep);
            c->ep = nullptr;
        }
    {
        std::lock_guard<std::mutex> lk(p->table_mu);
        p->conns.clear();
        p->by_id.clear();
        p->topic_subs.clear();
        p->reap_queue.clear();
    }
}

void Broker::authorize_id(const std::string &id, long pid)
{
    std::lock_guard<std::mutex> lk(p_->table_mu);
    p_->authorized_pids[id] = pid;
    p_->confirmed_ids.erase(id); /* a fresh pid binding (spawn or RESPAWN) forces re-confirmation: a
                                  * resurrected id must NOT inherit the dead instance's routing-confirm,
                                  * else a squatter in the respawn-before-checkin window would route */
    /* If a peer already holds this id under a DIFFERENT pid, it squatted before authorization landed
     * (a tight spawn race) — evict it so the real pid-matching process can register. We still hold
     * table_mu, and a Conn present in by_id has not yet torn down (teardown erases by_id under this
     * same lock), so the reaper cannot have closed its ep: shutting it down here just unblocks its
     * reader, which then tears down + is reaped normally. Linux only (peer_pid > 0). */
    auto it = p_->by_id.find(id);
    if (it != p_->by_id.end() && it->second->peer_pid > 0 && it->second->peer_pid != pid) {
        /* Revoke routing authority SYNCHRONOUSLY (route() checks the flag), not just unblock I/O: a
         * shutdown only stops FUTURE reads, so frames the squatter already pipelined into its socket
         * buffer could otherwise still route stamped as the authentic id after this returns. */
        it->second->authority_revoked.store(true, std::memory_order_release);
        if (it->second->ep) hc_transport_shutdown(it->second->ep);
        p_->by_id.erase(it);
    }
}

void Broker::revoke_id(const std::string &id)
{
    std::lock_guard<std::mutex> lk(p_->table_mu);
    p_->authorized_pids.erase(id);
    p_->confirmed_ids.erase(id); /* a reaped/revoked id must re-check-in to become routable again */
    /* Also CUT a still-LIVE connection holding this id (a reap of a WEDGED worker whose process hasn't exited
     * / whose socket hasn't EOF'd yet). Without this, clearing only the authority sets would DEMOTE a still-
     * confirmed worker to a generic, freely-routing id — the reap contract is "the freed id can no longer
     * route REGARDLESS of process liveness." Mirrors authorize_id's squatter eviction, but UNGATED by pid (the
     * token, not the recyclable pid, is the routing authority — so it holds on macOS too). The synchronous
     * authority_revoked flag drops even frames the worker already pipelined; the shutdown unblocks its reader,
     * which then tears down normally (clearing its by_id/topic_subs). On a clean reap the socket has usually
     * already EOF'd + torn down, so by_id is absent here and this is a no-op. */
    auto it = p_->by_id.find(id);
    if (it != p_->by_id.end()) {
        it->second->authority_revoked.store(true, std::memory_order_release);
        if (it->second->ep) hc_transport_shutdown(it->second->ep);
        p_->by_id.erase(it);
    }
}

void Broker::confirm_id(const std::string &id)
{
    std::lock_guard<std::mutex> lk(p_->table_mu);
    p_->confirmed_ids.insert(id); /* the supervisor verified its token (or the host self-confirms) */
}

Broker::~Broker()
{
    stop();
    delete p_;
}

/* ---------- BusClient ---------- */

struct BusClient::Impl {
    hc_transport *ep = nullptr;
    std::string   id;
};

BusClient::BusClient() : p_(new Impl) {}

BusClient::~BusClient()
{
    if (p_->ep) hc_transport_close(p_->ep);
    delete p_;
}

BusClient *BusClient::connect(const char *sock_path, const std::string &id)
{
    hc_transport *ep = hc_transport_connect(sock_path);
    if (!ep) return nullptr;
    BusClient *c = new BusClient();
    c->p_->ep = ep;
    c->p_->id = id;

    Message hello;
    hello.type = "hello";
    hello.from = id;
    if (!send_msg(ep, hello)) {
        delete c;
        return nullptr;
    }
    Message w;
    if (!c->recv(w) || w.type != "welcome") {
        delete c;
        return nullptr;
    }
    return c;
}

bool BusClient::subscribe(const std::string &topic)
{
    Message s;
    s.type = "sub";
    s.from = p_->id;
    s.topic = topic;
    if (!send_msg(p_->ep, s)) return false;
    Message ok;
    return recv(ok) && ok.type == "subok";
}

bool BusClient::send_request(const std::string &to, uint64_t corr, const std::string &body)
{
    Message m;
    m.type = "req";
    m.from = p_->id;
    m.to = to;
    m.corr = corr;
    m.body = body;
    return send_msg(p_->ep, m);
}

bool BusClient::send_reply(const std::string &to, uint64_t corr, const std::string &body)
{
    Message m;
    m.type = "reply";
    m.from = p_->id;
    m.to = to;
    m.corr = corr;
    m.body = body;
    return send_msg(p_->ep, m);
}

bool BusClient::publish(const std::string &topic, const std::string &body)
{
    Message m;
    m.type = "pub";
    m.from = p_->id;
    m.topic = topic;
    m.body = body;
    return send_msg(p_->ep, m);
}

bool BusClient::recv(Message &out)
{
    void *buf = nullptr;
    size_t n = 0;
    if (hc_transport_recv(p_->ep, &buf, &n) != HC_TRANSPORT_OK) {
        free(buf);
        return false;
    }
    bool ok = Message::decode((const char *)buf, n, out);
    free(buf);
    return ok;
}

const std::string &BusClient::id() const { return p_->id; }

bool BusClient::alive() const { return p_->ep && hc_transport_alive(p_->ep); }

void BusClient::shutdown()
{
    if (p_->ep) hc_transport_shutdown(p_->ep); /* unblocks a concurrent recv(); ep freed in dtor */
}

void BusClient::set_recv_timeout(int timeout_ms)
{
    if (p_->ep) hc_transport_set_recv_timeout(p_->ep, timeout_ms);
}

} // namespace hc
