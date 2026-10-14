# Windows build (proof of concept)

`cq64.exe` runs Costume Quest's Android version on Windows **without an Android emulator**. It is
the same host as the Android build — dynarmic translating the game's 32-bit ARM code, the libc,
pthread, GL and JNI bridges — packaged as a desktop program:

| On Android | On Windows |
|---|---|
| Java (SDLActivity, CqActivity) creates the window and starts the game | `host/src/win/win_main.cpp` does the same calls in the same order |
| The game's JNI calls reach the real Java VM | `host/src/win/fakejvm.cpp`: a small stand-in VM with the Java classes the game uses (SDL's activity, assets read from your APK, files, clipboard) |
| EGL/OpenGL ES from the phone's driver | ANGLE (`libEGL.dll`, `libGLESv2.dll`): OpenGL ES on Direct3D 11 |
| Touch, keys, controllers | SDL2: the mouse acts as touch, keys as Android key codes, controllers go to the virtual pad |
| Linux system calls | `host/src/win/posix_win.cpp` and the headers in `host/src/win/include` |

## Status

- Boots to the title screen and renders (tested under Wine/Proton with DXVK on a Steam Deck;
  ~170 fps on the menus there).
- **No sound yet**: FMOD's Android output is pulled by a Java thread (`FMODAudioDevice`), which
  the stand-in VM doesn't run yet.
- Mouse, keyboard and controller input are wired but barely tested. No developer menu.

## Build (from Linux or WSL2)

Needs what the Android build needs (the NDK for the guest runtime and headers, CMake + Ninja)
plus `curl`, `unzip`, `zstd`:

```sh
scripts/build_win.sh CostumeQuest.apk           # or: ... original
```

`scripts/fetch_win_deps.sh` (run by `build_win.sh`) downloads, checksum-verified, into
`third_party/win` (set `WIN_DEPS` to put them elsewhere; ~700 MB):
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the cross-compiler), SDL2 and zlib (MSYS2
packages), and ANGLE from [Electron](https://github.com/electron/electron)'s official Windows
release (Chromium's ANGLE build).

The result is `out/windows/`: `cq64.exe`, its DLLs, the game's ARM libraries taken from your APK,
and the licenses of the bundled DLLs. Copy your APK into that folder as `CostumeQuest.apk` (or
pass its path: `cq64.exe D:\Games\CostumeQuest.apk`) and run `cq64.exe`. Saves and settings go to
`cq64-data/` next to it; the log to `cq64.log`. F12 saves a screenshot (`screenshot.bmp`).

As with the Android build, the output contains the game's own code — keep it to yourself.
