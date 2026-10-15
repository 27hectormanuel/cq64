// SPDX-License-Identifier: CC0-1.0
// Debug console for the official Windows release of Costume Quest (Cq.exe 1.0.0.11, SHA-256
// 9d5acea0...addef8, and the current Steam build, d9e1ee3e...b46cf). Built as dinput8.dll and placed next to Cq.exe, which imports DINPUT8.dll and
// so loads this first; DirectInput8Create is passed on to the system's dinput8.dll.
//
// The retail game kept its Lua console (Console::ExecuteCommand) and the code that draws it
// (Console::RequestRender, every frame), but not the code that read the keyboard into it. This
// library puts that part back:
//   ` or F1        open / close the console (its "active" flag, console+0x10); also LB + RB + A
//                  on a controller (XInputGetState is wrapped; A doesn't reach the game meanwhile)
//   typing         fills the console's input line, which the game draws as "> text_"
//   Enter          runs the line with the game's own ExecuteCommand: results and errors appear
//                  in the console's line list and on screen, as the developers saw them
//   Up / Down      history; Escape closes
// Keys go through the game window's message handler (subclassed), which runs on the game's main
// thread, so commands run there as the original console's did.
//
// Files in the game folder, for testing without a keyboard (checked every 200 ms on that thread):
//   cqconsole-commands.txt   Lua lines, run in turn
//   cqconsole-keys.txt       lines "key <virtual-key code>" / "text <characters>" (posted to the window),
//                            "input <virtual-key code>" (SendInput, as from the keyboard),
//                            "pad lb+rb+a [ms]" (controller 0's buttons, added for that long)
//   cqconsole-shot           saves the next frame as cqconsole-shot.bmp
// Everything is logged to cqconsole.log, including each new console line.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define EXPORT __declspec(dllexport)
#define WM_CONSOLE_TOGGLE (WM_APP + 0x51)  // posted by the controller hook

// ---- addresses in Cq.exe (see docs/debug-console-pc.md) ----------------------------------------
// Two builds are known: the retail 1.0.0.11 (SHA-256 9d5acea0...addef8) and the current Steam build
// (d9e1ee3e...b46cf, Jan 2026), where code moved by +0x420 and globals by -0x1900.
static const struct build {
    const char *name;
    DWORD console, execute, font;  // Console *g_pConsole, Console::ExecuteCommand, the console's font
    struct {
        DWORD addr;
        unsigned char bytes[8];
        int n;
    } check[3];
} kBuilds[] = {
    {"Cq.exe 1.0.0.11", 0xa915b8, 0x565150, 0xa9124c,
     {{0x565150, {0x83, 0xec, 0x14, 0x53, 0x8b, 0x5c, 0x24, 0x1c}, 8},  // ExecuteCommand: sub esp,14h; push ebx; mov ebx,[esp+1Ch]
      {0x564f00, {0x55, 0x8b, 0x2d, 0xb8, 0x15, 0xa9, 0x00}, 7},  // QueueCommand: push ebp; mov ebp,[g_pConsole]
      {0x565b50, {0xa1, 0xb8, 0x15, 0xa9, 0x00}, 5}}},            // luaapp_ToggleConsoleSpam: mov eax,[g_pConsole]
    {"Cq.exe (Steam, 2026)", 0xa8fcb8, 0x565570, 0xa8f94c,
     {{0x565570, {0x83, 0xec, 0x14, 0x53, 0x8b, 0x5c, 0x24, 0x1c}, 8},
      {0x565320, {0x55, 0x8b, 0x2d, 0xb8, 0xfc, 0xa8, 0x00}, 7},
      {0x565f70, {0xa1, 0xb8, 0xfc, 0xa8, 0x00}, 5}}},
};
static const struct build *g_build;
#define G_CONSOLE ((char **)g_build->console)
#define EXECUTE_COMMAND (g_build->execute)    // Console::ExecuteCommand: EAX = String*, stack: console, playerIndex
#define G_FONT ((void **)g_build->font)       // the console's font (drawn only when non-null)
#define C_ACTIVE 0x10
#define C_CURSOR 0x1c
#define C_LOCK 0x28                           // CRITICAL_SECTION guarding the lines
#define C_LINES 0x50                          // Array<ConsoleLine>: count<<6 | flags, capacity, data
#define C_INPUT 0x5c                          // Array<char>: count<<6 | flags, capacity, data
#define LINE_SIZE 0x24                        // ConsoleLine: String text, float seconds, bool keep, Color

