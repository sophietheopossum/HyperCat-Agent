/* worker_role — the role tool-subset parser. See worker_role.hpp for the contract. Pure string logic over the
 * --role-tools csv: no I/O, no dependencies beyond <string>, so it is testable without the bus/llm/agent stack
 * (test_role_tools links this TU alone). */

#include "worker_role.hpp"

namespace hc {

RoleToolset parse_role_tools(const std::string &csv)
{
    RoleToolset rs; /* default: all-on */
    if (csv.empty()) return rs;
    rs.deep_reason = rs.memory_recall = rs.memory_write = false; /* a non-empty list is EXACTLY the listed */
    rs.fs_read = rs.fs_list = rs.fs_write = rs.fs_update = false;
    rs.load_skill = false;
    size_t i = 0;
    while (i < csv.size()) {
        size_t      j = csv.find(',', i);
        std::string name = csv.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = (j == std::string::npos) ? csv.size() : j + 1;
        if (name == "deep_reason") rs.deep_reason = true;
        else if (name == "memory_recall") rs.memory_recall = true;
        else if (name == "memory_write") rs.memory_write = true;
        else if (name == "fs_read") rs.fs_read = true;
        else if (name == "fs_list") rs.fs_list = true;
        else if (name == "fs_write") rs.fs_write = true;
        else if (name == "fs_update") rs.fs_update = true;
        else if (name == "load_skill") rs.load_skill = true;
        /* an unknown name is ignored — a role config can never grant a tool that doesn't exist */
    }
    return rs;
}

} // namespace hc
