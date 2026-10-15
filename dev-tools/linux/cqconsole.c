// SPDX-License-Identifier: CC0-1.0
// Debug console for the official Linux release of Costume Quest (GOG 2.0.0.3, Cq.bin.x86, SHA-256
// 7104dc7d...5100ef), loaded with LD_PRELOAD (see run.sh).
//
// The retail game kept its Lua console (Console::ExecuteCommand, which also prints results and
// errors into the console's line list), but stubbed out the typing (Console::HandleInput) and the
// drawing (Console::RequestRender, and the platform font). This library adds both back:
//   ` or F1        open / close (also LB + RB + A on a controller)
//   Enter          run the line with the game's ExecuteCommand
//   Up / Down      history             Escape      close
// Keys: SDL_PollEvent is wrapped; the game calls it from its main loop (DFApp::PlatformLoop), so
// commands run on the game thread, between frames. While the console is open, keyboard events
// don't reach the game. Drawing: SDL_GL_SwapWindow is wrapped (the render thread): the console's
// lines (as Console::Update ages them) and the prompt are drawn over the frame with desktop GL.
//
// Files in $CQCONSOLE_DIR (default: the current directory), checked every 200 ms on the game thread:
//   cqconsole-commands.txt   Lua lines, run in turn
//   cqconsole-keys.txt       lines "key <SDL key name>" (Return, F1, Up, ...) or "text <characters>",
//                            pushed as SDL key events (the console's own path), or
//                            "pad lb+rb+a [ms]" (controller buttons, as the game polls them)
//   cqconsole-shot           saves the next frame as cqconsole-shot.bmp
//   cqconsole-record         starts / stops recording: each frame is written as raw RGB24
//                            (bottom-up) to cqconsole-record.fifo, a named pipe that a reader
//                            such as ffmpeg must already have open; the frame size is logged
// Each new console line is also printed to stdout and cqconsole.log.
//
// Built without the system's C headers (SteamOS has none for i386): the few libc, SDL and GL
// declarations it needs are below.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"

// ---- libc / libdl --------------------------------------------------------------------------------
typedef struct FILE FILE;
extern FILE *stdout;
FILE *fopen(const char *, const char *);
int fclose(FILE *);
int fflush(FILE *);
int fputs(const char *, FILE *);
char *fgets(char *, int, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int rename(const char *, const char *);
int remove(const char *);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int sscanf(const char *, const char *, ...);
char *getenv(const char *);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
size_t strcspn(const char *, const char *);
char *strcpy(char *, const char *);
long readlink(const char *, char *, size_t);
int mprotect(void *, size_t, int);
int open(const char *, int, ...);
long write(int, const void *, size_t);
int close(int);
int fcntl(int, int, ...);
#define O_WRONLY 1
#define O_NONBLOCK 04000
#define F_SETFL 4
void *malloc(size_t);
void *dlsym(void *, const char *);
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1l)

// ---- SDL2 (the parts used) -------------------------------------------------------------------------
typedef union {
    uint32_t type;
    struct {
        uint32_t type, timestamp, windowID;
        uint8_t state, repeat, pad2, pad3;
        int32_t scancode, sym;
        uint16_t mod;
        uint32_t unused;
    } key;
    uint8_t padding[56];
} SDL_Event;
enum { SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301, SDL_TEXTEDITING = 0x302, SDL_TEXTINPUT = 0x303 };
enum {
    K_RETURN = 13, K_ESCAPE = 27, K_BACKSPACE = 8, K_TAB = 9, K_DELETE = 127, K_BACKQUOTE = '`',
    K_F1 = 0x4000003a, K_HOME = 0x4000004a, K_PAGEUP = 0x4000004b, K_END = 0x4000004d,
    K_PAGEDOWN = 0x4000004e, K_RIGHT = 0x4000004f, K_LEFT = 0x40000050, K_DOWN = 0x40000051,
    K_UP = 0x40000052, K_KP_ENTER = 0x40000058,
};
#define KMOD_SHIFT 0x0003
#define KMOD_CTRL 0x00c0
static int (*sdl_poll)(SDL_Event *);
static void (*sdl_swap)(void *);
static int (*sdl_push)(SDL_Event *);
static uint32_t (*sdl_ticks)(void);
static int32_t (*sdl_key_from_name)(const char *);
static int32_t (*sdl_scancode_from_key)(int32_t);
static void (*sdl_drawable_size)(void *, int *, int *);
static uint8_t (*sdl_pad_button)(void *, int);
enum { PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_BACK = 4, PAD_START = 6, PAD_LB = 9, PAD_RB = 10 };

