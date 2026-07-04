/* text_diff — see text_diff.hpp. An LCS line diff with git-style hunking (P11). Pure + reentrant (no
 * shared state, no I/O); all arguments and the returned hunks are caller-owned. diff_hunks is a 3-phase
 * state machine — LCS table -> backtrack into ops -> coalesce into hunks — whose shared locals (the dp
 * table, the op list, the line-number maps) flow between phases, so it reads cleanest as one function;
 * the kMaxLines cap bounds the O(n*m) table so untrusted content can't drive an unbounded diff. */

#include "text_diff.hpp"

#include <algorithm>

namespace hcapp {

namespace {

constexpr size_t kMaxLines = 2000; /* bounds the O(n*m) LCS table; beyond this -> blob fallback */
constexpr int    kCtx = 3;         /* context lines kept around each change */

std::vector<std::string> split_lines(const std::string &s)
{
    std::vector<std::string> lines;
    if (s.empty()) return lines;
    for (size_t start = 0;;) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(s.substr(start));
            break;
        }
        lines.push_back(s.substr(start, nl - start));
        start = nl + 1;
        if (start == s.size()) break; /* trailing newline -> no phantom empty final line */
    }
    return lines;
}

} // namespace

std::vector<hc::ui::DiffHunk> diff_hunks(const std::string &old_text, const std::string &new_text,
                                         bool *too_large, int *added, int *removed)
{
    if (too_large) *too_large = false;
    if (added) *added = 0;
    if (removed) *removed = 0;

    std::vector<std::string> A = split_lines(old_text), B = split_lines(new_text);
    if (A.size() > kMaxLines || B.size() > kMaxLines) {
        if (too_large) *too_large = true;
        return {};
    }
    const size_t n = A.size(), m = B.size();

    /* LCS length table */
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 1; i <= n; i++)
        for (size_t j = 1; j <= m; j++)
            dp[i][j] = (A[i - 1] == B[j - 1]) ? dp[i - 1][j - 1] + 1
                                              : std::max(dp[i - 1][j], dp[i][j - 1]);

    /* backtrack into ops (' ' context | '-' removed | '+' added), then reverse to forward order */
    struct Op {
        char               k;
        const std::string *line;
    };
    std::vector<Op> ops;
    size_t          i = n, j = m;
    while (i > 0 && j > 0) {
        if (A[i - 1] == B[j - 1]) {
            ops.push_back({' ', &A[i - 1]});
            i--;
            j--;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            ops.push_back({'-', &A[i - 1]});
            i--;
        } else {
            ops.push_back({'+', &B[j - 1]});
            j--;
        }
    }
    while (i > 0) ops.push_back({'-', &A[--i]});
    while (j > 0) ops.push_back({'+', &B[--j]});
    std::reverse(ops.begin(), ops.end());

    const size_t     N = ops.size();
    std::vector<int> oln(N), nln(N); /* 1-based line numbers before each op */
    for (size_t x = 0, o = 1, nn = 1; x < N; x++) {
        oln[x] = (int)o;
        nln[x] = (int)nn;
        if (ops[x].k == ' ') {
            o++;
            nn++;
        } else if (ops[x].k == '-')
            o++;
        else
            nn++;
    }

    std::vector<size_t> chg; /* indices of changed ops */
    for (size_t x = 0; x < N; x++)
        if (ops[x].k != ' ') chg.push_back(x);
    if (chg.empty()) return {}; /* identical */

    std::vector<hc::ui::DiffHunk> hunks;
    int                           tot_add = 0, tot_rem = 0;
    for (size_t gi = 0; gi < chg.size();) {
        size_t first = chg[gi], last = chg[gi], gj = gi + 1;
        /* merge change runs whose ±context would overlap (git's default hunk coalescing) */
        while (gj < chg.size() && chg[gj] <= last + (size_t)(2 * kCtx + 1)) {
            last = chg[gj];
            gj++;
        }
        size_t          hs = first >= (size_t)kCtx ? first - kCtx : 0;
        size_t          he = last + kCtx < N ? last + kCtx : N - 1;
        hc::ui::DiffHunk h;
        h.old_start = oln[hs];
        h.new_start = nln[hs];
        for (size_t x = hs; x <= he; x++) {
            h.lines.push_back({ops[x].k, *ops[x].line});
            if (ops[x].k == '+') tot_add++;
            else if (ops[x].k == '-') tot_rem++;
        }
        hunks.push_back(std::move(h));
        gi = gj;
    }
    if (added) *added = tot_add;
    if (removed) *removed = tot_rem;
    return hunks;
}

} // namespace hcapp
