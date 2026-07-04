#ifndef HC_UI_GRAPH_HPP
#define HC_UI_GRAPH_HPP

/* ui_graph — zero-dependency sparkline / area-graph widgets drawn with ImDrawList (the same 1px-stroke +
 * low-alpha-fill + 1px-border vocabulary the timeline/DAG panels use), so they are on-theme by construction
 * within doc-10's restrained palette. ImPlot is deliberately rejected (a heavy dep that fights the flat
 * aesthetic). For the WI-3 system/usage graphs. UI thread only. */

#include "imgui.h"

namespace hc::ui {

/* A sparkline: reserve `size` (size.x<=0 => fill the content width), draw a 1px border box, then the
 * `n` values as a 1px accent polyline scaled to [vmin,vmax] (clamped). n<2 or vmax<=vmin => just the box. */
void Sparkline(const float *values, int n, ImVec2 size, float vmin, float vmax);

/* Like Sparkline but with a low-alpha accent fill under the line (an area graph). */
void AreaGraph(const float *values, int n, ImVec2 size, float vmin, float vmax);

} // namespace hc::ui

#endif /* HC_UI_GRAPH_HPP */
