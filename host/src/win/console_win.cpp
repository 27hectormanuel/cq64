// SPDX-License-Identifier: CC0-1.0
// Windows build: the debug command line in the game window (Android has the Java DevTools dialog
// instead). ` or F1 opens it over the game; commands go to devtools::submit, like the Android
// console's Run button, and the output is devtools' own buffer. Keys reach it on the main thread
// (handle_event); it is drawn on the game's GL thread just before each swap (draw), with its own
// small GLES 2 program and a font atlas rendered once with GDI. The GL state it touches is saved
// and restored, so the game's next frame starts as it left it.
#include "console_win.h"

#include "../common.h"
#include "../devtools.h"
#include "../luahints.h"

#include <windows.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace winconsole {
namespace {
std::mutex g_lock;  // everything below: written on the main thread, read on the GL thread
bool g_open = false;
std::string g_line;  // what is being typed
size_t g_cursor = 0;
std::vector<std::string> g_history;
int g_hist_pos = -1;  // -1: not browsing the history
int g_scroll = 0;     // output lines scrolled back
std::string g_notes;  // local lines (completions), shown after the output

bool is_word(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || c == ':'; }

// Lua the console runs once to list every function in the game's Lua tables (as the Android dev
// menu's LIST_LUA does), one "table.name" per line, via hostlist().
const char *const kListLua =
    "\x01local skip = {string=1, table=1, math=1, io=1, os=1, coroutine=1, package=1, _G=1, bit=1, jit=1} "
    "local o = {} "
    "for n, m in pairs(_G) do if type(n) == 'string' and type(m) == 'table' and not skip[n] then "
    "pcall(function() for k, v in pairs(m) do "
    "if type(k) == 'string' and type(v) == 'function' then o[#o + 1] = n .. '.' .. k end end end) "
    "elseif type(n) == 'string' and type(m) == 'function' then o[#o + 1] = n end end "
    "table.sort(o) hostlist(table.concat(o, '\\n'))";
bool g_listed = false;

// Names to complete from: every function the game's Lua exposes (once the list has come back),
// plus the console's argument hints.
const std::vector<std::string> &names() {
    static std::vector<std::string> list;
    static bool with_lua = false;
    if (list.empty() || !with_lua) {
        std::set<std::string> s(list.begin(), list.end());
        std::string lua = devtools::command_list();
        with_lua = !lua.empty();
        std::istringstream lin(lua);
        for (std::string l; std::getline(lin, l);)
            if (!l.empty()) s.insert(l);
        std::istringstream in(luahints::get());
        for (std::string l; std::getline(in, l);) {
            if (l.size() < 3 || (l[0] != 'C' && l[0] != 'S') || l[1] != '\t') continue;
            size_t e = l.find('\t', 2);
            s.insert(l.substr(2, e == std::string::npos ? std::string::npos : e - 2));
        }
        for (const char *t : {"debug.", "cq.", "app.", "input.", "game."}) s.insert(t);
        list.assign(s.begin(), s.end());
    }
    return list;
}

void complete() {
    size_t b = g_cursor;
    while (b > 0 && is_word(g_line[b - 1])) b--;
    std::string word = g_line.substr(b, g_cursor - b);
    if (word.empty()) return;
    std::vector<std::string> hits;
    for (const std::string &n : names())
        if (n.compare(0, word.size(), word) == 0) hits.push_back(n);
    if (hits.empty()) return;
    std::string common = hits[0];  // longest common prefix
    for (const std::string &h : hits) {
        size_t i = 0;
        while (i < common.size() && i < h.size() && common[i] == h[i]) i++;
        common.resize(i);
    }
    if (common.size() > word.size()) {
        g_line.insert(g_cursor, common.substr(word.size()));
        g_cursor += common.size() - word.size();
    } else if (hits.size() > 1) {
        std::string l;
        for (size_t i = 0; i < hits.size() && i < 40; i++) l += (i ? "  " : "") + hits[i];
        if (hits.size() > 40) l += "  ... (" + std::to_string(hits.size()) + ")";
        g_notes = l;
    }
}

void set_open(bool open) {
    if (open == g_open) return;
    g_open = open;
    g_notes.clear();
    if (open && !g_listed) {
        devtools::submit(kListLua);
        g_listed = true;
    }
    g_scroll = 0;
    LOGI("console %s", open ? "opened" : "closed");
}

// ---- drawing -------------------------------------------------------------------------------
constexpr int kFirst = 32, kLast = 126;
struct Font {
    GLuint tex = 0;
    int cw = 0, ch = 0, cols = 16, tw = 0, th = 0;
};

// Renders the printable ASCII characters into an alpha texture with GDI's monospace font.
Font make_font(int height) {
    Font f;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-height, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(dc, font);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32A(dc, "M", 1, &sz);
    f.cw = sz.cx;
    f.ch = tm.tmHeight;
    int rows = (kLast - kFirst + 1 + f.cols - 1) / f.cols;
    f.tw = f.cw * f.cols;
    f.th = f.ch * rows;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), f.tw, -f.th, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(dc, bmp);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all{0, 0, f.tw, f.th};
    ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &all, "", 0, nullptr);
    for (int c = kFirst; c <= kLast; c++) {
        char s = (char)c;
        int i = c - kFirst;
        TextOutA(dc, (i % f.cols) * f.cw, (i / f.cols) * f.ch, &s, 1);
    }
    GdiFlush();
    std::vector<uint8_t> alpha((size_t)f.tw * f.th);
    const uint32_t *px = (const uint32_t *)bits;
    for (size_t i = 0; i < alpha.size(); i++) alpha[i] = (uint8_t)((px[i] >> 8) & 0xff);
    // One solid texel (bottom-right corner) for the background and the cursor.
    alpha.back() = 255;
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    glGenTextures(1, &f.tex);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, f.tw, f.th, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return f;
}

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

