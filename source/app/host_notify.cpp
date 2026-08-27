/* host_notify — see host_notify.hpp. */

#include "host_notify.hpp"

/* The real implementation is walled behind HC_WITH_NOTIFY (set only when pkg-config found gio-2.0), so a
 * build without GIO compiles this file to four inert stubs -- one file, no second TU. Mirrors how
 * hc_secrets gates its keychain arms. */
#ifdef HC_WITH_NOTIFY
#include <gio/gio.h>

#include <cstdio>

#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#endif

#include <string>
#include <utility>
#include <vector>

namespace hcapp {

#ifdef HC_WITH_NOTIFY

namespace {

/* The freedesktop notification service. Its `actions` capability is advertised by the server; a server
 * that does not implement buttons simply shows the text, which is still the point of this. */
constexpr const char *kBusName = "org.freedesktop.Notifications";
constexpr const char *kObjPath = "/org/freedesktop/Notifications";
constexpr const char *kIface = "org.freedesktop.Notifications";

/* Untrusted text reaches the notification body: `summary` is model-authored (a worker's caption, an argv,
 * a memory payload). Strip control bytes so it cannot forge lines or inject terminal escapes into whatever
 * renders it, and bound the length so a huge payload cannot become a screen-filling popup. */
std::string tame(const std::string &in, size_t cap)
{
    std::string out;
    out.reserve(in.size() < cap ? in.size() : cap);
    for (char c : in) {
        if (out.size() >= cap) {
            out += "\xE2\x80\xA6"; /* ellipsis */
            break;
        }
        const unsigned char u = (unsigned char)c;
        if (c == '\n' || c == '\t') out += ' ';
        else if (u < 0x20 || u == 0x7f) continue; /* drop control bytes entirely */
        else out += c;
    }
    return out;
}

} // namespace

/* One queued Notify, handed from the host thread to the loop thread by send(). */
struct SendJob;

struct Notifier::Impl {
    GDBusConnection *bus = nullptr;
    GMainContext    *ctx = nullptr;
    GMainLoop       *loop = nullptr;
    std::thread      thread;
    guint            action_sub = 0;
    guint            closed_sub = 0;

    /* Cancels every in-flight Notify at teardown, so a reply callback cannot land on a freed Impl.
     * Without it a call still in the 5s window at shutdown either leaks its per-call state or wakes on
     * a dangling pointer -- the kind of thing the ASan/TSan builds exist to catch. */
    GCancellable *cancel = nullptr;