// ---- desktop GL (the parts used) -------------------------------------------------------------------
typedef unsigned int GLenum, GLuint;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_QUADS 0x0007
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_BGR 0x80E0
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_TEXTURE 0x1702
#define GL_ALL_ATTRIB_BITS 0x000fffff
#define GL_CLIENT_ALL_ATTRIB_BITS 0xffffffff
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_ARRAY_BUFFER 0x8892
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE0 0x84C0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_ALPHA_TEST 0x0BC0
#define GL_LIGHTING 0x0B50
#define GL_FOG 0x0B60
#define GL_READ_FRAMEBUFFER 0x8CA8
static struct {
    void (*GetIntegerv)(GLenum, GLint *);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*PushAttrib)(GLuint);
    void (*PopAttrib)(void);
    void (*PushClientAttrib)(GLuint);
    void (*PopClientAttrib)(void);
    void (*MatrixMode)(GLenum);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*LoadIdentity)(void);
    void (*Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    void (*Begin)(GLenum);
    void (*End)(void);
    void (*Vertex2f)(GLfloat, GLfloat);
    void (*TexCoord2f)(GLfloat, GLfloat);
    void (*Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*PixelStorei)(GLenum, GLint);
    void (*UseProgram)(GLuint);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*ActiveTexture)(GLenum);
} gl;

// ---- the game (Cq.bin.x86; checked in init) ---------------------------------------------------------
#define G_CONSOLE ((char **)0x0917eab0)  // g_pConsole
#define C_LUA 0x18
#define C_LOCK 0x24                      // LwMutex guarding the lines
#define C_LINES 0x34                     // Array<ConsoleLine>: count<<6 | flags, capacity, data
#define LINE_SIZE 0x24                   // ConsoleLine: String text, float seconds, bool keep, Color
typedef void (*ExecuteCommand)(void *console, void *str, int player);  // Console::ExecuteCommand
typedef void (*StringCopy)(void *str, const char *text, int len);       // String::Copy
typedef void (*StringFree)(void *str);                                  // String::~String
typedef void (*MutexOp)(void *mutex);                                   // LwMutex::Lock / Release
static const struct {
    uint32_t addr;
    uint8_t bytes[8];
} kCheck[] = {
    {0x08557790, {0x31, 0xc0, 0xc3, 0x90, 0x8d, 0xb6, 0x00, 0x00}},  // Console::HandleInput (the stub)
    {0x08558780, {0x55, 0x57, 0x56, 0x53, 0x83, 0xec, 0x6c, 0x8b}},  // Console::ExecuteCommand
    {0x0849ea90, {0x83, 0xec, 0x2c, 0x89, 0x74, 0x24, 0x24, 0x8b}},  // String::Copy
    {0x0841f9f0, {0x53, 0x83, 0xec, 0x28, 0x8b, 0x5c, 0x24, 0x30}},  // String::~String
    {0x084af570, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00, 0x89, 0x44}},  // LwMutex::Lock
    {0x084af5a0, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00, 0x89, 0x44}},  // LwMutex::Release
};
static const ExecuteCommand execute_command = (ExecuteCommand)0x08558780;
static const StringCopy string_copy = (StringCopy)0x0849ea90;
static const StringFree string_free = (StringFree)0x0841f9f0;
static const MutexOp mutex_lock = (MutexOp)0x084af570, mutex_release = (MutexOp)0x084af5a0;

