/* test_toolhost — the end-to-end Custom Tooling integration gate (Wave D). Stands up a real broker, starts the
 * ToolHost over a temp tool package containing the SDK's hello_tool binary, lets it launch + token-check-in,
 * then plays the role of a worker: sends a tool.invoke to "toolhost" and asserts the tool's result relays back.
 * Plus negatives (unknown function denied). This exercises the SDK harness + the ToolHost launcher + the bus
 * round-trip together. Process/timing-sensitive (spawns a tool process), like the other *_gate tests. */

#include "hc_bus.hpp"
#include "hc_toolhost.hpp"

#include "hc_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace hcapp;

static int g_fail = 0;
#define CHECK(c, m)                                                                                              \
    do {                                                                                                         \
        if (!(c)) {                                                                                              \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                            \
            ++g_fail;                                                                                            \
        }                                                                                                        \
    } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

static bool write_file(const std::string &path, const char *data)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fputs(data, f);
    std::fclose(f);
    return true;
}

static std::string read_file(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char        buf[4096];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

/* COPY the built tool binary into the package's bin/<id> as a REAL file (the supply-chain pin opens it
 * O_NOFOLLOW, so a symlink — what an earlier version used — is now correctly refused; a real install copies). */
static bool copy_exec(const char *src, const std::string &dst)
{
    FILE *in = std::fopen(src, "rb");
    if (!in) return false;
    FILE *out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    char   buf[65536];
    size_t n;
    bool   ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    std::fclose(in);
    std::fclose(out);
    return ok && chmod(dst.c_str(), 0700) == 0;
}

/* read "ok" + a substring check on "result" from a reply body */
static bool reply_ok(const std::string &body)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    bool     ok = o && hc_json_get_bool(o, "ok", false);
    if (o) hc_json_free(o);
    return ok;
}
static bool reply_result_has(const std::string &body, const char *needle)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    bool     hit = false;
    if (o) {
        const char *r = hc_json_get_str(o, "result", "");
        hit = r && std::strstr(r, needle) != nullptr;
        hc_json_free(o);
    }
    return hit;
}

/* send a tool.invoke to "toolhost" and wait (bounded) for the corr-matched reply */
static bool invoke(hc::BusClient *w, uint64_t corr, const char *fn, hc::Message &out)
{
    std::string body = std::string("{\"cmd\":\"tool.invoke\",\"tool\":\"") + fn + "\",\"args\":\"{}\"}";
    if (!w->send_request("toolhost", corr, body)) return false;
    w->set_recv_timeout(5000);
    for (;;) {
        hc::Message r;
        if (!w->recv(r)) break;
        if (r.corr == corr && (r.type == "reply" || r.type == "err")) {
            out = r;
            w->set_recv_timeout(0);
            return r.type == "reply";
        }
    }
    w->set_recv_timeout(0);
    return false;
}

/* like invoke(), but with a caller-supplied arguments object (a JSON string embedded as the "args" string field) */
static bool invoke_args(hc::BusClient *w, uint64_t corr, const char *fn, const std::string &args_json, hc::Message &out)
{
    hc_json *b = hc_json_new_object();
    if (!b) return false;
    hc_json_obj_set_str(b, "cmd", "tool.invoke");
    hc_json_obj_set_str(b, "tool", fn);
    hc_json_obj_set_str(b, "args", args_json.c_str()); /* properly escapes the args string into the body */
    char *bs = hc_json_print(b, false);
    hc_json_free(b);
    if (!bs) return false;
    bool sent = w->send_request("toolhost", corr, bs);
    free(bs);
    if (!sent) return false;
    w->set_recv_timeout(5000);
    for (;;) {
        hc::Message r;
        if (!w->recv(r)) break;
        if (r.corr == corr && (r.type == "reply" || r.type == "err")) {
            out = r;
            w->set_recv_timeout(0);
            return r.type == "reply";
        }
    }
    w->set_recv_timeout(0);
    return false;
}

