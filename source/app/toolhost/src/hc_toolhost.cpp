/* hc_toolhost — implementation. See hc_toolhost.hpp.
 *
 * The recv loop is an ASYNC ROUTER (not a blocking await): it processes each frame as it arrives — a tool
 * check-in, a worker's tool.invoke, or a tool's reply — and tracks pending invokes by our forwarded corr so a
 * reply relays back without ever blocking the loop (which would stall other tools' check-ins). A time slice on
 * recv lets it sweep timed-out invokes (kill the hung tool, deny the caller). */

#include "hc_toolhost.hpp"

#include "hc_bus.hpp"
#include "tool_manifest.hpp"

#include "hc_fs.h"
#include "hc_hash.h"
#include "hc_json.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hcapp {

namespace {

long now_mono_ms()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* a 128-bit CSPRNG token, hex-encoded (mirrors hc_supervisor's gen_token). "" on failure. */
std::string gen_token()
{
    unsigned char raw[16];
    int           fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); /* never leak the rng fd into a spawned tool */
    if (fd < 0) return std::string();
    size_t got = 0;
    while (got < sizeof raw) {
        ssize_t r = read(fd, raw + got, sizeof raw - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return std::string();
        }
        got += (size_t)r;
    }
    close(fd);
    static const char *hex = "0123456789abcdef";
    std::string        s;
    s.reserve(sizeof raw * 2);
    for (unsigned char b : raw) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xf]);
    }
    return s;
}

bool write_all_fd(int fd, const char *p, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

/* posix_spawn a tool binary with a one-time token down a private pipe at an inherited fd (--token-fd). MIRRORS
 * app/supervisor/src/hc_spawn.cpp spawn_worker (the read end inherited at its fd number; the write end CLOEXEC
 * so it never reaches the child; posix_spawn to stay off the fork/exec async-signal tightrope). Kept local so
 * the ToolHost stays decoupled from the supervisor; a shared spawn leaf is the noted dedup. -1 on failure. */
long spawn_tool(const std::string &exe, const std::vector<std::string> &args, const std::string &token)
{
    int p[2];
    if (pipe(p) != 0) return -1;
    fcntl(p[1], F_SETFD, FD_CLOEXEC);

    char fdbuf[16];
    std::snprintf(fdbuf, sizeof fdbuf, "%d", p[0]);

    std::vector<std::string> store;
    store.reserve(args.size() + 3);
    store.push_back(exe);
    for (const auto &a : args) store.push_back(a);
    store.push_back("--token-fd");
    store.push_back(fdbuf);

    std::vector<char *> argv;
    argv.reserve(store.size() + 1);
    for (auto &s : store) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    /* SCRUBBED env (mirrors the audio helper): a tool inherits NO environment, so the host's provider API key
     * (which the host mirrors into its own environ for the workers that DO call the LLM) never reaches an
     * untrusted tool process — a tool has no legitimate use for it, and getenv() needs no syscall, so a fully
     * confined tool would otherwise still read it. Everything a tool needs is on argv / the token fd. */
    char *const empty_env[] = {nullptr};

    pid_t pid = -1;
    int   rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), empty_env);
    close(p[0]);
    if (rc != 0) {
        close(p[1]);
        return -1;
    }
    write_all_fd(p[1], token.data(), token.size());
    close(p[1]);
    return (long)pid;
}

std::string body_str(const std::string &body, const char *key)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return std::string();
    std::string v = hc_json_get_str(o, key, "");
    hc_json_free(o);
    return v;
}

/* Resolve an allowlisted managed-runtime interpreter NAME to an absolute SYSTEM binary. The package never supplies
 * its own interpreter: we search a fixed set of root-owned system bindirs (NOT $PATH, NOT the package dir) and
 * return the first existing, executable, regular-file match. The name is re-checked against the allowlist here as
 * defense-in-depth (the manifest parse already enforced it). "" if not allowlisted or not found. */
