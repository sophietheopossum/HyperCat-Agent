/* test_ui_texcache — the PURE LruBudget core of the OperatorTextureCache (W4 P4.2): MRU ordering, byte +
 * count accounting, LRU-first eviction to fit a budget, the count cap, and the soft-budget over-cap case. No
 * ImGui/GL context (LruBudget touches neither — the GPU texture lifecycle is exercised separately by ui_smoke
 * + the headless image screenshot). */

#include "ui_texcache.hpp"

#include <cstdio>
#include <vector>

using hc::ui::LruBudget;

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                          \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

static bool eq(const std::vector<std::uint64_t> &v, std::initializer_list<std::uint64_t> e)
{
    if (v.size() != e.size()) return false;
    size_t i = 0;
    for (std::uint64_t x : e)
        if (v[i++] != x) return false;
    return true;
}

int main()
{
    /* --- basic insert + byte accounting --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 40);
        b.insert(2, 40);
        CHECK(b.bytes() == 80 && b.count() == 2, "two inserts sum bytes + count");
        CHECK(b.contains(1) && b.contains(2) && !b.contains(3), "contains tracks membership");
        b.insert(1, 999); /* contract: duplicate key ignored */
        CHECK(b.bytes() == 80 && b.count() == 2, "duplicate insert is a no-op");
    }

    /* --- remove returns bytes + decrements; absent is 0 --- */
    {
        LruBudget b(100, 8);
        b.insert(7, 40);
        CHECK(b.remove(7) == 40 && b.bytes() == 0 && b.count() == 0, "remove returns bytes + clears");
        CHECK(b.remove(7) == 0, "remove of an absent key is 0");
    }

    /* --- already-fits: no victims --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 10);
        CHECK(eq(b.victims_for(10), {}), "fits the budget -> no eviction");
    }

    /* --- LRU-first eviction to fit a byte budget --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 40); /* oldest -> LRU */
        b.insert(2, 40); /* newest -> MRU */
        /* inserting 40 more would be 120 > 100; free >= 20 -> evict the LRU (key 1, 40B). */
        CHECK(eq(b.victims_for(40), {1}), "evicts the LRU to fit the byte budget");
    }

    /* --- touch reorders: the touched key survives, the other becomes LRU --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 40);
        b.insert(2, 40);
        b.touch(1); /* 1 is now MRU, 2 is LRU */
        CHECK(eq(b.victims_for(40), {2}), "touch moves a key to MRU so the OTHER is evicted");
        b.touch(999); /* touching an absent key is a no-op */
        CHECK(b.count() == 2, "touch of an absent key changes nothing");
    }

    /* --- multi-victim: evict several LRU entries until it fits --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 30); /* LRU */
        b.insert(2, 30);
        b.insert(3, 30); /* MRU; bytes=90 */
        /* +50 -> 140; free >= 40 -> evict 1 (30, still short), then 2 (60 >= 40) -> stop. */
        CHECK(eq(b.victims_for(50), {1, 2}), "evicts LRU-first until the incoming fits");
    }

    /* --- the COUNT cap evicts even when bytes are ample --- */
    {
        LruBudget b(100000, 2);
        b.insert(1, 1); /* LRU */
        b.insert(2, 1); /* MRU; count=2 == cap */
        CHECK(eq(b.victims_for(1), {1}), "the count cap evicts the LRU even with bytes to spare");
    }

    /* --- soft budget: a single over-budget entry evicts everything, caller still inserts --- */
    {
        LruBudget b(100, 8);
        b.insert(1, 10);
        b.insert(2, 10);
        CHECK(eq(b.victims_for(500), {1, 2}), "an over-budget incoming evicts all evictable (soft budget)");
    }

    /* --- a fresh cache yields no victims --- */
    {
        LruBudget b(100, 8);
        CHECK(eq(b.victims_for(10), {}), "empty cache -> no victims");
        CHECK(eq(b.victims_for(99999), {}), "empty cache + huge incoming -> still no victims (nothing to evict)");
    }

    if (g_fail == 0) std::printf("test_ui_texcache: all LruBudget checks passed\n");
    return g_fail ? 1 : 0;
}
