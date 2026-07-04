/* ui_graph — sparkline / area-graph widgets. See ui_graph.hpp.
 *
 * Both reserve a rectangle via Dummy (so layout flows + the panel scrolls), then draw into the window draw
 * list in absolute coords from the reserved origin — the timeline/DAG idiom. A 1px border box frames the
 * plot; the line is a 1px accent polyline; the area adds a low-alpha fill as per-segment quads (ImDrawList
 * has no concave fill, so the area under each segment is one quad down to the baseline). */

#include "ui_graph.hpp"

#include "ui_theme.hpp" /* accent_v4 */

#include <vector>

namespace hc::ui {

namespace {

/* Reserve `size`, draw the border, and project the values to screen points scaled to [vmin,vmax]. Returns
 * false (only the box drawn) when there is nothing plottable. `out_pts` and the baseline Y are filled. */
bool prep(const float *values, int n, ImVec2 &size, float vmin, float vmax, ImVec2 &a, ImVec2 &b,
          ImDrawList *&dl, std::vector<ImVec2> &out_pts)
{
    a = ImGui::GetCursorScreenPos();
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.x < 1.0f) size.x = 1.0f;
    ImGui::Dummy(size); /* reserve the rect so the widget participates in layout/scroll */
    dl = ImGui::GetWindowDrawList();
    b = ImVec2(a.x + size.x, a.y + size.y);
    dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), 0.0f, 0, 1.0f); /* the signature 1px box */
    if (!values || n < 2 || vmax <= vmin) return false;
    out_pts.resize((size_t)n);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        float v = (values[i] - vmin) / (vmax - vmin);
        if (v < 0.0f) v = 0.0f;
        else if (v > 1.0f) v = 1.0f;
        out_pts[(size_t)i] = ImVec2(a.x + t * size.x, b.y - v * size.y);
    }
    return true;
}

} // namespace

void Sparkline(const float *values, int n, ImVec2 size, float vmin, float vmax)
{
    ImVec2              a, b;
    ImDrawList         *dl = nullptr;
    std::vector<ImVec2> pts;
    if (!prep(values, n, size, vmin, vmax, a, b, dl, pts)) return;
    dl->AddPolyline(pts.data(), (int)pts.size(), ImGui::GetColorU32(accent_v4()), 0, 1.0f);
}

void AreaGraph(const float *values, int n, ImVec2 size, float vmin, float vmax)
{
    ImVec2              a, b;
    ImDrawList         *dl = nullptr;
    std::vector<ImVec2> pts;
    if (!prep(values, n, size, vmin, vmax, a, b, dl, pts)) return;
    ImVec4 acc = accent_v4();
    ImU32  fill = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.14f)); /* low-alpha accent fill */
    for (size_t i = 0; i + 1 < pts.size(); i++)
        dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, b.y), ImVec2(pts[i].x, b.y), fill);
    dl->AddPolyline(pts.data(), (int)pts.size(), ImGui::GetColorU32(acc), 0, 1.0f);
}

} // namespace hc::ui
