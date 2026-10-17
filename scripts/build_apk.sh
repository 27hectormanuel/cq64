#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Builds a 64-bit-capable Costume Quest APK from the apktool decompile (scripts/decompile.sh).
#   scripts/build_apk.sh [abi...]      default ABIs: arm64-v8a x86_64
# VARIANT=optimized (default): controller support (with PC-style battle prompts and button icons),
#   developer menu (Lua console, FPS counter, frame cap, render resolution), full screen, Game Mode,
#   controller fixes for the journal, stamp shop and battle screen. Output: $OUT_DIR/costumequest64.apk
# VARIANT=original: the game exactly as the 32-bit APK plays it, with only the changes running it on
#   a 64-bit-only phone needs (and without the repack's androeed.ru splash, if present).
#   Output: $OUT_DIR/costumequest64-original.apk
# (default $OUT_DIR: out/; signed with the debug key)
# ICON=<image> uses a higher-resolution picture for the launcher icon.
# FULL=1 includes the 1 GB game data pack. SPLIT=1 instead writes it to $OUT_DIR/cqdata.zip, to be
# installed with install_split.sh (for phones without room for the full APK).
set -euo pipefail
source "$(dirname "$0")/env.sh"
require
HERE=$ROOT
OUT_DIR=${OUT_DIR:-$ROOT/out}
BT=$BUILD_TOOLS
VARIANT=${VARIANT:-optimized}
case "$VARIANT" in optimized) APK_NAME=costumequest64.apk ;; original) APK_NAME=costumequest64-original.apk ;;
    *) echo "VARIANT must be optimized or original" >&2; exit 1 ;; esac
ABIS=("$@")
[ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a x86_64)
STRIP=$NDK_TC/bin/llvm-strip
[ -f "$DECOMP/apktool.yml" ] || { echo "no decompiled game in $DECOMP: run scripts/decompile.sh <apk>" >&2; exit 1; }

mkdir -p "$OUT_DIR"

echo "== guest runtime"
NDK=$NDK "$HERE/guest/build.sh" "$OUT_DIR"

for abi in "${ABIS[@]}"; do
    echo "== host library ($abi)"
    bdir=$HERE/build/cq-$VARIANT-$abi
    if [ ! -f "$bdir/build.ninja" ]; then
        cmake -S "$HERE/host" -B "$bdir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$abi" -DANDROID_PLATFORM=26 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DGAME=cq -DCQ_VARIANT="$VARIANT" >/dev/null
    fi
    cmake --build "$bdir" -j"$(nproc)"
done

echo "== staging ($VARIANT)"
STAGE=$HERE/work/stage
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -r "$DECOMP/AndroidManifest.xml" "$DECOMP/apktool.yml" "$DECOMP/res" "$DECOMP/smali" "$STAGE/"
# The only game asset is the 1 GB data pack (assets/ant.mp3), zipped in afterwards (FULL=1). Other
# files in assets/ are not needed (some repacked APKs add splash screens there) and are left out.
cp "$HERE/java_patch/HostLoader.smali" "$STAGE/smali/com/doublefine/cq/"
# Adaptive launcher icon made from the game's own icon (both variants; see the XML for details).
mkdir -p "$STAGE/res/drawable" "$STAGE/res/drawable-anydpi-v26"
cp "$HERE/java_patch/res/drawable/cq_launcher.xml" "$HERE/java_patch/res/drawable/cq_launcher_mono.xml" "$STAGE/res/drawable/"
cp "$HERE/java_patch/res/drawable-anydpi-v26/cq_launcher.xml" "$STAGE/res/drawable-anydpi-v26/"
# ICON=<png/jpg/webp>: a higher-resolution square picture for the adaptive icon (e.g. the game's
# 1024 px store icon) instead of the APK's 144 px one.
if [ -n "${ICON:-}" ]; then
    ext=$(echo "${ICON##*.}" | tr 'A-Z' 'a-z'); [ "$ext" = jpeg ] && ext=jpg
    case "$ext" in png | jpg | webp) ;; *) echo "ICON must be a .png, .jpg or .webp file" >&2; exit 1 ;; esac
    mkdir -p "$STAGE/res/drawable-nodpi"
    cp "$ICON" "$STAGE/res/drawable-nodpi/cq_icon_art.$ext"
    sed -i 's|@drawable/ic_launcher|@drawable/cq_icon_art|' "$STAGE/res/drawable-anydpi-v26/cq_launcher.xml"
fi
if [ "$VARIANT" = optimized ]; then
    mkdir -p "$STAGE/res/xml"
    cp "$HERE/java_patch/res/xml/game_mode_config.xml" "$STAGE/res/xml/"
fi

python3 - "$STAGE" "$VARIANT" "${SPLIT:-0}" <<'EOF'
import glob, os, re, sys
stage, variant, split = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
def add_app_attrs(s, **attrs):
    """Adds android:<name>="<value>" to <application> unless already present."""
    m = re.search(r"<application\b[^>]*>", s)
    tag = m.group(0)
    for k, v in attrs.items():
        if f"android:{k}=" not in tag:
            tag = tag.replace("<application ", f'<application android:{k}="{v}" ', 1)
    return s[:m.start()] + tag + s[m.end():]
