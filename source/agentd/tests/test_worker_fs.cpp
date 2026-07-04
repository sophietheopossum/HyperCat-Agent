/* test_worker_fs — the worker's sandbox-backed file-op core against a REAL hc_sandbox jail: fs_read_text /
 * fs_list_text (W1.1), fs_read_raw + fs_apply_edit (the fs_update core, W1.2), and fs_ensure_parent_dirs (the
 * subdir-deliverable enabler — creates a missing parent chain so an fs_write to "deep/nested/app.py" lands
 * instead of looping). A planted file reads back; an empty file + an empty dir report cleanly; a directory
 * lists sorted with dir/file markers; a missing path, a `..` escape, and an absolute path are all refused
 * (the confinement is hc_sandbox's, here verified end-to-end through the tool core). No bus/llm. */

#include "worker_fs.hpp"

#include "hc_sandbox.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                   \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                                       \
            g_fail++;                                                                                      \
        }                                                                                                  \
    } while (0)

static void plant(const std::string &path, const std::string &content)
{
    std::ofstream f(path, std::ios::binary);
    f << content;
}

static int rm_entry(const char *p, const struct stat *, int, struct FTW *) { return remove(p); }
/* recursively remove the temp jail (the test creates nested subtrees via mkdirs — a flat unlink/rmdir list
 * would miss them and leak the temp dir). FTW_DEPTH so children are removed before their parent. */
static void rm_rf(const std::string &path) { nftw(path.c_str(), rm_entry, 8, FTW_DEPTH | FTW_PHYS); }

static bool err(const std::string &s) { return s.rfind("error:", 0) == 0; }

int main()
{
    char  tmpl[] = "/tmp/hc_wfs_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string root = dir;
    plant(root + "/hello.txt", "hi there"); /* 8 bytes */
    plant(root + "/empty.txt", "");
    mkdir((root + "/sub").c_str(), 0700);

    hc_sandbox_status serr = HC_SANDBOX_OK;
    hc_sandbox       *sb = hc_sandbox_open(root.c_str(), &serr);
    CHECK(sb != nullptr, "sandbox opens on the temp root");

    if (sb) {
        /* --- fs_read --- */
        CHECK(hc::fs_read_text(sb, "hello.txt") == "hi there", "fs_read returns the file contents");
        CHECK(hc::fs_read_text(sb, "empty.txt") == "(empty file)", "fs_read of a 0-byte file");
        CHECK(err(hc::fs_read_text(sb, "")), "fs_read of an empty path errors");
        CHECK(err(hc::fs_read_text(sb, "nope.txt")), "fs_read of a missing file errors");
        CHECK(err(hc::fs_read_text(sb, "../escape")), "fs_read of a .. escape is refused (confinement)");
        CHECK(err(hc::fs_read_text(sb, "/etc/hostname")), "fs_read of an absolute path is refused");

        /* --- fs_list --- */
        const std::string ls = hc::fs_list_text(sb, ".");
        CHECK(ls.find("hello.txt\t8") != std::string::npos, "fs_list shows a file + its size");
        CHECK(ls.find("empty.txt\t0") != std::string::npos, "fs_list shows the empty file");
        CHECK(ls.find("sub/") != std::string::npos, "fs_list marks a directory with a trailing /");
        CHECK(ls.find("empty.txt") < ls.find("hello.txt"), "fs_list is sorted by name");
        CHECK(hc::fs_list_text(sb, "sub") == "(empty)", "fs_list of an empty dir");
        CHECK(err(hc::fs_list_text(sb, "nope")), "fs_list of a missing dir errors");

        /* --- fs_read_raw (the fs_update read path: raw bytes, no sentinels) --- */
        std::string raw, rerr;
        CHECK(hc::fs_read_raw(sb, "hello.txt", raw, rerr) && raw == "hi there", "fs_read_raw returns raw bytes");
        CHECK(hc::fs_read_raw(sb, "empty.txt", raw, rerr) && raw.empty(), "fs_read_raw of an empty file is \"\"");
        CHECK(!hc::fs_read_raw(sb, "nope.txt", raw, rerr) && err(rerr), "fs_read_raw of a missing file errors");
        CHECK(!hc::fs_read_raw(sb, "../escape", raw, rerr) && err(rerr), "fs_read_raw of a .. escape is refused");

        /* --- fs_ensure_parent_dirs (so a subdir deliverable like "src/app.py" can be written) --- */
        std::string derr;
        CHECK(hc::fs_ensure_parent_dirs(sb, "flat.txt", derr), "a flat path needs no parent (no-op success)");
        CHECK(hc::fs_ensure_parent_dirs(sb, "deep/nested/app.py", derr),
              "creates the missing parent chain for a subdir deliverable");
        /* the chain now exists, so the actual write lands (the exact case that used to loop) */
        hc_sandbox_fd wfd = HC_SANDBOX_FD_INVALID;
        CHECK(hc_sandbox_open_fd(sb, "deep/nested/app.py", O_WRONLY | O_CREAT | O_TRUNC, 0600, &wfd) ==
                  HC_SANDBOX_OK,
              "the subdir write succeeds after the parents are ensured");
        if (wfd != HC_SANDBOX_FD_INVALID) close(wfd);
        CHECK(hc::fs_ensure_parent_dirs(sb, "deep/nested/again.py", derr), "ensuring an existing chain is idempotent");
        CHECK(!hc::fs_ensure_parent_dirs(sb, "../evil/x.txt", derr) && err(derr),
              "a .. escape in the parent is refused");

        hc_sandbox_close(sb);
    }

    /* --- fs_apply_edit (PURE — the fs_update edit core) --- */
    {
        std::string out, e;
        CHECK(hc::fs_apply_edit("hello world", "replace", "world", "there", "", out, e) && out == "hello there",
              "replace: a unique match swaps");
        CHECK(!hc::fs_apply_edit("hello", "replace", "xyz", "a", "", out, e) && e.find("not found") != std::string::npos,
              "replace: no match errors");
        CHECK(!hc::fs_apply_edit("a a", "replace", "a", "b", "", out, e) && e.find("more than once") != std::string::npos,
              "replace: a non-unique match errors (no silent corruption)");
        CHECK(!hc::fs_apply_edit("hi", "replace", "", "x", "", out, e), "replace: an empty old_string errors");
        CHECK(hc::fs_apply_edit("hi", "append", "", "", " there", out, e) && out == "hi there",
              "append: concatenates");
        CHECK(!hc::fs_apply_edit("hi", "append", "", "", "", out, e), "append: empty append_text errors");
        CHECK(!hc::fs_apply_edit("hi", "frobnicate", "", "", "", out, e), "an unknown mode errors");
    }

    rm_rf(root); /* the test created nested subtrees (mkdirs) — remove the whole jail recursively */

    if (g_fail) {
        std::fprintf(stderr, "test_worker_fs: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("test_worker_fs: all checks passed\n");
    return 0;
}
