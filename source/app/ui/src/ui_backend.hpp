#ifndef HC_UI_BACKEND_HPP
#define HC_UI_BACKEND_HPP

/* ui_backend — the renderer/platform isolation seam (GLFW + OpenGL3 + Dear ImGui). INTERNAL.
 *
 * Purpose:   own the window, the GL context, and the ImGui platform+renderer backends, behind a
 *            narrow class. A later macOS Metal path is a swap of this .cpp for ui_backend_metal.cpp
 *            implementing the SAME class — a backend-file swap, not a refactor (doc 10; locked
 *            decision 4: "renderer isolated for a later macOS Metal path").
 * Owns:      the GLFWwindow, the GL context, and the ImGui context + the two impl backends; frees
 *            them in the destructor (reverse order). Single-owner, one per UI thread.
 * Threading: GL + GLFW are single-threaded; create/begin_frame/end_frame run on one thread.
 * Lifetime:  caller owns the returned Backend*; the destructor releases ImGui + GL + the window in
 *            reverse init order. On a partial-init failure create() deletes itself and returns null.
 */

#include <string>
#include <vector>

namespace hc::ui {

class Backend {
public:
    /* Create the window + GL context + ImGui platform/renderer. visible==false makes a HIDDEN
     * window (the headless smoke — no window appears on screen). Returns null if no display / GL
     * is available (e.g. a truly headless host), so callers can skip gracefully. */
    static Backend *create(const char *title, int w, int h, bool visible);
    ~Backend();

    bool begin_frame(); /* poll events + start an ImGui frame; false if the window should close */
    /* Render the ImGui draw data and swap. If `capture_ppm_path` is non-null, the rendered back buffer
     * is written there as a binary PPM (P6) BEFORE the swap — for headless screenshots (sighted UI
     * iteration; works on a HIDDEN window because GL renders regardless of visibility). */
    void end_frame(const char *capture_ppm_path = nullptr);

    /* Ask the compositor to mark this window as wanting attention (a flashing taskbar entry, an urgency
     * hint -- the compositor decides how). The one thing a Wayland client MAY do about its own window:
     * positioning and raising are both refused by the protocol, this is not. No-op where unsupported. */
    void request_attention();

    /* A.4: absolute OS file paths the operator DRAGGED onto the window since the last call (the GLFW drop
     * callback), moved out + the queue cleared. UNTRUSTED, NON-JAILED paths — the host reads them through its
     * hardened, operator-gated os-file path, NOT the sandbox. Empty on a headless / no-window backend. */
    std::vector<std::string> drain_dropped_paths();

private:
    Backend();
    struct Impl;
    Impl *p_;
};

} // namespace hc::ui

#endif /* HC_UI_BACKEND_HPP */