    std::mutex                                   mu;
    std::unordered_map<guint32, std::string>     by_notif; /* notification id -> AuthGate id */
    std::unordered_map<std::string, guint32>     by_auth;  /* AuthGate id -> notification id */
    std::vector<std::pair<std::string, bool>>    verdicts;
    /* Requests closed BEFORE their Notify reply arrived. close() cannot withdraw a notification whose
     * id it does not have yet, so it records the intent here and the reply callback withdraws it on
     * arrival -- otherwise a request resolved inside the D-Bus round trip (auto-approve, a fast click,
     * a worker giving up) strands a popup offering a verdict on something already decided. */
    std::unordered_set<std::string>              closing;
};

struct SendJob {
    Notifier::Impl *p;
    std::string     id, title, body;
};

namespace {

void on_action(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
               GVariant *params, gpointer user)
{
    auto *p = (Notifier::Impl *)user;
    /* Defence in depth behind the sender pin below: g_variant_get() on a payload of another shape is a
     * GLib type error, not a soft failure, so a malformed signal would abort the host. The subscription
     * does not police the signature -- check it here. */
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(us)"))) return;
    guint32     nid = 0;
    const char *action = nullptr;
    g_variant_get(params, "(u&s)", &nid, &action);
    if (!action) return;
    const bool approved = (g_strcmp0(action, "approve") == 0);
    if (!approved && g_strcmp0(action, "deny") != 0) return; /* some other action -- not ours */
    std::lock_guard<std::mutex> lk(p->mu);
    auto it = p->by_notif.find(nid);
    if (it == p->by_notif.end()) return; /* not one of ours, or already resolved */
    p->verdicts.emplace_back(it->second, approved);
    p->by_auth.erase(it->second);
    p->by_notif.erase(it);
}

void on_closed(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
               GVariant *params, gpointer user)
{
    /* Forget the mapping when a notification goes away. A dismissal is NOT a verdict: the request stays
     * pending and the operator can still answer it in the Approvals panel. Failing closed like this is
     * the only safe reading -- treating a swipe-away as "deny" would silently kill work. */
    auto *p = (Notifier::Impl *)user;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(uu)"))) return; /* see on_action */
    guint32 nid = 0, reason = 0;
    g_variant_get(params, "(uu)", &nid, &reason);
    std::lock_guard<std::mutex> lk(p->mu);
    auto it = p->by_notif.find(nid);
    if (it == p->by_notif.end()) return;
    p->by_auth.erase(it->second);
    p->by_notif.erase(it);
}

} // namespace

bool Notifier::available() { return true; }

Notifier::Notifier() : p_(new Impl()) {}

Notifier *Notifier::start()
{
    Notifier *n = new Notifier();
    Impl     *p = n->p_;

    /* A PRIVATE main context: the subscription must be driven by our own thread, and pushing it as the
     * thread-default before connecting binds both the connection and its signal callbacks to it. Using
     * the global default would mean fighting whatever else might one day run a loop in this process. */
    p->ctx = g_main_context_new();
    g_main_context_push_thread_default(p->ctx);

    GError *err = nullptr;
    p->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (err) {
        g_error_free(err);
        p->bus = nullptr;
    }
    if (!p->bus) { /* no session bus (headless, a chroot, a service manager) -- not an error */
        g_main_context_pop_thread_default(p->ctx);
        g_main_context_unref(p->ctx);
        delete n;
        return nullptr;
    }

    /* SECURITY -- the sender argument is kBusName, NOT NULL, and this is load-bearing.
     *
     * An ActionInvoked signal carries a tool VERDICT: on_action queues it, the host turns it into an
     * ordinary ToolVerdict and it approves a tool call. Subscribing with sender=NULL matches that signal
     * from ANY peer on the session bus, and a signal's object path and interface are chosen freely by
     * whoever emits it -- so any process in the operator's session could forge
     * ActionInvoked(nid, "approve") and walk straight through the human approval gate. Notification ids
     * are small sequential integers, so finding the live one is a short sweep, not a guess. Measured: an
     * unprivileged second connection forging that signal WAS delivered to the NULL-sender form and was
     * NOT delivered to this one.
     *
     * Naming the well-known name makes GDBus track its owner and drop signals from anyone else, which is
     * the same posture the bus already takes for its own peers (08-secrets-and-security: bus
     * impersonation -> SO_PEERCRED). The human gate is a must-keep security floor; it does not get a
     * second, unauthenticated entrance. */
    p->action_sub = g_dbus_connection_signal_subscribe(p->bus, kBusName, kIface, "ActionInvoked", kObjPath,
                                                       nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_action, p,
                                                       nullptr);
    p->closed_sub = g_dbus_connection_signal_subscribe(p->bus, kBusName, kIface, "NotificationClosed",
                                                       kObjPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                       on_closed, p, nullptr);
    p->cancel = g_cancellable_new();
    p->loop = g_main_loop_new(p->ctx, FALSE);
    g_main_context_pop_thread_default(p->ctx);

    p->thread = std::thread([p] {
        g_main_context_push_thread_default(p->ctx);
        g_main_loop_run(p->loop);
        g_main_context_pop_thread_default(p->ctx);
    });
    return n;
}

Notifier::~Notifier()
{
    /* Order matters. Cancel first: every in-flight Notify then completes promptly with CANCELLED, on the
     * loop thread, which is what frees its per-call state. Only THEN ask the loop to stop -- and ask at
     * LOW priority, so those cancellation callbacks (dispatched at DEFAULT) are drained before the loop
     * they run on goes away. Quitting first would strand them: either leaked, or waking later on an Impl
     * this destructor has already freed. */
    if (p_->cancel) g_cancellable_cancel(p_->cancel);
    if (p_->loop) {
        g_main_context_invoke_full(
            p_->ctx, G_PRIORITY_LOW,
            [](gpointer l) -> gboolean {
                g_main_loop_quit((GMainLoop *)l);
                return G_SOURCE_REMOVE;
            },
            p_->loop, nullptr);
        if (p_->thread.joinable()) p_->thread.join();
        g_main_loop_unref(p_->loop);
    }
    if (p_->cancel) g_object_unref(p_->cancel);
    if (p_->bus) {
        if (p_->action_sub) g_dbus_connection_signal_unsubscribe(p_->bus, p_->action_sub);
        if (p_->closed_sub) g_dbus_connection_signal_unsubscribe(p_->bus, p_->closed_sub);
        g_object_unref(p_->bus);
    }
    if (p_->ctx) g_main_context_unref(p_->ctx);
    delete p_;
}

void Notifier::send(const std::string &id, const std::string &agent, const std::string &tool,
                    const std::string &summary)
{
    if (!p_->bus) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (p_->by_auth.count(id)) return; /* already showing -- never re-post the same request */
    }

