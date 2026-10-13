// SPDX-License-Identifier: GPL-3.0-or-later
#include "loader.h"
#include "cpu.h"

#include <elf.h>
#include <algorithm>
#include <mutex>

namespace {
std::vector<Module *> g_modules;
std::mutex g_lock;
std::unordered_map<std::string, u32> g_thunk_addrs;  // import name -> thunk address

std::unordered_map<std::string, ThunkFn> &registry() {
    static std::unordered_map<std::string, ThunkFn> r;
    return r;
}

// Imports with no host implementation get a thunk that reports the name when called.
void missing_import(GuestThread &t);
}  // namespace

namespace thunks {
void register_thunk(const char *name, ThunkFn fn) { registry()[name] = fn; }
ThunkFn find(const char *name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}
}  // namespace thunks

namespace {
u32 thunk_for(const char *name, bool create_missing) {
    auto it = g_thunk_addrs.find(name);
    if (it != g_thunk_addrs.end()) return it->second;
    ThunkFn fn = thunks::find(name);
    if (!fn) {
        if (!create_missing) return 0;
        fn = missing_import;
    }
    u32 a = cpu::add_thunk(name, fn);
    g_thunk_addrs[name] = a;
    return a;
}

void missing_import(GuestThread &t) {
    u32 lr = t.r(14);
    fatal("unimplemented import called: %s (from %s)", cpu::thunk_name(t.cur_svc),
          loader::symbolize(lr).c_str());
}

u32 lookup_module(Module *m, const char *name) {
    auto it = m->exports.find(name);
    return it == m->exports.end() ? 0 : it->second;
}

// Symbol resolution order mirrors bionic's for dlopen'd libraries: the library itself,
// its DT_NEEDED guest libraries, the guest runtime, then host thunks.
u32 resolve(Module *self, const char *name, bool weak, bool is_func) {
    // Host overrides ("override:<name>") win over every guest definition, except inside the
    // defining library itself, so an override can still call the original.
    std::string ov = std::string("override:") + name;
    if (thunks::find(ov.c_str()) && !lookup_module(self, name)) {
        auto it = g_thunk_addrs.find(ov);
        if (it != g_thunk_addrs.end()) return it->second;
        return g_thunk_addrs[ov] = cpu::add_thunk(ov.c_str(), thunks::find(ov.c_str()));
    }
    if (u32 a = lookup_module(self, name)) return a;
    for (auto &n : self->needed)
        for (Module *m : g_modules)
            if (m->name == n)
                if (u32 a = lookup_module(m, name)) return a;
    for (Module *m : g_modules)
        if (m->name == "libguestrt.so")
            if (u32 a = lookup_module(m, name)) return a;
    if (u32 a = thunk_for(name, false)) return a;
    if (weak) return 0;
    if (!is_func) {
        u32 a = mem::halloc(4096, 16);
        LOGW("%s: unresolved data import %s, using zeroed block", self->name.c_str(), name);
        return a;
    }
    return thunk_for(name, true);
}
}  // namespace

namespace loader {

Module *load(const std::string &soname, const std::vector<u8> &f) {
    auto *eh = (const Elf32_Ehdr *)f.data();
    if (f.size() < sizeof(Elf32_Ehdr) || memcmp(eh->e_ident, ELFMAG, 4) || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_machine != EM_ARM)
        fatal("%s: not an ARM32 ELF", soname.c_str());
    auto *ph = (const Elf32_Phdr *)(f.data() + eh->e_phoff);

    u32 lo = 0xffffffff, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        lo = std::min(lo, ph[i].p_vaddr & ~0xfffu);
        hi = std::max(hi, ph[i].p_vaddr + ph[i].p_memsz);
    }
    u32 span = ((hi - lo) + 0xffff) & ~0xffffu;
    u32 base = mem::map(span) - lo;
    if (base + lo == 0) fatal("%s: cannot map %u bytes", soname.c_str(), span);

    auto *m = new Module;
    m->name = soname;
    m->base = base;
    m->size = span;
    u32 dyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const auto &p = ph[i];
        if (p.p_type == PT_LOAD) {
            memcpy(g2h(base + p.p_vaddr), f.data() + p.p_offset, p.p_filesz);
            // bss is already zero (fresh mapping)
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = base + p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            m->exidx = base + p.p_vaddr;
            m->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dyn) fatal("%s: no PT_DYNAMIC", soname.c_str());

    u32 strtab = 0, symtab = 0, hash = 0, gnuhash = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    u32 init_array = 0, init_arraysz = 0;
    std::vector<u32> needed_off;
    for (u32 d = dyn;; d += 8) {
        s32 tag = rd<s32>(d);
        u32 val = rd<u32>(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_STRTAB: strtab = base + val; break;
        case DT_SYMTAB: symtab = base + val; break;
        case DT_HASH: hash = base + val; break;
        case DT_GNU_HASH: gnuhash = base + val; break;
        case DT_REL: rel = base + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT: m->init = base + val; break;
        case DT_FINI: m->fini = base + val; break;
        case DT_INIT_ARRAY: init_array = base + val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = val; break;
        case DT_NEEDED: needed_off.push_back(val); break;
        case DT_RELA: fatal("%s: RELA relocations unsupported", soname.c_str());
        default: break;
        }
    }
    auto str = [&](u32 off) { return (const char *)g2h(strtab + off); };
    for (u32 off : needed_off) m->needed.push_back(str(off));

