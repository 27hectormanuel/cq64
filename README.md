# CQ64 — Costume Quest on 64-bit-only Android

Costume Quest's Android release ships only 32-bit ARM (`armeabi-v7a`) native code. Phones such as
the Pixel 8 and later have no 32-bit support at all, so the game cannot start on them. This
project rebuilds your own copy of the game into an APK that runs there: a small 64-bit host
library embeds an ARM32 dynamic recompiler and runs the game's original native libraries inside
it.

It is a compatibility layer, not a re-implementation: **no game code or data is included**, and
you need your own copy of the game's APK to build anything.

Tested with Costume Quest for Android **v1.1.7 (versionCode 121030)** on a Pixel 8 Pro
(Android 17, arm64). An x86_64 build for the Android emulator is also produced.

## How it works

- **`host/`** — `libcqhost.so`, a 64-bit Android library (C++20, CMake):
  - an ELF loader for the 32-bit libraries (`libCq.so`, FMOD Ex, FMOD Event and the guest
    runtime) in a 4 GiB guest address space;
  - [dynarmic](https://github.com/azahar-emu/dynarmic) running the ARM32 code, one JIT per thread;
  - "thunks" implementing the libraries' imports on the host: libc, pthreads, EGL/OpenGL ES,
    Android logging/assets, and a JNI bridge so the 32-bit code can talk to the Java side;
  - fixes for running in a 64-bit process (e.g. a synthetic ARMv7 `/proc/cpuinfo`, without which
    FMOD finds no audio output);
  - extras: a revived Lua debug console, an FPS counter and frame cap, a 16:9 render resolution
    picker (the UI is laid out for 16:9), game controller support including battle prompts, and
    small same-size patches applied to the data pack as it is read (white menu highlight).
- **`guest/`** — `libguestrt.so`, a tiny ARM32 runtime (string functions, `printf` family, dlmalloc,
  compiler-rt helpers) loaded next to the game in place of 32-bit bionic.
- **`java_patch/`** — `HostLoader` (loads `libcqhost` instead of the 32-bit libraries) and
  `DevTools` (developer menu, controller input, immersive mode, Game Mode frame rates).
- **`scripts/build_apk.sh`** — patches the decompiled APK's smali, adds the libraries, and signs
  the result with your debug key.

### Why the build decompiles your APK

Nothing from the game is stored in this repository. At build time `scripts/decompile.sh` unpacks
**your** APK with apktool, because the port has to change the game's Java side, not just add a
library: `SDLActivity` loads the three 32-bit libraries with `System.loadLibrary`, which fails on a
64-bit-only device, so those calls are rewritten to load `libcqhost`; `CqActivity` gets hooks for
the developer menu, controller input and the resolution setting; and a few manifest attributes
change. The same step provides the game's 32-bit libraries and data pack, which are copied into the
output APK unchanged. apktool then rebuilds the patched app.

## Other 32-bit-only games

The core — the ARM32 ELF loader, dynarmic CPU, libc/pthread/EGL/GLES/JNI thunks and the guest
runtime — is not specific to Costume Quest, so the same approach could run other Android games that
ship only `armeabi-v7a` native code. This is **untested**: other games will need the imports they
use implemented as thunks, their own Java-side patches, and none of the Costume Quest specific parts
(`gamepad.cpp`'s battle hooks, `datapatch.cpp`, the FMOD workarounds) apply as is.

## Building

Requirements: Linux, JDK 17+, Python 3, `git`, `curl`, `zip`, and the Android SDK with an NDK
(tested with NDK 29), build-tools, platform 34 and CMake ≥ 3.22 with Ninja.

```sh
scripts/fetch_deps.sh                      # dynarmic, Boost headers, apktool (pinned)
scripts/decompile.sh CostumeQuest.apk      # your copy of the game -> work/decompiled
FULL=1 scripts/build_apk.sh arm64-v8a      # -> out/costumequest64.apk
```

`scripts/env.sh` finds the SDK in `$ANDROID_HOME` (or `~/Android/Sdk`); set `NDK`,
`BUILD_TOOLS`, `ANDROID_JAR` or `CMAKE_BIN` to override individual tools.

Build options:
- `FULL=1` puts the 1 GB data pack into the APK (simplest to install).
- `SPLIT=1` builds a small APK and writes the data pack to `out/cqdata.zip`; install both with
  `scripts/install_split.sh out` (needs about half the free space on the phone).
- ABIs: `arm64-v8a` for phones, `x86_64` for the emulator (both by default).

The build was developed against an APK that contains the data pack as `assets/ant.mp3`. APKs
that ship it as a separate OBB expansion file are untested.

## Using it

- Install with `adb install -r --no-incremental out/costumequest64.apk` (`-r` keeps your saves).
- **Three-finger tap** opens the developer menu: FPS counter, Lua debug command line, frame rate
  cap (follows Android Game Mode by default) and render resolution.
- **Controllers** work everywhere. In battle prompts the coloured icons map to the console
  buttons: A = green, B = red, X = blue/purple, Y = yellow; flick the stick for swipe prompts.
  Connect the controller before starting the game.

## Status

Tested so far through the opening areas, menus, saving/loading and battles. Known issues: a PC mouse pointer
graphic appears on the title screen while a controller is connected; swipe and hold-meter battle
prompts are lightly tested with controllers.

## License

This project is dedicated to the public domain under
[CC0 1.0 Universal](LICENSE): to the extent possible under law, the authors have waived all
copyright and related rights to the code in this repository. You can copy, modify and distribute
it, even commercially, without asking permission. Third-party components keep their own licenses —
see [THIRD_PARTY.md](THIRD_PARTY.md).

Costume Quest is © Double Fine Productions. This project is not affiliated with or endorsed by
Double Fine Productions; it only modifies a copy of the game that you supply.
