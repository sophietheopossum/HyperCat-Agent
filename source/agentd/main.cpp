/* agentd — agent worker process entry. One agent per OS process (isolated address space; a crash
 * cannot spread — see doc 02). Deliberately thin: parse the bus coordinates, then hand off to
 * run_worker, which owns the protocol and the loop.
 *
 * Usage: agentd --sock <path> --id <agent:id> [--token-fd <n>] [--controller <id>]
 *               [--model <m>] [--base-url <u>] [--workspace <dir>] [--crash-on-task]
 *   --token-fd is an inherited pipe fd carrying the one-time spawn token, kept off argv/env so it
 *   is not visible in `ps` or /proc/<pid>/environ; the supervisor sets it up. Absent => no token.
 *   --controller is the only id allowed to send task.assign (the orchestrator). --role/--role-prompt/
 *   --role-tools define the worker's spawn-time identity (label + appended persona overlay + tool-id
 *   subset; all non-secret, so they ride on argv). --model + the env key OPENROUTER_API_KEY switch a
 *   task to a real LLM turn; otherwise it is a deterministic echo.
 *   --workspace is an existing dir the worker jails an hc_sandbox to + exposes as the human-gated
 *   fs_write tool; absent => no filesystem tool. --sessions is an hc_store root the worker persists
 *   each task's transcript to (for the host session browser); absent => no persistence.
 */

#include "worker.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s --sock <path> --id <agent:id> [--token-fd <n>] [--controller <id>]\n"
                 "          [--role <r>] [--role-prompt <text>] [--role-tools <csv>]\n"
                 "          [--model <m>] [--base-url <u>] [--workspace <dir>] [--sessions <dir>]\n"
                 "          [--egress-allow <ip>]... [--crash-on-task]\n",
                 argv0);
}

/* Value following `flag` in argv, or nullptr if absent or it is the last token. */
const char *opt(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

/* Whether a value-less `flag` is present in argv. */
bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

/* Collect every value following a REPEATED `flag` (e.g. --egress-allow 10.0.0.5 --egress-allow ::1). */
std::vector<std::string> opt_all(int argc, char **argv, const char *flag)
{
    std::vector<std::string> out;
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], flag) == 0) out.emplace_back(argv[i + 1]);
    return out;
}

/* Parse a --token-fd value: a real, non-std inherited fd (>= 3). -1 on absent/invalid — atoi would
 * silently turn garbage into 0 (== stdin), making read_fd_all consume stdin as the token. */
int parse_fd(const char *s)
{
    if (!s || !*s) return -1;
    char *end = nullptr;
    long  v = std::strtol(s, &end, 10);
    if (*end != '\0' || v < 3 || v > 1000000) return -1;
    return (int)v;
}

} // namespace

int main(int argc, char **argv)
{
    const char *sock = opt(argc, argv, "--sock");
    const char *id = opt(argc, argv, "--id");
    const char *token_fd = opt(argc, argv, "--token-fd");
    if (!sock || !id) {
        usage(argv[0]);
        return 1;
    }

    const char *model = opt(argc, argv, "--model");
    const char *provider = opt(argc, argv, "--provider"); /* per-role OpenRouter routing (not a secret) */
    const char *base_url = opt(argc, argv, "--base-url");
    const char *controller = opt(argc, argv, "--controller");
    const char *workspace = opt(argc, argv, "--workspace");
    const char *sessions = opt(argc, argv, "--sessions");
    /* Worker Revamp W2: the spawn-time role identity + its persona overlay + its tool subset. ALL
     * non-secret (only the API key stays env-only): `--role` is the label/selector, `--role-prompt`
     * is appended after the worker base prompt, `--role-tools` is a tool-id csv ("" => all tools). */
    const char *role = opt(argc, argv, "--role");
    const char *role_prompt = opt(argc, argv, "--role-prompt");
    const char *role_tools = opt(argc, argv, "--role-tools");
    /* W6 P6.2: per-project Skills — the host-built fenced catalog (appended to the prompt) + the jailed
     * skills/ dir (the load_skill tool reads a body on demand). Both non-secret. */
    const char *skills_catalog = opt(argc, argv, "--skills-catalog");
    const char *skills_dir = opt(argc, argv, "--skills-dir");

    hc::WorkerConfig cfg;
    cfg.sock = sock;
    cfg.id = id;
    cfg.token_fd = parse_fd(token_fd); /* validated; -1 when absent or garbage */
    cfg.crash_on_task = has_flag(argc, argv, "--crash-on-task");
    if (controller) cfg.controller_id = controller;
    if (role) cfg.role = role;
    if (role_prompt) cfg.role_prompt = role_prompt;
    if (role_tools) cfg.role_tools = role_tools;
    cfg.exec_enabled = has_flag(argc, argv, "--exec-enabled"); /* W4.3: register the run tool (host-gated) */
    if (model) cfg.model = model;
    if (base_url) cfg.base_url = base_url;
    if (provider) cfg.provider = provider;
    if (workspace) cfg.workspace = workspace;
    if (sessions) cfg.sessions = sessions;
    cfg.egress_allow = opt_all(argc, argv, "--egress-allow"); /* WI-2 E0: repeatable LAN re-permits */
    if (skills_catalog) cfg.skills_catalog = skills_catalog;
    if (skills_dir) cfg.skills_dir = skills_dir;
    return hc::run_worker(cfg);
}
