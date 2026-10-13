// SPDX-License-Identifier: GPL-3.0-or-later
// Same-size byte patches to files inside the data pack.
//
// The game reads its data straight out of assets/ant.mp3 (a zip stored, uncompressed, inside the
// APK or cqdata.zip) with its own zip code, so a patched file cannot simply be dropped next to it.
// Instead each patch is located once at startup (outer zip -> ant.mp3 -> member, all stored),
// checked against the expected original bytes, and spliced into every read of that range.
#include "datapatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace datapatch {
namespace {
struct Patch {
    const char *member;  // path inside ant.mp3
    u32 offset;          // within the member
    std::vector<u8> old_bytes, new_bytes;
};

const Patch PATCHES[] = {
    // FrontEnd.gfx, MenuItem "Focus" frame: the selection box's colour transform tints its
    // (dark) texture red (green/blue x0, red +200). Make it white instead, as on PC: colour x0 +255.
    {"Data/UI/FrontEnd/Opt/FrontEnd.gfx", 0x892,
     {0xe9, 0x00, 0x00, 0x00, 0x03, 0x98, 0xc8, 0x00, 0x00, 0x00, 0x00},
     {0xe8, 0x00, 0x00, 0x00, 0x03, 0x98, 0xff, 0x3f, 0xcf, 0xf0, 0x00}},
    // Pause.gfx: the same selection box in the in-game menu.
    {"Data/UI/Pause/Opt/Pause.gfx", 0xc04,
     {0xe9, 0x00, 0x00, 0x00, 0x03, 0x98, 0xc8, 0x00, 0x00, 0x00, 0x00},
     {0xe8, 0x00, 0x00, 0x00, 0x03, 0x98, 0xff, 0x3f, 0xcf, 0xf0, 0x00}},
};

struct Located {
    s64 pos;  // absolute offset in the pack file
    const std::vector<u8> *bytes;
};
std::vector<Located> g_located;
dev_t g_dev;
ino_t g_ino;

bool pread_all(int fd, void *buf, size_t n, s64 pos) { return pread(fd, buf, n, pos) == (ssize_t)n; }
u16 le16(const u8 *p) { return p[0] | p[1] << 8; }
u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// Finds `name` in the zip occupying [base, base + size) of fd; returns the absolute offset of its
// data (and its size in *len) if it is stored uncompressed, else -1.
s64 find_stored(int fd, s64 base, s64 size, const char *name, s64 *len = nullptr) {
    u8 tail[65557];
    s64 tlen = size < (s64)sizeof tail ? size : (s64)sizeof tail;
    if (!pread_all(fd, tail, tlen, base + size - tlen)) return -1;
    s64 eocd = -1;
    for (s64 i = tlen - 22; i >= 0; i--)
        if (le32(tail + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return -1;
    u32 count = le16(tail + eocd + 10), cd_size = le32(tail + eocd + 12), cd_off = le32(tail + eocd + 16);
    std::vector<u8> cd(cd_size);
    if (!pread_all(fd, cd.data(), cd_size, base + cd_off)) return -1;
    size_t nlen = strlen(name);
    for (u32 i = 0, p = 0; i < count && p + 46 <= cd_size; i++) {
        const u8 *e = cd.data() + p;
        if (le32(e) != 0x02014b50) return -1;
        u16 n = le16(e + 28), x = le16(e + 30), c = le16(e + 32);
        if (n == nlen && memcmp(e + 46, name, n) == 0) {
            if (le16(e + 10) != 0) return -1;  // compressed
            if (len) *len = le32(e + 24);
            u8 lh[30];
            s64 lho = base + le32(e + 42);
            if (!pread_all(fd, lh, 30, lho) || le32(lh) != 0x04034b50) return -1;
            return lho + 30 + le16(lh + 26) + le16(lh + 28);
        }
        p += 46 + n + x + c;
    }
    return -1;
}
}  // namespace

void init(const std::string &pack_path) {
    int fd = open(pack_path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        LOGW("datapatch: cannot open %s", pack_path.c_str());
        if (fd >= 0) close(fd);
        return;
    }
    s64 inner_size = 0;
    s64 inner = find_stored(fd, 0, st.st_size, "assets/ant.mp3", &inner_size);
    if (inner < 0 || inner_size <= 0) {
        LOGW("datapatch: assets/ant.mp3 not found stored in %s", pack_path.c_str());
        close(fd);
        return;
    }
    for (const Patch &p : PATCHES) {
        s64 m = find_stored(fd, inner, inner_size, p.member);
        std::vector<u8> cur(p.old_bytes.size());
        if (m < 0 || !pread_all(fd, cur.data(), cur.size(), m + p.offset) || cur != p.old_bytes) {
            LOGW("datapatch: %s not found or unexpected contents; not patched", p.member);
            continue;
        }
        g_located.push_back({m + p.offset, &p.new_bytes});
        LOGI("datapatch: %s +0x%x", p.member, p.offset);
    }
    g_dev = st.st_dev;
    g_ino = st.st_ino;
    close(fd);
}

void apply(int fd, s64 pos, void *buf, s64 n) {
    if (g_located.empty() || n <= 0 || pos < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_ino != g_ino || st.st_dev != g_dev) return;
    for (const Located &l : g_located) {
        s64 len = (s64)l.bytes->size();
        s64 lo = l.pos > pos ? l.pos : pos, hi = l.pos + len < pos + n ? l.pos + len : pos + n;
        if (lo < hi) memcpy((u8 *)buf + (lo - pos), l.bytes->data() + (lo - l.pos), hi - lo);
    }
}
}  // namespace datapatch