    u32 nsyms = 0;
    if (hash) {
        nsyms = rd<u32>(hash + 4);  // nchain
    } else if (gnuhash) {
        u32 nbuckets = rd<u32>(gnuhash), symoffset = rd<u32>(gnuhash + 4), bloom = rd<u32>(gnuhash + 8);
        u32 buckets = gnuhash + 16 + bloom * 4;
        u32 chains = buckets + nbuckets * 4;
        u32 maxb = 0;
        for (u32 i = 0; i < nbuckets; i++) maxb = std::max(maxb, rd<u32>(buckets + i * 4));
        if (maxb >= symoffset) {
            while (!(rd<u32>(chains + (maxb - symoffset) * 4) & 1)) maxb++;
            nsyms = maxb + 1;
        } else nsyms = symoffset;
    }
    for (u32 i = 1; i < nsyms; i++) {
        Elf32_Sym s = rd<Elf32_Sym>(symtab + i * sizeof(Elf32_Sym));
        if (s.st_shndx == SHN_UNDEF) continue;
        int bind = ELF32_ST_BIND(s.st_info), type = ELF32_ST_TYPE(s.st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        u32 addr = base + s.st_value;
        m->exports.emplace(str(s.st_name), addr);
        if (type == STT_FUNC) m->funcs.emplace_back(addr & ~1u, str(s.st_name));
    }
    std::sort(m->funcs.begin(), m->funcs.end());

    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_modules.push_back(m);
    }

    auto do_rel = [&](u32 table, u32 size) {
        for (u32 r = table; r < table + size; r += 8) {
            u32 off = rd<u32>(r), info = rd<u32>(r + 4);
            u32 type = ELF32_R_TYPE(info), symi = ELF32_R_SYM(info);
            u32 where = base + off;
            u32 S = 0;
            if (symi) {
                Elf32_Sym s = rd<Elf32_Sym>(symtab + symi * sizeof(Elf32_Sym));
                const char *name = str(s.st_name);
                if (s.st_shndx != SHN_UNDEF && ELF32_ST_BIND(s.st_info) == STB_LOCAL) S = base + s.st_value;
                else {
                    std::lock_guard<std::mutex> lk(g_lock);
                    S = resolve(m, name, ELF32_ST_BIND(s.st_info) == STB_WEAK,
                                ELF32_ST_TYPE(s.st_info) != STT_OBJECT);
                }
            }
            switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: wr<u32>(where, rd<u32>(where) + base); break;
            case R_ARM_ABS32: wr<u32>(where, rd<u32>(where) + S); break;
            case R_ARM_REL32: wr<u32>(where, rd<u32>(where) + S - where); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: wr<u32>(where, S); break;
            default: fatal("%s: unsupported relocation type %u", soname.c_str(), type);
            }
        }
    };
    if (rel) do_rel(rel, relsz);
    if (jmprel) do_rel(jmprel, pltrelsz);

    for (u32 i = 0; i < init_arraysz / 4; i++) m->init_array.push_back(rd<u32>(init_array + i * 4));
    LOGI("loaded %s at 0x%08x (%u exports)", soname.c_str(), base + lo, (unsigned)m->exports.size());
    return m;
}

void run_init(Module *m) {
    if (m->init) cpu::call(m->init, {});
    for (u32 fn : m->init_array)
        if (fn && fn != 0xffffffff) cpu::call(fn, {});
    LOGI("initialized %s", m->name.c_str());
}

Module *find(const std::string &soname) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (Module *m : g_modules)
        if (m->name == soname) return m;
    return nullptr;
}

Module *find_by_addr(u32 a) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (Module *m : g_modules)
        if (a >= m->base && a < m->base + m->size) return m;
    return nullptr;
}

u32 lookup_in(Module *m, const char *name) { return lookup_module(m, name); }

u32 lookup(const char *name, bool create_thunk) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (Module *m : g_modules)
        if (u32 a = lookup_module(m, name)) return a;
    (void)create_thunk;
    return thunk_for(name, false);
}

std::string symbolize(u32 a) {
    char buf[256];
    if (a >= GUEST_THUNK_BASE && a < GUEST_THUNK_BASE + GUEST_THUNK_MAX * 4) {
        snprintf(buf, sizeof buf, "0x%08x <thunk %s>", a, cpu::thunk_name((a - GUEST_THUNK_BASE) / 4));
        return buf;
    }
    Module *m = find_by_addr(a);
    if (!m) {
        snprintf(buf, sizeof buf, "0x%08x", a);
        return buf;
    }
    u32 ca = a & ~1u;
    auto it = std::upper_bound(m->funcs.begin(), m->funcs.end(), std::make_pair(ca, std::string("\xff")));
    if (it != m->funcs.begin()) {
        --it;
        snprintf(buf, sizeof buf, "0x%08x <%s+0x%x> (%s+0x%x)", a, it->second.c_str(), ca - it->first,
                 m->name.c_str(), a - m->base);
    } else {
        snprintf(buf, sizeof buf, "0x%08x (%s+0x%x)", a, m->name.c_str(), a - m->base);
    }
    return buf;
}

const std::vector<Module *> &modules() { return g_modules; }
}  // namespace loader