// ---- state -------------------------------------------------------------------------------------------
static int g_ok;           // the right game binary, everything found
static char g_dir[512] = ".";
static FILE *g_log;
static volatile int g_spin;  // guards the line being typed (game thread) against drawing (render thread)
static int g_open;
static char g_line[512];
static int g_len, g_cursor;
static char g_history[32][512];
static int g_hist_n, g_hist_pos = -1;
static volatile int g_shot;
static volatile int g_record;  // wanted by the game thread; the render thread opens/closes the pipe
static volatile uint32_t g_test_pad, g_test_pad_until;  // test buttons ("pad lb+rb+a 300"), every controller

static void lock(void) { while (__atomic_exchange_n(&g_spin, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_spin, 0, __ATOMIC_RELEASE); }

static void logf(const char *fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    if (g_log) {
        fputs(buf, g_log);
        fputs("\n", g_log);
        fflush(g_log);
    }
}

static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_dir, name); }

// ---- running a command -------------------------------------------------------------------------------
static void execute(const char *cmd) {
    char *c = *G_CONSOLE;
    if (!c) {
        logf("no console yet: %s", cmd);
        return;
    }
    logf("> %s", cmd);
    uint32_t str[4] = {0, 0, 0, 0};  // String (Array<char>), filled by the game's own String::Copy
    string_copy(str, cmd, -1);
    execute_command(c, str, -1);     // player index -1: run locally
    string_free(str);
}

// ---- typing --------------------------------------------------------------------------------------------
static void set_open(int open) {
    if (open == g_open) return;
    g_open = open;
    logf("console %s", open ? "opened" : "closed");
}

static void set_line(const char *s) {
    lock();
    g_len = g_cursor = (int)strlen(s);
    memcpy(g_line, s, g_len);
    unlock();
}

static void insert(char ch) {
    lock();
    if (g_len < (int)sizeof g_line - 1) {
        memmove(g_line + g_cursor + 1, g_line + g_cursor, g_len - g_cursor);
        g_line[g_cursor++] = ch;
        g_len++;
    }
    unlock();
}

static void enter(void) {
    char cmd[512];
    lock();
    memcpy(cmd, g_line, g_len);
    cmd[g_len] = 0;
    unlock();
    if (*cmd) {
        if (!g_hist_n || strcmp(g_history[(g_hist_n - 1) % 32], cmd)) strcpy(g_history[g_hist_n++ % 32], cmd);
        execute(cmd);
    }
    g_hist_pos = -1;
    set_line("");
}

static void history(int dir) {
    int first = g_hist_n > 32 ? g_hist_n - 32 : 0;
    if (!g_hist_n) return;
    if (dir < 0) g_hist_pos = g_hist_pos < 0 ? g_hist_n - 1 : (g_hist_pos > first ? g_hist_pos - 1 : first);
    else if (g_hist_pos >= 0) g_hist_pos = g_hist_pos + 1 < g_hist_n ? g_hist_pos + 1 : -1;
    set_line(g_hist_pos < 0 ? "" : g_history[g_hist_pos % 32]);
}