    /* Marshal the whole call ONTO the loop thread. A GDBus reply callback is dispatched on whatever
     * context was thread-default when the call was made, and send() runs on the host thread, whose
     * default context nobody runs -- so the reply never arrived, by_notif stayed empty, and a click could
     * not be mapped back to a request. Pushing our context here instead is NOT the fix: a GMainContext
     * can only be acquired by one thread at a time and the loop thread already holds it, so the push
     * fails outright ("assertion 'acquired_context' failed"). Running the call on the owning thread is
     * the only correct arrangement. */
    auto *job = new SendJob{p_, id, tame(agent, 64) + " wants " + tame(tool, 32), tame(summary, 400)};
    g_main_context_invoke_full(
        p_->ctx, G_PRIORITY_DEFAULT,
        [](gpointer user) -> gboolean {
            auto *j = (SendJob *)user;

            GVariantBuilder actions;
            g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&actions, "s", "approve");
            g_variant_builder_add(&actions, "s", "Approve");
            g_variant_builder_add(&actions, "s", "deny");
            g_variant_builder_add(&actions, "s", "Deny");

            GVariantBuilder hints;
            g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
            /* Critical: this is a question blocking a worker, and most servers keep critical
             * notifications on screen rather than expiring them. */
            g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(2));

            GVariant *args = g_variant_new("(susss@as@a{sv}i)", "HyperCat", (guint32)0, "dialog-question",
                                           j->title.c_str(), j->body.c_str(),
                                           g_variant_builder_end(&actions),
                                           g_variant_builder_end(&hints), (gint32)0);

            auto *ctxid = new std::pair<Impl *, std::string>(j->p, j->id);
            g_dbus_connection_call(
                j->p->bus, kBusName, kObjPath, kIface, "Notify", args, G_VARIANT_TYPE("(u)"),
                G_DBUS_CALL_FLAGS_NONE, 5000, j->p->cancel,
                [](GObject *src, GAsyncResult *res, gpointer user2) {
                    auto     *pc = (std::pair<Impl *, std::string> *)user2;
                    GError   *e = nullptr;
                    GVariant *r = g_dbus_connection_call_finish((GDBusConnection *)src, res, &e);
                    if (r) {
                        guint32 nid = 0;
                        g_variant_get(r, "(u)", &nid);
                        bool withdraw = false;
                        {
                            std::lock_guard<std::mutex> lk(pc->first->mu);
                            /* close() ran while this call was still in flight and had no id to withdraw.
                             * Record nothing and take the popup straight back down: the request it asks
                             * about is already decided, and a notification offering a verdict on a
                             * settled request is worse than none. */
                            withdraw = pc->first->closing.erase(pc->second) != 0;
                            if (!withdraw) {
                                pc->first->by_notif[nid] = pc->second;
                                pc->first->by_auth[pc->second] = nid;
                            }
                        }
                        if (withdraw)
                            g_dbus_connection_call(pc->first->bus, kBusName, kObjPath, kIface,
                                                   "CloseNotification", g_variant_new("(u)", nid), nullptr,
                                                   G_DBUS_CALL_FLAGS_NONE, 5000, pc->first->cancel, nullptr,
                                                   nullptr);
                        g_variant_unref(r);
                    }
                    if (e) {
                        { /* the notification never existed; drop any pending withdrawal for it */
                            std::lock_guard<std::mutex> lk(pc->first->mu);
                            pc->first->closing.erase(pc->second);
                        }
                        /* Report ONCE. A notification that never appears is indistinguishable from an
                         * operator who did not look, and that ambiguity has already cost real debugging
                         * time -- so say it out loud, then stay quiet rather than spamming a broken bus. */
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            std::fprintf(stderr,
                                         "host: approval notification failed (%s) — approvals still work "
                                         "in the Approvals panel\n",
                                         e->message ? e->message : "?");
                        }
                        g_error_free(e);
                    }
                    delete pc;
                },
                ctxid);
            return G_SOURCE_REMOVE;
        },
        job, [](gpointer user) { delete (SendJob *)user; });
}

void Notifier::close(const std::string &id)
{
    if (!p_->bus) return;
    guint32 nid = 0;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->by_auth.find(id);
        if (it == p_->by_auth.end()) {
            /* No id yet -- the Notify reply is still in flight. Leave a note for the reply callback to
             * withdraw it on arrival instead of dropping the request on the floor. */
            p_->closing.insert(id);
            return;
        }
        nid = it->second;
        p_->by_notif.erase(nid);
        p_->by_auth.erase(it);
    }
    g_dbus_connection_call(p_->bus, kBusName, kObjPath, kIface, "CloseNotification",
                           g_variant_new("(u)", nid), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, p_->cancel,
                           nullptr, nullptr);
}

void Notifier::drain(std::vector<std::pair<std::string, bool>> &out)
{
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->verdicts.empty()) return;
    out.insert(out.end(), p_->verdicts.begin(), p_->verdicts.end());
    p_->verdicts.clear();
}

#else /* !HC_WITH_NOTIFY — no GIO at build time: approvals behave exactly as they did before */

struct Notifier::Impl {};
bool Notifier::available() { return false; }
Notifier::Notifier() : p_(nullptr) {}
Notifier::~Notifier() {}
Notifier *Notifier::start() { return nullptr; }
void Notifier::send(const std::string &, const std::string &, const std::string &, const std::string &) {}
void Notifier::close(const std::string &) {}
void Notifier::drain(std::vector<std::pair<std::string, bool>> &) {}

#endif /* HC_WITH_NOTIFY */

} // namespace hcapp
