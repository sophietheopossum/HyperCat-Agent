/* hc-replay — re-drive a recorded agent session OFFLINE and print the per-turn manifest diff (the
 * divergence oracle). No key, no network, no real side effects (tools are mocked from the recording).
 *
 * Usage: hc-replay <session_dir> [--task <task>] [--system <system_prompt>]
 *   <session_dir>   a session directory holding manifest.jsonl (and usually transcript.jsonl).
 *   --task          the original user task; if omitted, read from <session_dir>/transcript.jsonl.
 *   --system        the original system prompt. The bulky input is hashed-not-stored in the manifest, so
 *                   a CLEAN match needs the SAME system_prompt + task as the run; a mismatch is reported
 *                   as a turn-0 divergence (the oracle still works — it just won't say "match").
 * Exit: 0 on a clean match, 1 on a divergence or a setup/I-O error. */

#include "replay.hpp"

#include "hc_fs.h"
#include "hc_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* First user message in a session transcript (JSONL of {ts,role,content}); "" if none/unreadable. */
static std::string transcript_first_task(const std::string &session_dir)
{
    std::string path = session_dir + "/transcript.jsonl";
    size_t      len = 0;
    char       *buf = hc_fs_read_file(path.c_str(), 1u * 1024 * 1024, &len);
    if (!buf) return "";
    std::string out;
    size_t      start = 0;
    for (size_t i = 0; i <= len && out.empty(); i++) {
        if (i == len || buf[i] == '\n') {
            if (i > start) {
                if (hc_json *o = hc_json_parse(buf + start, i - start)) {
                    if (std::strcmp(hc_json_get_str(o, "role", ""), "user") == 0)
                        out = hc_json_get_str(o, "content", "");
                    hc_json_free(o);
                }
            }
            start = i + 1;
        }
    }
    free(buf);
    return out;
}

int main(int argc, char **argv)
{
    const char *session = nullptr;
    std::string task, system_prompt;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--task") == 0 && i + 1 < argc)
            task = argv[++i];
        else if (std::strcmp(argv[i], "--system") == 0 && i + 1 < argc)
            system_prompt = argv[++i];
        else if (argv[i][0] != '-' && !session)
            session = argv[i];
    }
    if (!session) {
        std::fprintf(stderr, "usage: hc-replay <session_dir> [--task <task>] [--system <system_prompt>]\n");
        return 1;
    }
    if (task.empty()) task = transcript_first_task(session);
    if (task.empty()) {
        std::fprintf(stderr, "hc-replay: no --task and no user message in transcript — cannot replay\n");
        return 1;
    }

    hc::replay::Report rep = hc::replay::replay_run(session, system_prompt, task);
    if (!rep.ok) {
        std::fprintf(stderr, "hc-replay: %s\n", rep.error.c_str());
        return 1;
    }
    std::printf("%s\n", rep.diff.detail);
    if (!rep.diff.match)
        std::printf("  matched %zu leading turn(s); first divergence at turn %d\n", rep.diff.matched,
                    rep.diff.first_diff);
    return rep.diff.match ? 0 : 1;
}