// US keyboard: the character a key gives, with and without Shift.
static const char kShifted[][2] = {{'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
                                   {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'},
                                   {'[', '{'}, {']', '}'}, {';', ':'}, {'\'', '"'}, {',', '<'}, {'.', '>'},
                                   {'/', '?'}, {'\\', '|'}, {'`', '~'}};
static char key_char(int32_t sym, int shift) {
    if (sym >= 'a' && sym <= 'z') return (char)(shift ? sym - 32 : sym);
    if (sym == ' ') return ' ';
    for (size_t i = 0; i < sizeof kShifted / sizeof kShifted[0]; i++)
        if (sym == kShifted[i][0]) return shift ? kShifted[i][1] : kShifted[i][0];
    return 0;
}

// Returns 1 if the console used the event (the game must not see it).
static int consume(const SDL_Event *e) {
    if (e->type == SDL_KEYDOWN) {
        int32_t k = e->key.sym;
        if (k == K_F1 || (k == K_BACKQUOTE && !(e->key.mod & KMOD_SHIFT))) {  // Shift+` types ~
            if (!e->key.repeat) set_open(!g_open);
            return 1;
        }
        if (!g_open) return 0;
        switch (k) {
        case K_RETURN:
        case K_KP_ENTER: enter(); break;
        case K_ESCAPE: set_open(0); break;
        case K_BACKSPACE:
            lock();
            if (g_cursor > 0) {
                memmove(g_line + g_cursor - 1, g_line + g_cursor, g_len - g_cursor);
                g_cursor--, g_len--;
            }
            unlock();
            break;
        case K_DELETE:
            lock();
            if (g_cursor < g_len) memmove(g_line + g_cursor, g_line + g_cursor + 1, g_len - g_cursor - 1), g_len--;
            unlock();
            break;
        case K_LEFT: lock(); if (g_cursor > 0) g_cursor--; unlock(); break;
        case K_RIGHT: lock(); if (g_cursor < g_len) g_cursor++; unlock(); break;
        case K_HOME: lock(); g_cursor = 0; unlock(); break;
        case K_END: lock(); g_cursor = g_len; unlock(); break;
        case K_UP: history(-1); break;
        case K_DOWN: history(1); break;
        default:
            if (!(e->key.mod & KMOD_CTRL)) {
                char ch = key_char(k, e->key.mod & KMOD_SHIFT);
                if (ch) insert(ch);
            }
        }
        return 1;
    }
    if (e->type == SDL_KEYUP) return e->key.sym == K_BACKQUOTE || e->key.sym == K_F1;
    if (e->type == SDL_TEXTINPUT || e->type == SDL_TEXTEDITING) return g_open;
    return 0;
}

// ---- test files and new lines (game thread) ----------------------------------------------------------
static FILE *take(const char *name) {  // opens a trigger file and deletes it
    char p[600], q[620];
    path(p, sizeof p, name);
    snprintf(q, sizeof q, "%s.taken", p);
    remove(q);
    if (rename(p, q)) return 0;
    return fopen(q, "r");
}

static void push_key(int32_t sym, uint16_t mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.key.type = SDL_KEYDOWN;
    e.key.state = 1;
    e.key.sym = sym;
    e.key.scancode = sdl_scancode_from_key(sym);  // what the game itself reads
    e.key.mod = mod;
    sdl_push(&e);
    e.key.type = SDL_KEYUP;
    e.key.state = 0;
    sdl_push(&e);
}

static void poll_files(void) {
    char l[600];
    FILE *f = take("cqconsole-commands.txt");
    if (f) {
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (*l) execute(l);
        }
        fclose(f);
    }
    if ((f = take("cqconsole-keys.txt"))) {
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            logf("test input: %s", l);
            if (!strncmp(l, "key ", 4)) {
                int32_t k = sdl_key_from_name(l + 4);
                if (k) push_key(k, 0);
            } else if (!strncmp(l, "pad ", 4)) {  // "pad lb+rb+a [ms]": controller buttons, held that long
                char names[64] = "";
                int ms = 300;
                sscanf(l + 4, "%63s %d", names, &ms);
                uint32_t mask = 0;
                for (char *t = names; *t;) {
                    size_t n = strcspn(t, "+");
                    static const char *const k[] = {"a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb"};
                    for (int i = 0; i < 11; i++)
                        if (strlen(k[i]) == n && !strncmp(t, k[i], n)) mask |= 1u << i;
                    t += n + (t[n] == '+');
                }
                g_test_pad_until = sdl_ticks() + ms;
                g_test_pad = mask;
            } else if (!strncmp(l, "text ", 5)) {
                for (const char *p = l + 5; *p; p++) {
                    int32_t sym = *p;
                    uint16_t mod = 0;
                    if (*p >= 'A' && *p <= 'Z') sym = *p + 32, mod = 1;
                    for (size_t i = 0; i < sizeof kShifted / sizeof kShifted[0]; i++)
                        if (*p == kShifted[i][1]) sym = kShifted[i][0], mod = 1;
                    push_key(sym, mod);
                }
            }
        }
        fclose(f);
    }
    if ((f = take("cqconsole-shot"))) {
        fclose(f);
        g_shot = 1;
    }
    if ((f = take("cqconsole-record"))) {
        fclose(f);
        g_record = !g_record;
    }
}

