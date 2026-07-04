/* genlock — DEV-ONLY utility (not shipped): compute + write a third-party tool's manifest.lock (the tree-hash
 * supply-chain pin) for HEADLESS install/approval, since the operator's consent-enable UI normally writes it.
 * Uses the SAME hcapp::tool_lock_hex the ToolHost verifies at launch, so the lock matches by construction.
 * Usage:  genlock <tools_root> <id>   ->  writes <tools_root>/<id>/manifest.lock (prints the hex). */

#include "hc_toolhost.hpp"

#include "hc_fs.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: genlock <tools_root> <id>\n");
        return 2;
    }
    std::string dir = std::string(argv[1]) + "/" + argv[2];
    size_t      mlen = 0;
    char       *mbuf = hc_fs_read_file((dir + "/manifest.json").c_str(), 64u * 1024, &mlen);
    if (!mbuf) {
        std::fprintf(stderr, "genlock: no readable manifest at %s/manifest.json\n", dir.c_str());
        return 1;
    }
    std::string hex = hcapp::tool_lock_hex(dir, mbuf, mlen);
    free(mbuf);
    if (hex.empty()) {
        std::fprintf(stderr, "genlock: tree-hash compute failed for %s\n", dir.c_str());
        return 1;
    }
    FILE *f = std::fopen((dir + "/manifest.lock").c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "genlock: cannot write %s/manifest.lock\n", dir.c_str());
        return 1;
    }
    bool ok = std::fwrite(hex.data(), 1, hex.size(), f) == hex.size();
    std::fclose(f);
    if (!ok) return 1;
    std::printf("%s\n", hex.c_str());
    return 0;
}
