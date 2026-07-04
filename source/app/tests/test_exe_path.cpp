/* Unit tests for exe_path — the relocatable sibling-helper resolver. Drives resolve_sibling()'s branches with a
 * temp-dir fixture, then confirms resolve_sibling_exe() locates the test's OWN binary next to itself via
 * /proc/self/exe (the real bundle mechanism, end to end). Exit non-zero on any failure (CTest reads the code). */

#include "exe_path.hpp"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                                      \
            g_fails++;                                                                       \
        }                                                                                   \
    } while (0)

int main()
{
    char  tmpl[] = "/tmp/hc_exe_path_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp");
    if (!dir) return 1;
    const std::string d(dir);
    const char *const FB = "/baked/fallback/agentd"; /* the compile-time constant stand-in */

    /* an EXECUTABLE sibling -> resolved to dir/name (the relocated-bundle case) */
    const std::string exe = d + "/agentd";
    if (FILE *f = std::fopen(exe.c_str(), "w")) {
        std::fputs("stub", f);
        std::fclose(f);
    }
    chmod(exe.c_str(), 0755);
    CHECK(hc::resolve_sibling(d, "agentd", FB) == exe, "executable sibling resolves to dir/name");

    /* a MISSING sibling -> the fallback (the dev-tree case) */
    CHECK(hc::resolve_sibling(d, "nope", FB) == std::string(FB), "missing sibling -> fallback");

    /* a NON-executable file of the same name -> the fallback (access X_OK must fail, not just existence) */
    const std::string nox = d + "/data.txt";
    if (FILE *g = std::fopen(nox.c_str(), "w")) {
        std::fputs("x", g);
        std::fclose(g);
    }
    chmod(nox.c_str(), 0644);
    CHECK(hc::resolve_sibling(d, "data.txt", FB) == std::string(FB), "non-executable sibling -> fallback");

    /* an empty dir (exe_dir() couldn't be read) -> the fallback */
    CHECK(hc::resolve_sibling("", "agentd", FB) == std::string(FB), "empty dir -> fallback");
    /* a null/empty fallback is returned as "" (never a crash) */
    CHECK(hc::resolve_sibling("", "agentd", nullptr).empty(), "null fallback -> empty string");

    /* END TO END: resolve_sibling_exe locates THIS test binary as a sibling of itself via /proc/self/exe. */
    const std::string self = hc::resolve_sibling_exe("test_exe_path", FB);
    CHECK(self != std::string(FB), "resolve_sibling_exe finds a real sibling (self), not the fallback");
    CHECK(self.size() >= 13 && self.compare(self.size() - 13, 13, "test_exe_path") == 0,
          "resolved path ends with the requested name");
    CHECK(access(self.c_str(), X_OK) == 0, "the resolved self path is actually executable");

    unlink(exe.c_str());
    unlink(nox.c_str());
    rmdir(d.c_str());

    if (g_fails) {
        std::fprintf(stderr, "exe_path: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("exe_path: all checks passed\n");
    return 0;
}