// Copies up to `max` of the console's lines (text, seconds left, colour).
typedef struct {
    char text[200];
    float seconds, rgba[4];
    int keep;
} Line;
static int copy_lines(Line *out, int max) {
    char *c = *G_CONSOLE;
    if (!c) return 0;
    int n = 0;
    mutex_lock(c + C_LOCK);
    uint32_t count = *(uint32_t *)(c + C_LINES) >> 6;
    char *data = *(char **)(c + C_LINES + 8);
    int first = count > (uint32_t)max ? (int)count - max : 0;
    for (uint32_t i = first; i < count; i++, n++) {
        uint32_t *s = (uint32_t *)(data + i * LINE_SIZE);
        const char *t = (s[0] >> 6) > 1 ? (const char *)s[2] : "";
        snprintf(out[n].text, sizeof out[n].text, "%s", t);
        memcpy(&out[n].seconds, data + i * LINE_SIZE + 0x0c, 4);
        out[n].keep = data[i * LINE_SIZE + 0x10];
        memcpy(out[n].rgba, data + i * LINE_SIZE + 0x14, 16);
    }
    mutex_release(c + C_LOCK);
    return n;
}

static void log_new_lines(void) {
    static char seen[32][200];
    static int seen_n;
    static Line now[32];
    int n = copy_lines(now, 32);
    for (int i = 0; i < n; i++) {
        int old = 0;
        for (int j = 0; j < seen_n && !old; j++) old = !strcmp(now[i].text, seen[j]);
        if (!old) {
            logf("console line: %s", now[i].text);
            fputs(now[i].text, stdout);
            fputs("\n", stdout);
            fflush(stdout);
        }
    }
    for (int i = 0; i < n; i++) strcpy(seen[i], now[i].text);
    seen_n = n;
}

// LB + RB + A on a controller: open / close. The game polls its buttons with this, so A is hidden
// from it while both shoulders are held.
static uint8_t pad_button(void *gc, int b) {
    uint8_t v = sdl_pad_button(gc, b);
    if (g_test_pad && (int32_t)(sdl_ticks() - g_test_pad_until) < 0 && (g_test_pad >> b & 1)) v = 1;
    return v;
}

uint8_t SDL_GameControllerGetButton(void *gc, int button) {
    if (!g_ok) return sdl_pad_button(gc, button);
    static void *seen;
    if (gc != seen) logf("controller %p polled by the game", (seen = gc));
    if (button != PAD_A) return pad_button(gc, button);
    static void *held_gc;
    int both = pad_button(gc, PAD_LB) && pad_button(gc, PAD_RB), a = pad_button(gc, PAD_A);
    if (!both) {
        if (held_gc == gc) held_gc = 0;
        return a;
    }
    if (a && held_gc != gc) {
        held_gc = gc;
        logf("LB + RB + A");
        set_open(!g_open);
    } else if (!a && held_gc == gc) {
        held_gc = 0;
    }
    return 0;
}

int SDL_PollEvent(SDL_Event *e) {
    if (!g_ok) return sdl_poll(e);
    static uint32_t last;
    uint32_t t = sdl_ticks();
    if (t - last >= 200) {
        last = t;
        poll_files();
        log_new_lines();
    }
    if (!e) return sdl_poll(e);
    for (;;) {
        int r = sdl_poll(e);
        if (!r || !consume(e)) return r;
    }
}