# Java: load libcqhost instead of the three 32-bit libraries.
p = f"{stage}/smali/org/libsdl/app/SDLActivity.smali"
s = open(p).read()
load = '    invoke-static {v1}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V'
n = s.count(load)
assert n == 5, n
s = s.replace('const-string v1, "fmodex"\n\n' + load, 'const-string v1, "fmodex"\n\n    invoke-static {}, Lcom/doublefine/cq/HostLoader;->load()V', 1)
for lib in ("fmodevent", "Cq"):
    s = s.replace(f'const-string v1, "{lib}"\n\n' + load, f'const-string v1, "{lib}"', 1)
assert s.count(load) == 2  # debug-library fallback path, now unreachable
open(p, "w").write(s)
# Manifest: native libs extracted to disk (HostLoader loads the 32-bit libraries from there);
# debuggable (adb run-as: install_split.sh, dev tools) except for a full original APK.
p = f"{stage}/AndroidManifest.xml"
s = open(p).read()
s = add_app_attrs(s, extractNativeLibs="true")
# Launcher icon: the adaptive one (java_patch/res/drawable*/cq_launcher.xml).
assert s.count('android:icon="@drawable/ic_launcher"') == 1
s = s.replace('android:icon="@drawable/ic_launcher"', 'android:icon="@drawable/cq_launcher"')
if variant == "optimized" or split:
    s = add_app_attrs(s, debuggable="true")
open(p, "w").write(s)
# Some repacked APKs reference an injected class "億" (signature-spoofing shim) whose code is
# not present; point its call sites back at the framework methods it wraps.
for f in glob.glob(f"{stage}/smali/億*.smali"):  # the shim itself, if the decompile has it
    os.remove(f)
