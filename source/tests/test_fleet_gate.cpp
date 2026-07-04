/* test_fleet_gate — the Fleet's add-by-role + slot assignment + remove over REAL worker processes (offline).
 * Proves: add_worker(role) assigns the next FREE id from id_pool() (agent:A, agent:B, ...), spawns it, and it
 * checks in; remove_worker reaps it (Supervisor::reap) + frees the slot; a subsequent add REUSES the freed
 * slot. This is the runtime add/remove the operator drives from the Fleet panel (W2 P2.3). AGENTD_PATH from
 * CMake. Exit non-zero on any failure. */

#include "hc_bus.hpp"
#include "hc_fleet.hpp"
#include "hc_roles.hpp"
#include "hc_settings.hpp"
#include "hc_supervisor.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using hc::Broker;
using hc::Supervisor;
using namespace hcapp;

static int g_fail = 0;
#define CHECK(c, m)                                                                                        \
    do {                                                                                                   \
        if (!(c)) {                                                                                        \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

static bool has(const Fleet &f, const std::string &id, const std::string &role)
{
    for (const auto &w : f.pool())
        if (w.first == id && w.second == role) return true;
    return false;
}

int main()
{
    char dir[] = "/tmp/hc_fleetg_XXXXXX";
    if (!mkdtemp(dir)) {
        std::perror("mkdtemp");
        return 1;
    }
    std::string sock = std::string(dir) + "/bus.sock";
    std::string sess = std::string(dir) + "/sessions";

    Broker *broker = Broker::start(sock.c_str(), -1);
    CHECK(broker != nullptr, "broker start");
    if (!broker) return 1;
    Supervisor *sup = Supervisor::create(sock, AGENTD_PATH, broker); /* broker -> authorize/revoke on add/reap */
    CHECK(sup != nullptr, "supervisor create");
    if (!sup) {
        broker->stop();
        delete broker;
        return 1;
    }

    RoleTable  roles = roletable_builtin_defaults();
    std::mutex roles_mu; /* P2.3b: guards the role table; the Fleet locks it around the spawn-time read */
    Settings   settings;
    auto fleet = Fleet::create(sup, &roles, &settings, "", sess, &roles_mu); /* offline (no HC_MODEL); empty ws_root */
    CHECK(fleet != nullptr, "fleet create");

    /* P2.3b race check (TSan): mutate the role table on a SECOND thread (as EditRole does, under roles_mu) while
     * the main thread spawns workers (add_worker reads the table under roles_mu). The overlay is grown/shrunk to
     * force a std::string reallocation — the exact torn-read a missing lock would cause. Both sides take roles_mu,
     * so TSan sees no race; were the lock absent, this is what would trip it. The edits don't touch the role NAMES
     * the pool reports (def.role is the REQUESTED role), so the assertions below stay deterministic. */
    std::atomic<bool> stop_editor{false};
    std::thread       editor([&] {
        while (!stop_editor.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lk(roles_mu);
                if (!roles.roles.empty()) {
                    std::string &ov = roles.roles[0].prompt_overlay;
                    ov = ov.size() > 200 ? std::string("short") : std::string(600, 'y'); /* force realloc */
                }
            }
            std::this_thread::yield();
        }
    });

    /* add BY ROLE -> the next free pool slot (agent:A, then agent:B) */
    std::string a = fleet->add_worker("dev");
    CHECK(a == "agent:A", "add dev -> agent:A (the first free pool slot)");
    std::string b = fleet->add_worker("qa");
    CHECK(b == "agent:B", "add qa -> agent:B (the next free pool slot)");
    CHECK(fleet->wait_ready(6000), "both added workers check in");
    CHECK(fleet->pool().size() == 2 && has(*fleet, "agent:A", "dev") && has(*fleet, "agent:B", "qa"),
          "the pool reflects both added workers with their roles");

    /* remove the first -> it leaves the roster, is reaped, and its slot frees */
    CHECK(fleet->remove_worker("agent:A"), "remove agent:A");
    CHECK(fleet->pool().size() == 1 && has(*fleet, "agent:B", "qa") && !has(*fleet, "agent:A", "dev"),
          "agent:A is gone; agent:B remains");
    CHECK(!sup->is_alive("agent:A"), "the supervisor reaped agent:A");

    /* a NEW add REUSES the freed slot (agent:A) with the new role */
    std::string c = fleet->add_worker("research");
    CHECK(c == "agent:A", "add research REUSES the freed slot agent:A");
    CHECK(fleet->wait_ready(6000), "the reused-slot worker checks in (the freed id re-authorized)");
    CHECK(fleet->pool().size() == 2 && has(*fleet, "agent:A", "research"), "agent:A is back with the new role");

    CHECK(!fleet->remove_worker("agent:Z"), "removing an unknown id returns false");

    stop_editor.store(true, std::memory_order_relaxed); /* stop the concurrent role-table editor + join it */
    editor.join();

    fleet.reset(); /* dtor (the supervisor still owns + reaps the live processes below) */
    sup->shutdown();
    delete sup;
    broker->stop();
    delete broker;
    unlink(sock.c_str());
    rmdir(sess.c_str());
    rmdir(dir);

    if (g_fail) {
        std::fprintf(stderr, "fleet_gate: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("fleet_gate: all checks passed\n");
    return 0;
}