// ---- drawing (render thread) -----------------------------------------------------------------------------
static void save_bmp(int w, int h) {
    int stride = (w * 3 + 3) & ~3;
    unsigned char *px = malloc((size_t)stride * h);
    if (!px) return;
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px);  // bottom-up, as BMP wants
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t size = 54 + stride * h, v;
    memcpy(hdr + 2, &size, 4);
    v = 54, memcpy(hdr + 10, &v, 4);
    v = 40, memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1, hdr[28] = 24;
    char p[600];
    path(p, sizeof p, "cqconsole-shot.bmp");
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(hdr, 1, 54, f);
        fwrite(px, 1, (size_t)stride * h, f);
        fclose(f);
        logf("screenshot %s (%dx%d)", p, w, h);
    }
}

// Recording: every frame, raw, into the named pipe (render thread). Blocks while the reader is
// behind, so the game slows down rather than dropping frames.
static void record_frame(int w, int h) {
    static int fd = -1, rw, rh;
    static unsigned char *px;
    static size_t cap;
    char p[600];
    if (g_record && fd >= 0 && (w != rw || h != rh)) g_record = 0;  // size changed: stop
    if (!g_record) {
        if (fd >= 0) close(fd), fd = -1, logf("recording stopped");
        return;
    }
    if (fd < 0) {
        path(p, sizeof p, "cqconsole-record.fifo");
        if ((fd = open(p, O_WRONLY | O_NONBLOCK)) < 0) {  // no reader: give up
            logf("recording: no reader on %s", p);
            g_record = 0;
            return;
        }
        fcntl(fd, F_SETFL, 0);  // then blocking: the game waits for the reader
        rw = w, rh = h;
        logf("recording %dx%d rgb24 bottom-up to %s", w, h, p);
    }
    size_t n = (size_t)w * 3 * h;
    if (n > cap && !(px = malloc(cap = n))) {
        cap = 0;
        return;
    }
    if (gl.BindFramebuffer) gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (size_t off = 0; off < n;) {
        long r = write(fd, px + off, n - off);
        if (r <= 0) {  // reader gone
            close(fd), fd = -1, g_record = 0;
            logf("recording stopped: reader closed the pipe");
            return;
        }
        off += (size_t)r;
    }
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    gl.TexCoord2f(u0, v0), gl.Vertex2f(x0, y0);
    gl.TexCoord2f(u1, v0), gl.Vertex2f(x1, y0);
    gl.TexCoord2f(u1, v1), gl.Vertex2f(x1, y1);
    gl.TexCoord2f(u0, v1), gl.Vertex2f(x0, y1);
}

#define ATLAS_W (FONT_W * FONT_COUNT + 1)  // one solid column at the end, for boxes
static void text(float x, float y, float s, const char *t, int max) {
    for (int i = 0; t[i] && i < max; i++, x += FONT_W * s) {
        int c = (unsigned char)t[i] - FONT_FIRST;
        if (c <= 0 || c >= FONT_COUNT) continue;
        quad(x, y, x + FONT_W * s, y + FONT_H * s, (float)(c * FONT_W) / ATLAS_W, 0,
             (float)((c + 1) * FONT_W) / ATLAS_W, 1);
    }
}

static void rect(float x0, float y0, float x1, float y1) {
    float u = (ATLAS_W - 0.5f) / ATLAS_W;
    quad(x0, y0, x1, y1, u, 0.5f, u, 0.5f);
}

