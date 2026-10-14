// SPDX-License-Identifier: CC0-1.0
// Argument hints for the debug console: what each Lua function expects, worked out from the
// running game rather than stored here.
//
// Engine (C) functions: Double Fine's engine registers each with
// LuaFunctionRegistry::AddFunction(fn, name) from static initializers. The game library's code is
// scanned for those calls (the name and function are PC-relative constants), then each function's
// code up to the next symbol for its argument checks: Lua_CheckNumberOfArguments(L, min, max)
// and typed reads such as Lua_ToBooleanStrict(L, index), whose index is a constant.
// Script functions: the data pack's Lua sources give parameter lists ("function t.f(a, b)") and
// calls with literal arguments, the best examples of what to pass.
#include "luahints.h"

#include "datapatch.h"
#include "game.h"
#include "loader.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <unordered_map>

namespace luahints {
namespace {

struct Binding {
    std::string name;
    u32 fn;
};

// Calls to LuaFunctionRegistry::AddFunction in [start, end): ARM code, constants built as
// "ldr rX, [pc, #n]" + "add rY, rA, rB" (one operand pc or a register holding such a value).
std::vector<Binding> find_bindings(u32 start, u32 end, u32 add_function) {
    std::vector<Binding> out;
    u32 regs[16];
    u16 known = 0;  // bit r: regs[r] is a known constant
    auto set = [&](int r, u32 v) { regs[r] = v, known |= 1 << r; };
    auto val = [&](int r, u32 pc, u32 &v) {
        if (r == 15) return v = pc + 8, true;
        return (known >> r & 1) ? (v = regs[r], true) : false;
    };
    for (u32 a = start; a + 4 <= end; a += 4) {
        u32 x = rd<u32>(a);
        if ((x & 0x0fff0000) == 0x092d0000) {  // push: a new function
            known = 0;
            continue;
        }
        if (x >> 28 != 0xe) continue;
        int rd_ = (x >> 12) & 15;
        if ((x & 0x0f7f0000) == 0x051f0000) {  // ldr rd, [pc, #+-imm]
            u32 imm = x & 0xfff, lit = a + 8 + ((x & 0x800000) ? imm : -imm);
            if (lit >= start && lit + 4 <= end) set(rd_, rd<u32>(lit));
            else known &= ~(1 << rd_);
        } else if ((x & 0x0fe00ff0) == 0x00800000) {  // add rd, rn, rm
            u32 n, m;
            if (val((x >> 16) & 15, a, n) && val(x & 15, a, m)) set(rd_, n + m);
            else known &= ~(1 << rd_);
        } else if ((x & 0x0fef0ff0) == 0x01a00000) {  // mov rd, rm
            u32 m;
            if (val(x & 15, a, m)) set(rd_, m);
            else known &= ~(1 << rd_);
        } else if ((x & 0x0f000000) == 0x0b000000) {  // bl
            s32 off = (s32)(x << 8) >> 8;
            if (a + 8 + off * 4 == add_function && (known & 6) == 6) {
                const char *name = (const char *)g2h(regs[2]);
                if (regs[2] && name[0]) out.push_back({name, regs[1]});
            }
            known &= ~0x100f;  // r0-r3, r12
        } else if ((x & 0x0c000000) == 0) {  // other data processing: rd changes
            known &= ~(1 << rd_);
        }
    }
    return out;
}

// Type names for the argument readers.
const std::pair<const char *, const char *> READERS[] = {
    {"_Z18Lua_ToNumberStrictP9lua_Statei", "number"},
    {"_Z19Lua_ToBooleanStrictP9lua_Statei", "bool"},
    {"_Z18Lua_ToStringStrictP9lua_Statei", "string"},
    {"_Z18Lua_ToResourceNameP9lua_Statei", "resource"},
    {"_Z10Lua_ToEnumP9lua_Statei", "enum"},
    {"_Z13Lua_ToMissionP9lua_Statei", "mission"},
    {"_Z12Lua_ToDomainP9lua_Statei", "domain"},
    {"_Z16Lua_ToRTTIObjectP9lua_StateiP9RTTIClassb", "object"},
    {"_Z25Lua_ToComponentFromEntityP9lua_StateiP9RTTIClassb", "entity"},
    {"_Z15Lua_ToPrototypeP9lua_Stateib", "prototype"},
    {"_Z25Lua_ToLightUserdataStrictP9lua_Statei", "object"},
    {"_Z10Lua_ToVec2P9lua_Statei", "vec2"},
    {"_Z10Lua_ToVec3P9lua_Statei", "vec3"},
    {"lua_tonumber", "number"},
    {"lua_toboolean", "bool"},
    {"lua_tolstring", "string"},
    {"lua_touserdata", "object"},
    {"lua_type", "any"},
};

std::string engine_hints(Module *m) {
    u32 add_function = loader::lookup_in(m, "_ZN19LuaFunctionRegistry11AddFunctionEPFiP9lua_StateEPKc");
    u32 check_args = loader::lookup_in(m, "_Z26Lua_CheckNumberOfArgumentsP9lua_Stateii");
    if (!add_function) return "";
    std::unordered_map<u32, const char *> readers;
    for (const auto &r : READERS)
        if (u32 a = loader::lookup_in(m, r.first)) readers[a & ~1u] = r.second;
    std::vector<Binding> binds = find_bindings(m->base, m->base + m->size, add_function & ~1u);
    // Function bounds: the next exported function or binding.
    std::vector<u32> starts;
    for (const auto &f : m->funcs) starts.push_back(f.first & ~1u);
    for (const auto &b : binds) starts.push_back(b.fn & ~1u);
    std::sort(starts.begin(), starts.end());
    std::string out;
    for (const Binding &b : binds) {
        u32 fn = b.fn & ~1u;
        if (b.fn & 1) continue;  // Thumb: not scanned
        auto it = std::upper_bound(starts.begin(), starts.end(), fn);
        u32 end = std::min<u32>(it == starts.end() ? fn + 0x800 : *it, fn + 0x800);
        u32 imm[16];
        u16 known = 0;
        int lo = -1, hi = -1;
        std::map<int, const char *> args;
        for (u32 a = fn; a + 4 <= end; a += 4) {
            u32 x = rd<u32>(a);
            if (x >> 28 != 0xe) continue;
            if ((x & 0x0ff00000) == 0x03a00000) {  // mov rd, #imm (rotated)
                u32 rot = ((x >> 8) & 15) * 2, v = x & 0xff;
                imm[(x >> 12) & 15] = rot ? (v >> rot) | (v << (32 - rot)) : v;
                known |= 1 << ((x >> 12) & 15);
            } else if ((x & 0x0f000000) == 0x0b000000) {  // bl
                s32 off = (s32)(x << 8) >> 8;
                u32 t = a + 8 + off * 4;
                if (t == (check_args & ~1u)) {
                    if (known & 2) lo = (int)imm[1];
                    if (known & 4) hi = (int)imm[2];
                } else if (auto r = readers.find(t); r != readers.end() && (known & 2) && imm[1] >= 1 &&
                                                     imm[1] <= 12) {
                    args.emplace((int)imm[1], r->second);
                }
                known = 0;
            }
        }
        if (lo < 0 && args.empty()) continue;
        out += "C\t" + b.name + "\t" + std::to_string(lo) + "\t" + std::to_string(hi) + "\t";
        int next = 1;
        for (const auto &[i, type] : args) {
            for (; next < i; next++) out += "any,";  // read some other way
            out += type;
            out += ',';
            next = i + 1;
        }
        if (out.back() == ',') out.pop_back();
        out += '\n';
    }
    return out;
}

// ---- scripts

bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// "table.name" ending just before `end` (an index into s), or "".
std::string dotted_name_before(const std::string &s, size_t end) {
    size_t e = end;
    while (e > 0 && isspace((unsigned char)s[e - 1])) e--;
    size_t b = e;
    while (b > 0 && (ident_char(s[b - 1]) || s[b - 1] == '.' || s[b - 1] == ':')) b--;
    std::string n = s.substr(b, e - b);
    size_t dot = n.find_first_of(".:");
    if (dot == std::string::npos || dot == 0 || n.find_first_of(".:", dot + 1) != std::string::npos) return "";
    if (!isalpha((unsigned char)n[0]) && n[0] != '_') return "";
    if (n[dot] == ':') n[dot] = '.';
    return n;
}

// The text inside the parentheses opening at s[open], or false if unbalanced / too long.
bool paren_text(const std::string &s, size_t open, std::string &inner, size_t &close) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < s.size() && i < open + 200; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0) {
            inner = s.substr(open + 1, i - open - 1);
            close = i;
            return true;
        } else if (c == '\n') {
            return false;
        }
    }
    return false;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// Whether every argument is a literal: number, string, true / false / nil, RESOURCE("...").