struct Vertex {
    float x, y, u, v, r, g, b, a;
};

struct Batch {
    std::vector<Vertex> v;
    const Font *f;
    int sw, sh;
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
              const float *c) {
        auto P = [&](float x, float y, float u, float vv) {
            v.push_back({x / sw * 2 - 1, 1 - y / sh * 2, u, vv, c[0], c[1], c[2], c[3]});
        };
        P(x0, y0, u0, v0), P(x1, y0, u1, v0), P(x1, y1, u1, v1);
        P(x0, y0, u0, v0), P(x1, y1, u1, v1), P(x0, y1, u0, v1);
    }
    void rect(float x0, float y0, float x1, float y1, const float *c) {
        float u = 1 - 0.5f / f->tw, vv = 1 - 0.5f / f->th;
        quad(x0, y0, x1, y1, u, vv, u, vv, c);
    }
    void text(float x, float y, const std::string &s, const float *c) {
        for (char ch : s) {
            int i = (unsigned char)ch;
            if (i >= kFirst && i <= kLast && i != ' ') {
                i -= kFirst;
                float u0 = (float)(i % f->cols) * f->cw / f->tw, v0 = (float)(i / f->cols) * f->ch / f->th;
                quad(x, y, x + f->cw, y + f->ch, u0, v0, u0 + (float)f->cw / f->tw, v0 + (float)f->ch / f->th, c);
            }
            x += f->cw;
        }
    }
};

// Splits text into lines of at most `width` characters.
void wrap_into(const std::string &text, size_t width, std::vector<std::string> &out) {
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        for (char &c : l)
            if (c == '\t') c = ' ';
        do {
            out.push_back(l.substr(0, width));
            l = l.size() > width ? l.substr(width) : std::string();
        } while (!l.empty());
    }
}
}  // namespace

bool is_open() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_open;
}

