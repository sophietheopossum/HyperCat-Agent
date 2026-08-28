#ifndef HC_HOST_BRIDGE_HPP
#define HC_HOST_BRIDGE_HPP

/* host_bridge — the host-side bus ADAPTERS that bridge worker bus streams <-> the UI snapshot/commands.
 *
 * Purpose:   keep app/main.cpp a thin wiring entry by housing the long-lived host bus clients that
 *            translate between the multi-process backend and the pure-renderer UI:
 *              - UiAdapter: subscribes to the workers' "tokens" pub topic (streamed deltas -> the
 *                console/chat buffer) AND the "reasoning" topic (the latest deep_reason chain); the
 *                broker stamps the authentic agent id, and pubs from non-fleet ids are dropped — state
 *                flowing IN to the snapshot.
 *              - AuthGate:  receives `tool.authorize` reqs from confirmed workers, queues them for a
 *                human verdict (surfaced as UiSnapshot::pending_auth), and replies allow/deny when the
 *                host dispatches a ToolVerdict command — the human-in-the-loop tool gate (Phase C). A
 *                second, IN-HOST path (request_and_wait) lets a host thread — the conductor, for a shared
 *                memory_write — submit the same kind of request and BLOCK on an internal condvar until the
 *                UI thread's resolve() signals it (deny-by-default on timeout / stop): the conductor thread
 *                waits, the UI thread signals (Conductor P5).
 *              - MemoryBroker: receives `memory.query` reqs, embeds + scope-enforces + recalls from the
 *                host's hc_memory store, and replies with fenced reference text — the semantic-recall
 *                service (P01). It adds a second, EMBEDDER thread (the network embed cannot run on the
 *                reader without breaking the one-sender rule) + owns an embedding client.
 * Owns:      each adapter owns ONE BusClient + its reader thread + its bounded buffer/queue; they share
 *            no state. MemoryBroker additionally owns the embedder thread + embedding client and guards
 *            the (borrowed) hc_memory store.
 * Threading: each follows the proven one-reader-one-sender split — a dedicated reader thread does the
 *            blocking recv(); the reader NEVER sends (single sender). UiAdapter/AuthGate reply from the
 *            UI thread (mutex-guarded copy/resolve); MemoryBroker replies from its embedder/sender thread
 *            and mutex-guards the store + embed client. The mutex is never held across bus I/O. stop()
 *            shuts the client down to unblock recv() and joins the thread(s).
 * Lifetime:  start() connects + subscribes and returns null on failure (caller skips); the caller owns
 *            the returned pointer and `delete`s it (the destructor calls stop() — idempotent). The
 *            BusClient ids ("ui", "authgate") are authorized + confirmed by the host bootstrap, in one
 *            place, BEFORE these connect — start() only connects, keeping the broker auth surface
 *            auditable in a single read. See app/main.cpp + Docs/Plan_HyperCat/04-bus-and-messaging.md.
 * Decoupling: host_bridge is BACKEND-side — it knows the bus, not the UI. It exposes its own
 *            PendingAuthView; main.cpp maps that into the UiSnapshot, exactly as it maps orchestrator
 *            state in. So host_bridge depends only on hc_bus/hc_json, never on hc_ui. */

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* The MemoryBroker holds the host's memory store + its own embedding client as opaque C handles — only
 * pointers, so a forward declaration suffices and the real headers stay confined to the .cpp (no ABI
 * leak into every consumer of this header). */
struct hc_http;
struct hc_llm;
struct hc_memory;

namespace hc {
class BusClient; /* forward — the adapters hold only a pointer; hc_bus is included in the .cpp */
}

namespace hc::host {

class CapabilityAuthority; /* P09.3 (cap_authority.hpp) — AuthGate::resolve_scoped mints through it (ptr only) */

/* Live stream adapter: a host bus client ("ui") subscribed to the workers' "tokens" pub topic (the
 * streamed model deltas, appended to a byte+count-bounded buffer — drop-oldest, lossy by design) AND
 * the "reasoning" topic (the deep_reason 5-stage chains, where only the LATEST is kept — replace, not
 * append). Both are broker-stamped, so the sender id is authentic, and a pub from an id NOT in the
 * known-fleet set is dropped — so a same-uid generic peer cannot plant a spoofed console/chain line
 * (the same filter AuthGate applies). The UI copies the buffers each frame. */
class UiAdapter {
public:
    /* known_agents: only pubs from these fleet ids are collected (others dropped). */
    static UiAdapter *start(const std::string &sock, std::unordered_set<std::string> known_agents);
    ~UiAdapter();