bool literal_args(const std::string &args) {
    std::vector<std::string> parts;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (size_t i = 0; i < args.size(); i++) {
        char c = args[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(' || c == '{') {
            depth++;
        } else if (c == ')' || c == '}') {
            depth--;
        } else if (c == ',' && depth == 0) {
            parts.push_back(trim(cur)), cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(trim(cur));
    for (const std::string &p : parts) {
        if (p.empty()) return false;
        if (p == "true" || p == "false" || p == "nil") continue;
        if ((p[0] == '\'' || p[0] == '"') && p.size() >= 2 && p.back() == p[0] &&
            p.find(p[0], 1) == p.size() - 1)
            continue;
        if (p.rfind("RESOURCE(", 0) == 0 || p.rfind("RESOURCE_NOCLUMP(", 0) == 0) {
            size_t q = p.find_first_of("'\"");
            if (q != std::string::npos && p.back() == ')') continue;
            return false;
        }
        char *end;
        strtod(p.c_str(), &end);
        if (*end == 0) continue;
        return false;
    }
    return true;
}

std::string script_hints() {
    std::map<std::string, std::string> params;
    std::map<std::string, std::set<std::string>> examples;
    for (const auto &[path, src] : datapatch::lua_scripts()) {
        // Definitions: "function t.f(params)" and "t.f = function(params)".
        for (size_t p = src.find("function"); p != std::string::npos; p = src.find("function", p + 8)) {
            if ((p > 0 && ident_char(src[p - 1])) || (p + 8 < src.size() && ident_char(src[p + 8]))) continue;
            size_t open = src.find('(', p + 8);
            if (open == std::string::npos || open > p + 120) continue;
            std::string inner;
            size_t close;
            if (!paren_text(src, open, inner, close)) continue;
            std::string head = trim(src.substr(p + 8, open - p - 8)), name;
            if (!head.empty()) {
                name = dotted_name_before(head, head.size());
            } else {  // t.f = function(...)
                size_t eq = src.rfind('=', p);
                if (eq != std::string::npos && p - eq < 4 && trim(src.substr(eq + 1, p - eq - 1)).empty())
                    name = dotted_name_before(src, eq);
            }
            if (!name.empty()) params[name] = trim(inner);
        }
        // Calls with literal arguments: "t.f(...)".
        for (size_t open = src.find('('); open != std::string::npos; open = src.find('(', open + 1)) {
            std::string name = dotted_name_before(src, open);
            if (name.empty()) continue;
            size_t b = open;
            while (b > 0 && isspace((unsigned char)src[b - 1])) b--;
            b -= name.size();
            if (b >= 9 && src.compare(b - 9, 9, "function ") == 0) continue;  // a definition
            std::string inner;
            size_t close;
            if (!paren_text(src, open, inner, close)) continue;
            inner = trim(inner);
            auto &ex = examples[name];
            if (!inner.empty() && ex.size() < 3 && literal_args(inner)) ex.insert(inner);
        }
    }
    std::string out;
    for (const auto &[n, p] : params) out += "S\t" + n + "\t" + p + "\n";
    for (const auto &[n, ex] : examples)
        for (const std::string &e : ex) {
            std::string one = e;
            std::replace(one.begin(), one.end(), '\t', ' ');
            std::replace(one.begin(), one.end(), '\n', ' ');
            out += "E\t" + n + "\t" + one + "\n";
        }
    return out;
}
}  // namespace

const std::string &get() {
    static const std::string hints = [] {
        Module *m = loader::find(game::kLibs.back().soname);
        if (!m) return std::string();
        std::string h = engine_hints(m);
        if (h.empty()) return h;  // not Double Fine's engine
        h += script_hints();
        for (char &c : h)  // NewStringUTF needs valid (modified) UTF-8; scripts may hold Latin-1
            if ((unsigned char)c >= 0x80) c = '?';
        LOGI("luahints: %zu bytes of argument hints", h.size());
        return h;
    }();
    return hints;
}
}  // namespace luahints
