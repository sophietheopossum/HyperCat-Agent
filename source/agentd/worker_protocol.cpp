/* worker_protocol — the worker's bus protocol marshaling + the bounded reply-wait, moved out of
 * worker.cpp so the loop, the tools, and the protocol are each one concern. The canonical banner
 * (purpose / threading / lifetime) is in worker_protocol.hpp; this TU just implements it. */

#include "worker_protocol.hpp"

#include "hc_json.h"

#include <cerrno>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

namespace hc {

bool read_fd_all(int fd, std::string &out)
{
    char buf[256];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return true;
        out.append(buf, (size_t)r);
        if (out.size() > 4096) return false; /* the token is tiny; cap a runaway/hostile writer */
    }
}

std::string body_str(const std::string &body, const char *key, const char *defv)
{
    hc_json *o = hc_json_parse(body.data(), body.size());
    if (!o) return defv;
    std::string v = hc_json_get_str(o, key, defv);
    hc_json_free(o);
    return v;
}

TaskFields parse_task_fields(const std::string &body)
{
    TaskFields t;
    hc_json   *o = hc_json_parse(body.data(), body.size());
    if (!o) return t;
    t.task_id = hc_json_get_str(o, "task_id", "");
    t.title = hc_json_get_str(o, "title", "");
    t.desc = hc_json_get_str(o, "description", "");
    t.capability = hc_json_get_str(o, "capability", ""); /* P04: "verify" => the skeptic branch */
    t.artifact_path = hc_json_get_str(o, "artifact_path", ""); /* W1.3: the deliverable file ("" = none) */
    hc_json_free(o);
    return t;
}

std::string reply_body(bool ok, const char *key, const std::string &val)
{
    hc_json *o = hc_json_new_object();
    if (!o) return ok ? "{\"ok\":true}" : "{\"ok\":false}";
    hc_json_obj_set_bool(o, "ok", ok);
    if (key) hc_json_obj_set_str(o, key, val.c_str());
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

std::string checkin_body(const std::string &token)
{
    hc_json *o = hc_json_new_object();
    if (!o) return "{\"cmd\":\"checkin\"}";
    hc_json_obj_set_str(o, "cmd", "checkin");
    hc_json_obj_set_str(o, "token", token.c_str());
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

bool await_reply(BusClient &bus, uint64_t want, int budget_ms, Message *out)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool answered = false;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = budget_ms - elapsed;
        if (remaining <= 0) break; /* budget spent */
        bus.set_recv_timeout((int)remaining);
        Message r;
        if (!bus.recv(r)) break; /* timed out, or the bus dropped */
        if (r.corr == want && (r.type == "reply" || r.type == "err")) {
            answered = (r.type == "reply");
            if (answered && out) *out = r;
            break;
        }
        if (r.type == "req") /* someone called us mid-wait — decline, don't block them */
            bus.send_reply(r.from, r.corr, reply_body(false, "err", "busy"));
        /* else: an unrelated reply — ignore and keep waiting for our corr within the budget */
    }
    bus.set_recv_timeout(0); /* restore indefinite blocking for the main idle recv */
    return answered;
}

bool await_reply_patient(BusClient &bus, uint64_t want, long total_ms, Message *out)
{
    constexpr int kSliceMs = 3000; /* a healthy no-verdict slice blocks this long, then we re-check + loop */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (await_reply(bus, want, kSliceMs, out)) return true; /* the operator decided */
        /* the slice returned with no verdict: distinguish "still waiting" (connected) from "the host went away"
         * (closed) — the latter must NOT busy-spin and is not a deny-by-design, just an unreachable gate. */
        if (!bus.alive()) return false;
        if (total_ms > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= total_ms) return false; /* the operator opted into a bounded timeout, and it elapsed */
        }
        /* else: connected, no verdict yet -> keep waiting (patient; the default never times out) */
    }
}

bool ping_peer(BusClient &bus, const std::string &peer, uint64_t inner_corr)
{
    if (!bus.send_request(peer, inner_corr, "{\"cmd\":\"ping\"}")) return false;
    return await_reply(bus, inner_corr, 2000);
}

std::string task_result_body(const std::string &task_id, bool ok, const std::string &payload)
{
    hc_json *o = hc_json_new_object();
    if (!o) return "{\"cmd\":\"task.result\"}";
    hc_json_obj_set_str(o, "cmd", "task.result");
    hc_json_obj_set_str(o, "task_id", task_id.c_str());
    hc_json_obj_set_bool(o, "ok", ok);
    hc_json_obj_set_str(o, "payload", payload.c_str());
    char *s = hc_json_print(o, false);
    hc_json_free(o);
    std::string r = s ? s : "";
    free(s);
    return r;
}

} // namespace hc