    void copy_into(std::vector<std::string> &out); /* snapshot the buffered token lines (UI thread) */
    void copy_reasoning(std::string &out);         /* snapshot the latest deep_reason chain          */
    /* Per-agent token totals summed from the workers' "turn.usage" pubs (P12). UI-agnostic — main maps
     * UsageView into the UI snapshot, like PendingAuthView. */
    struct UsageView {
        std::string agent;
        long        input_tokens = 0, output_tokens = 0;
        int         calls = 0;
    };
    void copy_usage(std::vector<UsageView> &out);
    /* P08.2: one recorded egress decision from the workers' "egress.event" pubs — broker-stamped agent +
     * the requested host/ip/port + the verdict. main maps it into the UI snapshot's Network panel. */
    struct EgressEvent {
        std::string    agent;
        std::string    host;
        std::string    ip;
        std::string    verdict;
        unsigned short port = 0;
        uint64_t       at_ms = 0;
    };
    void copy_egress(std::vector<EgressEvent> &out); /* snapshot the recent egress decisions (UI thread) */
    void stop();                                   /* idempotent; unblocks + joins the reader        */
    /* W2 P2.3: REPLACE the live-fleet filter (the host pushes fleet.ids() on every add/remove). Only pubs from
     * CURRENTLY-live fleet ids are collected — a removed / never-spawned id is dropped, closing the same-uid
     * display-spoof a STATIC pool filter would reopen on a de-seeded / dynamic fleet. */
    void set_known_agents(std::unordered_set<std::string>);

private:
    void reader_loop();