bool handle_event(const SDL_Event &e) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        bool ctrl = e.key.keysym.mod & KMOD_CTRL;
        if (k == SDLK_F1 || (k == SDLK_BACKQUOTE && !(e.key.keysym.mod & KMOD_SHIFT))) {  // Shift+` types ~
            set_open(!g_open);
            return true;
        }
        if (!g_open) return false;
        switch (k) {
        case SDLK_ESCAPE: set_open(false); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (!g_line.empty()) {
                devtools::submit(g_line);
                if (g_history.empty() || g_history.back() != g_line) g_history.push_back(g_line);
            }
            g_line.clear();
            g_cursor = 0;
            g_hist_pos = -1;
            g_scroll = 0;
            g_notes.clear();
            break;
        case SDLK_BACKSPACE:
            if (g_cursor > 0) g_line.erase(--g_cursor, 1);
            break;
        case SDLK_DELETE:
            if (g_cursor < g_line.size()) g_line.erase(g_cursor, 1);
            break;
        case SDLK_LEFT: if (g_cursor > 0) g_cursor--; break;
        case SDLK_RIGHT: if (g_cursor < g_line.size()) g_cursor++; break;
        case SDLK_HOME: g_cursor = 0; break;
        case SDLK_END: g_cursor = g_line.size(); break;
        case SDLK_UP:
        case SDLK_DOWN:
            if (g_history.empty()) break;
            if (k == SDLK_UP) g_hist_pos = g_hist_pos < 0 ? (int)g_history.size() - 1 : std::max(0, g_hist_pos - 1);
            else if (g_hist_pos >= 0) g_hist_pos = g_hist_pos + 1 < (int)g_history.size() ? g_hist_pos + 1 : -1;
            g_line = g_hist_pos < 0 ? std::string() : g_history[g_hist_pos];
            g_cursor = g_line.size();
            break;
        case SDLK_PAGEUP: g_scroll += 10; break;
        case SDLK_PAGEDOWN: g_scroll = std::max(0, g_scroll - 10); break;
        case SDLK_TAB: complete(); break;
        default:
            if (ctrl && k == SDLK_v) {
                if (char *t = SDL_GetClipboardText()) {
                    std::string s(t);
                    SDL_free(t);
                    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }), s.end());
                    g_line.insert(g_cursor, s);
                    g_cursor += s.size();
                }
            } else if (ctrl && k == SDLK_l) {
                devtools::clear_output();
            }
        }
        return true;
    }
    if (e.type == SDL_KEYUP) {
        SDL_Keycode k = e.key.keysym.sym;
        return k == SDLK_BACKQUOTE || k == SDLK_F1;  // other releases still reach the game
    }
    if (e.type == SDL_TEXTINPUT) {
        if (!g_open) return false;
        for (const char *p = e.text.text; *p; p++)
            if (*p >= 32 && *p < 127 && *p != '`') g_line.insert(g_cursor++, 1, *p);
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL && g_open) {
        g_scroll = std::max(0, g_scroll + e.wheel.y * 3);
        return true;
    }
    return false;
}