static void draw(int w, int h) {
    static Line lines[24];
    int n = copy_lines(lines, 24);
    char line[600];
    int cursor, open;
    lock();
    open = g_open;
    snprintf(line, sizeof line, "> %.*s", g_len, g_line);
    cursor = g_cursor + 2;
    unlock();
    if (!n && !open) return;

    static GLuint tex;
    GLint program = 0, fb = 0, buf = 0, active = 0, unpack = 4;
    if (gl.UseProgram) gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (gl.BindFramebuffer) gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if (gl.BindBuffer) gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
    if (gl.ActiveTexture) gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    if (gl.ActiveTexture) gl.ActiveTexture(GL_TEXTURE0);
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    gl.MatrixMode(GL_TEXTURE), gl.PushMatrix(), gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION), gl.PushMatrix(), gl.LoadIdentity();
    gl.Ortho(0, w, h, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW), gl.PushMatrix(), gl.LoadIdentity();
    if (gl.UseProgram) gl.UseProgram(0);
    if (gl.BindFramebuffer) gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (gl.BindBuffer) gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.Viewport(0, 0, w, h);
    gl.Disable(GL_DEPTH_TEST), gl.Disable(GL_CULL_FACE), gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_STENCIL_TEST), gl.Disable(GL_ALPHA_TEST), gl.Disable(GL_LIGHTING), gl.Disable(GL_FOG);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Enable(GL_TEXTURE_2D);
    if (!tex) {
        static unsigned char atlas[ATLAS_W * FONT_H];
        for (int y = 0; y < FONT_H; y++) {
            memcpy(atlas + y * ATLAS_W, kFont + y * FONT_W * FONT_COUNT, FONT_W * FONT_COUNT);
            atlas[y * ATLAS_W + ATLAS_W - 1] = 255;
        }
        gl.GenTextures(1, &tex);
        gl.BindTexture(GL_TEXTURE_2D, tex);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, FONT_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl.BindTexture(GL_TEXTURE_2D, tex);

    // Like the original console: its lines at the bottom left, fading out, the prompt under them.
    float s = h >= 1000 ? 2.0f : 1.0f, lh = FONT_H * s, x = 12 * s;
    int width = (int)((w - 2 * x) / (FONT_W * s));
    float y = h - 12 * s - lh * (open ? n + 1 : n);
    gl.Begin(GL_QUADS);
    if (open) {
        gl.Color4f(0.03f, 0.03f, 0.08f, 0.75f);
        rect(0, y - 6 * s, (float)w, (float)h);
    }
    for (int i = 0; i < n; i++, y += lh) {
        float a = lines[i].keep ? 1.0f : lines[i].seconds < 1.0f ? (lines[i].seconds > 0 ? lines[i].seconds : 0) : 1.0f;
        float *c = lines[i].rgba;
        int black = c[0] + c[1] + c[2] < 0.1f;  // lines added without a colour
        gl.Color4f(0, 0, 0, a * 0.8f);
        text(x + s, y + s, s, lines[i].text, width);
        gl.Color4f(black ? 1 : c[0], black ? 1 : c[1], black ? 1 : c[2], a);
        text(x, y, s, lines[i].text, width);
    }
    if (open) {
        int start = cursor >= width ? cursor - width + 1 : 0;
        gl.Color4f(1.0f, 0.8f, 0.4f, 1);
        text(x, y, s, line + start, width);
        if ((sdl_ticks() / 500) % 2 == 0) {
            float cx = x + (cursor - start) * FONT_W * s;
            gl.Color4f(1, 1, 1, 1);
            rect(cx, y + 2 * s, cx + 2 * s, y + lh - 2 * s);
        }
    }
    gl.End();

    gl.MatrixMode(GL_MODELVIEW), gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION), gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE), gl.PopMatrix();
    gl.PopClientAttrib();
    gl.PopAttrib();
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    if (gl.ActiveTexture) gl.ActiveTexture(active);
    if (gl.BindBuffer) gl.BindBuffer(GL_ARRAY_BUFFER, buf);
    if (gl.BindFramebuffer) gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
    if (gl.UseProgram) gl.UseProgram(program);
}

void SDL_GL_SwapWindow(void *window) {
    if (g_ok) {
        int w = 0, h = 0;
        sdl_drawable_size(window, &w, &h);
        if (w > 0 && h > 0) {
            draw(w, h);
            if (g_shot) {
                g_shot = 0;
                if (gl.BindFramebuffer) gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                save_bmp(w, h);
            }
            record_frame(w, h);
        }
    }
    sdl_swap(window);
}

