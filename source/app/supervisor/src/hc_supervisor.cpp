/* hc_supervisor — see hc_supervisor.hpp. Spawns + monitors + authenticates agentd workers.
 *
 * Two internal threads, each blocking on a different source:
 *   - monitor : waitpid(-1) — reaps worker exits; respawns when autorestart is on and not stopping.
 *   - check-in: host BusClient recv — answers worker check-ins, verifying the one-time spawn token
 *               against the registry (so a bus id is bound to the process we actually launched).
 * The registry (id -> {pid, token, ready, alive}) is guarded by one mutex; a condition variable
 * lets the monitor sleep when there are no children instead of spinning on ECHILD. Spawns are
 * serialized by holding that mutex across spawn_worker (its inheritable token-pipe fd requires it).
 */

#include "hc_supervisor.hpp"

#include "hc_spawn.hpp"

#include "hc_bus.hpp"
#include "hc_json.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime> /* nanosleep — reap()'s bounded wait */
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hc {

namespace {

/* Pull a string field from a JSON object body; defv when absent/malformed. */
std::string body_str(const std::string &body, const char *key, const char *defv)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return defv;
    std::string v = hc_json_get_str(o, key, defv);
    hc_json_free(o);
    return v;
}

/* {"ok":<ok>} reply body. */
std::string reply_body(bool ok)
{
    hc_json *o = hc_json_new_object();
    if (!o) return ok ? "{\"ok\":true}" : "{\"ok\":false}";
    hc_json_obj_set_bool(o, "ok", ok);
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

/* 128-bit token from the OS CSPRNG, hex-encoded. Empty string on failure (caller fails the spawn
 * rather than emit a guessable token). */
std::string gen_token()
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    unsigned char raw[16];
    size_t got = 0;
    while (got < sizeof raw) {
        ssize_t r = read(fd, raw + got, sizeof raw - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return "";
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != sizeof raw) return "";
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(sizeof raw * 2);
    for (unsigned char b : raw) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xf]);
    }
    return s;
}

/* Bounded sleep for reap()'s SIGTERM->reaped wait (the monitor owns waitpid; reap polls for the erase). */
void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

struct Worker {
    long                     pid = -1;
    std::string              token;
    std::vector<std::string> extra_args; /* per-worker agentd args, re-applied on respawn */
    bool                     ready = false; /* connected AND passed token check-in          */
    bool                     alive = false; /* spawned, not yet reaped                       */
    bool                     retiring = false; /* reap() set it: do NOT respawn; revoke + erase on reap */
    int                      restarts = 0;  /* consecutive auto-respawns without a check-in   */
};

constexpr int kMaxRestarts = 5; /* circuit breaker: stop auto-respawning an id that never starts
                                 * cleanly (e.g. its name is squatted) instead of a hot fork loop */
constexpr int kReapPollIter = 50; /* reap()'s SIGTERM->reaped poll budget: 50 x 10ms = ~500ms before escalating */
constexpr int kReapPollMs   = 10; /* the poll interval */

/* The bus socket must live in a host-private directory (mode 0700, owned by us) so a same-uid peer
 * cannot pre-bind or relink the path — the peer-cred uid gate only stops OTHER uids. Verify the
 * parent dir of `sock_path` before trusting the broker there. */
bool dir_is_private(const std::string &sock_path)
{
    std::string dir = sock_path;
    size_t      slash = dir.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    if (dir.empty()) dir = "/";
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != geteuid()) return false;
    return (st.st_mode & (S_IRWXG | S_IRWXO)) == 0; /* no group/other access => 0700-equivalent */
}

} // namespace

struct Supervisor::Impl {
    std::string sock_path;
    std::string worker_exe;
    BusClient  *host = nullptr;
    Broker     *broker = nullptr; /* borrowed; binds id<->pid at spawn/reap (may be null in tests) */

    std::mutex                                mu;
    std::condition_variable                   cv; /* monitor sleeps here when alive_count == 0 */
    std::unordered_map<std::string, Worker>   workers;    /* by id  */
    std::unordered_map<long, std::string>     pid_to_id;  /* reverse */
    int                                       alive_count = 0;
    bool                                      stopping = false;
    bool                                      autorestart = false;

    std::thread monitor_thread;
    std::thread checkin_thread;

    bool spawn_locked(const std::string &id, const std::vector<std::string> &extra_args); /* mu held */
    void monitor_loop();
    void checkin_loop();
    bool wait_gone(const std::string &id, int iters); /* reap(): poll for the monitor's retiring-erase */
};

bool Supervisor::Impl::spawn_locked(const std::string &id, const std::vector<std::string> &extra_args)
{
    std::string token = gen_token();
    if (token.empty()) return false;
    std::vector<std::string> args = {"--sock", sock_path, "--id", id};
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    long pid = spawn_worker(worker_exe, args, token);
    if (pid < 0) return false;

    Worker w;
    w.pid = pid;
    w.token = token;
    w.extra_args = extra_args;
    w.ready = false;
    w.alive = true;
    workers[id] = w;
    pid_to_id[pid] = id;
    alive_count++;
    cv.notify_all(); /* wake the monitor if it was idle */
    return true;
}