    BusClient                      *bus_ = nullptr;
    std::thread                     reader_;
    std::mutex                      mu_;
    std::deque<std::string>         lines_;
    size_t                          bytes_ = 0; /* total bytes in lines_, to bound by size not count */
    std::string                     reasoning_; /* the latest deep_reason chain (replace-on-arrival) */
    std::unordered_map<std::string, UsageView> usage_; /* agent -> running token totals (P12)        */
    std::deque<EgressEvent>         egress_;    /* P08.2: recent egress decisions (drop-oldest, bounded)       */
    std::mutex                      known_mu_;  /* guards known_agents_ (set_known_agents vs the reader thread) */
    std::unordered_set<std::string> known_agents_;
};

/* A queued tool-authorization request: the private routing info (from/corr) the gate needs to reply,
 * plus the tool/summary and the arrival time for the deny-by-default age-out. For fs_write it also
 * captures the proposed path + content (bounded) so the host can record the approved write as a
 * content-addressed artifact and diff it — the host has no other access to the proposed bytes. */
/* INVARIANT: an AuthRequest is IMMUTABLE once inserted (only created in reader_loop, only erased in
 * resolve()/age-out — never mutated in place). The operator reviews `content` via peek_content() and the
 * host writes `content` via resolve() at two different times; that they are byte-identical for a given id
 * depends on this immutability (the review→approve binding for fs_write diffs + memory_write content). */
struct AuthRequest {
    std::string                           agent;   /* broker-stamped requester id (== from)      */
    std::string                           tool;    /* the tool name                              */
    std::string                           summary; /* bounded human-readable action description  */
    std::string                           path;    /* fs_write target (artifact label / diff path)*/
    std::string                           content; /* fs_write / memory_write proposed bytes (bounded) */
    uint64_t                              corr;    /* the worker's req corr — reply target       */
    std::chrono::steady_clock::time_point at;      /* arrival, for the deny-by-default age-out   */
    bool                                  in_host = false; /* Conductor P5: an IN-HOST request (resolve signals
                                                            * a condvar, not a bus reply — no worker is blocked) */
    /* W4.3 exec: an approved `run` request hands these to the exec-worker thread, which runs hc_exec at `cwd`.
     * `argv` is the RE-VALIDATED command (argv[0] an allowlisted, realpath'd, non-setuid absolute binary). The
     * verdict binds to THIS stored argv — a worker can NEVER resupply a different command after approval. (A
     * separate, same-uid FILESYSTEM TOCTOU on what argv[0]'s PATH resolves to is re-checked just before spawn
     * and, even if it wins, is fully jail-contained by hc_exec — confined + unprivileged, never an escape.) */
    bool                     is_exec = false;
    std::vector<std::string> argv;
    std::string              cwd; /* the requesting agent's workspace jail (== where hc_exec confines) */
};

/* What resolve() hands back after a verdict — enough for the host to record an approved fs_write as an
 * artifact (the host has no other path to the proposed bytes). approved==false (and empty fields) on a
 * deny, or on an unknown/aged-out id. */
struct AuthResolution {
    bool        approved = false;
    std::string agent;   /* the producing agent (provenance)             */
    std::string tool;    /* the tool name ("fs_write")                   */
    std::string path;    /* the write target (the artifact label)        */
    std::string content; /* the approved bytes (to put into the CAS)     */
};

/* The host-facing view of a pending request (no routing internals). main.cpp maps this into the UI
 * snapshot's PendingAuth — host_bridge itself stays UI-agnostic. */
struct PendingAuthView {
    std::string id;      /* the ToolVerdict key (resolve(id, ...))     */
    std::string agent;   /* the requesting agent id                    */
    std::string tool;    /* the tool name ("fs_write" / "run" / ...)   */
    std::string summary; /* bounded action summary — for a "run" req, the full argv + cwd the operator reviews */
    long        age_ms = 0; /* B1: how long it has been pending — the UI shows "pending Nm ago" (PATIENT: never aged out) */
};

/* B3: a record of one AUTO-approved request, buffered by the gate for the host to drain each frame — so an
 * auto-approval still gets the SAME artifact provenance as a human approve + a quiet toast (visible, never silent). */
struct AutoApprovedView {
    std::string agent;   /* the broker-stamped requester */
    std::string tool;    /* "fs_write" / "fs_update"     */
    std::string path;    /* the jailed write target      */
    std::string content; /* the proposed bytes (for the artifact record) */
};

/* Tool-authorization gate: a host bus client ("authgate") that queues `tool.authorize` reqs from KNOWN
 * fleet agents for a human verdict, then replies allow/deny. A request from an id NOT in the known-fleet
 * set is dropped (closes the spurious-prompt vector — only real, broker-authenticated agents can prompt;
 * an unconfirmed squatter cannot even route a req here). The verdict reply confers no host authority —
 * it only tells the (already same-uid) worker whether the human approved — so this is a UX/mediation
 * gate over a trusted worker's tool, not a privilege boundary against an arbitrary process. */
class AuthGate {
public:
    /* known_agents: the fleet ids whose tool.authorize reqs are surfaced (others are ignored). */
    static AuthGate *start(const std::string &sock, std::unordered_set<std::string> known_agents);
    ~AuthGate();

