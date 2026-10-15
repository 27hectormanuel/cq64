// SPDX-License-Identifier: CC0-1.0
// OpenGL ES 1.x / 2.0 imports. Arguments are mostly scalars; pointer arguments are guest
// addresses, except vertex/index "pointers" that are buffer offsets while a VBO is bound.
#include "abi.h"
#include "game.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
GLuint g_array_buffer = 0, g_element_buffer = 0;

const void *client_ptr(u32 p, GLuint bound) {
    return bound ? (const void *)(uintptr_t)p : g2h(p);
}

void t_glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) g_array_buffer = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
    glBindBuffer(target, b);
}
THUNK(glBindBuffer, t_glBindBuffer);
void t_glDeleteBuffers(GLsizei n, G<const GLuint> b) {
    for (GLsizei i = 0; i < n; i++) {
        if (b.get()[i] == g_array_buffer) g_array_buffer = 0;
        if (b.get()[i] == g_element_buffer) g_element_buffer = 0;
    }
    glDeleteBuffers(n, b);
}
THUNK(glDeleteBuffers, t_glDeleteBuffers);

void t_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, u32 p) {
    glVertexAttribPointer(i, size, type, norm, stride, client_ptr(p, g_array_buffer));
}
THUNK(glVertexAttribPointer, t_glVertexAttribPointer);
void t_glVertexPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glVertexPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glVertexPointer, t_glVertexPointer);
void t_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glTexCoordPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glTexCoordPointer, t_glTexCoordPointer);
void t_glColorPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glColorPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glColorPointer, t_glColorPointer);
void t_glNormalPointer(GLenum type, GLsizei stride, u32 p) {
    glNormalPointer(type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glNormalPointer, t_glNormalPointer);
// Debug switch skip_shader=<text>[,<text>...] (game::debug_setting): draws whose shaders' names
// (the "// Name" line the game's shaders start with) contain one of these are skipped, to find
// which effect draws something wrong.
const std::string &skip_list() {
    static const std::string list = game::debug_setting("skip_shader");
    return list;
}
std::mutex g_names_lock;
std::unordered_map<GLuint, std::string> g_shader_names, g_program_names;
bool g_skip_draws = false;
bool skip_program(GLuint prog) {
    const std::string &list = skip_list();
    if (list.empty()) return false;
    std::lock_guard<std::mutex> lk(g_names_lock);
    const std::string &names = g_program_names[prog];
    for (size_t a = 0; a < list.size();) {
        size_t b = list.find(',', a);
        if (b == std::string::npos) b = list.size();
        if (b > a && names.find(list.substr(a, b - a)) != std::string::npos) return true;
        a = b + 1;
    }
    return false;
}
void t_glDrawElements(GLenum mode, GLsizei count, GLenum type, u32 p) {
    if (g_skip_draws) return;
    glDrawElements(mode, count, type, client_ptr(p, g_element_buffer));
}
THUNK(glDrawElements, t_glDrawElements);

std::mutex g_str_lock;
std::unordered_map<GLenum, u32> g_strings;
u32 t_glGetString(GLenum name) {
    std::lock_guard<std::mutex> lk(g_str_lock);
    auto it = g_strings.find(name);
    if (it != g_strings.end()) return it->second;
    const char *s = (const char *)glGetString(name);
    if (!s) return 0;
    LOGI("glGetString(0x%x) = %s", name, s);
    std::string v = s;
#ifdef _WIN32
    // Extensions the game checks for (its GL wrapper's GLUE_* flags) that ANGLE offers but the
    // Mali GPU of the reference phone (Pixel 8 Pro, Mali-G715) doesn't. Each switches the game to
    // a drawing path it never takes on the phone (float render targets, instancing through ANGLE
    // entry points the guest can't get, map_buffer_range, ...); together they caused coloured
    // light beams and full-screen flashes in battles. Hiding them gives the game the phone's
    // paths. (The DXT names stay: the Tegra APK needs them, and they only affect texture uploads.)
    if (name == GL_EXTENSIONS) {
        static const char *const hidden[] = {
            "GL_AMD_performance_monitor", "GL_ANGLE_depth_texture", "GL_ANGLE_framebuffer_blit",
            "GL_ANGLE_framebuffer_multisample", "GL_ANGLE_instanced_arrays", "GL_ANGLE_pack_reverse_row_order",
            "GL_ANGLE_texture_usage", "GL_ANGLE_translated_shader_source", "GL_EXT_debug_label", "GL_EXT_frag_depth",
            "GL_EXT_map_buffer_range", "GL_EXT_shader_texture_lod", "GL_NV_fence", "GL_NV_framebuffer_blit",
            "GL_NV_pack_subimage", "GL_NV_pixel_buffer_object", "GL_OES_depth32", "GL_OES_texture_float",
            "GL_OES_texture_half_float", "GL_EXT_instanced_arrays", "GL_NV_draw_instanced", "GL_EXT_texture_storage"};
        std::string kept, tok;
        for (size_t i = 0; i <= v.size(); i++) {
            if (i < v.size() && v[i] != ' ') {
                tok += v[i];
                continue;
            }
            bool hide = false;
            for (const char *h : hidden) hide |= tok == h;
            if (!tok.empty() && !hide) kept += (kept.empty() ? "" : " ") + tok;
            tok.clear();
        }
        v = kept;
    }
    // ATC (Adreno) textures are decoded to RGBA8 below when the GPU lacks them. Costume Quest's
    // data pack has only ATC textures; without ATC in this list the game uploads them as
    // "compressed GL_RGBA", which ANGLE rejects, and the world draws black.
    if (name == GL_EXTENSIONS && !strstr(s, "GL_AMD_compressed_ATC_texture")) v += " GL_AMD_compressed_ATC_texture";
    // Likewise PVRTC (the PowerVR build), decoded below. And ANGLE does support DXT1/3/5 but only
    // names them GL_EXT_texture_compression_dxt1 and GL_ANGLE_texture_compression_dxt3/5; with the
    // standard names the game uploads its DXT5 textures (the Tegra build) as DXT5 instead of ATC.
    if (name == GL_EXTENSIONS && !strstr(s, "GL_IMG_texture_compression_pvrtc")) v += " GL_IMG_texture_compression_pvrtc";
    if (name == GL_EXTENSIONS && strstr(s, "GL_EXT_texture_compression_dxt1") && strstr(s, "GL_ANGLE_texture_compression_dxt5") &&
        !strstr(s, "GL_EXT_texture_compression_s3tc "))
        v += " GL_EXT_texture_compression_s3tc GL_NV_texture_compression_s3tc";
#endif
    return g_strings[name] = mem::hstrdup(v.c_str());
}
THUNK(glGetString, t_glGetString);

void t_glShaderSource(GLuint sh, GLsizei count, G<const u32> strs, G<const GLint> lens) {
    std::vector<const char *> v(count);
    for (GLsizei i = 0; i < count; i++) v[i] = (const char *)g2h(strs.get()[i]);
    // Debug switch dump_shaders: every shader the game compiles, to files/shaders/<n>_<id>.glsl.
    static const bool dump = !game::debug_setting("dump_shaders").empty();
    if (dump) {
        static std::atomic<int> n{0};
        std::string dir = game::files_dir() + "/shaders";
        mkdir(dir.c_str(), 0700);
        if (FILE *f = fopen((dir + "/" + std::to_string(n++) + "_" + std::to_string(sh) + ".glsl").c_str(), "w")) {
            for (GLsizei i = 0; i < count; i++) {
                if (lens && lens.get()[i] >= 0) fwrite(v[i], 1, lens.get()[i], f);
                else fputs(v[i], f);
            }
            fclose(f);
        }
    }
    if (!skip_list().empty() && count > 0 && !strncmp(v[0], "// ", 3)) {  // names for skip_shader
        const char *end = strchr(v[0], '\n');
        std::lock_guard<std::mutex> lk(g_names_lock);
        g_shader_names[sh].assign(v[0] + 3, end ? end - v[0] - 3 : strlen(v[0] + 3));
    }
    glShaderSource(sh, count, v.data(), lens);
}
THUNK(glShaderSource, t_glShaderSource);

// Buffer mapping: the guest writes into a staging copy that is uploaded on unmap
// (OES_mapbuffer / EXT_map_buffer_range mappings are write-only in practice here).
struct Mapping {
    u32 guest = 0, size = 0, offset = 0;
};
std::unordered_map<GLenum, Mapping> g_mappings;
u32 map_buffer(GLenum target, u32 offset, u32 length) {
    if (!length) {
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        length = (u32)size - offset;
    }
    Mapping m{mem::map(length), length, offset};
    g_mappings[target] = m;
    return m.guest;
}
u32 t_glMapBufferOES(GLenum target, GLenum) { return map_buffer(target, 0, 0); }
THUNK(glMapBufferOES, t_glMapBufferOES);
u32 t_glMapBufferRangeEXT(GLenum target, s32 offset, s32 length, GLbitfield) {
    return map_buffer(target, (u32)offset, (u32)length);
}
THUNK(glMapBufferRangeEXT, t_glMapBufferRangeEXT);
GLboolean t_glUnmapBufferOES(GLenum target) {
    auto it = g_mappings.find(target);
    if (it == g_mappings.end()) return GL_FALSE;
    glBufferSubData(target, it->second.offset, it->second.size, g2h(it->second.guest));
    mem::unmap(it->second.guest, it->second.size);
    g_mappings.erase(it);
    return GL_TRUE;
}
THUNK(glUnmapBufferOES, t_glUnmapBufferOES);
THUNK(glUnmapBufferEXT, t_glUnmapBufferOES);
void t_glGetBufferPointervOES(GLenum target, GLenum, G<u32> out) {
    auto it = g_mappings.find(target);
    *out.get() = it == g_mappings.end() ? 0 : it->second.guest;
}
THUNK(glGetBufferPointervOES, t_glGetBufferPointervOES);
void t_glFlushMappedBufferRangeEXT(GLenum, s32, s32) {}
THUNK(glFlushMappedBufferRangeEXT, t_glFlushMappedBufferRangeEXT);

// ---- straightforward pass-through ----
#define P(T) G<T>
void t_glActiveTexture(GLenum a) { glActiveTexture(a); } THUNK(glActiveTexture, t_glActiveTexture);
void t_glAttachShader(GLuint p, GLuint s) {
    if (!skip_list().empty()) {
        std::lock_guard<std::mutex> lk(g_names_lock);
        g_program_names[p] += g_shader_names[s] + " ";
    }
    glAttachShader(p, s);
}
THUNK(glAttachShader, t_glAttachShader);
void t_glBindAttribLocation(GLuint p, GLuint i, P(const char) n) { glBindAttribLocation(p, i, n); } THUNK(glBindAttribLocation, t_glBindAttribLocation);
void t_glBindFramebuffer(GLenum t, GLuint f) { glBindFramebuffer(t, f); } THUNK(glBindFramebuffer, t_glBindFramebuffer);
void t_glBindRenderbuffer(GLenum t, GLuint r) { glBindRenderbuffer(t, r); } THUNK(glBindRenderbuffer, t_glBindRenderbuffer);
void t_glBindTexture(GLenum t, GLuint x) { glBindTexture(t, x); } THUNK(glBindTexture, t_glBindTexture);
void t_glBlendEquationSeparate(GLenum a, GLenum b) { glBlendEquationSeparate(a, b); } THUNK(glBlendEquationSeparate, t_glBlendEquationSeparate);
void t_glBlendFunc(GLenum a, GLenum b) { glBlendFunc(a, b); } THUNK(glBlendFunc, t_glBlendFunc);
void t_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { glBlendFuncSeparate(a, b, c, d); } THUNK(glBlendFuncSeparate, t_glBlendFuncSeparate);
THUNK(glBlendFuncSeparateOES, t_glBlendFuncSeparate);
void t_glBufferData(GLenum t, s32 s, P(const void) d, GLenum u) { glBufferData(t, s, d, u); } THUNK(glBufferData, t_glBufferData);
void t_glBufferSubData(GLenum t, s32 o, s32 s, P(const void) d) { glBufferSubData(t, o, s, d); } THUNK(glBufferSubData, t_glBufferSubData);
GLenum t_glCheckFramebufferStatus(GLenum t) { return glCheckFramebufferStatus(t); } THUNK(glCheckFramebufferStatus, t_glCheckFramebufferStatus);
THUNK(glCheckFramebufferStatusOES, t_glCheckFramebufferStatus);
void t_glClear(GLbitfield m) { glClear(m); } THUNK(glClear, t_glClear);
void t_glClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); } THUNK(glClearColor, t_glClearColor);
void t_glClearDepthf(float d) { glClearDepthf(d); } THUNK(glClearDepthf, t_glClearDepthf);
void t_glClearStencil(GLint s) { glClearStencil(s); } THUNK(glClearStencil, t_glClearStencil);
void t_glColor4f(float r, float g, float b, float a) { glColor4f(r, g, b, a); } THUNK(glColor4f, t_glColor4f);
void t_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, a); } THUNK(glColorMask, t_glColorMask);
void t_glCompileShader(GLuint s) {
    glCompileShader(s);
#ifdef _WIN32  // shaders ANGLE's translation rejects (on Android, asking right away would stall)
    GLint ok = GL_TRUE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        static std::atomic<int> shown{0};
        if (shown++ < 20) LOGW("shader %u failed to compile: %s", s, log);
    }
