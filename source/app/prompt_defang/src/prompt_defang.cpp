/* prompt_defang — implementation of the shared prompt-injection defangs (see prompt_defang.hpp). PURE. */

#include "prompt_defang.hpp"

namespace hcapp {

namespace {

/* Replace the opening '[' of every occurrence of each marker with '(', breaking the literal fence delimiter so
 * untrusted text cannot open/close the fence. Markers begin with '[' by contract. */
void neutralize(std::string &t, std::initializer_list<const char *> markers)
{
    for (const char *mark : markers)
        for (std::size_t p = t.find(mark); p != std::string::npos; p = t.find(mark, p + 1))
            t[p] = '(';
}

} // namespace

std::string defang_inline(const std::string &raw, std::initializer_list<const char *> markers)
{
    std::string t = raw;
    for (char &c : t)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    neutralize(t, markers);
    return t;
}

std::string defang_block(const std::string &raw, std::initializer_list<const char *> markers)
{
    std::string t;
    t.reserve(raw.size());
    for (unsigned char c : raw)
        if ((c >= 0x20 && c != 0x7f) || c == '\n' || c == '\t') t.push_back((char)c);
    neutralize(t, markers);
    return t;
}

} // namespace hcapp
