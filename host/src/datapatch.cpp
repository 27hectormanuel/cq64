// SPDX-License-Identifier: CC0-1.0
// Same-size byte patches to files inside the data pack.
//
// The game reads its data straight out of assets/ant.mp3 (a zip stored, uncompressed, inside the
// APK or cqdata.zip) with its own zip code, so a patched file cannot simply be dropped next to it.
// Instead each patch is located once at startup (outer zip -> ant.mp3 -> member, all stored),
// checked against the expected original bytes, and spliced into every read of that range.
//
// One file is generated instead: the button icon library. Every menu imports its button icons
// from "Shared/ControllerIcons.gfx", which Android maps to Mobile_InputIcons.gfx (touch icons
// only), so the menus' console frames (A/B/X/Y, d-pad) come up empty. A library holding both the
// mobile icons and Xbox360_ControllerIcons.gfx's is built from those two files at startup and
// spliced over Mobile_InputIcons.gfx (the engine preloads that file as a shared movie; a copy
// elsewhere would not get its textures), padded to its size.
#include "datapatch.h"

#include <zlib.h>

#include <deque>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_map>
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
std::deque<std::vector<u8>> g_generated;  // contents of generated files (stable addresses)
bool g_icons;
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

// ---- button icon library

u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
void put16(u8 *p, u16 v) { p[0] = v & 0xff, p[1] = v >> 8; }

struct Tag {
    u16 code;
    std::vector<u8> data;
};
struct Movie {
    u8 version = 0;
    std::vector<u8> header;  // frame rect, rate, count
    std::vector<Tag> tags;
};

// Parses a GFx movie ("GFX", or "CFX" = zlib-compressed after the 8-byte header).
bool parse_movie(const std::vector<u8> &file, Movie &m) {
    if (file.size() < 8 || (memcmp(file.data(), "CFX", 3) && memcmp(file.data(), "GFX", 3))) return false;
    m.version = file[3];
    u32 want = le32(file.data() + 4) - 8;
    std::vector<u8> body;
    if (file[0] == 'C') {
        // The game's compressed movies inflate to more than the header's length (zero padding).
        uLongf len = want + 65536;
        body.resize(len);
        if (uncompress(body.data(), &len, file.data() + 8, file.size() - 8) != Z_OK || len < want) return false;
        body.resize(want);
    } else {
        body.assign(file.begin() + 8, file.end());
    }
    if (body.empty()) return false;
    size_t p = ((5 + 4 * (body[0] >> 3) + 7) / 8) + 4;
    if (p > body.size()) return false;
    m.header.assign(body.begin(), body.begin() + p);
    while (p + 2 <= body.size()) {
        u16 cl = rd16(&body[p]);
        size_t n = cl & 0x3f, h = 2;
        if (n == 0x3f) {
            if (p + 6 > body.size()) return false;
            n = le32(&body[p + 2]), h = 6;
        }
        if (p + h + n > body.size()) return false;
        m.tags.push_back({(u16)(cl >> 6), std::vector<u8>(body.begin() + p + h, body.begin() + p + h + n)});
        p += h + n;
        if (cl >> 6 == 0) break;
    }
    return true;
}

std::vector<u8> write_movie(const Movie &m) {
    std::vector<u8> body = m.header;
    for (const Tag &t : m.tags) {
        size_t n = t.data.size();
        if (n < 0x3f) {
            body.push_back((t.code << 6 | n) & 0xff), body.push_back((t.code << 6 | n) >> 8);
        } else {
            u16 cl = t.code << 6 | 0x3f;
            body.push_back(cl & 0xff), body.push_back(cl >> 8);
            for (int i = 0; i < 4; i++) body.push_back(n >> (8 * i));
        }
        body.insert(body.end(), t.data.begin(), t.data.end());
    }
    uLongf clen = compressBound(body.size());
    std::vector<u8> out(8 + clen);
    memcpy(out.data(), "CFX", 3);
    out[3] = m.version;
    u32 total = body.size() + 8;
    for (int i = 0; i < 4; i++) out[4 + i] = total >> (8 * i);
    if (compress2(out.data() + 8, &clen, body.data(), body.size(), 9) != Z_OK) {
        LOGW("datapatch: icon library: compression failed");
        return {};
    }
    out.resize(8 + clen);
    return out;
}