    void snapshot(std::vector<PendingAuthView> &out);    /* copy pending (age-out applied) for the UI */
    /* Copy a still-pending request's fs_write path + content for the diff-review panel (P11). false if
     * the id is no longer pending. Called ONCE per prompt (the host caches the computed diff), so the
     * bounded content does not transit every frame. */
    bool peek_content(const std::string &id, std::string &out_path, std::string &out_content);
    /* Reply allow/deny to the blocked worker, dequeue the request, and return it so the host can record
     * an approved fs_write as an artifact. {approved=false} on a deny or an unknown/aged-out id. UI thread. */
    AuthResolution resolve(const std::string &id, bool approved);
    /* P09.3: approve an fs_write request AND mint a SCOPED capability for the requesting agent — "allow writes
     * under <scope_prefix> for the next `budget` uses." The reply approves the current write AND carries the
     * token (+ cleartext verb/scope) the worker stores, so subsequent COVERED writes go PROMPT-FREE (re-verified
     * by the CapabilityAuthority on each cap.check). Degrades to a plain one-shot allow if no authority is wired
     * or minting fails. Only valid for a pending fs_write (not exec / in-host). UI thread. */
    AuthResolution resolve_scoped(const std::string &id, const std::string &scope_prefix, uint32_t budget);
    /* B1: clear a pending request WITHOUT a verdict — the operator DEFERS it ("decide later"), which is NOT a
     * denial. The blocked worker is released with a neutral "dismissed" reply, so its tool returns "deferred"
     * (nothing written/run) and the model may ask again later. An in-host (conductor) request resolves as a plain
     * decline (its caller only distinguishes approve/not). Dequeues it; a no-op on an unknown/already-resolved id.
     * UI thread (replies via the same send_mu_ as resolve). */
    void dismiss(const std::string &id);
    /* B3: enable/disable deterministic AUTO-MODE (the host sets it live from the operator's settings each frame —
     * default OFF). When ON, the gate auto-approves ONLY sandbox-contained writes (fs_write/fs_update) at the
     * enqueue seam, so the human is never prompted for them; everything else (run/memory_write/novel) still goes to
     * the human, and it NEVER auto-denies. Thread-safe (atomic). */
    void set_auto_mode(bool on);
    /* B4: arm/disarm ALLOW-ALL (the host sets it live from settings; default OFF). When armed, the gate auto-approves
     * EVERY request (run included, still allowlist-re-validated) — the power-user / stress-test escape hatch, gated
     * in the UI behind a type-to-confirm consent window + a loud sticky warning. Thread-safe (atomic). */
    void set_allow_all(bool on);
    /* B3b: enable/disable auto-approval of READ-ONLY EGRESS third-party tools (the host sets it live from
     * settings each frame; default OFF), plus the set of function names that qualify. The host derives that set
     * from manifests IT parsed (ToolManifest::envelope_readonly_egress) -- the gate never trusts a caller's claim
     * about its own risk, which is why the names arrive from the host rather than riding the authorize body.
     * An empty set disables the path regardless of the flag. Never auto-denies. Thread-safe. */
    void set_readonly_egress_auto(bool on);
    void set_readonly_egress_tools(std::unordered_set<std::string> names);
    /* B3: drain the auto-approved trace (the host records each as an artifact, same provenance as a human approve,
     * and raises a quiet toast). Bounded; clears the buffer. UI thread. */
    void copy_auto_approved(std::vector<AutoApprovedView> &out);
    /* Conductor P5 — the IN-HOST gate: submit a tool-authorization request from a host thread (the conductor,
     * for a shared memory_write) and BLOCK until the operator resolves it (or the timeout fires). Surfaced in
     * snapshot() exactly like a bus request and decided by the SAME resolve(id, approved) the UI already calls.
     * B1: `timeout_ms` <= 0 is PATIENT — it waits INDEFINITELY for the operator (no silent timeout-deny), released
     * only by a verdict, a dismiss, or cancel_inhost_waiters() on teardown/switch (stop -> deny-by-default). A
     * positive `timeout_ms` caps the wait at that many ms (deny-by-default on expiry). Returns true iff approved.
     * Call OFF the UI thread (it blocks); the conductor thread does. */
    bool request_and_wait(const std::string &tool, const std::string &summary, const std::string &content,
                          int timeout_ms);
    /* Release any thread blocked in request_and_wait to deny-by-default, WITHOUT tearing the bus reader down (so
     * worker tool.authorize keeps working through it). Teardown calls this BEFORE joining the conductor: a
     * conductor thread blocked here has no mid-turn cancel, so the join would otherwise hang up to the TTL.
     * Idempotent; once called, later in-host requests also deny-closed (shutting down). */
    void           cancel_inhost_waiters();
    /* Conductor conversation SWITCH (P2): re-enable in-host gating that cancel_inhost_waiters() latched off, so a
     * conductor rebuilt for a different conversation can gate shared-memory writes again. Host thread only, during a
     * live switch (never mid-shutdown). A no-op on the initial bring-up (gating starts enabled). */
    void           resume_inhost_gate();
    /* W4.3: enable the brokered `run` exec tool. `allow` = the operator's exec allowlist (absolute binary
     * paths); `ws_root`/`shared` resolve a requesting agent's workspace as the exec cwd. A `tool.exec` req is
     * then RE-VALIDATED here (argv[0] absolute + realpath ∈ allow + non-setuid regular file) and operator-gated;
     * on approval a dedicated exec-worker thread runs hc_exec at the agent's workspace and replies with the
     * bounded output. Call ONCE at startup before traffic; a NO-OP (exec stays disabled) when `allow` is empty.
     * P06 (Wave 5) — `role_exec_allow_fn` (optional) resolves the requesting agent's PER-ROLE exec allowlist
     * (the host RoleTable, via the fleet roster). When it returns a non-empty list, the effective allowlist is
     * the INTERSECTION of the global `allow` and the role's — a role can only SUBTRACT commands, never widen.
     * An empty/absent result inherits the global allowlist (today's behaviour). Set once with the gate config. */
    void           enable_exec(std::vector<std::string> allow, std::string ws_root, bool shared,
                               std::function<std::vector<std::string>(const std::string &)> role_exec_allow_fn = {});
    void           stop();                               /* idempotent; unblocks + joins the reader + exec thread */
    /* W2 P2.3: track the LIVE fleet (the host pushes fleet.ids() on add/remove). Only tool.authorize reqs from a
     * currently-live worker raise an operator prompt — a removed / never-spawned id can't forge one. */
    void           set_known_agents(std::unordered_set<std::string>);
    /* P09.3: wire the CapabilityAuthority that resolve_scoped() mints through. Call ONCE at startup before
     * traffic (read-only after). Null (the default) => a scoped grant degrades to a plain one-shot allow. */
    void           set_cap_authority(CapabilityAuthority *ca);

private:
    void reader_loop();
    void exec_loop(); /* W4.3: the exec-worker thread — runs APPROVED commands (hc_exec) off the reader/UI threads */

