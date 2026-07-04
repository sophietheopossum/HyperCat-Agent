/* Dep de-risk: verify the pinned Dear ImGui fetches, compiles, and links — no window, no GL. */

#include "imgui.h"

#include <cstdio>
#include <cstring>

int main()
{
    IMGUI_CHECKVERSION();
    ImGuiContext *ctx = ImGui::CreateContext();
    if (!ctx) {
        std::fprintf(stderr, "imgui_link: CreateContext failed\n");
        return 1;
    }
    const char *v = ImGui::GetVersion();
    bool        ok = v && std::strlen(v) > 0;
    ImGui::DestroyContext(ctx);
    if (!ok) {
        std::fprintf(stderr, "imgui_link: bad version string\n");
        return 1;
    }
    std::printf("imgui_link: ok (Dear ImGui %s)\n", v);
    return 0;
}