#endif
}
THUNK(glCompileShader, t_glCompileShader);
// Debug switch gl_stats: texture uploads counted by GL format, logged every 500 uploads.
void tex_stat(const char *kind, GLenum format) {
    static const bool on = !game::debug_setting("gl_stats").empty();
    if (!on) return;
    static std::mutex lock;
    static std::unordered_map<std::string, int> counts;
    static int total = 0;
    std::lock_guard<std::mutex> lk(lock);
    char key[48];
    snprintf(key, sizeof key, "%s 0x%x", kind, format);
    counts[key]++;
    if (++total % 500 == 0) {
        std::string out;
        for (auto &[k, c] : counts) out += " [" + k + "]=" + std::to_string(c);
        LOGI("texture uploads after %d:%s", total, out.c_str());
    }
}
// ---- ATC (ATI/AMD texture compression) fallback
// Some games ship only ATC textures (GL_AMD_compressed_ATC_texture, Qualcomm Adreno only). On
// other GPUs they are decoded to RGBA8 here and uploaded uncompressed.
constexpr GLenum ATC_RGB = 0x8C92, ATC_RGBA_EXPLICIT = 0x8C93, ATC_RGBA_INTERPOLATED = 0x87EE;
bool is_atc(GLenum f) { return f == ATC_RGB || f == ATC_RGBA_EXPLICIT || f == ATC_RGBA_INTERPOLATED; }
bool gpu_has_atc() {
    static const bool has = [] {
        const char *e = (const char *)glGetString(GL_EXTENSIONS);
        return e && strstr(e, "GL_AMD_compressed_ATC_texture");
    }();
    return has;
}
inline u8 bits5(u32 x) { x &= 31; return (u8)((x << 3) | (x >> 2)); }
inline u8 bits6(u32 x) { x &= 63; return (u8)((x << 2) | (x >> 4)); }
// One 8-byte ATC colour block -> 16 RGBA pixels (alpha left untouched).
void atc_colors(const u8 *b, u8 (*px)[4]) {
    u32 c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    u8 c[4][3];
    const u8 col0[3] = {bits5(c0 >> 10), bits5(c0 >> 5), bits5(c0)};  // 555, top bit = mode
    const u8 col1[3] = {bits5(c1 >> 11), bits6(c1 >> 5), bits5(c1)};  // 565
    for (int k = 0; k < 3; k++) {
        if (!(c0 & 0x8000)) {
            c[0][k] = col0[k];
            c[1][k] = (u8)((5 * col0[k] + 3 * col1[k]) / 8);
            c[2][k] = (u8)((3 * col0[k] + 5 * col1[k]) / 8);
            c[3][k] = col1[k];
        } else {
            c[0][k] = 0;
            c[1][k] = (u8)std::max(0, col0[k] - col1[k] / 4);
            c[2][k] = col0[k];
            c[3][k] = col1[k];
        }
    }
    u32 idx = b[4] | b[5] << 8 | b[6] << 16 | (u32)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px[i], c[(idx >> (2 * i)) & 3], 3);
}
// Decodes a w x h ATC image to RGBA8.
std::vector<u8> atc_decode(GLenum f, int w, int h, const u8 *src) {
    std::vector<u8> out((size_t)w * h * 4);
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = f == ATC_RGB ? 8 : 16;
    u8 px[16][4];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++, src += bsize) {
            if (f == ATC_RGB) {
                atc_colors(src, px);
                for (auto &p : px) p[3] = 255;
            } else {
                atc_colors(src + 8, px);
                if (f == ATC_RGBA_EXPLICIT) {
                    for (int i = 0; i < 16; i++) px[i][3] = (u8)(((src[i / 2] >> (4 * (i & 1))) & 15) * 17);
                } else {  // interpolated alpha, as in DXT5
                    u8 a[8] = {src[0], src[1]};
                    if (a[0] > a[1])
                        for (int i = 1; i < 7; i++) a[i + 1] = (u8)(((7 - i) * a[0] + i * a[1]) / 7);
                    else {
                        for (int i = 1; i < 5; i++) a[i + 1] = (u8)(((5 - i) * a[0] + i * a[1]) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    u64 bits = 0;
                    for (int i = 0; i < 6; i++) bits |= (u64)src[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
                }
            }
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(&out[((size_t)y * w + x) * 4], px[i], 4);
            }
        }
    return out;
}

// ---- PVRTC (PowerVR texture compression, version 1) fallback
// The PowerVR builds of some games ship PVRTC textures (GL_IMG_texture_compression_pvrtc, PowerVR
// GPUs only). On other GPUs they are decoded to RGBA8 here. Each 64-bit block covers 4x4 (4 bpp) or
// 8x4 (2 bpp) pixels and holds two low-resolution colours, A and B, plus per-pixel modulation
// weights; A and B are bilinearly upscaled from the four nearest block centres. Blocks are stored
// in Morton (Z) order.
constexpr GLenum PVRTC_RGB_4BPP = 0x8C00, PVRTC_RGB_2BPP = 0x8C01, PVRTC_RGBA_4BPP = 0x8C02, PVRTC_RGBA_2BPP = 0x8C03;
bool is_pvrtc(GLenum f) { return f == PVRTC_RGB_4BPP || f == PVRTC_RGB_2BPP || f == PVRTC_RGBA_4BPP || f == PVRTC_RGBA_2BPP; }
bool gpu_has_pvrtc() {
    static const bool has = [] {
        const char *e = (const char *)glGetString(GL_EXTENSIONS);
        return e && strstr(e, "GL_IMG_texture_compression_pvrtc");
    }();
    return has;
}
u32 pvrtc_twiddle(u32 bw, u32 bh, u32 x, u32 y) {
    u32 min_dim = std::min(bw, bh), rest = bh < bw ? x : y, out = 0, shift = 0;
    for (u32 bit = 1; bit < min_dim; bit <<= 1, shift++) {
        if (y & bit) out |= 1u << (2 * shift);
        if (x & bit) out |= 2u << (2 * shift);
    }
    return out | ((rest >> shift) << (2 * shift));
}
// A block colour as 8-bit RGBA. B is bits 16..31 of the colour word; A is bits 0..15 (its lowest
// bit is the block's modulation mode, so A has one bit less blue).
void pvrtc_colour(u32 c, bool is_b, u8 out[4]) {
    u32 r, g, b, a;
    if (c & 0x8000) {  // opaque: 5:5:5 (A: 5:5:4)
        r = (c >> 10) & 31, g = (c >> 5) & 31, b = is_b ? c & 31 : ((c & 0x1e) | ((c >> 4) & 1)), a = 15;
    } else {  // translucent: 3-bit alpha, 4:4:4 (A: 4:4:3)
        r = ((c >> 7) & 0x1e) | ((c >> 11) & 1);
        g = ((c >> 3) & 0x1e) | ((c >> 7) & 1);
        b = is_b ? (((c << 1) & 0x1e) | ((c >> 3) & 1)) : (((c << 2) & 0x1c) | ((c >> 1) & 3));
        a = (c >> 11) & 0xe;
    }
    out[0] = (u8)((r << 3) | (r >> 2)), out[1] = (u8)((g << 3) | (g >> 2)), out[2] = (u8)((b << 3) | (b >> 2));
    out[3] = (u8)((a << 4) | a);
}
std::vector<u8> pvrtc_decode(GLenum f, int w, int h, const u8 *src) {
    const bool two = f == PVRTC_RGB_2BPP || f == PVRTC_RGBA_2BPP;
    const int bx = two ? 8 : 4, by = 4;
    const int pw = std::max(w, two ? 16 : 8), ph = std::max(h, 8);  // the format's minimum size
    const u32 nbx = pw / bx, nby = ph / by;
    std::vector<u8> cola(nbx * nby * 4), colb(nbx * nby * 4);
    std::vector<s8> mod(pw * ph), mode(pw * ph);  // raw modulation codes and per-pixel mode
    for (u32 j = 0; j < nby; j++)
        for (u32 i = 0; i < nbx; i++) {
            const u8 *blk = src + 8 * pvrtc_twiddle(nbx, nby, i, j);
            u32 m = blk[0] | blk[1] << 8 | blk[2] << 16 | (u32)blk[3] << 24;
            u32 c = blk[4] | blk[5] << 8 | blk[6] << 16 | (u32)blk[7] << 24;
            pvrtc_colour(c & 0xffff, false, &cola[(j * nbx + i) * 4]);
            pvrtc_colour(c >> 16, true, &colb[(j * nbx + i) * 4]);
            int md = c & 1;
            if (two && md) {  // 2 bpp, interpolated modulation: which interpolation, from the data
                if (m & 1) {
                    md = (m & (1u << 20)) ? 3 : 2;  // vertical only / horizontal only
                    if (m & (1u << 21)) m |= 1u << 20; else m &= ~(1u << 20);
                }
                if (m & 2) m |= 1; else m &= ~1u;
            }
            for (int y = 0; y < by; y++)
                for (int x = 0; x < bx; x++) {
                    int p = (j * by + y) * pw + i * bx + x;
                    mode[p] = (s8)md;
                    if (!two) {
                        mod[p] = (s8)(m & 3), m >>= 2;
                    } else if (!md) {
                        mod[p] = (m & 1) ? 3 : 0, m >>= 1;
                    } else if (((x ^ y) & 1) == 0) {
                        mod[p] = (s8)(m & 3), m >>= 2;
                    }
                }
        }
    static const int rep[4] = {0, 3, 5, 8};
    auto at = [&](int x, int y) { return rep[mod[((y + ph) % ph) * pw + (x + pw) % pw]]; };
    std::vector<u8> out((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            // Modulation weight (0..8 towards B) and punch-through alpha.
            const int p = y * pw + x, md = mode[p];
            int wgt;
            bool punch = false;
            if (!two) {
                static const int punch_rep[4] = {0, 4, 4, 8};
                wgt = md ? punch_rep[mod[p]] : rep[mod[p]];
                punch = md && mod[p] == 2;
            } else if (!md || ((x ^ y) & 1) == 0) {
                wgt = rep[mod[p]];
            } else if (md == 1) {
                wgt = (at(x, y - 1) + at(x, y + 1) + at(x - 1, y) + at(x + 1, y) + 2) / 4;
            } else if (md == 2) {
                wgt = (at(x - 1, y) + at(x + 1, y) + 1) / 2;
            } else {
                wgt = (at(x, y - 1) + at(x, y + 1) + 1) / 2;
            }
            // A and B, bilinearly upscaled from the four nearest block centres (wrapping).
            int u = x - bx / 2 + pw, v = y - by / 2 + ph;
            u32 i0 = (u / bx) % nbx, j0 = (v / by) % nby, i1 = (i0 + 1) % nbx, j1 = (j0 + 1) % nby;
            int fx = u % bx, fy = v % by;
            int w00 = (bx - fx) * (by - fy), w10 = fx * (by - fy), w01 = (bx - fx) * fy, w11 = fx * fy;
            u8 *o = &out[((size_t)y * w + x) * 4];
            for (int k = 0; k < 4; k++) {
                auto lerp = [&](const std::vector<u8> &c) {
                    return c[(j0 * nbx + i0) * 4 + k] * w00 + c[(j0 * nbx + i1) * 4 + k] * w10 +
                           c[(j1 * nbx + i0) * 4 + k] * w01 + c[(j1 * nbx + i1) * 4 + k] * w11;
                };
                const int area = bx * by, a = lerp(cola), b = lerp(colb);
                o[k] = (u8)((a * (8 - wgt) + b * wgt + area * 4) / (area * 8));
            }
            if (punch) o[3] = 0;
            if (f == PVRTC_RGB_4BPP || f == PVRTC_RGB_2BPP) o[3] = 255;
        }
    return out;
}

void t_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, P(const void) d) {
    tex_stat("compressed", f);
    if (is_pvrtc(f) && !gpu_has_pvrtc() && d) {
        std::vector<u8> rgba = pvrtc_decode(f, w, h, (const u8 *)d.get());
        glTexImage2D(t, l, GL_RGBA, w, h, b, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return;
    }
    if (is_atc(f) && !gpu_has_atc() && d) {
        std::vector<u8> rgba = atc_decode(f, w, h, (const u8 *)d.get());
        glTexImage2D(t, l, GL_RGBA, w, h, b, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return;
    }
    glCompressedTexImage2D(t, l, f, w, h, b, s, d);
}
THUNK(glCompressedTexImage2D, t_glCompressedTexImage2D);
void t_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei s, P(const void) d) {
    if (is_atc(f) && !gpu_has_atc() && d) {
        std::vector<u8> rgba = atc_decode(f, w, h, (const u8 *)d.get());
        glTexSubImage2D(t, l, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return;
    }
    glCompressedTexSubImage2D(t, l, x, y, w, h, f, s, d);
}
THUNK(glCompressedTexSubImage2D, t_glCompressedTexSubImage2D);
void t_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) { glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h); } THUNK(glCopyTexSubImage2D, t_glCopyTexSubImage2D);
GLuint t_glCreateProgram() { return glCreateProgram(); } THUNK(glCreateProgram, t_glCreateProgram);
GLuint t_glCreateShader(GLenum t) { return glCreateShader(t); } THUNK(glCreateShader, t_glCreateShader);
void t_glCullFace(GLenum m) { glCullFace(m); } THUNK(glCullFace, t_glCullFace);
void t_glDeleteFramebuffers(GLsizei n, P(const GLuint) f) { glDeleteFramebuffers(n, f); } THUNK(glDeleteFramebuffers, t_glDeleteFramebuffers);
THUNK(glDeleteFramebuffersOES, t_glDeleteFramebuffers);
void t_glDeleteProgram(GLuint p) { glDeleteProgram(p); } THUNK(glDeleteProgram, t_glDeleteProgram);
void t_glDeleteRenderbuffers(GLsizei n, P(const GLuint) r) { glDeleteRenderbuffers(n, r); } THUNK(glDeleteRenderbuffers, t_glDeleteRenderbuffers);
void t_glDeleteShader(GLuint s) { glDeleteShader(s); } THUNK(glDeleteShader, t_glDeleteShader);
void t_glDeleteTextures(GLsizei n, P(const GLuint) t) { glDeleteTextures(n, t); } THUNK(glDeleteTextures, t_glDeleteTextures);
void t_glDepthFunc(GLenum f) { glDepthFunc(f); } THUNK(glDepthFunc, t_glDepthFunc);
void t_glDepthMask(GLboolean f) { glDepthMask(f); } THUNK(glDepthMask, t_glDepthMask);
void t_glDisable(GLenum c) { glDisable(c); } THUNK(glDisable, t_glDisable);
void t_glDisableClientState(GLenum c) { glDisableClientState(c); } THUNK(glDisableClientState, t_glDisableClientState);
void t_glDisableVertexAttribArray(GLuint i) { glDisableVertexAttribArray(i); } THUNK(glDisableVertexAttribArray, t_glDisableVertexAttribArray);
void t_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    if (!g_skip_draws) glDrawArrays(m, f, c);
} THUNK(glDrawArrays, t_glDrawArrays);
void t_glDrawTexfOES(float x, float y, float z, float w, float h) { glDrawTexfOES(x, y, z, w, h); } THUNK(glDrawTexfOES, t_glDrawTexfOES);
void t_glEnable(GLenum c) { glEnable(c); } THUNK(glEnable, t_glEnable);
void t_glEnableClientState(GLenum c) { glEnableClientState(c); } THUNK(glEnableClientState, t_glEnableClientState);
void t_glEnableVertexAttribArray(GLuint i) { glEnableVertexAttribArray(i); } THUNK(glEnableVertexAttribArray, t_glEnableVertexAttribArray);
void t_glFinish() { glFinish(); } THUNK(glFinish, t_glFinish);
void t_glFlush() { glFlush(); } THUNK(glFlush, t_glFlush);
void t_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) { glFramebufferRenderbuffer(t, a, rt, r); } THUNK(glFramebufferRenderbuffer, t_glFramebufferRenderbuffer);
void t_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint x, GLint l) { glFramebufferTexture2D(t, a, tt, x, l); } THUNK(glFramebufferTexture2D, t_glFramebufferTexture2D);
THUNK(glFramebufferTexture2DOES, t_glFramebufferTexture2D);
THUNK(glBindFramebufferOES, t_glBindFramebuffer);
void t_glFrontFace(GLenum m) { glFrontFace(m); } THUNK(glFrontFace, t_glFrontFace);
void t_glGenBuffers(GLsizei n, P(GLuint) b) { glGenBuffers(n, b); } THUNK(glGenBuffers, t_glGenBuffers);
void t_glGenFramebuffers(GLsizei n, P(GLuint) f) { glGenFramebuffers(n, f); } THUNK(glGenFramebuffers, t_glGenFramebuffers);
THUNK(glGenFramebuffersOES, t_glGenFramebuffers);
void t_glGenRenderbuffers(GLsizei n, P(GLuint) r) { glGenRenderbuffers(n, r); } THUNK(glGenRenderbuffers, t_glGenRenderbuffers);
void t_glGenTextures(GLsizei n, P(GLuint) t) { glGenTextures(n, t); } THUNK(glGenTextures, t_glGenTextures);
void t_glGetBooleanv(GLenum p, P(GLboolean) v) { glGetBooleanv(p, v); } THUNK(glGetBooleanv, t_glGetBooleanv);
GLenum t_glGetError() { return glGetError(); } THUNK(glGetError, t_glGetError);
void t_glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, P(GLint) v) { glGetFramebufferAttachmentParameteriv(t, a, p, v); } THUNK(glGetFramebufferAttachmentParameteriv, t_glGetFramebufferAttachmentParameteriv);
void t_glGetIntegerv(GLenum p, P(GLint) v) { glGetIntegerv(p, v); } THUNK(glGetIntegerv, t_glGetIntegerv);
void t_glGetProgramInfoLog(GLuint p, GLsizei n, P(GLsizei) l, P(char) s) { glGetProgramInfoLog(p, n, l, s); } THUNK(glGetProgramInfoLog, t_glGetProgramInfoLog);
void t_glGetProgramiv(GLuint p, GLenum n, P(GLint) v) { glGetProgramiv(p, n, v); } THUNK(glGetProgramiv, t_glGetProgramiv);
void t_glGetRenderbufferParameteriv(GLenum t, GLenum p, P(GLint) v) { glGetRenderbufferParameteriv(t, p, v); } THUNK(glGetRenderbufferParameteriv, t_glGetRenderbufferParameteriv);
void t_glGetShaderInfoLog(GLuint s, GLsizei n, P(GLsizei) l, P(char) x) { glGetShaderInfoLog(s, n, l, x); } THUNK(glGetShaderInfoLog, t_glGetShaderInfoLog);
void t_glGetShaderiv(GLuint s, GLenum n, P(GLint) v) { glGetShaderiv(s, n, v); } THUNK(glGetShaderiv, t_glGetShaderiv);
GLint t_glGetUniformLocation(GLuint p, P(const char) n) { return glGetUniformLocation(p, n); } THUNK(glGetUniformLocation, t_glGetUniformLocation);
GLboolean t_glIsFramebuffer(GLuint f) { return glIsFramebuffer(f); } THUNK(glIsFramebuffer, t_glIsFramebuffer);
GLboolean t_glIsRenderbuffer(GLuint r) { return glIsRenderbuffer(r); } THUNK(glIsRenderbuffer, t_glIsRenderbuffer);
void t_glLineWidth(float w) { glLineWidth(w); } THUNK(glLineWidth, t_glLineWidth);
void t_glLinkProgram(GLuint p) {
    glLinkProgram(p);
#ifdef _WIN32
    GLint ok = GL_TRUE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        static std::atomic<int> shown{0};
        if (shown++ < 20) LOGW("program %u failed to link: %s", p, log);
    }