static FILE *g_log;
static char g_dir[MAX_PATH];
static HWND g_wnd;
static WNDPROC g_old_proc;
static char g_line[1024];
static int g_len, g_cursor;
static char g_history[32][1024];
static int g_hist_n, g_hist_pos = -1;
static int g_open;
static DWORD g_saved_input[3];  // the console's own input Array, put back on close
static volatile LONG g_shot;
static volatile LONG g_test_pad, g_test_pad_until;  // test buttons ("pad lb+rb+a 300"), added to pad 0

static void logf(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static char *console(void) { return *G_CONSOLE; }

// ---- running a command -------------------------------------------------------------------------
static void execute(const char *cmd) {
    char *c = console();
    if (!c) {
        logf("no console yet: %s", cmd);
        return;
    }
    // A String (Array<char>) pointing at our text: count includes the NUL, in bits 6 and up.
    // ExecuteCommand only reads it.
    size_t n = strlen(cmd) + 1;
    static char buf[1024];
    memcpy(buf, cmd, n > sizeof buf ? sizeof buf : n);
    buf[sizeof buf - 1] = 0;
    DWORD str[3] = {(DWORD)(n << 6), (DWORD)n, (DWORD)buf};
    logf("> %s", cmd);
    __asm__ volatile(
        "push $-1\n\t"
        "push %1\n\t"
        "mov %0, %%eax\n\t"
        "call *%2\n\t"
        :
        : "r"(str), "r"(c), "r"(EXECUTE_COMMAND)
        : "eax", "ecx", "edx", "memory", "cc");
}

// ---- the input line, as the game draws it ---------------------------------------------------------
static void show_line(void) {
    char *c = console();
    if (!c || !g_open) return;
    static char shown[1024];
    memcpy(shown, g_line, g_len);
    shown[g_len] = 0;
    DWORD *in = (DWORD *)(c + C_INPUT);
    in[0] = (in[0] & 0x3f) | ((DWORD)(g_len + 1) << 6);
    in[1] = sizeof shown;
    in[2] = (DWORD)shown;
    *(int *)(c + C_CURSOR) = g_cursor;
}

static void set_open(int open) {
    char *c = console();
    if (!c || open == g_open) return;
    DWORD *in = (DWORD *)(c + C_INPUT);
    if (open) memcpy(g_saved_input, in, sizeof g_saved_input);
    g_open = open;
    c[C_ACTIVE] = (char)open;
    if (open) show_line();
    else memcpy(in, g_saved_input, sizeof g_saved_input);
    logf("console %s (font %p)", open ? "opened" : "closed", *G_FONT);
}

static void insert(const char *s, int n) {
    if (g_len + n >= (int)sizeof g_line - 1) return;
    memmove(g_line + g_cursor + n, g_line + g_cursor, g_len - g_cursor);
    memcpy(g_line + g_cursor, s, n);
    g_len += n, g_cursor += n;
    show_line();
}

static void set_line(const char *s) {
    g_len = g_cursor = (int)strlen(s);
    memcpy(g_line, s, g_len);
    show_line();
}

static void enter(void) {
    g_line[g_len] = 0;
    if (g_len) {
        if (!g_hist_n || strcmp(g_history[(g_hist_n - 1) % 32], g_line))
            strcpy(g_history[g_hist_n++ % 32], g_line);
        execute(g_line);
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

// ---- new console lines -> log ---------------------------------------------------------------------
static void log_new_lines(void) {
    static char seen[64][256];
    static int seen_n;
    char *c = console();
    if (!c) return;
    char now[64][256];
    int n = 0;
    EnterCriticalSection((CRITICAL_SECTION *)(c + C_LOCK));
    DWORD count = *(DWORD *)(c + C_LINES) >> 6;
    char *data = *(char **)(c + C_LINES + 8);
    for (DWORD i = 0; i < count && n < 64; i++) {
        DWORD *s = (DWORD *)(data + i * LINE_SIZE);
        const char *t = (s[0] >> 6) > 1 ? (const char *)s[2] : "";
        snprintf(now[n++], 256, "%s", t);
    }
    LeaveCriticalSection((CRITICAL_SECTION *)(c + C_LOCK));
    for (int i = 0; i < n; i++) {
        int old = 0;
        for (int j = 0; j < seen_n && !old; j++) old = !strcmp(now[i], seen[j]);
        if (!old) logf("console line: %s", now[i]);
    }
    memcpy(seen, now, sizeof now);
    seen_n = n;
}

// ---- test files --------------------------------------------------------------------------------
static FILE *take(const char *name) {  // opens and deletes a trigger file, if present
    char p[MAX_PATH], q[MAX_PATH];
    snprintf(p, sizeof p, "%s\\%s", g_dir, name);
    snprintf(q, sizeof q, "%s.taken", p);
    DeleteFileA(q);
    if (!MoveFileA(p, q)) return NULL;
    return fopen(q, "r");
}

static void poll_files(void) {
    char l[1024];
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
            int vk;
            logf("test input: %s", l);
            if (sscanf(l, "key %i", &vk) == 1) {
                PostMessageA(g_wnd, WM_KEYDOWN, vk, 1);
                PostMessageA(g_wnd, WM_KEYUP, vk, 0xc0000001);
            } else if (sscanf(l, "input %i", &vk) == 1) {  // as from the keyboard: DirectInput sees it too
                INPUT in[2] = {{0}};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = (WORD)vk;
                in[0].ki.wScan = in[1].ki.wScan = (WORD)MapVirtualKeyA(vk, 0);
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &in[0], sizeof in[0]);
                Sleep(80);
                SendInput(1, &in[1], sizeof in[1]);
            } else if (!strncmp(l, "pad ", 4)) {  // "pad lb+rb+a [ms]": pad 0 buttons, held that long
                char names[64] = "";
                int ms = 300;
                sscanf(l + 4, "%63s %d", names, &ms);
                LONG mask = 0;
                for (char *t = strtok(names, "+"); t; t = strtok(NULL, "+"))
                    mask |= !strcmp(t, "a") ? 0x1000 : !strcmp(t, "b") ? 0x2000 : !strcmp(t, "x") ? 0x4000
                          : !strcmp(t, "y") ? 0x8000 : !strcmp(t, "lb") ? 0x100 : !strcmp(t, "rb") ? 0x200
                          : !strcmp(t, "start") ? 0x10 : !strcmp(t, "back") ? 0x20 : 0;
                InterlockedExchange(&g_test_pad_until, (LONG)(GetTickCount() + ms));
                InterlockedExchange(&g_test_pad, mask);
            } else if (!strncmp(l, "text ", 5)) {
                for (const char *p = l + 5; *p; p++) PostMessageA(g_wnd, WM_CHAR, (unsigned char)*p, 1);
            }
        }
        fclose(f);
    }
    if ((f = take("cqconsole-shot"))) {
        fclose(f);
        InterlockedExchange(&g_shot, 1);
    }
}