/* recursive best-effort cleanup of a temp tree (the negatives build several packages). */
static void rm_rf(const std::string &path)
{
    if (DIR *d = opendir(path.c_str())) {
        for (struct dirent *e; (e = readdir(d));) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = path + "/" + n;
            struct stat st;
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

enum class Lock { Valid, Wrong, None };

/* Build a tool package <root>/<id>/{manifest.json, bin/<id> -> binpath, [manifest.lock]} with one function
 * <fn> + the given timeout, and a Valid / Wrong / absent supply-chain lock. false on a setup error. */
static bool make_pkg(const std::string &root, const std::string &id, const std::string &fn, const char *binpath,
                     int timeout_ms, Lock lock)
{
    std::string dir = root + "/" + id;
    mkdir(dir.c_str(), 0700);
    mkdir((dir + "/bin").c_str(), 0700);
    char manifest[512];
    std::snprintf(manifest, sizeof manifest,
                  "{\"manifest_version\":1,\"id\":\"%s\",\"name\":\"%s\",\"tools\":[{\"name\":\"%s\","
                  "\"spec_json\":{\"type\":\"function\",\"function\":{\"name\":\"%s\"}}}],"
                  "\"permissions\":{\"fs\":{\"mode\":\"none\"}},\"limits\":{\"timeout_ms\":%d}}",
                  id.c_str(), id.c_str(), fn.c_str(), fn.c_str(), timeout_ms);
    if (!write_file(dir + "/manifest.json", manifest)) return false;
    if (!copy_exec(binpath, dir + "/bin/" + id)) return false;
    if (lock == Lock::None) return true;
    std::string hex;
    if (lock == Lock::Valid) {
        std::string man = read_file(dir + "/manifest.json");
        hex = hcapp::tool_lock_hex(dir, man.data(), man.size());
    } else {
        hex = std::string(64, '0'); /* valid-shape but WRONG digest */
    }
    return write_file(dir + "/manifest.lock", hex.c_str());
}

/* spin up an isolated broker for one scenario (own socket); authorizes+confirms toolhost + agent:test. */
static hc::Broker *scenario_broker(const std::string &sock)
{
    hc::Broker *b = hc::Broker::start(sock.c_str(), -1);
    if (!b) return nullptr;
    b->authorize_id("toolhost", (long)getpid());
    b->confirm_id("toolhost");
    b->authorize_id("agent:test", (long)getpid());
    b->confirm_id("agent:test");
    return b;
}

/* D6: a MISSING lock and a WRONG lock must BOTH refuse launch (the supply-chain pin) — no tool readies. */
static void test_hash_pin_refuses()
{
    char droot[] = "/tmp/hc_toolhost_pin.XXXXXX";
    if (!mkdtemp(droot)) { CHECK(0, "pin: mkdtemp"); return; }
    std::string root = droot, sock = root + "/bus.sock";
    CHECK(make_pkg(root, "nolock", "f", HELLO_TOOL_PATH, 5000, Lock::None), "pin: built no-lock package");
    CHECK(make_pkg(root, "badlock", "g", HELLO_TOOL_PATH, 5000, Lock::Wrong), "pin: built wrong-lock package");
    hc::Broker *broker = scenario_broker(sock);
    CHECK(broker != nullptr, "pin: broker started");
    if (broker) {
        auto th = ToolHost::start(sock, broker, root, /*launcher=*/"", /*kill_switch_on=*/false,
                                  {"nolock", "badlock"}); /* native fixtures: no launcher needed */
        CHECK(th != nullptr, "pin: ToolHost started");
        bool any = false;
        for (int i = 0; i < 50 && th && !(any = th->any_ready()); i++) sleep_ms(10);
        CHECK(!any, "pin: a missing/mismatched manifest.lock refuses launch (no tool readied)");
        CHECK(th && th->functions().empty(), "pin: no functions exposed when every package is refused");
        th.reset();
        broker->stop();
        delete broker;
    }
    rm_rf(root);
}

/* D6: arming the kill-switch (disable_all_live) reaps the running tool + denies every further invoke. */
static void test_kill_switch_denies()
{
    char droot[] = "/tmp/hc_toolhost_kill.XXXXXX";
    if (!mkdtemp(droot)) { CHECK(0, "kill: mkdtemp"); return; }
    std::string root = droot, sock = root + "/bus.sock";
    CHECK(make_pkg(root, "hello", "hello", HELLO_TOOL_PATH, 5000, Lock::Valid), "kill: built hello package");
    hc::Broker *broker = scenario_broker(sock);
    CHECK(broker != nullptr, "kill: broker started");
    if (broker) {
        auto th = ToolHost::start(sock, broker, root, /*launcher=*/"", false, {"hello"});
        bool ready = false;
        for (int i = 0; i < 300 && th && !(ready = th->any_ready()); i++) sleep_ms(10);
        CHECK(ready, "kill: hello readied");
        if (th && ready) {
            th->disable_all_live(); /* the operator arms the global kill-switch */
            CHECK(th->functions().empty(), "kill: functions cleared after disable_all_live");
            if (hc::BusClient *w = hc::BusClient::connect(sock.c_str(), "agent:test")) {
                hc::Message r;
                CHECK(invoke(w, 1, "hello", r), "kill: an invoke after disable still returns a (deny) reply");
                CHECK(!reply_ok(r.body), "kill: the call is DENIED once the kill-switch is armed");
                delete w;
            }
        }
        if (th) th.reset();
        broker->stop();
        delete broker;
    }
    rm_rf(root);
}

/* D6: a tool that never replies within its manifest timeout is reaped (SIGKILL) and the caller gets a bounded
 * deny — never a stall. The sleeper fixture sleeps ~forever; the manifest timeout is 500ms. */
static void test_timeout_reaps()
{
    char droot[] = "/tmp/hc_toolhost_to.XXXXXX";
    if (!mkdtemp(droot)) { CHECK(0, "timeout: mkdtemp"); return; }
    std::string root = droot, sock = root + "/bus.sock";
    CHECK(make_pkg(root, "sleeper", "sleep_forever", SLEEPER_TOOL_PATH, 500, Lock::Valid),
          "timeout: built sleeper package");
    hc::Broker *broker = scenario_broker(sock);
    CHECK(broker != nullptr, "timeout: broker started");
    if (broker) {
        auto th = ToolHost::start(sock, broker, root, /*launcher=*/"", false, {"sleeper"});
        bool ready = false;
        for (int i = 0; i < 300 && th && !(ready = th->any_ready()); i++) sleep_ms(10);
        CHECK(ready, "timeout: sleeper readied");
        if (th && ready) {
            if (hc::BusClient *w = hc::BusClient::connect(sock.c_str(), "agent:test")) {
                hc::Message r;
                CHECK(invoke(w, 1, "sleep_forever", r), "timeout: a hung tool still yields a bounded reply (no stall)");
                CHECK(!reply_ok(r.body), "timeout: the hung call is DENIED");
                CHECK(r.body.find("timed out") != std::string::npos, "timeout: the deny names the timeout");
                delete w;
            }
        }
        if (th) th.reset(); /* must not hang even though a tool was killed mid-call */
        broker->stop();
        delete broker;
    }
    rm_rf(root);
}

/* is a system python3 resolvable (the launcher resolves it from these fixed bindirs)? gate the managed test. */
static bool python3_available()
{
    const char *dirs[] = {"/usr/bin", "/bin", "/usr/local/bin"};
    for (const char *d : dirs) {
        std::string p = std::string(d) + "/python3";
        if (access(p.c_str(), X_OK) == 0) return true;
    }
    return false;
}

/* Build a MANAGED (Python) tool package: <root>/wordcount-py/{manifest.json, tool.py, hypercat_tool.py} + a valid
 * tree-hash lock. No bin/<id> — the host runs the system python3 on tool.py under the managed jail. */
static bool make_managed_pkg(const std::string &root, const std::string &id)
{
    std::string dir = root + "/" + id;
    mkdir(dir.c_str(), 0700);
    std::string manifest = "{\"manifest_version\":1,\"id\":\"" + id +
                           "\",\"name\":\"WordCount\",\"tools\":[{\"name\":\"word_count\",\"spec_json\":{\"type\":"
                           "\"function\",\"function\":{\"name\":\"word_count\"}}}],\"runtime\":{\"mode\":\"managed\","
                           "\"interpreter\":\"python3\",\"entry\":\"tool.py\"},\"permissions\":{\"fs\":{\"mode\":"
                           "\"none\"}},\"limits\":{\"timeout_ms\":5000}}";
    if (!write_file(dir + "/manifest.json", manifest.c_str())) return false;
    /* the entry script: one function, served over the launcher-provided --bus-fd (no socket() of its own) */
    const char *tool_py = "import hypercat_tool\n"
                          "def word_count(args):\n"
                          "    return str(len(args.get('text', '').split()))\n"
                          "raise SystemExit(hypercat_tool.serve({'word_count': word_count}))\n";
    if (!write_file(dir + "/tool.py", tool_py)) return false;
    std::string helper = read_file(HYPERCAT_TOOL_PY); /* vendor the SDK helper into the package */
    if (helper.empty()) return false;
    if (!write_file(dir + "/hypercat_tool.py", helper.c_str())) return false;
    std::string man = read_file(dir + "/manifest.json"); /* the tree-hash lock covers tool.py + hypercat_tool.py too */
    std::string hex = hcapp::tool_lock_hex(dir, man.data(), man.size());
    if (hex.empty()) return false;
    return write_file(dir + "/manifest.lock", hex.c_str());
}

/* THE NON-C PATH, end-to-end: a managed Python tool launched via hc_tool_launch (jail self -> pre-open the bus ->
 * exec python3 tool.py), which checks in over the inherited bus fd and serves an invoke — proving an interpreter
 * runs jailed with ZERO C in the tool. Skipped (not failed) where python3 is unavailable. */
static void test_managed_python()
{
    if (!python3_available()) {
        std::fprintf(stderr, "managed: python3 not found — SKIPPING the managed-Python gate (documented)\n");
        return;
    }
    char droot[] = "/tmp/hc_toolhost_managed.XXXXXX";
    if (!mkdtemp(droot)) { CHECK(0, "managed: mkdtemp"); return; }
    std::string root = droot, sock = root + "/bus.sock";
    CHECK(make_managed_pkg(root, "wordcount-py"), "managed: built the Python package (manifest + tool.py + helper)");
    hc::Broker *broker = scenario_broker(sock);
    CHECK(broker != nullptr, "managed: broker started");
    if (broker) {
        auto th = ToolHost::start(sock, broker, root, HC_TOOL_LAUNCH_PATH, /*kill_switch_on=*/false, {"wordcount-py"});
        CHECK(th != nullptr, "managed: ToolHost started");
        bool ready = false;
        for (int i = 0; i < 600 && th && !(ready = th->any_ready()); i++) sleep_ms(10); /* python startup is slower */
        CHECK(ready, "managed: the Python tool launched (jailed via the launcher) + checked in over --bus-fd");
        if (th && ready) {
            if (hc::BusClient *w = hc::BusClient::connect(sock.c_str(), "agent:test")) {
                hc::Message r;
                CHECK(invoke_args(w, 1, "word_count", "{\"text\":\"a b c d\"}", r),
                      "managed: word_count returned a reply");
                CHECK(reply_ok(r.body), "managed: the invoke succeeded (ok:true)");
                CHECK(reply_result_has(r.body, "4"),
                      "managed: the jailed Python tool computed the word count (4) and relayed it back");
                delete w;
            }
        }
        if (th) th.reset();
        broker->stop();
        delete broker;
    }
    rm_rf(root);
}

/* The NATIVE Rust SDK path: tools/rust-tool-check.sh cargo-builds the `reverse` example against the SDK fat lib
 * and passes its binary in via HC_RUST_REVERSE_BIN; this runs it through the ToolHost like any native tool — it
 * self-confines via the hc_tool_confine FFI, checks in, and serves an invoke. Skipped (not failed) when the env
 * var is absent (the normal test run, or a CI box without a Rust toolchain): the Rust crate is verified by the
 * CI gate, not by this default ctest run. */
static void test_native_rust()
{
    const char *rbin = getenv("HC_RUST_REVERSE_BIN");
    if (!rbin || !*rbin || access(rbin, X_OK) != 0) {
        std::fprintf(stderr, "rust: HC_RUST_REVERSE_BIN unset/not-exec — SKIPPING the native-Rust gate "
                             "(set by tools/rust-tool-check.sh)\n");
        return;
    }
    char droot[] = "/tmp/hc_toolhost_rust.XXXXXX";
    if (!mkdtemp(droot)) { CHECK(0, "rust: mkdtemp"); return; }
    std::string root = droot, sock = root + "/bus.sock";
    CHECK(make_pkg(root, "reverse-rs", "reverse", rbin, 5000, Lock::Valid), "rust: built the native Rust package");
    hc::Broker *broker = scenario_broker(sock);
    CHECK(broker != nullptr, "rust: broker started");
    if (broker) {
        auto th = ToolHost::start(sock, broker, root, /*launcher=*/"", /*kill_switch_on=*/false, {"reverse-rs"});
        CHECK(th != nullptr, "rust: ToolHost started");
        bool ready = false;
        for (int i = 0; i < 400 && th && !(ready = th->any_ready()); i++) sleep_ms(10);
        CHECK(ready, "rust: the native Rust tool launched (self-confined via the hc_tool_confine FFI) + checked in");
        if (th && ready) {
            if (hc::BusClient *w = hc::BusClient::connect(sock.c_str(), "agent:test")) {
                hc::Message r;
                CHECK(invoke_args(w, 1, "reverse", "{\"text\":\"HyperCat\"}", r), "rust: reverse returned a reply");
                CHECK(reply_ok(r.body), "rust: the invoke succeeded (ok:true)");
                CHECK(reply_result_has(r.body, "taCrepyH"), "rust: the Rust tool reversed the text + relayed it back");
                delete w;
            }
        }
        if (th) th.reset();
        broker->stop();
        delete broker;
    }
    rm_rf(root);
}

int main()
{
    char droot[] = "/tmp/hc_toolhost_test.XXXXXX";
    if (!mkdtemp(droot)) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string root = droot;
    const std::string sock = root + "/bus.sock";

    /* a tool package: <root>/hello/{manifest.json, bin/hello -> the built SDK example} */
    mkdir((root + "/hello").c_str(), 0700);
    mkdir((root + "/hello/bin").c_str(), 0700);
    CHECK(write_file(root + "/hello/manifest.json",
                     "{\"manifest_version\":1,\"id\":\"hello\",\"name\":\"Hello\",\"tools\":[{\"name\":\"hello\","
                     "\"spec_json\":{\"type\":\"function\",\"function\":{\"name\":\"hello\"}}}],"
                     "\"permissions\":{\"fs\":{\"mode\":\"none\"}},\"limits\":{\"timeout_ms\":5000}}"),
          "wrote manifest");
    CHECK(copy_exec(HELLO_TOOL_PATH, root + "/hello/bin/hello"), "copied the hello tool binary");

    /* the supply-chain pin: write manifest.lock = sha256(manifest||binary) so start() admits the tool (this is
     * exactly what the operator-approved install will do in Wave E). */
    {
        std::string man = read_file(root + "/hello/manifest.json");
        std::string hex = hcapp::tool_lock_hex(root + "/hello", man.data(), man.size());
        CHECK(!hex.empty(), "computed manifest.lock hash");
        CHECK(write_file(root + "/hello/manifest.lock", hex.c_str()), "wrote manifest.lock");
    }

    hc::Broker *broker = hc::Broker::start(sock.c_str(), -1);
    CHECK(broker != nullptr, "broker started");
    if (!broker) return 1;
    /* authorize+confirm the host-side ids (same-process: bound to this pid) */
    broker->authorize_id("toolhost", (long)getpid());
    broker->confirm_id("toolhost");
    broker->authorize_id("agent:test", (long)getpid());
    broker->confirm_id("agent:test");

    auto th = ToolHost::start(sock, broker, root, /*launcher=*/"", /*kill_switch_on=*/false, {"hello"});
    CHECK(th != nullptr, "ToolHost started");

    /* wait for the spawned tool to connect + token-check-in (it self-confines first) */
    bool ready = false;
    for (int i = 0; i < 300 && th && !(ready = th->any_ready()); i++) sleep_ms(10);
    CHECK(ready, "the hello tool launched + checked in");

    if (th && ready) {
        hc::BusClient *w = hc::BusClient::connect(sock.c_str(), "agent:test");
        CHECK(w != nullptr, "worker client connected");
        if (w) {
            hc::Message r;
            CHECK(invoke(w, 1, "hello", r), "tool.invoke('hello') returned a reply");
            CHECK(reply_ok(r.body), "the invoke succeeded (ok:true)");
            CHECK(reply_result_has(r.body, "greeting"), "the tool's result relayed back through the ToolHost");

            hc::Message r2;
            invoke(w, 2, "does_not_exist", r2); /* unknown function => a graceful denial, not a hang/crash */
            CHECK(!reply_ok(r2.body), "an unknown function is denied");
            delete w;
        }
    }

    th.reset();    /* stops the loop + reaps the tool process */
    broker->stop();
    delete broker;
    rm_rf(root);

    /* D6 — the negative security gates (each isolated: own broker, own socket, own temp root) */
    test_hash_pin_refuses(); /* a missing / mismatched manifest.lock refuses launch (supply-chain pin) */
    test_kill_switch_denies(); /* the kill-switch reaps + denies live */
    test_timeout_reaps();      /* a hung tool -> bounded deny + SIGKILL, never a stall */
    test_managed_python();     /* the NON-C path: a managed Python tool, launched + jailed + invoked end-to-end */
    test_native_rust();        /* the native Rust SDK path (gated on HC_RUST_REVERSE_BIN; the CI gate sets it) */

    if (g_fail == 0) std::printf("test_toolhost: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