subs = [
    (r"\n\s*invoke-static \{[^}]*\}, L億;->三\(Landroid/content/Context;\)V\n", "\n"),
    (r"invoke-static (\{[^}]*\}), L億;->getPackageInfo\(Landroid/content/pm/PackageManager;Ljava/lang/String;I\)Landroid/content/pm/PackageInfo;",
     r"invoke-virtual \1, Landroid/content/pm/PackageManager;->getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;"),
    (r"invoke-static (\{[^}]*\}), L億;->length\(Ljava/io/File;\)J", r"invoke-virtual \1, Ljava/io/File;->length()J"),
    (r"invoke-static (\{[^}]*\}), L億;->invokeHook\(Ljava/lang/reflect/Method;Ljava/lang/Object;\[Ljava/lang/Object;\)Ljava/lang/Object;",
     r"invoke-virtual \1, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
]
for f in glob.glob(f"{stage}/smali/**/*.smali", recursive=True):
    s = open(f).read()
    if "L億;" not in s:
        continue
    for a, b in subs:
        s = re.sub(a, b, s)
    assert "L億;" not in s, f
    open(f, "w").write(s)
# Some repacked APKs start a splash screen from CqActivity through an injected class disguised as
# android.support.v4.net.MobileLinkQualityInfo; drop that call if present.
p = f"{stage}/smali/com/doublefine/cq/CqActivity.smali"
s = open(p).read()
call = "\n    invoke-static {v0}, Landroid/support/v4/net/MobileLinkQualityInfo;->iLLLLiiIIiIILLii(Landroid/content/Context;)V\n"
assert s.count(call) <= 1
open(p, "w").write(s.replace(call, "\n"))
# Android 15+ refuses to install apps targeting SDK < 24.
p = f"{stage}/apktool.yml"
s = open(p).read()
s, n = re.subn(r"targetSdkVersion: '?(\d+)'?", lambda m: f"targetSdkVersion: {max(int(m.group(1)), 24)}", s)
assert n == 1
open(p, "w").write(s)
if variant == "original":
    sys.exit(0)
# ---- optimized variant only from here
# Dev tools (java_patch/src, shipped as classes2.dex): three-finger tap menu, FPS overlay,
# Game Mode frame cap and game controller input. Hook them into CqActivity.
p = f"{stage}/smali/com/doublefine/cq/CqActivity.smali"
s = open(p).read()
assert not any(m in s for m in ("dispatchTouchEvent", "dispatchKeyEvent", "dispatchGenericMotionEvent", "onResume"))
s += """
.method public dispatchTouchEvent(Landroid/view/MotionEvent;)Z
    .locals 1
    invoke-static {p1}, Lcom/doublefine/cq/DevTools;->onTouch(Landroid/view/MotionEvent;)V
    invoke-super {p0, p1}, Lorg/libsdl/app/SDLActivity;->dispatchTouchEvent(Landroid/view/MotionEvent;)Z
    move-result v0
    return v0
.end method

.method public dispatchKeyEvent(Landroid/view/KeyEvent;)Z
    .locals 1
    invoke-static {p1}, Lcom/doublefine/cq/DevTools;->onKey(Landroid/view/KeyEvent;)Z
    move-result v0
    if-nez v0, :handled
    invoke-super {p0, p1}, Lorg/libsdl/app/SDLActivity;->dispatchKeyEvent(Landroid/view/KeyEvent;)Z
    move-result v0
    :handled
    return v0
.end method

.method public dispatchGenericMotionEvent(Landroid/view/MotionEvent;)Z
    .locals 1
    invoke-static {p1}, Lcom/doublefine/cq/DevTools;->onMotion(Landroid/view/MotionEvent;)Z
    move-result v0
    if-nez v0, :handled
    invoke-super {p0, p1}, Lorg/libsdl/app/SDLActivity;->dispatchGenericMotionEvent(Landroid/view/MotionEvent;)Z
    move-result v0
    :handled
    return v0
.end method

.method protected onResume()V
    .locals 0
    invoke-super {p0}, Lorg/libsdl/app/SDLActivity;->onResume()V
    invoke-static {p0}, Lcom/doublefine/cq/DevTools;->onResume(Landroid/app/Activity;)V
    return-void
.end method
"""
open(p, "w").write(s)
# Render resolution: retail restrictTo720p() is true only on one Kindle model and caps the render
# buffer at 1280x720. Here DevTools sets the cap from the dev menu's resolution setting (default
# 16:9 at full display height; a 20:9 buffer cuts off the UI) and it returns false.
p = f"{stage}/smali/com/doublefine/cq/CqActivity.smali"
s = open(p).read()
s, n = re.subn(r"\.method private restrictTo720p\(\)Z\n.*?\.end method",
               ".method private restrictTo720p()Z\n    .locals 1\n"
               "    invoke-static {p0}, Lcom/doublefine/cq/DevTools;->force720(Landroid/app/Activity;)Z\n"
               "    move-result v0\n    return v0\n.end method",
               s, flags=re.S)
assert n == 1
# Immersive full screen is (re)applied by DevTools whenever the game window regains focus.
s += """
.method public onWindowFocusChanged(Z)V
    .locals 0
    invoke-super {p0, p1}, Lorg/libsdl/app/SDLActivity;->onWindowFocusChanged(Z)V
    invoke-static {p0, p1}, Lcom/doublefine/cq/DevTools;->onFocus(Landroid/app/Activity;Z)V
    return-void
.end method
"""
open(p, "w").write(s)
# Manifest: categorize as a game (Game Dashboard, Game Mode) and declare Game Mode support.
p = f"{stage}/AndroidManifest.xml"
s = open(p).read()
s = add_app_attrs(s, appCategory="game", isGame="true")
s = s.replace('<meta-data android:name="isGame" android:value="true"/>',
              '<meta-data android:name="isGame" android:value="true"/>\n'
              '        <meta-data android:name="android.game_mode_config" android:resource="@xml/game_mode_config"/>', 1)
assert "game_mode_config" in s
open(p, "w").write(s)
EOF

for abi in "${ABIS[@]}"; do
    bdir=$HERE/build/cq-$VARIANT-$abi
    d=$STAGE/lib/$abi
    mkdir -p "$d"
    "$STRIP" -o "$d/libcqhost.so" "$bdir/libcqhost.so"
    cp "$OUT_DIR/libguestrt.so" "$d/libguest_rt.so"
    for l in Cq fmodex fmodevent; do cp "$DECOMP/lib/armeabi-v7a/lib$l.so" "$d/libguest_$l.so"; done
done

echo "== apktool"
UNSIGNED=$OUT_DIR/cq-unaligned.apk
java -jar "$TP/apktool.jar" b "$STAGE" -o "$UNSIGNED"
if [ "$VARIANT" = optimized ]; then
    echo "== dev tools dex"
    JBUILD=$OUT_DIR/java
    rm -rf "$JBUILD" && mkdir -p "$JBUILD/classes" "$JBUILD/dex"
    javac --release 8 -Xlint:-options -classpath "$ANDROID_JAR" -d "$JBUILD/classes" \
        $(find "$HERE/java_patch/src" -name '*.java')
    "$BT/d8" --min-api 24 --lib "$ANDROID_JAR" --output "$JBUILD/dex" $(find "$JBUILD/classes" -name '*.class')
    mv "$JBUILD/dex/classes.dex" "$JBUILD/classes2.dex"
    (cd "$JBUILD" && zip -q "$UNSIGNED" classes2.dex)
fi
if [ "${FULL:-0}" = 1 ]; then
    echo "== adding assets/ant.mp3 (stored, uncompressed)"
    (cd "$DECOMP" && zip -0 -q "$UNSIGNED" assets/ant.mp3)
elif [ "${SPLIT:-0}" = 1 ]; then
    echo "== writing data pack to cqdata.zip"
    rm -f "$OUT_DIR/cqdata.zip"
    (cd "$DECOMP" && zip -0 -q "$OUT_DIR/cqdata.zip" assets/ant.mp3)
fi
"$BT/zipalign" -f -p 4 "$UNSIGNED" "$OUT_DIR/$APK_NAME"
rm -f "$UNSIGNED"

KS=$HOME/.android/debug.keystore
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android "$OUT_DIR/$APK_NAME"
ls -la "$OUT_DIR/$APK_NAME"
