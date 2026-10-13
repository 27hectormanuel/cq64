// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"
#include "cpu.h"

#include <string>
#include <unordered_map>
#include <vector>

struct Module {
    std::string name;  // soname, e.g. "libCq.so"
    u32 base = 0, size = 0;
    u32 exidx = 0, exidx_count = 0;
    u32 init = 0, fini = 0;
    std::vector<u32> init_array;
    std::vector<std::string> needed;
    std::unordered_map<std::string, u32> exports;  // defined dynamic symbols -> guest address
    std::vector<std::pair<u32, std::string>> funcs;  // sorted, for symbolization
};

namespace loader {
// Maps, relocates and registers an ARM32 ELF shared object. Does not run initializers.
Module *load(const std::string &soname, const std::vector<u8> &file);
void run_init(Module *m);
Module *find(const std::string &soname);
Module *find_by_addr(u32 addr);
// Global lookup (guest modules in load order, then host thunks). Returns 0 if unknown.
u32 lookup(const char *name, bool create_thunk = true);
u32 lookup_in(Module *m, const char *name);
std::string symbolize(u32 addr);
const std::vector<Module *> &modules();
}  // namespace loader

// Host-implemented imports, registered by name.
namespace thunks {
void register_thunk(const char *name, ThunkFn fn);
ThunkFn find(const char *name);
void init_all();  // called once at startup, after mem/cpu init
}  // namespace thunks
