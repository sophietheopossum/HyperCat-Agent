#ifndef HC_TEXT_DIFF_HPP
#define HC_TEXT_DIFF_HPP

/* text_diff — a pure, dependency-free unified line diff (P11). Splits both sides on '\n', runs an LCS,
 * and emits git-style hunks (3 lines of context) the diff-review panel renders. Bounded: beyond the
 * line cap it sets *too_large and returns no hunks (the caller falls back to the blob summary), so a
 * hostile/huge fs_write can't drive an unbounded O(n*m) diff on the host. No I/O, no ImGui — unit-tested
 * directly. Lives app-side (the consumer is the host approval path), not in libs/.
 * Threading: reentrant — pure, no shared state. Lifetime: args + returned hunks are caller-owned. */

#include "hc_ui.hpp"

#include <string>
#include <vector>

namespace hcapp {

/* Diff old_text -> new_text into unified hunks. *too_large is set (and {} returned) if either side is
 * too large to diff; *added / *removed receive the changed-line counts (for the panel header). Any
 * out-param may be null. */
std::vector<hc::ui::DiffHunk> diff_hunks(const std::string &old_text, const std::string &new_text,
                                         bool *too_large, int *added, int *removed);

} // namespace hcapp

#endif /* HC_TEXT_DIFF_HPP */
