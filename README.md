# CQ64 — Double Fine's 32-bit Android games on 64-bit-only phones

Double Fine Productions' Android games ship only 32-bit ARM (`armeabi-v7a`) native code. Phones
such as the Pixel 8 and later have no 32-bit support at all, so these games cannot start on them.
This project rebuilds **your own copy** of a game into an APK that runs there: a small 64-bit host
library embeds an ARM32 dynamic recompiler and runs the game's original native libraries inside
it.

It is a compatibility layer, not a re-implementation: **no game code or data is included**, and
you need your own copy of the game to build anything.

## Supported games

| Game | Version tested | Status |
|---|---|---|
| **Costume Quest** | v1.1.7 (versionCode 121030) | Playable — tested through the opening areas, menus, saving/loading and battles; controller support |
| **Middle Manager of Justice** | v1.3.4 (versionCode 121340) | **Experimental** — boots, intro and title screen render correctly, full screen, Game Mode frame rates; gameplay barely tested |

Both were tested on a Pixel 8 Pro (Android 17, arm64, Mali GPU).

## How it works

- **`host/`** — `libcqhost.so`, a 64-bit Android library (C++20, CMake), built per game:
  - a game-independent core: an ELF loader for the 32-bit libraries in a 4 GiB guest address
    space, [dynarmic](https://github.com/azahar-emu/dynarmic) running the ARM32 code (one JIT per
    thread), and "thunks" implementing the libraries' imports on the host — libc, libm, pthreads,
    EGL/OpenGL ES, Android logging/assets, and a JNI bridge so the 32-bit code can talk to Java;
  - fixes for running in a 64-bit process (e.g. a synthetic ARMv7 `/proc/cpuinfo`, without which
    FMOD finds no audio output) and for other GPUs (ATC-compressed textures, which only Qualcomm
    Adreno GPUs support, are decoded to RGBA on the fly);
  - a **game profile** per game (`game_cq.cpp`, `game_mmoj.cpp`, chosen with CMake `-DGAME=`):
    package name, which libraries to load, the Java native methods to forward, and game-specific
    extras.
  - Costume Quest extras: a revived Lua debug console, FPS counter and frame cap, 16:9 render
    resolution picker, game controller support including battle prompts judged like the PC
    version, and small patches applied to the data pack as it is read (white menu highlight;
    a button icon library with the Xbox controller icons added to the mobile ones).
- **`guest/`** — `libguestrt.so`, a tiny ARM32 runtime (string functions, `printf` family,
  dlmalloc, setjmp, compiler-rt helpers) loaded next to the game in place of 32-bit bionic.
- **`java_patch/`** — `HostLoader` (loads `libcqhost` instead of the 32-bit libraries) and a
  `DevTools` class per game (immersive full screen, Android Game Mode frame rates; for Costume
  Quest also the developer menu and controller input).
- **`scripts/`** — decompile your APK, patch its smali and manifest, add the libraries, and sign
  the result with your debug key.

### Why the build decompiles your APK

Nothing from the games is stored in this repository. At build time the decompile script unpacks
**your** APK with apktool, because the port has to change the game's Java side, not just add a
library: the game loads its 32-bit libraries with `System.loadLibrary`, which fails on a
64-bit-only device, so those calls are rewritten to load `libcqhost`; activity hooks are added for
full screen, Game Mode and (Costume Quest) the developer menu and controller input; and a few
manifest attributes change. The same step provides the game's 32-bit libraries and data, which go
into the output unchanged. apktool then rebuilds the patched app.

## Building

