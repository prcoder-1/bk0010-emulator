#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <fstream>
#include <cctype>

namespace bk {

// Symbol table loaded from a linker .map file. Supports GNU-ld map lines
// ("   0x1234   name") and simple "name = value" definitions (value in 0x hex,
// leading-0 octal or decimal). Pure C++ (no Qt) so both the core/GUI and the
// disassembler can resolve addresses to names. Same formats the MCP bk_symbols
// tool accepts.
class SymbolTable {
public:
    // Load from `path`, replacing any current symbols. Returns the count loaded,
    // or -1 if the file could not be opened.
    int loadMap(const std::string& path) {
        std::ifstream f(path);
        if (!f) return -1;
        byAddr_.clear(); byName_.clear();
        std::string line; int count = 0;
        while (std::getline(f, line)) {
            std::string name; long val;
            if (parseLdLine(line, name, val) || parseEqLine(line, name, val)) {
                uint16_t a = static_cast<uint16_t>(val & 0xFFFF);
                byAddr_[a] = name; byName_[name] = a; ++count;
            }
        }
        return count;
    }

    bool empty() const { return byAddr_.empty(); }
    std::size_t size() const { return byAddr_.size(); }
    const std::map<uint16_t, std::string>& all() const { return byAddr_; }

    // Exact symbol at `a` (nullptr if none).
    const std::string* nameAt(uint16_t a) const {
        auto it = byAddr_.find(a);
        return it == byAddr_.end() ? nullptr : &it->second;
    }
    // Nearest symbol at or below `a`, within `maxOff` bytes. Fills base+name.
    bool nearest(uint16_t a, uint16_t& base, std::string& name, uint16_t maxOff = 0x1000) const {
        if (byAddr_.empty()) return false;
        auto it = byAddr_.upper_bound(a);            // first > a
        if (it == byAddr_.begin()) return false;
        --it;                                        // last <= a
        if (static_cast<uint16_t>(a - it->first) > maxOff) return false;
        base = it->first; name = it->second; return true;
    }
    bool addrOf(const std::string& name, uint16_t& out) const {
        auto it = byName_.find(name);
        if (it == byName_.end()) return false;
        out = it->second; return true;
    }

private:
    std::map<uint16_t, std::string> byAddr_;
    std::map<std::string, uint16_t> byName_;

    static bool isNameStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$'; }
    static bool isNameChar(char c)  { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$'; }
    static int  hexv(char c) { return (c <= '9') ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10); }

    static bool parseNum(const std::string& s, std::size_t i, long& out) {
        if (i >= s.size()) return false;
        long v = 0;
        if (s[i] == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i += 2; std::size_t st = i;
            while (i < s.size() && std::isxdigit((unsigned char)s[i])) v = v * 16 + hexv(s[i++]);
            if (i == st) return false; out = v; return true;
        }
        if (std::isdigit((unsigned char)s[i])) {                 // decimal or leading-0 octal
            bool oct = (s[i] == '0');
            std::size_t st = i;
            if (oct) { while (i < s.size() && s[i] >= '0' && s[i] <= '7') v = v * 8 + (s[i++] - '0'); }
            else     { while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0'); }
            return i > st ? (out = v, true) : false;
        }
        return false;
    }

    // "   0x<hex>   <name>" with the name as the only trailing token (GNU-ld map).
    static bool parseLdLine(const std::string& s, std::string& name, long& val) {
        std::size_t i = 0;
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i + 1 >= s.size() || s[i] != '0' || (s[i + 1] != 'x' && s[i + 1] != 'X')) return false;
        i += 2; std::size_t hs = i; long v = 0;
        while (i < s.size() && std::isxdigit((unsigned char)s[i])) v = v * 16 + hexv(s[i++]);
        if (i == hs) return false;
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size() || !isNameStart(s[i])) return false;
        std::size_t ns = i;
        while (i < s.size() && isNameChar(s[i])) ++i;
        std::size_t j = i; while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
        if (j != s.size()) return false;                         // name must be the last token
        name = s.substr(ns, i - ns); val = v; return true;
    }

    // "<name> = <value>" (ignores any trailing text).
    static bool parseEqLine(const std::string& s, std::string& name, long& val) {
        std::size_t i = 0;
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size() || !isNameStart(s[i])) return false;
        std::size_t ns = i;
        while (i < s.size() && isNameChar(s[i])) ++i;
        std::string nm = s.substr(ns, i - ns);
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size() || s[i] != '=') return false;
        ++i; while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (!parseNum(s, i, val)) return false;
        name = nm; return true;
    }
};

} // namespace bk