// ---- start ---------------------------------------------------------------------------------------------
static void *gl_proc(const char *name) {
    static void *(*get)(const char *);
    if (!get) get = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *p = dlsym(RTLD_DEFAULT, name);
    return p ? p : get ? get(name) : 0;
}

__attribute__((constructor)) static void init(void) {
    sdl_poll = (int (*)(SDL_Event *))dlsym(RTLD_NEXT, "SDL_PollEvent");
    sdl_swap = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    sdl_pad_button = (uint8_t(*)(void *, int))dlsym(RTLD_NEXT, "SDL_GameControllerGetButton");
    if (!sdl_poll || !sdl_swap || !sdl_pad_button) return;  // not a program that uses SDL2: stay out of the way
    char exe[512] = "";
    long n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    if (n < 10 || strcmp(exe + n - 10, "Cq.bin.x86")) return;  // only the game, not its launcher
    if (getenv("CQCONSOLE_DIR")) snprintf(g_dir, sizeof g_dir, "%s", getenv("CQCONSOLE_DIR"));
    char p[600];
    path(p, sizeof p, "cqconsole.log");
    g_log = fopen(p, "w");
    for (size_t i = 0; i < sizeof kCheck / sizeof kCheck[0]; i++)
        for (int b = 0; b < 8; b++)
            if (((const uint8_t *)kCheck[i].addr)[b] != kCheck[i].bytes[b]) {
                logf("unexpected code at 0x%x: not the GOG 2.0.0.3 Cq.bin.x86; console off", kCheck[i].addr);
                return;
            }
    sdl_push = (int (*)(SDL_Event *))dlsym(RTLD_NEXT, "SDL_PushEvent");
    sdl_ticks = (uint32_t(*)(void))dlsym(RTLD_NEXT, "SDL_GetTicks");
    sdl_key_from_name = (int32_t(*)(const char *))dlsym(RTLD_NEXT, "SDL_GetKeyFromName");
    sdl_scancode_from_key = (int32_t(*)(int32_t))dlsym(RTLD_NEXT, "SDL_GetScancodeFromKey");
    sdl_drawable_size = (void (*)(void *, int *, int *))dlsym(RTLD_NEXT, "SDL_GL_GetDrawableSize");
    if (!sdl_drawable_size)  // the game ships SDL 2.0.0, which predates it (and high-DPI windows)
        sdl_drawable_size = (void (*)(void *, int *, int *))dlsym(RTLD_NEXT, "SDL_GetWindowSize");
#define GL(name) *(void **)&gl.name = gl_proc("gl" #name)
    GL(GetIntegerv), GL(ReadPixels), GL(PushAttrib), GL(PopAttrib), GL(PushClientAttrib), GL(PopClientAttrib);
    GL(MatrixMode), GL(PushMatrix), GL(PopMatrix), GL(LoadIdentity), GL(Ortho), GL(Enable), GL(Disable);
    GL(BlendFunc), GL(Begin), GL(End), GL(Vertex2f), GL(TexCoord2f), GL(Color4f), GL(GenTextures);
    GL(BindTexture), GL(TexImage2D), GL(TexParameteri), GL(Viewport), GL(PixelStorei), GL(UseProgram);
    GL(BindFramebuffer), GL(BindBuffer), GL(ActiveTexture);
    if (!sdl_push || !sdl_ticks || !sdl_pad_button || !sdl_key_from_name || !sdl_scancode_from_key || !sdl_drawable_size || !gl.Begin || !gl.GetIntegerv) {
        logf("SDL or GL functions missing (%p %p %p %p %p %p); console off", (void *)sdl_push, (void *)sdl_ticks,
             (void *)sdl_key_from_name, (void *)sdl_drawable_size, (void *)gl.Begin, (void *)gl.GetIntegerv);
        return;
    }
    g_ok = 1;
    logf("Costume Quest debug console: ` or F1 opens it (files in %s)", g_dir);
}