std::string resolve_system_interpreter(const std::string &name)
{
    if (!tool_interpreter_allowed(name)) return std::string();
    static const char *const dirs[] = {"/usr/bin", "/bin", "/usr/local/bin"};
    for (const char *d : dirs) {
        std::string cand = std::string(d) + "/" + name;
        struct stat st;
        if (stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(cand.c_str(), X_OK) == 0) return cand;
    }
    return std::string();
}

/* Deterministically fold one directory subtree into `buf` for the supply-chain tree hash. Pre-order DFS with
 * lexicographically-sorted siblings (so the byte stream is canonical regardless of on-disk readdir order). Each
 * entry is framed unambiguously with NUL separators: a regular file as `relpath\0 f \0 declen \0 <bytes>`, a
 * symlink as `relpath\0 l \0 target \0` (NOT followed — we pin the link itself, never its pointee), a directory
 * as `relpath\0 d \0` then its (recursed) children. Any other entry type (socket/fifo/device) FAILS the hash —
 * a tool package is regular files + dirs + (rare) symlinks, nothing else; fail-closed beats silently ignoring it.
 *
 * Symlink-safe + TOCTOU-tight: every step is an *at()-syscall on the owned directory fd (openat O_NOFOLLOW for
 * descent + file reads, fstatat AT_SYMLINK_NOFOLLOW, readlinkat) — no path is re-resolved, so no intermediate
 * component can be swapped for a symlink between stat and read. At the package ROOT (`rel` empty) the runtime
 * workspace `work/`, the pin file `manifest.lock`, and `manifest.json` are skipped (the manifest is folded in
 * separately as the validated bytes; the workspace mutates at runtime; the lock is the output). `dfd_owned` is
 * consumed (closedir/closed here). Bounded: total content may not exceed `cap`; depth is capped to stop a planted
 * deep tree from exhausting the stack / fd table. Returns false on ANY error so the caller fails closed. */
bool hash_tree(int dfd_owned, const std::string &rel, std::string &buf, size_t base_len, size_t cap, int depth)
{
    DIR *dirp = fdopendir(dfd_owned);
    if (!dirp) {
        close(dfd_owned);
        return false;
    }
    if (depth > 64) {
        closedir(dirp);
        return false;
    }
    const int                dfd = dirfd(dirp);
    std::vector<std::string> names;
    for (struct dirent *e; (e = readdir(dirp));) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        if (rel.empty() && (std::strcmp(e->d_name, "work") == 0 || std::strcmp(e->d_name, "manifest.lock") == 0 ||
                            std::strcmp(e->d_name, "manifest.json") == 0))
            continue; /* the runtime workspace, the pin output, and the (separately-folded) manifest */
        names.emplace_back(e->d_name);
    }
    std::sort(names.begin(), names.end());

    bool ok = true;
    for (const std::string &name : names) {
        const std::string relpath = rel.empty() ? name : rel + "/" + name;
        struct stat       st;
        if (fstatat(dfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int cfd = openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            if (cfd < 0) {
                ok = false;
                break;
            }
            buf.append(relpath).append(1, '\0').append("d").append(1, '\0');
            if (!hash_tree(cfd, relpath, buf, base_len, cap, depth + 1)) { /* consumes cfd */
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            int ffd = openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (ffd < 0) {
                ok = false;
                break;
            }
            std::string contents;
            char        chunk[65536];
            for (;;) {
                ssize_t r = read(ffd, chunk, sizeof chunk);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    ok = false;
                    break;
                }
                if (r == 0) break;
                contents.append(chunk, (size_t)r);
                if (buf.size() + contents.size() > base_len + cap) { /* refuse to pin (and thus launch) a huge tree */
                    ok = false;
                    break;
                }
            }
            close(ffd);
            if (!ok) break;
            buf.append(relpath).append(1, '\0').append("f").append(1, '\0').append(std::to_string(contents.size()));
            buf.append(1, '\0').append(contents);
        } else if (S_ISLNK(st.st_mode)) {
            char    tgt[4096];
            ssize_t n = readlinkat(dfd, name.c_str(), tgt, sizeof tgt - 1);
            if (n < 0) {
                ok = false;
                break;
            }
            buf.append(relpath).append(1, '\0').append("l").append(1, '\0').append(tgt, (size_t)n).append(1, '\0');
        } else { /* socket/fifo/device/unknown: not legitimate tool content — fail closed */
            ok = false;
            break;
        }
    }
    closedir(dirp); /* closes dfd */
    return ok;
}

} // namespace

