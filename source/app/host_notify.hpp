#ifndef HC_HOST_NOTIFY_HPP
#define HC_HOST_NOTIFY_HPP

/* host_notify — surface a pending tool approval as a DESKTOP notification, and take the verdict back.
 *
 * WHY: an approval request that the operator never sees is a task that blocks until it times out. The
 * Approvals panel is only visible when HyperCat is the focused window, and a real approval was missed
 * that way. Wayland offers no way for a client to raise or position its own window (glfwSetWindowPos and
 * GLFW_FLOATING are both refused), so a notification is not a fallback here -- it is the only mechanism
 * that reaches a user who is looking elsewhere.
 *
 * The notification carries `approve` / `deny` actions. Buttons that did nothing would be worse than no
 * buttons, so this also subscribes to ActionInvoked and queues the verdict for the host to apply.
 *
 * Threading: send() is called from the host/dispatch thread and never blocks on the bus. Signals arrive
 * on a private GMainContext driven by one owned thread; verdicts land in a mutex-guarded queue that the
 * host drains each frame, so AuthGate is still only ever touched from the host thread.
 *
 * Degrades silently and completely: no session bus, no notification daemon, or a build without GIO all
 * mean send() does nothing and drain() returns nothing. Approvals then work exactly as they did before.
 */

#include <string>
#include <utility>
#include <vector>

namespace hcapp {

class Notifier {
public:
    /* Whether this BUILD can notify at all (gio-2.0 was found at configure time). False on macOS and on
     * a Linux box without GIO. Distinct from start() returning null, which is a RUNTIME miss (no session
     * bus). The settings panel uses it to disable a control that could not do anything. */
    static bool available();

    /* Connect to the session bus and start the signal thread. Returns null when unavailable -- a null
     * Notifier is not an error, and every call site must tolerate it. */
    static Notifier *start();
    ~Notifier();

    /* Post one approval request. `id` is the AuthGate key echoed back by drain(). Fire-and-forget. */
    void send(const std::string &id, const std::string &agent, const std::string &tool,
              const std::string &summary);

    /* Collect verdicts the operator gave from the notification. Pairs of (AuthGate id, approved).
     * Call once per frame on the host thread; the queue is emptied. */
    void drain(std::vector<std::pair<std::string, bool>> &out);

    /* Withdraw a notification whose request is no longer pending (resolved in-app, or dismissed), so a
     * stale prompt cannot sit on screen offering a verdict on something already decided. */
    void close(const std::string &id);

    /* Opaque, but PUBLIC: the GDBus signal callbacks are free functions (a C callback cannot be a
     * private member), and they need the state. Nothing outside this module can do anything with it. */
    struct Impl;

private:
    Notifier();
    Impl *p_;
};

} // namespace hcapp

#endif