void draw(int sw, int sh) {
    std::string line, notes;
    size_t cursor;
    int scroll;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_open) return;
        line = g_line, notes = g_notes, cursor = g_cursor, scroll = g_scroll;
    }
    static Font font;
    static int font_h = 0;
    static GLuint prog = 0;
    static GLint a_pos, a_uv, a_col, u_tex;
    static GLuint vbo = 0;
    int want = std::max(12, sh / 40);
    if (font_h != want) {
        if (font.tex) glDeleteTextures(1, &font.tex);
        GLint tex0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        GLint unpack;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        font = make_font(want);
        font_h = want;
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glBindTexture(GL_TEXTURE_2D, tex0);
    }
    if (!prog) {
        prog = glCreateProgram();
        glAttachShader(prog, compile(GL_VERTEX_SHADER,
            "attribute vec2 p; attribute vec2 t; attribute vec4 c; varying vec2 vt; varying vec4 vc;\n"
            "void main() { vt = t; vc = c; gl_Position = vec4(p, 0.0, 1.0); }"));
        glAttachShader(prog, compile(GL_FRAGMENT_SHADER,
            "precision mediump float; uniform sampler2D s; varying vec2 vt; varying vec4 vc;\n"
            "void main() { gl_FragColor = vec4(vc.rgb, vc.a * texture2D(s, vt).a); }"));
        glLinkProgram(prog);
        a_pos = glGetAttribLocation(prog, "p");
        a_uv = glGetAttribLocation(prog, "t");
        a_col = glGetAttribLocation(prog, "c");
        u_tex = glGetUniformLocation(prog, "s");
        glGenBuffers(1, &vbo);
    }

    // Lay out: the output fills the top part of the screen, the prompt sits under it.
    const float pad = (float)font.cw;
    size_t width = std::max<size_t>(20, (size_t)((sw - 2 * pad) / font.cw));
    int rows = std::max(4, (int)(sh * 0.55f / font.ch));
    std::vector<std::string> out;
    wrap_into(devtools::output_text(), width, out);
    if (!notes.empty()) wrap_into(notes, width, out);
    int first = std::max(0, (int)out.size() - (rows - 1) - scroll);
    int last = std::min((int)out.size(), first + rows - 1);
    std::string prompt = "> " + line;
    size_t pstart = 0, pcur = cursor + 2;  // keep the cursor in view on long lines
    if (pcur >= width) pstart = pcur - width + 1;
    float h = (float)(rows + 1) * font.ch + pad;

    Batch b{{}, &font, sw, sh};
    static const float bg[4] = {0.04f, 0.03f, 0.08f, 0.9f}, edge[4] = {1.0f, 0.55f, 0.10f, 1.0f},
                       fg[4] = {0.92f, 0.92f, 0.92f, 1}, cmd[4] = {1.0f, 0.78f, 0.35f, 1},
                       err[4] = {1.0f, 0.45f, 0.40f, 1}, dim[4] = {0.6f, 0.6f, 0.7f, 1};
    b.rect(0, 0, (float)sw, h, bg);
    b.rect(0, h, (float)sw, h + 2, edge);
    float y = pad / 2;
    if (scroll > 0 && first > 0) b.text(sw - pad - font.cw * 6, y, "[more]", dim);
    for (int i = first; i < last; i++, y += font.ch) {
        const std::string &l = out[i];
        b.text(pad, y, l, l.rfind("> ", 0) == 0 ? cmd : l.rfind("error", 0) == 0 ? err : fg);
    }
    y = h - font.ch - pad / 4;
    b.text(pad, y, prompt.substr(pstart, width), cmd);
    if ((GetTickCount() / 500) % 2 == 0) {
        float cx = pad + (float)(pcur - pstart) * font.cw;
        b.rect(cx, y, cx + std::max(2, font.cw / 4), y + font.ch, fg);
    }
    std::string hint = "F1 / backquote: close   Tab: complete   Up/Down: history   PgUp/PgDn: scroll";
    if (hint.size() < width) b.text(sw - pad - font.cw * (float)hint.size(), h + 4, hint, dim);

    // Save the state this changes, draw, restore.
    GLint fb, program, array_buf, active_tex, tex, vp[4], blend_src_rgb, blend_dst_rgb, blend_src_a,
        blend_dst_a, blend_eq_rgb, blend_eq_a;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
              stencil = glIsEnabled(GL_STENCIL_TEST), mask[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    struct Attrib {
        GLint index, enabled, size, type, norm, stride, buf;
        void *ptr;
    } attribs[3];
    GLint idx[3] = {a_pos, a_uv, a_col};
    for (int i = 0; i < 3; i++) {
        Attrib &a = attribs[i];
        a.index = idx[i];
        if (a.index < 0) continue;
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.norm);
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(a.index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buf);
        glGetVertexAttribPointerv(a.index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.ptr);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, sw, sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, font.tex);
    glUniform1i(u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, b.v.size() * sizeof(Vertex), b.v.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(a_pos);
    glEnableVertexAttribArray(a_uv);
    glEnableVertexAttribArray(a_col);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(a_col, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)(4 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b.v.size());

    for (const Attrib &a : attribs) {
        if (a.index < 0) continue;
        glBindBuffer(GL_ARRAY_BUFFER, a.buf);
        glVertexAttribPointer(a.index, a.size, a.type, (GLboolean)a.norm, a.stride, a.ptr);
        if (a.enabled) glEnableVertexAttribArray(a.index);
        else glDisableVertexAttribArray(a.index);
    }
    glBindBuffer(GL_ARRAY_BUFFER, array_buf);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(active_tex);
    glUseProgram(program);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
}
}  // namespace winconsole