    BusClient                                    *bus_ = nullptr;
    std::thread                                   reader_;
    std::mutex                                    mu_;
    std::unordered_map<std::string, AuthRequest>  pending_; /* request id -> the queued request */
    std::mutex                                    known_mu_;        /* guards known_agents_ (setter vs reader) */
    std::unordered_set<std::string>               known_agents_;
    uint64_t                                      next_id_ = 1;     /* monotonic request-id source   */
    std::condition_variable                       inhost_cv_;       /* P5: the in-host verdict handshake (cond thread waits, UI resolve() signals) */
    std::unordered_map<std::string, int>          inhost_verdict_;  /* P5: in-host id -> -1 pending / 0 deny / 1 allow */
    bool                                          stopping_ = false; /* P5: set in stop() -> wakes any in-host waiter to deny */
    /* W4.3 exec: serialize bus_ framed writes — with exec there are now 3 potential senders (the reader's
     * auto-deny, the UI's verdict, the exec thread's result), so a send mutex preserves frame integrity (the
     * reader's recv is unguarded — full-duplex). */
    std::mutex                                    send_mu_;
    /* the exec config (set once by enable_exec) + the exec-worker thread that runs APPROVED commands. */
    bool                                          exec_enabled_ = false;
    std::vector<std::string>                      exec_allow_; /* the operator's allowlist (absolute paths) */
    /* P06: resolve a requesting agent's PER-ROLE exec allowlist (host RoleTable, via the fleet roster). Published
     * ONCE under mu_ in enable_exec (exactly like exec_allow_), before the exec thread / any traffic, then
     * read-only — so the reader calls it WITHOUT a lock (it does its OWN internal locking on the fleet + role
     * table). A non-empty result INTERSECTS the global allowlist (subtract-only); unset / empty => no narrowing. */
    std::function<std::vector<std::string>(const std::string &)> role_exec_allow_fn_;
    /* P09.3: the capability authority resolve_scoped() mints through. Set once by set_cap_authority, read-only
     * after — lock-free ONLY because that call runs before the reader thread sees traffic (the host bootstrap
     * wires it at startup). Null => scoped grants degrade to a plain one-shot allow. */
    CapabilityAuthority                          *capauth_ = nullptr;
    std::string                                   ws_root_;
    bool                                          shared_ws_ = false;
    struct ExecJob {
        std::vector<std::string> argv;
        std::string              cwd, to; /* the validated command + its workspace cwd + the worker reply id */
        uint64_t                 corr;
    };
    std::thread                                   exec_thread_;
    std::deque<ExecJob>                           exec_queue_; /* APPROVED jobs awaiting the exec thread */
    std::condition_variable                       exec_cv_;    /* wakes the exec thread (a job, or stop) */
    /* the exec thread's OWN stop flag — set ONLY by stop(), NOT by cancel_inhost_waiters (which is a partial
     * teardown that must leave the exec path alive so a worker mid-approval still gets its result). */
    bool                                          exec_stop_ = false;
    /* B3: deterministic AUTO-MODE — operator opt-in (default OFF; set live via set_auto_mode). The reader thread
     * reads it at the enqueue seam; the host (UI thread) writes it. Atomic so no lock is taken on the hot path.
     * relaxed ordering is correct: it's a boolean hint with no ordering dependency on the mu_-guarded state that
     * follows; the worst-case stale read is one extra HUMAN prompt (conservative — never an unseen auto-approve). */
    std::atomic<bool>                             auto_mode_enabled_{false};
    /* B4: ALLOW-ALL — the armed escape hatch (default OFF; set live via set_allow_all). When on, EVERY tool request
     * is auto-approved without a prompt (incl. run — but exec is STILL re-validated against the allowlist; allow-all
     * skips the prompt, never the exec floor). Gated in the UI behind a type-to-confirm consent window + a sticky
     * armed warning. Same relaxed-atomic rationale as auto_mode_enabled_. */
    std::atomic<bool>                             allow_all_{false};
    /* B3b: the read-only-egress auto-approve arm + the host-derived name set it applies to. Guarded by its own
     * mutex (refreshed on the host's poll tick, read on the bus-reader thread) so it never contends with mu_. */
    std::atomic<bool>                             readonly_egress_auto_{false};
    std::mutex                                    readonly_egress_mu_;
    std::unordered_set<std::string>               readonly_egress_tools_;
    /* B3: the auto-approved trace the host drains each frame (-> artifact record [fs writes] + a quiet toast). Bounded
     * drop-oldest, guarded by mu_; written by the reader, drained by the host. Mirrors the UiAdapter egress ring. */
    std::deque<AutoApprovedView>                  auto_approved_;
#ifdef HC_ENABLE_TEST_GATES
    /* TEST-ONLY (compiled in ONLY under -DHC_ENABLE_TEST_GATES — absent from a release binary): when
     * HC_AUTO_APPROVE is set in the env, the gate auto-approves every bus tool.authorize WITHOUT enqueuing or
     * waiting for a human — for headless live validation of the worker write path (the stand-in for an instant
     * operator approve). DEFAULT-OFF even in a test build (env-gated); a loud startup log announces it. The
     * in-host conductor gate (request_and_wait) is unaffected (separate path). Compile-time-fenced rather than
     * merely runtime-gated so a shipped binary cannot bypass the operator gate at all, and an interactive
     * release session can never have it silently on. */
    bool                                          auto_approve_ = false;
#endif
};

/* A live memory record surfaced to the UI Memory panel (text + provenance + scope; no vector). */
struct MemRow {
    std::string id;
    std::string scope;
    std::string text;
    std::string source;
};

/* MemoryBroker — the host bus service for semantic recall (P01). A "memorybroker" BusClient with the
 * proven one-reader/one-sender split PLUS a dedicated embedder/sender worker thread:
 *   - reader_loop: validates a `memory.query` req from a KNOWN fleet agent and ENQUEUES it (never replies
 *     on the reader — embedding is a network call that must not block recv or need a 2nd sender there).
 *   - worker_loop: pops a query, embeds it via an owned hc_llm (its own hc_http handle), enforces scope
 *     SERVER-SIDE (the private scope is "agent:"+the broker-stamped sender; a worker can NEVER name a
 *     foreign agent's scope), queries hc_memory (the lib filters-before-scoring), formats the hits as a
 *     fenced "reference, not instruction" block, and replies via the single sender. A slow/failed embed
 *     just denies-by-default at the worker's bounded await_reply — fail-closed, like fs_write.
 * Owns:      one BusClient + two threads + the embedding hc_http/hc_llm (built in start, freed in stop) +
 *            a bounded query queue. BORROWS the host's hc_memory store (single-owner; the broker guards
 *            ALL access — the worker thread's query, the UI thread's list/forget, and seed — with mem_mu_).
 * Threading: the reader never sends; the worker is the single sender. hc_memory + the embed client are
 *            each mutex-guarded so the worker/UI/seed callers don't race a non-thread-safe handle. */
class MemoryBroker {
public:
    /* Build an embedding client from (base_url, api_key, embed_model) and start the threads. `mem` is the
     * host's memory store (borrowed; the host opens/closes it, the broker guards access). NULL on a bad
     * socket / client / embed-client build. */
    static MemoryBroker *start(const std::string &sock, std::unordered_set<std::string> known_agents,
                               struct hc_memory *mem, const std::string &base_url,
                               const std::string &api_key, const std::string &embed_model);
    ~MemoryBroker();

