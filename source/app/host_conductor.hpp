#ifndef HC_APP_HOST_CONDUCTOR_HPP
#define HC_APP_HOST_CONDUCTOR_HPP

/* host_conductor — builds the Conductor (Conductor P5's conversational front-door agent) and wires its
 * ControlPlane to the host facades, keeping that construction out of main(). ONE responsibility: given the
 * live LLM config + the host facades it drives (Orchestrator / MemoryBroker / AuthGate / store), return a
 * started Conductor plus the chat client it owns. The wiring is THIN — each ControlPlane lambda is a small
 * adapter delegating to an existing facade (decompose / run_agenda / snapshot / query / seed / Reasoner);
 * the only non-trivial bits (the agenda id/title convention, the done/failed summary) are factored into
 * named helpers (agenda_for_goal in the .cpp; hc::orch::agenda_tally in the orchestrator).
 *
 * Ownership/threading: the returned ConductorWiring OWNS the Conductor (unique_ptr) and the conductor's
 * OWN hc_llm + hc_http (raw — main() frees them via close_chat_llm AFTER conductor.reset(), since the
 * ControlPlane lambdas run on the conductor thread and capture the llm). That llm is DISTINCT from the
 * orchestrator's planner_llm: decompose/deep_reason run on the conductor thread while planner_llm is driven
 * concurrently by the orchestrator's thread, and one curl handle must not be shared across threads. The
 * host facades are BORROWED — the caller owns them and must outlive the conductor (teardown stops the
 * conductor first). See app/main.cpp + Docs/Plan_Conductor. */

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hc_conductor.hpp" /* hc::conductor::Conductor — held by value-owning unique_ptr below */

namespace hc {
class Orchestrator;
class BusClient; /* fwd (app/bus) — D4c: the conductor's OWN client for third-party tool.invoke round-trips */
}
namespace hc::host {
class MemoryBroker;
class AuthGate;
}
struct hc_llm;
struct hc_http;
struct hc_store;
struct hc_artifacts; /* fwd (libs/hc_artifacts) — the agenda_results/read_artifact deliverable read-back tools */
struct hc_audio_player; /* fwd (libs/hc_audio) — the Music Player engine the set_mood tool drives */
struct hc_sandbox;      /* fwd (libs/hc_sandbox) — the jailed audio library the conductor reads tracks from */

namespace hcapp {

class Fleet; /* fwd (app/fleet) — the conductor reads the LIVE pool/caps + drives add/remove/list workers */
class ToolHost; /* fwd (app/toolhost) — D4c: the source of the conductor's opt-in third-party functions */
struct SettingsState; /* fwd (host_services.hpp) — the LIVE mood-enable gate + persisted volume */

/* What build_conductor hands back to main(): the started conductor + the chat client it owns. On any
 * failure (not live / no model+key / construction error) conductor is null and llm/http are null — nothing
 * to free. main() moves the conductor out, records llm/http, and frees them in the teardown order. */
struct ConductorWiring {
    std::unique_ptr<hc::conductor::Conductor> conductor;
    hc_llm                                   *llm = nullptr;  /* the conductor's OWN chat client */
    hc_http                                  *http = nullptr; /* its transport (free both AFTER reset) */
    hc::BusClient                            *tool_bus = nullptr; /* D4c: the conductor's third-party tool.invoke
                                                                   * client (null unless opt-in + tools exist).
                                                                   * Raw — the OWNER deletes it AFTER conductor.reset()
                                                                   * (its invoke seam runs on the conductor thread). */
};

/* Build + start the conductor and wire its ControlPlane to the host facades. `live`+`model` gate the
 * chat-client open (an empty wiring when not live / the client fails). `fleet` (BORROWED) supplies the LIVE
 * capabilities/pool (its distinct roles) AND the add/remove/list-workers ControlPlane ops; `orch`/`mb`/`gate`/
 * `store` are the BORROWED facades the tools drive; `ephemeral`/`data_dir` place the durable goal WAL
 * (ephemeral => in-memory goals). All borrowed args must outlive the conductor (teardown stops it first). */
/* `store` is the conductor's DEDICATED conversation store (P1: projects/<id>/conductor_sessions/, NOT the worker
 * session store) — null => no conversation persistence. `session_id` resumes that conversation ("" => fresh);
 * `new_title` titles a fresh session (a host-formatted timestamp). The caller (project_session) opens/owns the
 * store + records the active id; this just threads them into ConductorParams. */
/* D4c: `toolhost` + `bus_sock` are the third-party tool seam — when `settings.thirdparty_tools_conductor` is on
 * and the ToolHost is running confirmed tools, build_conductor opens a "conductor" bus client on `bus_sock` and
 * registers a proxy per function (each gated through `gate` for a sensitive call). Both may be null/empty (then
 * the conductor simply gets no third-party tools — the default). */
ConductorWiring build_conductor(bool live, const char *model, Fleet *fleet, hc::Orchestrator *orch,
                                hc_artifacts *art, hc::host::MemoryBroker *mb, hc::host::AuthGate *gate,
                                hc_store *store, bool ephemeral, const std::string &data_dir,
                                hc_audio_player *player, hc_sandbox *audio, SettingsState *settings,
                                const std::string &session_id, const std::string &new_title,
                                ToolHost *toolhost, const std::string &bus_sock);

} // namespace hcapp

#endif /* HC_APP_HOST_CONDUCTOR_HPP */