// ---- the window --------------------------------------------------------------------------------
#define TIMER_ID 0xc0de

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CONSOLE_TOGGLE:
        logf("LB + RB + A");
        set_open(!g_open);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) {
            poll_files();
            log_new_lines();
            if (g_open && console() && !console()[C_ACTIVE]) console()[C_ACTIVE] = 1;  // keep it open
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F1 || (wp == VK_OEM_3 && !(GetKeyState(VK_SHIFT) & 0x8000))) {  // Shift+` types ~
            set_open(!g_open);
            return 0;
        }
        if (!g_open) break;
        switch (wp) {
        case VK_RETURN: enter(); break;
        case VK_ESCAPE: set_open(0); break;
        case VK_BACK:
            if (g_cursor > 0) {
                memmove(g_line + g_cursor - 1, g_line + g_cursor, g_len - g_cursor);
                g_len--, g_cursor--;
                show_line();
            }
            break;
        case VK_DELETE:
            if (g_cursor < g_len) {
                memmove(g_line + g_cursor, g_line + g_cursor + 1, g_len - g_cursor - 1);
                g_len--;
                show_line();
            }
            break;
        case VK_LEFT: if (g_cursor > 0) g_cursor--; show_line(); break;
        case VK_RIGHT: if (g_cursor < g_len) g_cursor++; show_line(); break;
        case VK_HOME: g_cursor = 0; show_line(); break;
        case VK_END: g_cursor = g_len; show_line(); break;
        case VK_UP: history(-1); break;
        case VK_DOWN: history(1); break;
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp == VK_OEM_3 || wp == VK_F1) return 0;
        break;  // other releases still reach the game
    case WM_CHAR:
    case WM_SYSCHAR:
        if (!g_open) break;
        if (wp >= 32 && wp < 127 && wp != '`') {
            char ch = (char)wp;
            insert(&ch, 1);
        }
        return 0;
    }
    return CallWindowProcA(g_old_proc, w, m, wp, lp);
}