#endif
}
THUNK(glLinkProgram, t_glLinkProgram);
void t_glLoadIdentity() { glLoadIdentity(); } THUNK(glLoadIdentity, t_glLoadIdentity);
void t_glMatrixMode(GLenum m) { glMatrixMode(m); } THUNK(glMatrixMode, t_glMatrixMode);
void t_glOrthof(float l, float r, float b, float t, float n, float f) { glOrthof(l, r, b, t, n, f); } THUNK(glOrthof, t_glOrthof);
void t_glPixelStorei(GLenum p, GLint v) { glPixelStorei(p, v); } THUNK(glPixelStorei, t_glPixelStorei);
void t_glPopMatrix() { glPopMatrix(); } THUNK(glPopMatrix, t_glPopMatrix);
void t_glPushMatrix() { glPushMatrix(); } THUNK(glPushMatrix, t_glPushMatrix);
void t_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, P(void) d) { glReadPixels(x, y, w, h, f, t, d); } THUNK(glReadPixels, t_glReadPixels);
void t_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { glRenderbufferStorage(t, f, w, h); } THUNK(glRenderbufferStorage, t_glRenderbufferStorage);
void t_glRotatef(float a, float x, float y, float z) { glRotatef(a, x, y, z); } THUNK(glRotatef, t_glRotatef);
void t_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); } THUNK(glScissor, t_glScissor);
void t_glShaderBinary(GLsizei n, P(const GLuint) s, GLenum f, P(const void) b, GLsizei l) { glShaderBinary(n, s, f, b, l); } THUNK(glShaderBinary, t_glShaderBinary);
void t_glStencilFunc(GLenum f, GLint r, GLuint m) { glStencilFunc(f, r, m); } THUNK(glStencilFunc, t_glStencilFunc);
void t_glStencilFuncSeparate(GLenum fa, GLenum f, GLint r, GLuint m) { glStencilFuncSeparate(fa, f, r, m); } THUNK(glStencilFuncSeparate, t_glStencilFuncSeparate);
void t_glStencilMask(GLuint m) { glStencilMask(m); } THUNK(glStencilMask, t_glStencilMask);
void t_glStencilMaskSeparate(GLenum f, GLuint m) { glStencilMaskSeparate(f, m); } THUNK(glStencilMaskSeparate, t_glStencilMaskSeparate);
void t_glStencilOp(GLenum a, GLenum b, GLenum c) { glStencilOp(a, b, c); } THUNK(glStencilOp, t_glStencilOp);
void t_glStencilOpSeparate(GLenum f, GLenum a, GLenum b, GLenum c) { glStencilOpSeparate(f, a, b, c); } THUNK(glStencilOpSeparate, t_glStencilOpSeparate);
void t_glTexEnvf(GLenum t, GLenum p, float v) { glTexEnvf(t, p, v); } THUNK(glTexEnvf, t_glTexEnvf);
void t_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, P(const void) d) {
    tex_stat(ty == GL_UNSIGNED_BYTE ? "raw u8" : ty == GL_UNSIGNED_SHORT_5_6_5 ? "raw 565" : ty == GL_UNSIGNED_SHORT_4_4_4_4 ? "raw 4444"
             : ty == GL_UNSIGNED_SHORT_5_5_5_1 ? "raw 5551" : "raw other", f);
    glTexImage2D(t, l, i, w, h, b, f, ty, d);
}
THUNK(glTexImage2D, t_glTexImage2D);
void t_glTexParameteri(GLenum t, GLenum p, GLint v) { glTexParameteri(t, p, v); } THUNK(glTexParameteri, t_glTexParameteri);
void t_glTexParameteriv(GLenum t, GLenum p, P(const GLint) v) { glTexParameteriv(t, p, v); } THUNK(glTexParameteriv, t_glTexParameteriv);
void t_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, P(const void) d) { glTexSubImage2D(t, l, x, y, w, h, f, ty, d); } THUNK(glTexSubImage2D, t_glTexSubImage2D);
void t_glTranslatef(float x, float y, float z) { glTranslatef(x, y, z); } THUNK(glTranslatef, t_glTranslatef);
void t_glUniform1i(GLint l, GLint v) { glUniform1i(l, v); } THUNK(glUniform1i, t_glUniform1i);
void t_glUniform1f(GLint l, GLfloat v) { glUniform1f(l, v); } THUNK(glUniform1f, t_glUniform1f);
void t_glUniform2fv(GLint l, GLsizei n, P(const GLfloat) v) { glUniform2fv(l, n, v); } THUNK(glUniform2fv, t_glUniform2fv);
void t_glUniform3fv(GLint l, GLsizei n, P(const GLfloat) v) { glUniform3fv(l, n, v); } THUNK(glUniform3fv, t_glUniform3fv);
void t_glValidateProgram(GLuint p) { glValidateProgram(p); } THUNK(glValidateProgram, t_glValidateProgram);
void t_glBlendEquation(GLenum m) { glBlendEquation(m); } THUNK(glBlendEquation, t_glBlendEquation);
void t_glPointSize(GLfloat s) { glPointSize(s); } THUNK(glPointSize, t_glPointSize);
void t_glUniform1iv(GLint l, GLsizei n, P(const GLint) v) { glUniform1iv(l, n, v); } THUNK(glUniform1iv, t_glUniform1iv);
void t_glUniform4f(GLint l, float a, float b, float c, float d) { glUniform4f(l, a, b, c, d); } THUNK(glUniform4f, t_glUniform4f);
void t_glUniform4fv(GLint l, GLsizei n, P(const GLfloat) v) { glUniform4fv(l, n, v); } THUNK(glUniform4fv, t_glUniform4fv);
void t_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, P(const GLfloat) v) { glUniformMatrix4fv(l, n, tr, v); } THUNK(glUniformMatrix4fv, t_glUniformMatrix4fv);
void t_glUseProgram(GLuint p) {
    g_skip_draws = skip_program(p);
    glUseProgram(p);
} THUNK(glUseProgram, t_glUseProgram);
void t_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); } THUNK(glViewport, t_glViewport);
#undef P
}  // namespace
