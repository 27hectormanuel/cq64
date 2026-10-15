# Windows build (proof of concept)

`Costume Quest Mobile.exe` runs Costume Quest's Android version on Windows **without an Android emulator**. It is
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

- Plays: title screen, menus, new game and the opening cutscenes render correctly (tested under
  Wine/Proton with DXVK on a Steam Deck: ~170 fps on the menus, a steady 30 in game, the game's
  own simulation rate).
- **Sound**: FMOD's mixer is pulled by a thread in `win_main.cpp` (what `FMODAudioDevice` does on
  Android) and played through SDL audio.
- **Textures**: any of the four GPU-specific APKs works (Adreno, Mali, PowerVR, Tegra; see
  [apk-variants.md](apk-variants.md)). The extension list shown to the game adds ATC and PVRTC,
  which the host decodes to RGBA8, and the standard S3TC names (ANGLE has DXT1/3/5); without them
  the game uploads those textures with an invalid format and the world draws black.
  `CQ64_GL_STATS=1` logs the texture formats the game uploads.
- Input: the mouse acts as touch, keys as Android key codes, controllers go to the virtual pad
  (optimized variant). Lightly tested.
- **Debug console**: F1 or `` ` `` opens the game's Lua command line over the game (both variants;
  `host/src/win/console_win.cpp`). Enter runs a line, Tab completes from every function the game's
  Lua exposes, Up/Down is history, PgUp/PgDn scrolls, Ctrl+V pastes, Ctrl+L clears, Escape closes.
  Output is the same as the Android console's (`cq64-data/files/console.out` too). The Android
  developer menu's other entries (fps, frame cap, render scale) aren't there yet.
- Debugging: `cq64.log`; F12 or a file named `screenshot` in `cq64-data/files` saves the game's
  own frame; a file named `input` there sends input (`key 66`, `tap 0.5 0.4`, `wait 500`,
  `sdlkey F1`, `type debug.GetPlayer()`, one per line); `CQ64_DUMP_SHADERS=1` writes every shader the game compiles to `cq64-data/files/shaders`
  (all debug switches: [debug-console.md](debug-console.md#8-debug-switches-both-builds)).

## Build (from Linux or WSL2)

Needs what the Android build needs (the NDK for the guest runtime and headers, CMake + Ninja)
plus `curl`, `unzip`, `zip`, `zstd`. No APK is needed:

```sh
scripts/build_win.sh            # or: scripts/build_win.sh original
```

`scripts/fetch_win_deps.sh` (run by `build_win.sh`) downloads, checksum-verified, into
`third_party/win` (set `WIN_DEPS` to put them elsewhere; ~700 MB):
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the cross-compiler), SDL2 and zlib (MSYS2
packages), and ANGLE from [Electron](https://github.com/electron/electron)'s official Windows
release (Chromium's ANGLE build).

The result is `out/windows/` and `out/cq64-windows-<variant>.zip`: `Costume Quest Mobile.exe` with the guest runtime
(`libguestrt.so`, ARM code from `guest/`) built in via `src/win/guest_rt.S.in`, the SDL2/ANGLE DLLs,
their licenses and a README. No game files: drag your APK onto `Costume Quest Mobile.exe`; it reads the game's ARM
libraries (`lib/armeabi-v7a/`) and data (`assets/`) from the APK each time and remembers the APK in
`cq64-data/apk.txt`. Files named like the libraries next to `Costume Quest Mobile.exe` (`libguest_Cq.so`, ...) take
precedence, for development.