static BOOL CALLBACK find_window(HWND w, LPARAM unused) {
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    g_wnd = w;
    return FALSE;
}

// The game creates its window after startup; wait for it, then subclass it from its own thread
// (a timer message handled by the subclass runs everything else on the game's main thread).
static DWORD WINAPI attach(void *unused) {
    for (int i = 0; i < 600 && !g_wnd; i++) {
        Sleep(100);
        EnumWindows(find_window, 0);
    }
    if (!g_wnd) {
        logf("no game window found");
        return 0;
    }
    g_old_proc = (WNDPROC)SetWindowLongA(g_wnd, GWL_WNDPROC, (LONG)proc);
    SetTimer(g_wnd, TIMER_ID, 200, NULL);
    char title[128] = "";
    GetWindowTextA(g_wnd, title, sizeof title);
    logf("attached to window %p \"%s\"", (void *)g_wnd, title);
    return 0;
}

// ---- frame capture (Direct3D 9 Present) -----------------------------------------------------------
typedef HRESULT(WINAPI *SaveSurface)(const char *, int, IDirect3DSurface9 *, const PALETTEENTRY *, const RECT *);
typedef HRESULT(STDMETHODCALLTYPE *Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                 D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static Present g_present;
static CreateDevice g_create_device;

static void patch_vtable(void **slot, void *fn, void **old) {
    DWORD prot;
    VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &prot);
    if (*slot != fn) {
        *old = *slot;
        *slot = fn;
    }
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}

static HRESULT STDMETHODCALLTYPE my_present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w,
                                            const RGNDATA *r) {
    if (InterlockedExchange(&g_shot, 0)) {
        static SaveSurface save;
        if (!save) save = (SaveSurface)GetProcAddress(LoadLibraryA("d3dx9_43.dll"), "D3DXSaveSurfaceToFileA");
        IDirect3DSurface9 *s = NULL;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\cqconsole-shot.bmp", g_dir);
        if (save && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &s))) {
            HRESULT hr = save(path, 0 /* D3DXIFF_BMP */, s, NULL, NULL);
            IDirect3DSurface9_Release(s);
            logf("screenshot %s: 0x%lx", path, hr);
        }
    }
    return g_present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE my_create_device(IDirect3D9 *self, UINT ad, D3DDEVTYPE t, HWND w, DWORD f,
                                                  D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    HRESULT hr = g_create_device(self, ad, t, w, f, pp, out);
    if (SUCCEEDED(hr) && *out) {
        void **vt = *(void ***)*out;
        patch_vtable(&vt[17], (void *)my_present, (void **)&g_present);
        logf("D3D9 device %p (%ux%u)", (void *)*out, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    }
    return hr;
}

typedef IDirect3D9 *(WINAPI *Create9)(UINT);
static Create9 g_create9;
static IDirect3D9 *WINAPI my_create9(UINT v) {
    IDirect3D9 *d = g_create9(v);
    if (d) patch_vtable(&(*(void ***)d)[16], (void *)my_create_device, (void **)&g_create_device);
    return d;
}

// ---- LB + RB + A on a controller (XInput) ------------------------------------------------------------
typedef DWORD(WINAPI *XGetState)(DWORD, void *);
static XGetState g_xinput_get_state;
static DWORD WINAPI my_xinput_get_state(DWORD user, void *state) {
    DWORD r = g_xinput_get_state(user, state);
    if (r || !state) return r;
    WORD *buttons = (WORD *)((char *)state + 4);  // XINPUT_STATE: dwPacketNumber, XINPUT_GAMEPAD (wButtons first)
    if (user == 0 && g_test_pad && (LONG)(GetTickCount() - g_test_pad_until) < 0) *buttons |= (WORD)g_test_pad;
    static int held[4];
    int both = (*buttons & 0x0300) == 0x0300, now = both && (*buttons & 0x1000);  // LB 0x100, RB 0x200, A 0x1000
    if (user < 4) {
        if (now && !held[user] && g_wnd) PostMessageA(g_wnd, WM_CONSOLE_TOGGLE, 0, 0);  // on the game's thread
        held[user] = now;
    }
    if (both) *buttons &= ~0x1000;  // A stays with the console while both shoulders are held
    return r;
}

// Points Cq.exe's import of <dll>!<name> at fn; the original goes to *old.
static void hook_one(const char *dll, const char *name, void *fn, void **old) {
    char *base = (char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp(base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name)) continue;
            patch_vtable((void **)&iat->u1.Function, fn, old);
            logf("hooked %s!%s", dll, name);
        }
    }
}

