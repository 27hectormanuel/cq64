# Debug console for the official PC releases

Brings back Costume Quest's Lua developer console in the two official PC releases. See
[docs/debug-console-pc.md](../docs/debug-console-pc.md) for how it works and what was tested. No
game files are here; both check the game's code before changing anything, and do nothing on
any other version.

| | Works with | Install |
|---|---|---|
| `windows/` | `Cq.exe` 1.0.0.11 (SHA-256 `9d5acea0…addef8`) | Copy `dinput8.dll` next to `Cq.exe`. Under Wine/Proton also set `WINEDLLOVERRIDES=dinput8=n,b`. |
| `linux/` | GOG 2.0.0.3 `Cq.bin.x86` (SHA-256 `7104dc7d…5100ef`) | `run.sh "<game>/game"` (the folder with `Cq.bin.x86`). |

**Keys:** F1 or `` ` `` opens and closes the console, Enter runs the line, Up/Down is history,
Escape closes, Shift+`` ` `` types `~`. Results and errors show the way the game's own console
shows them. On Windows it is the game's original console; on Linux it is drawn in its style,
because the Linux build has no console font. Lua's `print` prints nothing in these builds;
evaluate an expression instead (`debug.GetPlayer()`, `game.GetCandyAmount()`).

**Files** (Windows: the game folder; Linux: `$CQCONSOLE_DIR`, default the game folder):
`cqconsole.log` (each command and each new console line), `cqconsole-commands.txt` (Lua lines to
run), `cqconsole-keys.txt` (test keys: Windows `key <virtual-key>`, `text <chars>`,
`input <virtual-key>`; Linux `key <SDL key name>`, `text <chars>`) and `cqconsole-shot` (saves the
next frame as `cqconsole-shot.bmp`).

## Build

- **Windows** (any i686 MinGW clang or gcc, e.g. llvm-mingw):
  `i686-w64-mingw32-clang -O2 -shared -o dinput8.dll windows/cqconsole.c -luser32 -Wl,--kill-at`
- **Linux:** `CLANG=<clang> linux/build.sh [out dir]`. This needs a clang with the x86 target and
  `ld.lld`, the system's 32-bit `libc.so.6` (`/usr/lib32`), and ffmpeg with `drawtext` plus a
  monospace font for the generated font atlas (`make_font.py`). It needs no i386 C headers.

Testing note: the game saves to `$XDG_DATA_HOME/doublefine/costumequest` (Linux) or the Wine
prefix (Windows). Point `XDG_DATA_HOME` at a scratch folder to keep test runs away from your saves.
