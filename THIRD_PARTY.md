# Third-party components

| Component | Where | License | Notes |
|---|---|---|---|
| [dynarmic](https://github.com/azahar-emu/dynarmic) (azahar-emu fork) | fetched into `third_party/dynarmic` | 0BSD | ARM32 dynamic recompiler; its bundled externals (fmt, mcl, xbyak, oaknut, zydis, zycore, robin-map, biscuit, Catch2) carry their own licenses |
| [Boost](https://www.boost.org/) 1.86.0 headers | fetched into `third_party/boost` | BSL-1.0 | used by dynarmic |
| [Apktool](https://apktool.org/) 2.11.1 | fetched to `third_party/apktool.jar` | Apache-2.0 | decompiles and rebuilds the APK |
| [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) 2.8.6 | `third_party/misc/malloc.c` | CC0 / public domain | heap allocator for the guest runtime and the host's guest-memory allocator |
| [stb_sprintf](https://github.com/nothings/stb) 1.10 | `third_party/misc/stb_sprintf.h` | MIT or public domain | printf family for the guest runtime |

`scripts/fetch_deps.sh` pins the exact dynarmic commit and checks the SHA-256 of the Boost and
Apktool downloads.

The game itself — `libCq.so`, the FMOD Ex libraries, the Java code and the data pack — belongs to
Double Fine Productions and its licensors and is **not** part of this project. You need your own
copy of the game to build anything.