// ---- DirectInput8Create, passed on; the keyboard reads nothing while the console is open --------
static const GUID kSysKeyboard = {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static void *g_keyboard;  // the game's IDirectInputDevice8 for the keyboard
typedef HRESULT(STDMETHODCALLTYPE *GetState)(void *, DWORD, void *);
typedef HRESULT(STDMETHODCALLTYPE *GetData)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *DICreateDevice)(void *, REFGUID, void **, void *);
static GetState g_get_state;
static GetData g_get_data;
static DICreateDevice g_di_create_device;

static HRESULT STDMETHODCALLTYPE my_get_state(void *dev, DWORD size, void *data) {
    HRESULT hr = g_get_state(dev, size, data);
    if (dev == g_keyboard && g_open && SUCCEEDED(hr)) memset(data, 0, size);
    return hr;
}

static HRESULT STDMETHODCALLTYPE my_get_data(void *dev, DWORD size, void *data, DWORD *n, DWORD flags) {
    HRESULT hr = g_get_data(dev, size, data, n, flags);
    if (dev == g_keyboard && g_open && SUCCEEDED(hr) && n && !(flags & 1 /* DIGDD_PEEK */)) *n = 0;
    return hr;
}

static HRESULT STDMETHODCALLTYPE my_di_create_device(void *di, REFGUID g, void **out, void *outer) {
    HRESULT hr = g_di_create_device(di, g, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        logf("DirectInput device {%08lx-...} %p", g->Data1, *out);
        if (IsEqualGUID(g, &kSysKeyboard)) {
            g_keyboard = *out;
            void **vt = *(void ***)*out;
            patch_vtable(&vt[9], (void *)my_get_state, (void **)&g_get_state);
            patch_vtable(&vt[10], (void *)my_get_data, (void **)&g_get_data);
        }
    }
    return hr;
}

typedef HRESULT(WINAPI *DI8Create)(HINSTANCE, DWORD, REFIID, void **, void *);
EXPORT HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID id, void **out, void *outer) {
    static DI8Create real;
    if (!real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, sizeof p);
        strcat(p, "\\dinput8.dll");
        real = (DI8Create)GetProcAddress(LoadLibraryA(p), "DirectInput8Create");
    }
    HRESULT hr = real ? real(inst, ver, id, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && g_log)
        patch_vtable(&(*(void ***)*out)[3], (void *)my_di_create_device, (void **)&g_di_create_device);
    return hr;
}

static int right_exe(void) {
    char *base = (char *)GetModuleHandleA(NULL);
    if ((DWORD)base != 0x400000) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    DWORD end = 0x400000 + nt->OptionalHeader.SizeOfImage;
    for (size_t b = 0; b < sizeof kBuilds / sizeof kBuilds[0]; b++) {
        int ok = 1;
        for (int i = 0; i < 3 && ok; i++)
            ok = kBuilds[b].check[i].addr + 8 <= end &&
                 !memcmp((void *)kBuilds[b].check[i].addr, kBuilds[b].check[i].bytes, kBuilds[b].check[i].n);
        if (ok) {
            g_build = &kBuilds[b];
            logf("game: %s", g_build->name);
            return 1;
        }
    }
    logf("unexpected code: not a known Cq.exe build, console off");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *unused) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    *strrchr(g_dir, '\\') = 0;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\cqconsole.log", g_dir);
    g_log = fopen(p, "w");
    if (!right_exe()) return TRUE;  // DirectInput still works; the console stays off
    logf("Costume Quest debug console: ` or F1 opens it");
    hook_one("d3d9.dll", "Direct3DCreate9", (void *)my_create9, (void **)&g_create9);
    hook_one("XINPUT9_1_0.dll", "XInputGetState", (void *)my_xinput_get_state, (void **)&g_xinput_get_state);
    hook_one("XINPUT1_3.dll", "XInputGetState", (void *)my_xinput_get_state, (void **)&g_xinput_get_state);  // Steam build
    CreateThread(NULL, 0, attach, NULL, 0, NULL);
    return TRUE;
}