std::string tool_lock_hex(const std::string &dir, const char *manifest_bytes, size_t manifest_len)
{
    /* The pin is a deterministic TREE hash over the package: sha256( <validated manifest bytes> || the canonical
     * fold of every other file under <dir> except work/ + manifest.lock ). One primitive covers BOTH runtime
     * modes — a native tool's bin/<id> AND a managed tool's interpreter script(s)/modules are all pinned, so a
     * change to any launched byte is caught. (This generalizes the earlier manifest||bin/<id> hash for the
     * non-C path; see hash_tree for the canonical, symlink-safe fold.) sha256 is one-shot, so the fold is
     * buffered and bounded by `cap`.
     *
     * NOTE (honest residual, unchanged from the binary-only pin): hash and launch resolve the package twice, so a
     * co-resident SAME-UID peer could swap a file's inode between the hash and the spawn; the 0700 host-private
     * data dir excludes other-uid attackers, and that same-uid window is the project-wide accepted residual. */
    std::string buf;
    /* Frame the manifest prefix with a label + length (like a fold entry) so the manifest||tree boundary is
     * unambiguous (review M1): a raw concatenation could let a crafted manifest tail + a shifted first filename
     * reproduce another package's byte stream. The label "manifest" can't be confused with a same-named file
     * entry — a file's record puts a type tag ('f') where this puts the decimal length. */
    if (manifest_bytes && manifest_len) {
        buf.append("manifest").append(1, '\0').append(std::to_string(manifest_len)).append(1, '\0');
        buf.append(manifest_bytes, manifest_len);
    }
    const size_t base_len = buf.size();
    const size_t cap = 64u * 1024 * 1024; /* tools are small; refuse to pin (and thus launch) a huge tree */
    int          top = open(dir.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    if (top < 0) return std::string();
    if (!hash_tree(top, "", buf, base_len, cap, 0)) return std::string(); /* hash_tree consumes `top` */
    char hex[HC_SHA256_HEX_LEN];
    hc_sha256_hex_str(buf.data(), buf.size(), hex);
    return std::string(hex);
}

std::string read_tool_lock(const std::string &dir)
{
    size_t n = 0;
    char  *b = hc_fs_read_file((dir + "/manifest.lock").c_str(), 256, &n); /* malloc'd; freed below */
    if (!b) return std::string();
    std::string s(b, n);
    free(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

/* ---------- the ToolHost ---------- */

struct ToolProc {
    ToolManifest manifest;
    std::string  token;
    long         pid = -1;
    bool         confirmed = false;
};

struct ToolHost::Impl {
    hc::BusClient *bus = nullptr;
    hc::Broker    *broker = nullptr;
    std::string    sock;          /* the bus socket path — a live launch_one re-spawns with --sock */
    std::string    tools_root;    /* <data>/tools — a live launch_one re-reads <tools_root>/<id>/   */
    std::string    launcher_path; /* hc_tool_launch (sibling of the host) — the managed-runtime jailer; if "" a
                                   * managed tool cannot launch (native tools are unaffected) */

    mutable std::mutex                mu; /* guards tools / fn_to_tool / pending */
    std::map<std::string, ToolProc>   tools;      /* bus id "tool:<id>" -> proc */
    std::map<std::string, std::string> fn_to_tool; /* function name -> bus id   */

    struct Pending {
        std::string worker;
        long        worker_corr = 0;
        std::string tool_busid;
        long        deadline_ms = 0;
    };
    std::map<long, Pending> pending; /* our forwarded corr -> the caller to relay back to */
    long                    next_corr = 1000;

    std::thread       loop;
    std::atomic<bool> stopping{false};
    std::atomic<bool> disabled{false}; /* the kill-switch armed live: reject all traffic, registry already reaped */

    void run_loop();
    void handle_checkin(const hc::Message &m);
    void handle_invoke(const hc::Message &m);
    void handle_list(const hc::Message &m);
    void handle_tool_reply(const hc::Message &m);
    void sweep_timeouts();
    void reap_all();
    bool launch_package(const std::string &id); /* validate + pin-verify + spawn + authorize + register ONE package */
    void reap_package(const std::string &id);   /* SIGTERM/SIGKILL + revoke + unregister ONE running tool */
};

ToolHost::ToolHost() : p_(new Impl) {}
ToolHost::~ToolHost()
{
    if (!p_) return;
    p_->stopping = true;
    if (p_->bus) p_->bus->shutdown(); /* unblock the recv loop */
    if (p_->loop.joinable()) p_->loop.join();
    p_->reap_all();
    delete p_->bus;
}

/* start() runs long by design — it is one sequential setup pass over enabled_ids (validate manifest -> verify
 * supply-chain pin -> spawn confined -> authorize-at-spawn -> register); splitting it would thread identical
 * per-tool state through every helper for no isolation gain. */
std::unique_ptr<ToolHost> ToolHost::start(const std::string &sock, hc::Broker *broker,
                                          const std::string &tools_root, const std::string &launcher_path,
                                          bool kill_switch_on, const std::vector<std::string> &enabled_ids)
{
    std::unique_ptr<ToolHost> th(new ToolHost());
    th->p_->broker = broker;
    th->p_->sock = sock;
    th->p_->tools_root = tools_root;
    th->p_->launcher_path = launcher_path;
    th->p_->bus = hc::BusClient::connect(sock.c_str(), "toolhost");
    if (!th->p_->bus) {
        std::fprintf(stderr, "toolhost: could not connect as 'toolhost' (is the id authorized + confirmed?)\n");
        return nullptr;
    }

    /* launch each enabled, valid tool package (unless the global kill-switch is armed). The launch must run
     * BEFORE the recv loop starts (single-threaded mutation), so no lock is needed here. */
    if (!kill_switch_on)
        for (const std::string &id : enabled_ids) th->p_->launch_package(id);

    th->p_->loop = std::thread([impl = th->p_.get()] { impl->run_loop(); });
    return th;
}

/* Validate + pin-verify + spawn + authorize + register ONE tool package by id. Returns true if it spawned (the
 * tool still has to token-check-in before it is `confirmed` + routable). Shared by start() (pre-loop) and the
 * live launch_one() (host thread, post-loop) — the registry writes are mu-guarded against the recv loop.
 * SERIALIZATION (review F3): this MUST be called from a single thread (start() before the loop exists; then only
 * the host thread). The "already running?" check and the final insert are separate critical sections, so two
 * CONCURRENT launches of the same id would both spawn (orphaning a child); the single-caller invariant rules that
 * out — do not call this from the recv loop or a second thread.
 * The native-vs-managed branch lives INLINE here (not a helper) because it only swaps WHAT is spawned — a native
 * tool's own `bin/<id>` + bus args, vs the launcher + jail config + interpreter + entry — within the one shared
 * lifecycle (pin-verify -> token -> spawn -> authorize -> register). Both arms feed the SAME spawn_tool/authorize/
 * register tail, so extracting them would split a single linear setup across helpers for no isolation gain. */
bool ToolHost::Impl::launch_package(const std::string &id)
{
    std::string busid = "tool:" + id;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (disabled.load() || tools.count(busid)) return false; /* kill-switch armed, or already running */
    }
    std::string dir = tools_root + "/" + id;
    size_t      mlen = 0;
    char       *mbuf = hc_fs_read_file((dir + "/manifest.json").c_str(), 64u * 1024, &mlen);
    if (!mbuf) {
        std::fprintf(stderr, "toolhost: tool '%s' has no readable manifest — skipped\n", id.c_str());
        return false;
    }
    ToolManifest man;
    std::string  err;
    bool         ok = tool_manifest_parse(mbuf, mlen, id, man, err);
    /* supply-chain pin: recompute the manifest+package tree hash while we still hold the manifest bytes. */
    std::string want_lock = ok ? tool_lock_hex(dir, mbuf, mlen) : std::string();
    free(mbuf);
    if (!ok) {
        std::fprintf(stderr, "toolhost: tool '%s' manifest rejected: %s\n", id.c_str(), err.c_str());
        return false;
    }
    /* refuse a tool whose bytes don't match its operator-approved manifest.lock (or that has none). The install
     * flow (Wave E) writes the lock at operator approval; a mismatch means the bytes changed since — a tripwire
     * that must re-prompt the operator, never silently run the new bytes. */
    std::string have_lock = read_tool_lock(dir);
    if (want_lock.empty() || have_lock.empty() || want_lock != have_lock) {
        std::fprintf(stderr, "toolhost: tool '%s' REFUSED — manifest.lock %s (supply-chain pin)\n", id.c_str(),
                     have_lock.empty() ? "missing (not operator-approved)" : "mismatch (bytes changed since approval)");
        return false;
    }
    std::string token = gen_token();
    if (token.empty()) return false;

    /* The fs grant + egress flag are identical for both runtimes; only WHO receives them differs. A native tool
     * self-confines, so the bus + workspace args go straight on its own argv. A managed tool can't self-confine,
     * so hc_tool_launch receives the jail config (--pkg/--workspace/--allow-net) BEFORE `--`, then the resolved
     * interpreter + entry + the same bus args AFTER `--` (the launcher jails itself, then execs the interpreter
     * with the tail). In both cases spawn_tool appends `--token-fd N` last — for managed that lands in the exec
     * tail, which the launcher preserves across its execve and the interpreter reads. */
    const std::string ws = dir + "/work";
    const bool        want_ws = man.fs_mode != ToolFsMode::None;
    const char       *ws_mode = man.fs_mode == ToolFsMode::ReadWrite ? "rw" : "ro"; /* Read => read-only jail */
    if (want_ws) hc_fs_mkdirs(ws.c_str());                                          /* the per-tool workspace subtree */

    std::string              exe;
    std::vector<std::string> args;
    if (man.runtime == ToolRuntime::Managed) {
        std::string interp = resolve_system_interpreter(man.interpreter);
        if (interp.empty()) {
            std::fprintf(stderr, "toolhost: tool '%s' managed interpreter '%s' not found in a system bindir — skipped\n",
                         id.c_str(), man.interpreter.c_str());
            return false;
        }
        if (launcher_path.empty()) { /* no jailer resolved => fail closed (never run a managed tool unconfined) */
            std::fprintf(stderr, "toolhost: tool '%s' is managed but no launcher is configured — skipped\n",
                         id.c_str());
            return false;
        }
        exe = launcher_path;
        args.push_back("--pkg"); /* the launcher's jail config (consumed before the `--` separator) */
        args.push_back(dir);
        if (want_ws) {
            args.push_back("--workspace");
            args.push_back(ws);
            args.push_back("--workspace-mode");
            args.push_back(ws_mode);
        }
        if (!man.egress_hosts.empty()) args.push_back("--allow-net");
        args.push_back("--"); /* the program to exec under the jail: interpreter + entry + the bus args */
        args.push_back(interp);
        args.push_back(dir + "/" + man.entry);
        args.push_back("--sock");
        args.push_back(sock);
        args.push_back("--id");
        args.push_back(busid);
        args.push_back("--checkin-to");
        args.push_back("toolhost");
    } else {
        exe = dir + "/bin/" + id; /* a native tool self-confines via the SDK; the bus args are its own */
        args = {"--sock", sock, "--id", busid, "--checkin-to", "toolhost"};
        if (want_ws) {
            args.push_back("--workspace");
            args.push_back(ws);
            args.push_back("--workspace-mode");
            args.push_back(ws_mode);
        }
        if (!man.egress_hosts.empty()) args.push_back("--allow-net");
    }

    long pid = spawn_tool(exe, args, token);
    if (pid < 0) {
        std::fprintf(stderr, "toolhost: tool '%s' failed to spawn (%s)\n", id.c_str(), exe.c_str());
        return false;
    }
    if (broker) broker->authorize_id(busid, pid); /* bind the id to this pid (id-squat floor) */
    {
        std::lock_guard<std::mutex> lk(mu);
        ToolProc proc;
        proc.manifest = man;
        proc.token = token;
        proc.pid = pid;
        tools[busid] = std::move(proc);
        for (const auto &fn : man.tools) fn_to_tool[fn.name] = busid;
    }
    std::fprintf(stderr, "toolhost: launched tool '%s' (pid %ld, %zu function(s))\n", id.c_str(), pid,
                 man.tools.size());
    return true;
}

/* SIGTERM/SIGKILL + revoke + unregister ONE running tool (the live disable + remove path). Idempotent. */
void ToolHost::Impl::reap_package(const std::string &id)
{
    std::string busid = "tool:" + id;
    long        pid = -1;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto                        it = tools.find(busid);
        if (it == tools.end()) return;
        pid = it->second.pid;
        for (auto f = fn_to_tool.begin(); f != fn_to_tool.end();) { /* drop its function routes */
            if (f->second == busid) f = fn_to_tool.erase(f);
            else ++f;
        }
        tools.erase(it);
    }
    if (pid > 0) {
        kill((pid_t)pid, SIGTERM);
        for (int i = 0; i < 20 && waitpid((pid_t)pid, nullptr, WNOHANG) == 0; i++) {
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        kill((pid_t)pid, SIGKILL);
        waitpid((pid_t)pid, nullptr, 0);
    }
    if (broker) broker->revoke_id(busid); /* the freed id can no longer route */
}

void ToolHost::Impl::run_loop()
{
    bus->set_recv_timeout(500); /* 500ms slices so we can sweep timed-out invokes */
    while (!stopping) {
        hc::Message m;
        if (!bus->recv(m)) {
            if (!bus->alive()) break; /* the broker/host went away */
            sweep_timeouts();
            continue; /* a timeout slice — sweep + loop */
        }
        if (m.type == "req") {
            std::string cmd = body_str(m.body, "cmd");
            if (cmd == "checkin") handle_checkin(m);
            else if (cmd == "tool.invoke") handle_invoke(m);
            else if (cmd == "tool.list") handle_list(m);
            else bus->send_reply(m.from, m.corr, "{\"ok\":false,\"error\":\"unknown command\"}");
        } else if (m.type == "reply" || m.type == "err") {
            handle_tool_reply(m);
        }
        sweep_timeouts();
    }
}

void ToolHost::Impl::handle_checkin(const hc::Message &m)
{
    if (disabled.load()) { /* kill-switch armed: never confirm a (late) tool's routing */
        bus->send_reply(m.from, m.corr, "{\"ok\":false}");
        return;
    }
    /* m.from is the broker-stamped authentic id "tool:<id>"; verify the one-time token, then confirm routing. */
    std::string token = body_str(m.body, "token");
    bool        ok = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto                        it = tools.find(m.from);
        if (it != tools.end() && !it->second.token.empty() && it->second.token == token) {
            it->second.confirmed = true;
            ok = true;
        }
    }
    if (ok && broker) broker->confirm_id(m.from); /* token-gate: the tool may now route */
    bus->send_reply(m.from, m.corr, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void ToolHost::Impl::handle_invoke(const hc::Message &m)
{
    if (disabled.load()) { /* the kill-switch is armed — every third-party call is a bounded deny */
        bus->send_reply(m.from, m.corr, "{\"ok\":false,\"error\":\"third-party tools are disabled (kill-switch)\"}");
        return;
    }
    /* a worker/conductor asked us to invoke a function: find its tool, forward, and track the pending reply. */
    std::string fn = body_str(m.body, "tool");
    std::string args = body_str(m.body, "args");
    std::string busid;
    long        timeout_ms = 10000;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto                        f = fn_to_tool.find(fn);
        if (f != fn_to_tool.end()) {
            auto t = tools.find(f->second);
            if (t != tools.end() && t->second.confirmed) {
                busid = f->second;
                timeout_ms = t->second.manifest.timeout_ms;
            }
        }
    }
    if (busid.empty()) {
        bus->send_reply(m.from, m.corr,
                        "{\"ok\":false,\"error\":\"no such third-party tool (disabled, unknown, or not ready)\"}");
        return;
    }
    /* forward with a fresh corr; remember who to relay the reply to */
    long fwd;
    {
        std::lock_guard<std::mutex> lk(mu);
        fwd = ++next_corr;
        pending[fwd] = {m.from, (long)m.corr, busid, now_mono_ms() + timeout_ms};
    }
    hc_json *b = hc_json_new_object();
    hc_json_obj_set_str(b, "cmd", "tool.invoke");
    hc_json_obj_set_int(b, "v", 1);
    hc_json_obj_set_str(b, "tool", fn.c_str());
    hc_json_obj_set_str(b, "args", args.c_str());
    char *bs = hc_json_print(b, false);
    hc_json_free(b);
    bool sent = bs && bus->send_request(busid, (uint64_t)fwd, bs);
    free(bs);
    if (!sent) { /* the tool is unreachable — relay a denial now, drop the pending */
        std::lock_guard<std::mutex> lk(mu);
        pending.erase(fwd);
        bus->send_reply(m.from, m.corr, "{\"ok\":false,\"error\":\"tool unreachable\"}");
    }
}

void ToolHost::Impl::handle_list(const hc::Message &m)
{
    /* a worker/conductor asks which third-party functions are available so it can register a proxy per one.
     * Only CONFIRMED tools' functions are listed. Body: {"tools":[{"name","spec_json","timeout_ms"}, ...]}. */
    hc_json *root = hc_json_new_object();
    hc_json *arr = hc_json_new_array();
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto &kv : tools) {
            if (!kv.second.confirmed) continue;
            const bool env_sensitive = kv.second.manifest.envelope_sensitive();
            for (const auto &fn : kv.second.manifest.tools) {
                hc_json *e = hc_json_new_object();
                if (!e) continue;
                hc_json_obj_set_str(e, "name", fn.name.c_str());
                hc_json_obj_set_str(e, "spec_json", fn.spec_json.c_str());
                hc_json_obj_set_int(e, "timeout_ms", kv.second.manifest.timeout_ms);
                /* the worker proxy human-gates this call iff sensitive (D5): the fn opted in, OR the manifest
                 * grants fs-write/egress/exec (an envelope-sensitive package). */
                hc_json_obj_set_bool(e, "sensitive", fn.sensitive || env_sensitive);
                hc_json_arr_append(arr, e);
            }
        }
    }
    hc_json_obj_set(root, "tools", arr);
    char *s = hc_json_print(root, false);
    hc_json_free(root);
    bus->send_reply(m.from, m.corr, s ? s : "{\"tools\":[]}");
    free(s);
}