    void list(std::vector<MemRow> &out);  /* UI thread: snapshot live records (most-recent-first, bounded) */
    bool forget(const std::string &id);   /* UI thread: prune a record by id (false if absent/error)       */
    /* Startup seed (host thread): embed `text` and write it to `scope` with provenance `source`. 0 ok. */
    int  seed(const std::string &scope, const std::string &text, const std::string &source);
    /* In-host recall (Conductor P5): embed `text`, query {`scope`, "shared"}, and return the fenced/defanged
     * "reference, not instruction" block (or "" on no hits / a failed embed — fail-closed). Thread-safe (it
     * guards the store + the embed client exactly like the worker recall path); it may BLOCK on the embed
     * network call, so call it OFF the UI thread (the conductor thread does). */
    std::string query(const std::string &scope, const std::string &text);
    void stop();                          /* idempotent; unblocks + joins both threads                     */
    /* W2 P2.3: track the LIVE fleet (the host pushes fleet.ids() on add/remove). A recall/write is honored ONLY
     * for a currently-live worker — closing a cross-slot private-memory leak/poison (a freed agent:X id squatted
     * by a same-uid peer would otherwise recall the departed worker's private scope). */
    void set_known_agents(std::unordered_set<std::string>);

private:
    void reader_loop();
    void worker_loop();
    void reply(const std::string &to, uint64_t corr, bool ok, const std::string &text); /* sender thread */
    int  embed(const std::string &text, std::vector<float> &out); /* via the embed client (mutex-guarded) */