constexpr u16 TAG_END = 0, TAG_SHOW_FRAME = 1, TAG_PLACE2 = 26, TAG_BG_COLOR = 9, TAG_SHAPE2 = 22, TAG_SHAPE3 = 32,
              TAG_SHAPE = 2, TAG_SPRITE = 39, TAG_EXPORT = 56, TAG_EXT_IMAGE = 1001;
bool has_id(u16 code) { return code == TAG_EXT_IMAGE || code == TAG_SHAPE || code == TAG_SPRITE; }

template <class F> void each_export(const Tag &t, F f) {
    size_t p = 2;
    for (u16 i = 0, n = rd16(t.data.data()); i < n && p + 2 < t.data.size(); i++) {
        u16 id = rd16(&t.data[p]);
        const char *name = (const char *)&t.data[p + 2];
        size_t len = strnlen(name, t.data.size() - p - 2);
        f(id, std::string(name, len));
        p += 2 + len + 1;
    }
}

// Adds the Xbox icons the mobile library lacks, as images (file names prefixed "X360_" so
// gamepad.cpp can find their textures), with ids renumbered. Menus import the icons as bitmaps
// to fill their own shapes. Xbox360_ControllerIcons exports BUTTON_A as a sprite around a 64x64
// image instead, which would leave those fills empty (GFx then crashes drawing them), so that
// image itself is exported under the sprite's name, declared 32x32 like the other icons.
std::vector<u8> merge_icons(const std::vector<u8> &mobile_file, const std::vector<u8> &xbox_file) {
    Movie mobile, xbox;
    if (!parse_movie(mobile_file, mobile) || !parse_movie(xbox_file, xbox)) {
        LOGW("datapatch: icon movies unreadable (%zu tags, %zu tags)", mobile.tags.size(), xbox.tags.size());
        return {};
    }
    u16 off = 0;
    std::unordered_map<std::string, bool> have;
    std::unordered_map<u16, bool> exported;
    for (const Tag &t : mobile.tags) {
        if (has_id(t.code) && t.data.size() >= 2) off = std::max<u16>(off, rd16(t.data.data()) + 1);
        if (t.code == TAG_EXPORT)
            each_export(t, [&](u16 id, const std::string &n) { have[n] = true, exported[id] = true; });
    }
    // The library must fit in Mobile_InputIcons.gfx's own place in the pack, so the mobile
    // movie's stage (a preview of the icons: unexported shapes, placements, background) goes.
    bool frame = false;
    std::vector<Tag> kept;
    for (Tag &t : mobile.tags) {
        u16 c = t.code;
        bool stage = c == TAG_PLACE2 || c == TAG_BG_COLOR ||
                     ((c == TAG_SHAPE || c == TAG_SHAPE2 || c == TAG_SHAPE3 || c == TAG_EXT_IMAGE) &&
                      t.data.size() >= 2 && !exported.count(rd16(t.data.data())));
        if (c == TAG_SHOW_FRAME && frame) stage = true;
        if (c == TAG_SHOW_FRAME) frame = true;
        if (!stage) kept.push_back(std::move(t));
    }
    mobile.tags = std::move(kept);

    std::unordered_map<u16, const Tag *> defs;
    for (const Tag &t : xbox.tags)
        if (has_id(t.code) && t.data.size() >= 2) defs[rd16(t.data.data())] = &t;
    auto is_image = [&](int id) { return defs.count(id) && defs[id]->code == TAG_EXT_IMAGE; };
    // The image a one-frame sprite wrapper shows: sprite -> PlaceObject2 (character at offset
    // 9) -> shape -> clipped bitmap fill (style 0x43 + image id).
    auto sprite_image = [&](u16 id) -> int {
        const Tag *t = defs.count(id) ? defs[id] : nullptr;
        if (!t || t->code != TAG_SPRITE || t->data.size() < 11) return -1;
        u16 sid = rd16(&t->data[9]);
        const Tag *shape = defs.count(sid) ? defs[sid] : nullptr;
        if (!shape || shape->code != TAG_SHAPE) return -1;
        for (size_t j = 2; j + 2 < shape->data.size(); j++)
            if (shape->data[j] == 0x43 && is_image(rd16(&shape->data[j + 1]))) return rd16(&shape->data[j + 1]);
        return -1;
    };
    std::vector<std::pair<u16, std::string>> exports;  // image id, export name
    std::unordered_map<u16, bool> resize;
    for (const Tag &t : xbox.tags)
        if (t.code == TAG_EXPORT)
            each_export(t, [&](u16 id, const std::string &n) {
                if (have.count(n)) return;
                if (is_image(id)) {
                    // Images wrapped by a sprite (BUTTON_Aorig) are exported under its name instead.
                    if (n.size() > 4 && n.compare(n.size() - 4, 4, "orig") == 0) return;
                    exports.push_back({id, n});
                } else if (int img = sprite_image(id); img >= 0) {
                    exports.push_back({(u16)img, n});
                    resize[img] = true;
                }
            });

    std::vector<Tag> added;
    for (auto &[id, name] : exports) {
        Tag t = *defs[id];
        put16(t.data.data(), id + off);
        if (resize.count(id)) put16(&t.data[4], 32), put16(&t.data[6], 32);
        // id, format, width, height, export name (u8 length + chars), file name (same)
        size_t p = 8;
        if (p >= t.data.size() || p + 1 + t.data[p] >= t.data.size()) {
            LOGW("datapatch: icon library: bad image tag");
            return {};
        }
        p += 1 + t.data[p];
        std::string file((const char *)&t.data[p + 1], std::min<size_t>(t.data[p], t.data.size() - p - 1));
        file = "X360_" + file;
        t.data.resize(p);
        t.data.push_back(file.size());
        t.data.insert(t.data.end(), file.begin(), file.end());
        added.push_back(std::move(t));
    }
    for (auto &[id, name] : exports) {
        Tag t{TAG_EXPORT, {}};
        t.data.resize(4);
        put16(&t.data[0], 1), put16(&t.data[2], id + off);
        t.data.insert(t.data.end(), name.begin(), name.end());
        t.data.push_back(0);
        added.push_back(std::move(t));
    }
    // Into the (only) frame: GFx stops reading at the header's frame count.
    auto end = std::find_if(mobile.tags.begin(), mobile.tags.end(),
                            [](const Tag &t) { return t.code == TAG_SHOW_FRAME || t.code == TAG_END; });
    mobile.tags.insert(end, added.begin(), added.end());
    LOGI("datapatch: icon library: %zu Xbox icons added", exports.size());
    return write_movie(mobile);
}

