# Dev console: how to use it

Costume Quest's Lua developer console, brought back in three PC builds. How it was found and
what each build kept: [RESEARCH.md](RESEARCH.md). No game files are here; bring your own game.

| Build | Get it | Open the console |
|---|---|---|
| **Costume Quest Mobile for Windows** (this repo, runs the Android game) | Release *Dev Console testing*: `CostumeQuestMobile-optimized.zip` or `-original.zip` | F1, `` ` `` or LB + RB + A |
| **Official Windows** `Cq.exe`: 1.0.0.11 (SHA-256 `9d5acea0…addef8`) or the current Steam build (`d9e1ee3e…b46cf`) | Release: `dinput8.dll` | F1, `` ` `` or LB + RB + A |
| **Official Linux** GOG 2.0.0.3 `Cq.bin.x86` (SHA-256 `7104dc7d…5100ef`) | Release: `cqconsole-linux.zip` | F1, `` ` `` or LB + RB + A |

With a controller only (Steam Deck), open the console with LB + RB + A, then type on the on-screen
keyboard (Steam + X).

The official-game files check the game's code at start and do nothing on any other version (the
game then runs as usual).

## Keys (all three)

| Key | |
|---|---|
| F1 or `` ` ``, or **LB + RB + A** on a controller | open / close (A doesn't reach the game while LB and RB are held) |
| Enter | run the line |
| Up / Down | history |
| Left / Right, Home / End, Backspace / Delete | edit |
| Shift+`` ` `` | types `~` (Lua's `~=`) |
| Escape | close |
| Tab, PgUp / PgDn, Ctrl+V, Ctrl+L | complete, scroll, paste, clear (Costume Quest Mobile only) |

Type Lua. An expression shows its value: `debug.GetPlayer()`, `game.GetCandyAmount()`,
`6*7 ~= 41`. Errors show in red. `print()` shows nothing in the official builds, so evaluate
expressions instead. Handy commands: `game.AddCandyAmount(100)`, `debug.AcquireAllCostumes()`,
`debug.GetAllStamps()`, `cq.WinBattle()`. The full list of the game's functions is in
[debug-console.md](../docs/debug-console.md#6-pushing-lua-from-a-pc-over-adb).

## Costume Quest Mobile for Windows

Unzip, drag your Costume Quest APK onto `Costume Quest Mobile.exe` (see
[windows.md](../docs/windows.md)), and press F1 in the game. The console is drawn over the game.
Its output also goes to `cq64-data/files/console.out`.

To send commands without the keyboard, write a file `cq64-data/files/console` (one Lua line per
line); `cq64-data/files/input` takes `sdlkey F1`, `type <text>`, `sdlkey Return` and
`pad lb+rb+a`. A file
`cq64-data/files/screenshot` saves the next frame as `screenshot.bmp`.

## Official Windows release (`Cq.exe`)

1. Copy `dinput8.dll` into the game folder, next to `Cq.exe`. The game loads it by itself.
   On Steam: right-click Costume Quest → Manage → Browse local files.
2. **Windows:** nothing else to set. Start the game as usual (from Steam or `Cq.exe`).
   **Wine / Proton / Steam Deck only:** also set `WINEDLLOVERRIDES=dinput8=n,b`. On Steam that
   goes in the launch options: `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
3. Start the game and press F1. This is the game's own console: small white text at the bottom
   left, and lines fade after a few seconds, as the developers had it.

To remove it, delete `dinput8.dll`.

Files in the game folder:
- `cqconsole.log`: what was run and each new console line.
- `cqconsole-commands.txt`: Lua lines to run.
- `cqconsole-keys.txt`: test keys, as `key <virtual-key code>`, `text <characters>`,
  `input <virtual-key code>` or `pad lb+rb+a [ms]`.
- `cqconsole-shot`: saves the next frame as `cqconsole-shot.bmp`.

## Official Linux release (GOG)

1. Unzip `cqconsole-linux.zip` anywhere: it has `cqconsole.so` and `run.sh`.
2. Run `./run.sh "<install dir>/game"`, using the folder that holds `Cq.bin.x86`. Add `-window` for
   a window.

`run.sh` uses Steam's runtime for the game's 32-bit libraries when Steam is installed, because the
game needs `libGLU` and few systems have it for i386. The console is drawn over the game in the
original style, because the Linux build has no console font of its own.

Files go in `$CQCONSOLE_DIR` (default: the game folder): the same `cqconsole.log`, `-commands.txt`,
`-keys.txt` (`key <SDL key name>` such as `key Return`, `text <characters>` or `pad lb+rb+a [ms]`) and `-shot` as
on Windows.

**Recording (Linux only):** creating the file `cqconsole-record` starts recording the game's own
frames, and creating it again stops. Each frame goes as raw RGB24, bottom-up, to the named pipe
`cqconsole-record.fifo`, which a reader must already have open (the frame size is in the log).
For example, with a 1280×800 window:

```sh
mkfifo cqconsole-record.fifo
ffmpeg -use_wallclock_as_timestamps 1 -f rawvideo -pix_fmt rgb24 -s 1280x800 -i cqconsole-record.fifo \
    -vf vflip -fps_mode cfr -r 30 -c:v libx264 -pix_fmt yuv420p out.mp4 &
touch cqconsole-record    # start; touch it again to stop, and ffmpeg finishes the file
```

The game waits for the reader, so a slow encoder slows the game down rather than dropping frames.

**Saves:** the game saves to `$XDG_DATA_HOME/doublefine/costumequest` (normally
`~/.local/share/...`). For tests that shouldn't touch your saves, run with
`XDG_DATA_HOME=/some/scratch/dir` and copy the saves there first if you need one.

## Building

- **Costume Quest Mobile for Windows:** `scripts/build_win.sh [optimized|original]`
  ([windows.md](../docs/windows.md)).
- **dinput8.dll** (any i686 MinGW clang or gcc, e.g. llvm-mingw):
  `i686-w64-mingw32-clang -O2 -shared -o dinput8.dll windows/cqconsole.c -luser32 -Wl,--kill-at`
- **cqconsole.so:** `CLANG=<clang> linux/build.sh [out dir]`. This needs a clang with the x86
  target and `ld.lld`, the system's 32-bit `libc.so.6` (`/usr/lib32`), and ffmpeg with `drawtext`
  plus a monospace font for the generated font atlas (`make_font.py`). It needs no i386 C headers.
  Use a regular LLVM clang (llvm-mingw's `clang` works): a library built with the Android NDK's
  clang crashed the game at start.

## Testing helpers (`testing/`)

These scripts were used for the tests in [RESEARCH.md](RESEARCH.md); set the paths at the top of
each:

- `runpc.sh`: starts `Cq.exe` under Proton from a folder of symlinks to the game, with the DLL.
- `pcshot.sh` / `lshot.sh`: capture the Windows / Linux game's frame through the console's
  screenshot file.

On Windows the game's main thread is named "Buddha Main", so stop it with
`pkill -x "Buddha Main"`.

## Known limits

- LB + RB + A was tested in Costume Quest Mobile and in `Cq.exe` under Proton (test buttons through
  the same hooks). On Linux the code is in place (`SDL_GameControllerGetButton` is wrapped), but it
  is untested: started outside Steam on a Steam Deck, the game's own SDL 2.0.0 sees no
  controller at all. The log says `controller ... polled by the game` once it sees one.
- **Not yet tested:** that a **real** keyboard is kept away from `Cq.exe`'s menus while the console
  is open. The game also reads DirectInput, which the DLL blanks while the console is open.
- Commands run as typed: some of the developers' leftover commands crash the retail game.
- Costume Quest Mobile's Android developer menu items (fps, frame cap, render scale) aren't in the
  Windows console yet.