void Supervisor::Impl::monitor_loop()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return alive_count > 0 || stopping; });
            if (stopping && alive_count == 0) return;
        }
        int status = 0;
        /* waitpid(-1) reaps ANY child — correct only while the supervisor is the host's sole
         * child-spawner (true in v1; no exec tool yet). Revisit with per-pid waitpid / signalfd
         * when another subsystem forks (the step-5 exec tool) — see EXPANSIONS. We hold no lock
         * while blocking here. */
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR || errno == ECHILD) continue; /* cv gate re-blocks us if idle */
            return;
        }
        std::string id;
        long        new_pid = -1;
        bool        respawned = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = pid_to_id.find((long)pid);
            if (it == pid_to_id.end()) continue; /* not one of ours */
            id = it->second;
            pid_to_id.erase(it);
            auto w = workers.find(id);
            bool is_retiring = (w != workers.end()) && w->second.retiring;
            if (w != workers.end()) {
                w->second.alive = false;
                w->second.ready = false;
            }
            if (alive_count > 0) alive_count--;
            /* A DELIBERATELY retired worker (reap()) is NEVER respawned — even with autorestart on. It takes
             * the revoke path below (respawned stays false) and is then ERASED from the registry (it is gone,
             * not dead-but-known); reap() polls for that erase. */
            if (!stopping && autorestart && !is_retiring) {
                int                      prior = (w != workers.end()) ? w->second.restarts : 0;
                std::vector<std::string> extra =
                    (w != workers.end()) ? w->second.extra_args : std::vector<std::string>{};
                if (prior < kMaxRestarts) {
                    if (spawn_locked(id, extra)) {
                        workers[id].restarts = prior + 1; /* preserve args */
                        new_pid = workers[id].pid;
                        respawned = true;
                    }
                } else {
                    std::fprintf(stderr,
                                 "supervisor: '%s' exceeded the restart budget (%d); giving up\n",
                                 id.c_str(), kMaxRestarts);
                }
            }
            /* A retired id leaves the registry. NOTE: this invalidates the iterator `w` — nothing below in
             * this scope may use it (the broker calls outside the lock use the local `id`/`new_pid`). */
            if (is_retiring) workers.erase(id);
        }
        /* Update the broker's id<->pid binding OUTSIDE the registry lock (unnested lock order). A
         * respawn re-binds to the new pid (overwriting the dead one, evicting any stale squatter); a
         * give-up clears the binding so the id falls back to the dup-refusal floor. */
        if (broker) {
            if (respawned) broker->authorize_id(id, new_pid);
            else           broker->revoke_id(id);
        }
    }
}

void Supervisor::Impl::checkin_loop()
{
    for (;;) {
        Message m;
        if (!host->recv(m)) return; /* host->shutdown() (teardown) or the bus dropped */
        if (m.type != "req") continue;

        std::string cmd = body_str(m.body, "cmd", "");
        bool        ok = false;
        if (cmd == "checkin") {
            std::string token = body_str(m.body, "token", "");
            std::lock_guard<std::mutex> lk(mu);
            auto it = workers.find(m.from); /* m.from is the broker-stamped authentic id */
            if (it != workers.end() && it->second.alive && !it->second.token.empty() &&
                it->second.token == token) {
                it->second.ready = true;
                it->second.restarts = 0; /* a clean start resets the restart budget */
                ok = true;
            }
        }
        /* On a verified check-in, promote the id to ROUTING-confirmed in the broker BEFORE replying
         * (so the worker is routable the moment it proceeds). OUTSIDE the registry mu — the broker
         * takes its own table lock; keeping them unnested preserves the one-way lock order. */
        if (ok && broker) broker->confirm_id(m.from);
        host->send_reply(m.from, m.corr, reply_body(ok));
    }
}

Supervisor::Supervisor() : p_(new Impl) {}

Supervisor *Supervisor::create(const std::string &sock_path, const std::string &worker_exe,
                               Broker *broker)
{
    if (!dir_is_private(sock_path)) return nullptr; /* refuse a world/group-accessible socket dir */
    Supervisor *s = new Supervisor();
    s->p_->sock_path = sock_path;
    s->p_->worker_exe = worker_exe;
    s->p_->broker = broker;
    s->p_->host = BusClient::connect(sock_path.c_str(), "host");
    if (!s->p_->host) {
        delete s;
        return nullptr;
    }
    s->p_->checkin_thread = std::thread([s] { s->p_->checkin_loop(); });
    s->p_->monitor_thread = std::thread([s] { s->p_->monitor_loop(); });
    return s;
}