bool read_member(int fd, s64 base, s64 size, const char *name, std::vector<u8> &out, s64 *pos = nullptr) {
    s64 len = 0, m = find_stored(fd, base, size, name, &len);
    if (m < 0 || len <= 0 || len > (1 << 20)) return false;
    out.resize(len);
    if (pos) *pos = m;
    return pread_all(fd, out.data(), len, m);
}

void build_icons(int fd, s64 base, s64 size) {
    std::vector<u8> mobile, xbox;
    s64 slot_pos = 0;
    if (!read_member(fd, base, size, ICON_SLOT, mobile, &slot_pos) ||
        !read_member(fd, base, size, "Data/UI/Xbox360_ControllerIcons/Opt/Xbox360_ControllerIcons.gfx", xbox)) {
        LOGW("datapatch: icon files not found; controller icons stay touch-only");
        return;
    }
    std::vector<u8> merged = merge_icons(mobile, xbox);
    if (merged.empty() || merged.size() > mobile.size()) {
        LOGW("datapatch: icon library not built (%zu bytes, room for %zu)", merged.size(), mobile.size());
        return;
    }
    merged.resize(mobile.size(), 0);  // GFx stops at the compressed stream's end
    g_generated.push_back(std::move(merged));
    g_located.push_back({slot_pos, &g_generated.back()});
    g_icons = true;
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
    build_icons(fd, inner, inner_size);
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

namespace datapatch {
bool icon_library_ready() { return g_icons; }
}  // namespace datapatch