Requirements: Linux (on Windows: WSL2, [see below](#building-on-windows-wsl2)), JDK 17+, Python 3, `git`, `curl`, `zip`, `unzip`, and the Android SDK with an
NDK (tested with NDK 29), build-tools, platform 34 and CMake ≥ 3.22 with Ninja.

```sh
scripts/fetch_deps.sh          # once: dynarmic, Boost headers, apktool (pinned, checksum-verified)
```

`scripts/env.sh` finds the SDK in `$ANDROID_HOME` (or `~/Android/Sdk`); set `NDK`,
`NDK_TC`, `BUILD_TOOLS`, `ANDROID_JAR` or `CMAKE_BIN` to override individual tools.

### Building on Windows (WSL2)

The scripts are Linux shell scripts; on Windows 10/11 they run unchanged in WSL2, a Linux
system built into Windows. Not tested by the author on Windows (the same steps work on Linux).

1. In PowerShell as administrator: `wsl --install`, restart, and finish setting up Ubuntu.
2. In the Ubuntu terminal, install the tools and the Linux Android SDK:

   ```sh
   sudo apt update && sudo apt install -y openjdk-17-jdk python3 git curl zip unzip
   mkdir -p ~/Android/Sdk/cmdline-tools && cd ~/Android/Sdk/cmdline-tools
   curl -LO https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
   unzip -q commandlinetools-linux-*.zip && mv cmdline-tools latest && rm commandlinetools-linux-*.zip
   yes | latest/bin/sdkmanager --licenses >/dev/null
   latest/bin/sdkmanager "ndk;29.0.14206865" "build-tools;36.0.0" "platforms;android-34" "cmake;4.1.2"
   ```

3. Get the source and your APK into the Linux home folder (building under `/mnt/c` works but
   is much slower):

   ```sh
   cd ~ && git clone https://github.com/27hectormanuel/cq64.git && cd cq64
   cp /mnt/c/Users/<your Windows user>/Downloads/CostumeQuest.apk ~/
   scripts/build_cq.sh ~/CostumeQuest.apk optimized --abi arm64-v8a     # or: original
   ```

4. Install it on the phone. Either open the output folder in Explorer with `explorer.exe out`,
   copy the APK to the phone and open it there, or use Windows' `adb.exe` (Android
   platform-tools for Windows) from the same terminal:

   ```sh
   adb.exe install -r --no-incremental "$(wslpath -w out/costumequest64.apk)"
   ```

   The full APK is the simplest on Windows: `install_split.sh` and `dev_push.sh` call a Linux
   `adb`, which can't see USB devices in WSL2 without extra setup (`usbipd`).

### Costume Quest

There are two variants to choose from:

| Variant | What you get | Output |
|---|---|---|
| **original** | The game 1:1 as the 32-bit APK plays it: touch only, no developer menu, no data changes. Only what running it on a 64-bit-only phone needs is changed (and a repack's androeed.ru splash is removed). | `out/costumequest64-original.apk` |
| **optimized** | Controller support with PC-style battle prompts and Xbox button icons, developer menu (Lua console, FPS counter, frame cap, render resolution), full screen, Game Mode, white menu highlight. | `out/costumequest64.apk` |

One step (fetches the dependencies if needed, decompiles your APK, builds a full APK for phones
and the emulator; asks for the variant if you leave it out):

```sh
scripts/build_cq.sh CostumeQuest.apk original     # or: optimized
adb install -r --no-incremental out/costumequest64-original.apk
```

Options: `--split` (small APK + `out/cqdata.zip`, see below), `--abi arm64-v8a` or
`--abi x86_64` to build for one ABI only, `--icon <picture>` (see below).

**Launcher icon:** both variants get an adaptive icon, so modern launchers (e.g. Pixel on
Android 17) show it in their own shape instead of shrinking the old square icon into a white
circle. It is made from the 144 px icon in your APK; for a sharper one, pass a higher-resolution
square picture of the same icon (e.g. the game's 1024 px store icon, `.png`/`.jpg`/`.webp`) with
`--icon` (or `ICON=` for `build_apk.sh`). The picture is not included here for the same reason
the game isn't.

Step by step:

```sh
scripts/decompile.sh CostumeQuest.apk                        # your copy of the game -> work/decompiled
VARIANT=optimized FULL=1 scripts/build_apk.sh arm64-v8a      # -> out/costumequest64.apk
adb install -r --no-incremental out/costumequest64.apk
```

- `VARIANT=original` or `VARIANT=optimized` (the default).
- `FULL=1` puts the 1 GB data pack into the APK (simplest to install).
- `SPLIT=1` builds a small APK and writes the data pack to `out/cqdata.zip`; install both with
  `scripts/install_split.sh out` (`VARIANT=original scripts/install_split.sh out` for the
  original variant; needs about half the free space on the phone).
- ABIs: `arm64-v8a` for phones, `x86_64` for the Android emulator (both by default). Both
  variants run on an x86_64 emulator image (tested on API 36).
- Developed against an APK that contains the data pack as `assets/ant.mp3`; APKs that ship it as
  a separate OBB expansion file are untested.

### Middle Manager of Justice (experimental)

```sh
scripts/decompile_mmoj.sh MiddleManagerOfJustice.xapk   # or: <the .apk> <its main .obb>
scripts/build_mmoj.sh arm64-v8a                         # -> out/mmoj64.apk
scripts/install_mmoj.sh                                 # installs it and copies the OBB to the phone
```

The game data is an OBB expansion file (`main.121340.com.doublefine.mmoj.obb`), which stays in
the phone's `Android/obb/com.doublefine.mmoj/` folder rather than inside the APK.

## Using it

- `adb install -r` keeps your saves when updating. If the original 32-bit game is installed,
  uninstall it first (same package name, different signature).
- **Full screen and Game Mode** in both games: Performance = uncapped (display's highest
  refresh rate), Standard = 60 fps, Battery = 30 fps.
- Costume Quest:
  - **Three-finger tap** (or **LB + RB + A** on a controller) opens the developer menu: FPS
    counter, Lua debug command line, frame rate cap and render resolution. The command line
    lists every function the game's Lua exposes (tap to select, tap again to run; A runs with a
    controller) and filters the list as you type (⌨ button / X). How the debug console works,
    and how to send Lua commands over adb: [docs/debug-console.md](docs/debug-console.md).
  - **High frame rate** (developer menu → FPS & Performance, experimental): runs the game world at 60 (or 120)
    simulation ticks per second instead of 30, as Costume Quest 2's "60 FPS" option does; the
    world otherwise shows 30 distinct frames a second however fast it is drawn. **Performance
    boost** gives the engine's thread pools worker threads (as on PC) and keeps the game's
    busiest thread on the fastest core with Android performance hints. 60 Hz needs the CPU at
    full speed: a warm, throttled phone can end up below a steady 30. Both apply on restart.
  - **Controllers** work everywhere. Battle prompts are played with the buttons, as in the PC
    version: press, mash or time the button the prompt shows (A = green, B = red, X = blue,
    Y = yellow), flick the left stick or d-pad for swipe prompts, and wiggle the left stick to
    rub. After controller input the battle screen shows Xbox button icons and hides the touch
    targets; touching the screen switches back to the touch layout. Connect the controller
    before starting the game.

## Other Double Fine Android apps and 32-bit apps

The core is not specific to these two games, so the same approach could bring other Android apps
that ship only `armeabi-v7a` native code to 64-bit-only phones — other **Double Fine Productions**
Android releases first of all, since they share engines and middleware (FMOD, Moai, Double Fine's
own engine) with the games above, and in principle **any 32-bit-only Android app or game**.

This is **untested**. A new app needs:

- a game profile (`host/src/game_<name>.cpp`) listing its libraries and Java native methods
  (generated from the APK's `native` method declarations, as was done for MMoJ);
- thunks for any imports the core does not implement yet (the loader reports missing symbols);
- Java-side patches to load `libcqhost` instead of its 32-bit libraries;
- possibly fixes like the ATC texture decoding or manifest changes needed for modern Android.

Contributions adding games are welcome.

## Status and known issues

- Costume Quest: a PC mouse-pointer graphic appears on the title screen while a controller is
  connected; swipe prompts are lightly tested with controllers (they rarely come up in play).
- Middle Manager of Justice: experimental — expect bugs past the title screen; no controller
  support or developer menu yet; networking (store, analytics) is stubbed out.

## License

This project is dedicated to the public domain under
[CC0 1.0 Universal](LICENSE): to the extent possible under law, the authors have waived all
copyright and related rights to the code in this repository. You can copy, modify and distribute
it, even commercially, without asking permission. Third-party components keep their own licenses —
see [THIRD_PARTY.md](THIRD_PARTY.md).

Costume Quest and Middle Manager of Justice are © Double Fine Productions. This project is not
affiliated with or endorsed by Double Fine Productions; it only modifies copies of the games that
you supply.