    struct PendingQuery {
        std::string from; /* broker-stamped sender — the recall/write identity */
        uint64_t    corr;
        std::string text;          /* the query (recall) or the memory text (write) */
        bool        is_write = false;
    };

    BusClient                      *bus_ = nullptr;
    std::thread                     reader_, worker_;
    std::mutex                      known_mu_; /* guards known_agents_ (set_known_agents vs the reader thread) */
    std::unordered_set<std::string> known_agents_;

    struct hc_memory               *mem_ = nullptr; /* borrowed; mem_mu_ guards the single-owner store */
    std::mutex                      mem_mu_;

    struct hc_http                 *http_ = nullptr;  /* owned embedding transport (freed in stop)      */
    struct hc_llm                  *embed_ = nullptr; /* owned embedding client                          */
    std::mutex                      embed_mu_;        /* serialize embed() (worker + seed)               */
    std::string                     embed_model_;

    std::deque<PendingQuery>        queue_;
    std::mutex                      q_mu_;
    std::condition_variable         q_cv_;
    bool                            stopping_ = false;
};

/* Pure, stateless helpers exposed for unit testing (the security-load-bearing transforms behind the
 * MemoryBroker — no I/O, no state). Kept here so test_host_bridge can lock their behaviour against
 * regression; the broker uses them internally. */
std::string defang_memory(const char *raw);  /* neutralize fence markers + newlines in recalled text   */
bool        scope_writable(const std::string &scope); /* write-side scope policy: shared/distilled/conductor/agent:/agenda: */

} // namespace hc::host

#endif /* HC_HOST_BRIDGE_HPP */