bool Supervisor::spawn(const std::string &id, const std::vector<std::string> &extra_args)
{
    long pid = -1;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->workers.find(id);
        if (it != p_->workers.end() && it->second.alive) return false; /* already running */
        if (!p_->spawn_locked(id, extra_args)) return false;
        pid = p_->workers[id].pid;
    }
    /* Bind the id to the just-spawned pid OUTSIDE the registry lock (the broker takes its own table
     * lock; keeping them unnested preserves a single lock order). Authorize beats the worker's first
     * connect (it retries with backoff), and authorize_id evicts a squatter that raced in first. */
    if (p_->broker) p_->broker->authorize_id(id, pid);
    return true;
}

bool Supervisor::is_ready(const std::string &id)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->workers.find(id);
    return it != p_->workers.end() && it->second.alive && it->second.ready;
}

bool Supervisor::is_alive(const std::string &id)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->workers.find(id);
    return it != p_->workers.end() && it->second.alive;
}

std::vector<std::pair<std::string, long>> Supervisor::get_worker_pids()
{
    std::vector<std::pair<std::string, long>> out;
    std::lock_guard<std::mutex>               lk(p_->mu);
    out.reserve(p_->workers.size());
    for (const auto &kv : p_->workers)
        if (kv.second.alive && kv.second.pid > 0) out.push_back({kv.first, kv.second.pid});
    return out;
}

bool Supervisor::signal(const std::string &id, int sig)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->workers.find(id);
    if (it == p_->workers.end() || !it->second.alive) return false;
    /* Narrow residual: if the monitor reaped this pid between its waitpid() and the alive=false
     * update, the pid may be recycled and this kill could hit an unrelated same-uid process. Low
     * risk in v1 (same-uid model; the OS keeps the pid as a zombie until the monitor reaps, and we
     * only signal during the test/shutdown). A pidfd path closes it — deferred (EXPANSIONS). */
    return kill((pid_t)it->second.pid, sig) == 0;
}

/* Poll (under mu) until `id` has left the registry (the monitor's retiring-erase) or the budget elapses;
 * true iff gone. reap() can't waitpid itself (the monitor owns waitpid(-1)), so it polls for the erase. */
bool Supervisor::Impl::wait_gone(const std::string &id, int iters)
{
    for (int i = 0; i < iters; i++) {
        sleep_ms(kReapPollMs);
        std::lock_guard<std::mutex> lk(mu);
        if (workers.find(id) == workers.end()) return true;
    }
    return false;
}

bool Supervisor::reap(const std::string &id)
{
    long pid = -1;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->workers.find(id);
        if (it == p_->workers.end()) return false; /* unknown id */
        if (it->second.alive) {
            it->second.retiring = true; /* the monitor will NOT respawn it; it revokes + erases on reap */
            pid = it->second.pid;
        } else {
            p_->workers.erase(it); /* already dead-but-known (crashed, autorestart off): erase here */
        }
    }
    if (pid > 0) {
        /* ESRCH = the monitor already reaped it between our unlock and this kill — fine, wait_gone confirms. */
        if (kill((pid_t)pid, SIGTERM) < 0 && errno != ESRCH)
            std::fprintf(stderr, "supervisor: reap SIGTERM '%s' failed (errno %d)\n", id.c_str(), errno);
        if (!p_->wait_gone(id, kReapPollIter)) { /* SIGTERM ignored / wedged -> hard-kill, then wait again */
            long kpid = -1;
            {
                std::lock_guard<std::mutex> lk(p_->mu);
                auto it = p_->workers.find(id);
                if (it != p_->workers.end() && it->second.alive) kpid = it->second.pid;
            }
            if (kpid > 0) kill((pid_t)kpid, SIGKILL);
            p_->wait_gone(id, kReapPollIter);
        }
    }
    /* De-authorize the id's bus routing on EVERY path. The monitor also revokes on reap, but doing it here
     * makes it synchronous + idempotent AND (via revoke_id) cuts a still-live wedged connection — so when
     * reap() returns the freed id can no longer route, closing the same-uid squat on a removed worker (the
     * token-gated routing authority withdrawn). */
    if (p_->broker) p_->broker->revoke_id(id);
    return true;
}

void Supervisor::set_autorestart(bool on)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->autorestart = on;
}

void Supervisor::shutdown()
{
    Impl *p = p_;
    {
        std::lock_guard<std::mutex> lk(p->mu);
        if (p->stopping) return;
        p->stopping = true;
        for (auto &kv : p->workers)
            if (kv.second.alive) kill((pid_t)kv.second.pid, SIGKILL); /* hard stop */
        p->cv.notify_all();
    }
    if (p->monitor_thread.joinable()) p->monitor_thread.join(); /* reaps every worker */
    if (p->host) p->host->shutdown();                           /* unblock the check-in recv */
    if (p->checkin_thread.joinable()) p->checkin_thread.join();
    delete p->host; /* check-in thread is joined; nobody else touches host */
    p->host = nullptr;
}

Supervisor::~Supervisor()
{
    shutdown();
    delete p_;
}

} // namespace hc