void ToolHost::Impl::handle_tool_reply(const hc::Message &m)
{
    Pending p;
    bool    found = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto                        it = pending.find((long)m.corr);
        /* match on corr AND the broker-stamped source: a reply is only honored from the EXACT tool we forwarded
         * this invoke to. Without the from-check a malicious confirmed tool could spray reply frames with guessed
         * corrs (they are a monotonic host counter) to hijack another tool's in-flight result and feed forged
         * output to that caller's model. The broker stamps m.from authentically, so this binds the relay. */
        if (it != pending.end() && it->second.tool_busid == m.from) {
            p = it->second;
            pending.erase(it);
            found = true;
        }
    }
    if (found) bus->send_reply(p.worker, (uint64_t)p.worker_corr, m.body); /* relay the tool's result verbatim */
}

void ToolHost::Impl::sweep_timeouts()
{
    long                                     now = now_mono_ms();
    std::vector<std::pair<long, Pending>>    expired;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.deadline_ms <= now) {
                expired.push_back(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &e : expired) {
        bus->send_reply(e.second.worker, (uint64_t)e.second.worker_corr,
                        "{\"ok\":false,\"error\":\"tool timed out\"}");
        /* kill the hung tool's process group — a tool that doesn't reply within its manifest timeout is reaped */
        long pid = -1;
        {
            std::lock_guard<std::mutex> lk(mu);
            auto                        t = tools.find(e.second.tool_busid);
            if (t != tools.end()) pid = t->second.pid;
        }
        if (pid > 0) kill((pid_t)pid, SIGKILL);
    }
}

