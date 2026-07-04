/* Unit tests for the P11 line diff (text_diff). Pure + offline. Exit non-zero on any failure. */

#include "text_diff.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace hcapp;
using hc::ui::DiffHunk;

static int g_fails = 0;
#define CHECK(c, m)                                                                                 \
    do {                                                                                            \
        if (!(c)) {                                                                                 \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                \
            g_fails++;                                                                              \
        }                                                                                           \
    } while (0)

static int count_kind(const std::vector<DiffHunk> &hs, char k)
{
    int n = 0;
    for (const auto &h : hs)
        for (const auto &l : h.lines)
            if (l.kind == k) n++;
    return n;
}

int main()
{
    bool big = false;
    int  add = -1, rem = -1;

    auto h0 = diff_hunks("a\nb\nc\n", "a\nb\nc\n", &big, &add, &rem);
    CHECK(h0.empty() && !big && add == 0 && rem == 0, "identical -> no hunks");

    auto h1 = diff_hunks("", "x\ny\nz\n", &big, &add, &rem);
    CHECK(!h1.empty() && rem == 0 && add == 3 && count_kind(h1, '+') == 3 && count_kind(h1, '-') == 0,
          "all-added (new file)");

    auto h2 = diff_hunks("x\ny\n", "", &big, &add, &rem);
    CHECK(!h2.empty() && add == 0 && rem == 2, "all-removed");

    auto h3 = diff_hunks("a\nb\nc\n", "a\nB\nc\n", &big, &add, &rem);
    CHECK(add == 1 && rem == 1 && count_kind(h3, ' ') >= 2, "middle change: +1 -1 with context");
    bool minusB = false, plusB = false;
    for (const auto &h : h3)
        for (const auto &l : h.lines) {
            if (l.kind == '-' && l.text == "b") minusB = true;
            if (l.kind == '+' && l.text == "B") plusB = true;
        }
    CHECK(minusB && plusB, "middle change shows -b +B");

    /* two distant changes -> two hunks */
    auto h4 = diff_hunks("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n", "A\nb\nc\nd\ne\nf\ng\nh\ni\nJ\n", &big,
                         &add, &rem);
    CHECK(h4.size() == 2 && add == 2 && rem == 2, "two distant changes -> two hunks");

    /* too large -> flagged, no hunks (bounds the O(n*m) diff) */
    std::string huge;
    for (int i = 0; i < 3000; i++) huge += "line\n";
    auto h5 = diff_hunks(huge, huge + "x\n", &big, &add, &rem);
    CHECK(big && h5.empty(), "too-large -> flagged, no hunks");

    /* adversarial: no trailing newline; a single huge identical line */
    auto h6 = diff_hunks("nonewline", "nonewline2", &big, &add, &rem);
    CHECK(!big && add == 1 && rem == 1, "no-newline lines diff");
    std::string longline(500000, 'z');
    auto        h7 = diff_hunks(longline, longline, &big, &add, &rem);
    CHECK(!big && h7.empty(), "one huge identical line -> no hunks, not flagged");

    if (g_fails) {
        std::fprintf(stderr, "text_diff: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("text_diff: all checks passed\n");
    return 0;
}
