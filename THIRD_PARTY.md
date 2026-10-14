# Third-party components

| Component | Where | License | Notes |
|---|---|---|---|
| [dynarmic](https://github.com/azahar-emu/dynarmic) (azahar-emu fork) | fetched into `third_party/dynarmic` | 0BSD | ARM32 dynamic recompiler; its bundled externals (fmt, mcl, xbyak, oaknut, zydis, zycore, robin-map, biscuit, Catch2) carry their own licenses |
| [Boost](https://www.boost.org/) 1.86.0 headers | fetched into `third_party/boost` | BSL-1.0 | used by dynarmic |
| [Apktool](https://apktool.org/) 2.11.1 | fetched to `third_party/apktool.jar` | Apache-2.0 | decompiles and rebuilds the APK |
| [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) 2.8.6 | `third_party/misc/malloc.c` | CC0 / public domain | heap allocator for the guest runtime and the host's guest-memory allocator |
| [stb_sprintf](https://github.com/nothings/stb) 1.10 | `third_party/misc/stb_sprintf.h` | MIT or public domain | printf family for the guest runtime |

The Windows build (docs/windows.md) also uses, fetched by `scripts/fetch_win_deps.sh` into
`third_party/win` with pinned SHA-256 checksums: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(Apache-2.0 with LLVM exceptions; MinGW-w64 runtime, whose `libwinpthread-1.dll` ships with the
build), SDL2 and zlib (zlib license, MSYS2 packages), and ANGLE (BSD-3-Clause; `libEGL.dll`,
`libGLESv2.dll` and `d3dcompiler_47.dll` from Electron's Windows release, whose
`LICENSES.chromium.html` ships with the build). jni.h and the Khronos EGL/GLES headers come from
the Android NDK.

`scripts/fetch_deps.sh` pins the exact dynarmic commit and checks the SHA-256 of the Boost and
Apktool downloads. It then applies this project's dynarmic changes from `patches/` (0BSD, like
dynarmic): `dynarmic-arm64-fast-dispatch.patch` adds the fast dispatch lookup table to the arm64
backend's A32 dispatcher.

The games themselves — Costume Quest's `libCq.so`, Middle Manager of Justice's `libmmoj.so` (built
on the Moai SDK), the FMOD Ex libraries, their Java code and data packs — belong to Double Fine
Productions and their licensors and are **not** part of this project. You need your own copy of a
game to build anything.
