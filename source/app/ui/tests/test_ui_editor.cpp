/* test_ui_editor — the Editor's edit-state logic (W5 P5.3): begin_edit seeds clean, a local edit is dirty, and
 * reconcile (called when the DISPLAYED content changes under an active edit) does the right thing — a save
 * round-trip (disk == buffer) re-baselines to clean while KEEPING the buffer, a genuine external change/reload
 * (disk != buffer) ADOPTS the disk version, and a same-hash reconcile is a no-op. No GL: these methods are pure
 * string/bookkeeping (the render path needs a context and is covered by ui_smoke). hc_imgui supplies only the
 * imgui.h include path ui_editor.hpp references (as for test_ui_sprite). */

#include "ui_editor.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using hc::ui::Editor;

static int g_fail = 0;
#define CHECK(c, m)                                                                                            \
    do {                                                                                                       \
        if (!(c)) {                                                                                            \
            std::fprintf(stderr, "FAIL: %s\n", (m));                                                          \
            g_fail++;                                                                                          \
        }                                                                                                      \
    } while (0)

static void seed(Editor &ed, const char *s, std::uint64_t h) { ed.begin_edit(s, std::strlen(s), h); }
static void recon(Editor &ed, const char *s, std::uint64_t h) { ed.reconcile(s, std::strlen(s), h); }

int main()
{
    Editor ed;

    /* begin_edit seeds the buffer clean */
    seed(ed, "line1\nline2\n", 0x11);
    CHECK(ed.editing(), "editing after begin_edit");
    CHECK(!ed.dirty(), "clean right after begin_edit");
    CHECK(ed.edit_buffer() == "line1\nline2\n", "buffer seeded from the bytes");

    /* a local edit makes it dirty */
    ed.edit_mutable() += "line3\n";
    CHECK(ed.dirty(), "dirty after a local edit");

    /* SAVE round-trip: the host re-synced the displayed content to OUR buffer (a new hash, same bytes) ->
     * re-baseline to clean, keep the buffer */
    std::string saved = ed.edit_buffer();
    recon(ed, saved.c_str(), 0x22);
    CHECK(!ed.dirty(), "a save round-trip re-baselines to clean");
    CHECK(ed.edit_buffer() == saved, "a save round-trip KEEPS the buffer");

    /* a fresh local edit, then an EXTERNAL change (different content at a new hash) -> ADOPT the disk content,
     * discard the local edits, clean */
    ed.edit_mutable() += "scratch";
    CHECK(ed.dirty(), "dirty again before the external change");
    recon(ed, "totally different\n", 0x33);
    CHECK(!ed.dirty(), "an external change adopted -> clean");
    CHECK(ed.edit_buffer() == "totally different\n", "an external change ADOPTS the disk content");

    /* a SAME-hash reconcile is a no-op: it must not stomp in-progress edits */
    ed.edit_mutable() = "in progress";
    recon(ed, "totally different\n", 0x33); /* same hash as the last seed */
    CHECK(ed.edit_buffer() == "in progress", "a same-hash reconcile keeps in-progress edits");
    CHECK(ed.dirty(), "a same-hash reconcile leaves the edits dirty");

    /* end_edit exits; a fresh begin_edit re-seeds from new bytes */
    ed.end_edit();
    CHECK(!ed.editing(), "end_edit exits edit mode");
    seed(ed, "brand new\n", 0x44);
    CHECK(ed.editing() && !ed.dirty() && ed.edit_buffer() == "brand new\n", "begin_edit re-seeds clean");

    if (g_fail == 0) std::printf("test_ui_editor: all edit-state checks passed\n");
    return g_fail ? 1 : 0;
}
