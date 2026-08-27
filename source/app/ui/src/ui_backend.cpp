/* ui_backend (GLFW + OpenGL3) — see ui_backend.hpp. The GL3 implementation of the renderer seam.
 * A macOS Metal build swaps this file for ui_backend_metal.cpp implementing the same class. */

#include "ui_backend.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <vector>

namespace hc::ui {

struct Backend::Impl {
    GLFWwindow              *win = nullptr;
    bool                     glfw_inited = false;
    bool                     impl_glfw = false;
    bool                     impl_gl = false;
    std::vector<std::string> dropped_; /* A.4: absolute OS paths from the GLFW drop callback; drained by the host */
};

namespace {
/* swallow GLFW errors — a headless host (create() returning null) is an expected path, not noise */
void glfw_error_cb(int, const char *) {}

/* A.4: GLFW file-drop callback — queue the dropped absolute OS paths on the sink the window user pointer holds
 * (the Backend's dropped_ vector, wired in create()). Runs on the UI thread during glfwPollEvents and is drained
 * on the same thread, so no locking. ImGui's GLFW backend does not use the window user pointer (it documents
 * this) and installs no drop callback, so this neither clobbers nor chains it. */
void drop_cb(GLFWwindow *win, int count, const char **paths)
{
    auto *sink = static_cast<std::vector<std::string> *>(glfwGetWindowUserPointer(win));
    if (!sink || !paths) return;
    for (int i = 0; i < count; i++)
        if (paths[i]) sink->emplace_back(paths[i]);
}
} // namespace

Backend::Backend() : p_(new Impl) {}

Backend *Backend::create(const char *title, int w, int h, bool visible)
{
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) return nullptr; /* no display / no GLFW — caller skips */

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow *win = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return nullptr; /* no GL context obtainable */
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    /* Hidden windows are the headless screenshot path: pin them to the fresh DockBuilder layout by
     * disabling imgui.ini persistence. Otherwise a stale ini from a prior interactive session would load
     * and override the default tab selection (non-deterministic captures), and the capture would also
     * overwrite the operator's saved layout. Persistence stays on for the real visible window. */
    if (!visible) ImGui::GetIO().IniFilename = nullptr;

    Backend *b = new Backend();
    b->p_->win = win;
    b->p_->glfw_inited = true;
    if (ImGui_ImplGlfw_InitForOpenGL(win, true)) b->p_->impl_glfw = true;
    if (ImGui_ImplOpenGL3_Init("#version 150")) b->p_->impl_gl = true;
    if (!b->p_->impl_glfw || !b->p_->impl_gl) {
        delete b; /* destructor unwinds whatever initialized */
        return nullptr;
    }
    /* A.4: receive OS file drag-drops. Point the (ImGui-unused) window user pointer at our drop sink, then install
     * the drop callback. ImGui installs no drop callback, so there is nothing to chain. */
    glfwSetWindowUserPointer(win, &b->p_->dropped_);
    glfwSetDropCallback(win, &drop_cb);
    return b;
}

Backend::~Backend()
{
    if (p_->impl_gl) ImGui_ImplOpenGL3_Shutdown();
    if (p_->impl_glfw) ImGui_ImplGlfw_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (p_->win) glfwDestroyWindow(p_->win);
    if (p_->glfw_inited) glfwTerminate();
    delete p_;
}

bool Backend::begin_frame()
{
    if (glfwWindowShouldClose(p_->win)) return false;
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    return true;
}

namespace {
/* Read the rendered back buffer + write a top-down binary PPM (GL is bottom-up, so flip). Best-effort
 * (a screenshot tool helper) — silently does nothing on a bad size / unwritable path. */
void write_back_buffer_ppm(const char *path, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> px((size_t)w * (size_t)h * 3u);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) /* flip vertically */
        std::fwrite(px.data() + (size_t)y * (size_t)w * 3u, 1u, (size_t)w * 3u, f);
    std::fclose(f);
}
} // namespace

void Backend::end_frame(const char *capture_ppm_path)
{
    ImGui::Render();
    int dw = 0, dh = 0;
    glfwGetFramebufferSize(p_->win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0.04f, 0.04f, 0.04f, 1.0f); /* near-black, matches --bg-panel */
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (capture_ppm_path) write_back_buffer_ppm(capture_ppm_path, dw, dh); /* before the swap */
    glfwSwapBuffers(p_->win);
}

void Backend::request_attention()
{
    if (p_->win) glfwRequestWindowAttention(p_->win);
}

std::vector<std::string> Backend::drain_dropped_paths()
{
    std::vector<std::string> out = std::move(p_->dropped_);
    p_->dropped_.clear(); /* guarantee empty (a moved-from std::vector is valid-but-unspecified) — like drain_commands */
    return out;
}

} // namespace hc::ui