void ToolHost::Impl::reap_all()
{
    std::vector<std::pair<std::string, long>> procs;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto &kv : tools)
            if (kv.second.pid > 0) procs.emplace_back(kv.first, kv.second.pid);
    }
    for (auto &pr : procs) kill((pid_t)pr.second, SIGTERM);
    /* brief grace, then SIGKILL stragglers */
    for (int i = 0; i < 20; i++) {
        bool any = false;
        for (auto &pr : procs)
            if (pr.second > 0 && waitpid((pid_t)pr.second, nullptr, WNOHANG) == 0) any = true;
        if (!any) break;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    for (auto &pr : procs) {
        kill((pid_t)pr.second, SIGKILL);
        waitpid((pid_t)pr.second, nullptr, 0);
        if (broker) broker->revoke_id(pr.first); /* the freed id can no longer route */
    }
    std::lock_guard<std::mutex> lk(mu);
    tools.clear();
    fn_to_tool.clear();
    pending.clear();
}

std::vector<ToolHost::FnView> ToolHost::functions() const
{
    std::vector<FnView> out;
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto &kv : p_->tools) {
        for (const auto &fn : kv.second.manifest.tools) {
            FnView v;
            v.tool_id = kv.first;
            v.name = fn.name;
            v.description = kv.second.manifest.description;
            v.spec_json = fn.spec_json;
            v.sensitive = fn.sensitive || kv.second.manifest.envelope_sensitive();
            v.running = kv.second.confirmed;
            out.push_back(std::move(v));
        }
    }
    return out;
}

bool ToolHost::any_ready() const
{
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto &kv : p_->tools)
        if (kv.second.confirmed) return true;
    return false;
}

void ToolHost::disable_all_live()
{
    if (!p_) return;
    p_->disabled.store(true); /* set FIRST so the recv loop stops confirming/forwarding before we reap */
    p_->reap_all();           /* SIGTERM/SIGKILL each tool, revoke its id, clear the registry (idempotent vs ~) */
}

bool ToolHost::launch_one(const std::string &id)
{
    return p_ ? p_->launch_package(id) : false;
}

void ToolHost::reap_one(const std::string &id)
{
    if (p_) p_->reap_package(id);
}

} // namespace hcapp
