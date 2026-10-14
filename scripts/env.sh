# SPDX-License-Identifier: CC0-1.0
# Locates the build tools. Sourced by the other scripts; every variable can be overridden.
#   ANDROID_HOME  Android SDK (default: $ANDROID_SDK_ROOT or ~/Android/Sdk)
#   NDK           NDK root (default: newest $ANDROID_HOME/ndk/*)
#   NDK_TC        NDK LLVM toolchain for this machine (default: $NDK/toolchains/llvm/prebuilt/*,
#                 e.g. linux-x86_64 on Linux and WSL, darwin-x86_64 on macOS)
#   BUILD_TOOLS   build-tools dir (default: newest $ANDROID_HOME/build-tools/*)
#   ANDROID_JAR   android.jar to compile against (default: platform 34, else newest)
#   CMAKE_BIN     dir with cmake >= 3.22 and ninja (default: newest $ANDROID_HOME/cmake/*/bin)

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ANDROID_HOME=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}

newest() { ls -d "$1"/*/ 2>/dev/null | sort -V | tail -1 | sed 's:/$::' || true; }

NDK=${NDK:-$(newest "$ANDROID_HOME/ndk")}
export NDK_TC=${NDK_TC:-$(newest "$NDK/toolchains/llvm/prebuilt")}
BUILD_TOOLS=${BUILD_TOOLS:-$(newest "$ANDROID_HOME/build-tools")}
if [ -z "${ANDROID_JAR:-}" ]; then
    ANDROID_JAR=$ANDROID_HOME/platforms/android-34/android.jar
    [ -f "$ANDROID_JAR" ] || ANDROID_JAR=$(newest "$ANDROID_HOME/platforms")/android.jar
fi
if [ -z "${CMAKE_BIN:-}" ] && [ -d "$ANDROID_HOME/cmake" ]; then
    CMAKE_BIN=$(newest "$ANDROID_HOME/cmake")/bin
fi
[ -n "${CMAKE_BIN:-}" ] && export PATH=$CMAKE_BIN:$PATH

# Where the decompiled games live (scripts/decompile.sh, scripts/decompile_mmoj.sh write them).
DECOMP=${DECOMP:-$ROOT/work/decompiled}
MMOJ_WORK=${MMOJ_WORK:-$ROOT/work/mmoj}
DECOMP_MMOJ=${DECOMP_MMOJ:-$MMOJ_WORK/decompiled}
TP=$ROOT/third_party

require() {
    local ok=1
    [ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "NDK not found (set NDK=...)" >&2; ok=0; }
    [ -x "$NDK_TC/bin/clang" ] || { echo "NDK toolchain for this machine not found (set NDK_TC=...)" >&2; ok=0; }
    [ -x "$BUILD_TOOLS/apksigner" ] || { echo "build-tools not found (set BUILD_TOOLS=...)" >&2; ok=0; }
    [ -f "$ANDROID_JAR" ] || { echo "android.jar not found (set ANDROID_JAR=...)" >&2; ok=0; }
    command -v cmake >/dev/null && command -v ninja >/dev/null || { echo "cmake/ninja not found (set CMAKE_BIN=...)" >&2; ok=0; }
    [ -f "$TP/apktool.jar" ] && [ -d "$TP/dynarmic" ] && [ -d "$TP/boost/boost" ] ||
        { echo "third-party sources missing: run scripts/fetch_deps.sh" >&2; ok=0; }
    [ "$ok" = 1 ] || exit 1
}
