#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Builds a 64-bit-capable Middle Manager of Justice APK (experimental) from the decompile made by
# scripts/decompile_mmoj.sh.
#   scripts/build_mmoj.sh [abi...]      default ABIs: arm64-v8a
# Output: $OUT_DIR/mmoj64.apk (default out/; signed with the debug key). The game data stays in its
# OBB file, which scripts/install_mmoj.sh copies to the phone.
set -euo pipefail
source "$(dirname "$0")/env.sh"
require
HERE=$ROOT
DECOMP=$DECOMP_MMOJ
OUT_DIR=${OUT_DIR:-$ROOT/out}
BT=$BUILD_TOOLS
ABIS=("$@")
[ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a)
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
PKG=com.doublefine.mmoj
[ -f "$DECOMP/apktool.yml" ] || { echo "no decompiled game in $DECOMP: run scripts/decompile_mmoj.sh first" >&2; exit 1; }
mkdir -p "$OUT_DIR"

echo "== guest runtime"
NDK=$NDK "$HERE/guest/build.sh" "$OUT_DIR"
for abi in "${ABIS[@]}"; do
    echo "== host library ($abi)"
    bdir=$HERE/build/mmoj-$abi
    if [ ! -f "$bdir/build.ninja" ]; then
        cmake -S "$HERE/host" -B "$bdir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$abi" -DANDROID_PLATFORM=26 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DGAME=mmoj >/dev/null
    fi
    cmake --build "$bdir" -j"$(nproc)"
done

echo "== staging"
STAGE=$HERE/work/stage-mmoj
rm -rf "$STAGE"
mkdir -p "$STAGE/smali/com/doublefine/cq"
cp -r "$DECOMP/AndroidManifest.xml" "$DECOMP/apktool.yml" "$DECOMP/res" "$DECOMP/smali" "$DECOMP/assets" "$STAGE/"
sed "s#/data/data/com.doublefine.cq/#/data/data/$PKG/#" "$HERE/java_patch/HostLoader.smali" \
    > "$STAGE/smali/com/doublefine/cq/HostLoader.smali"
mkdir -p "$STAGE/res/xml"
cp "$HERE/java_patch/res/xml/game_mode_config.xml" "$STAGE/res/xml/"

python3 - "$STAGE" <<'PY'
import re, sys
stage = sys.argv[1]
def add_app_attrs(s, **attrs):
    """Adds android:<name>="<value>" to <application> unless already present."""
    m = re.search(r"<application\b[^>]*>", s)
    tag = m.group(0)
    for k, v in attrs.items():
        if f"android:{k}=" not in tag:
            tag = tag.replace("<application ", f'<application android:{k}="{v}" ', 1)
    return s[:m.start()] + tag + s[m.end():]
# Java: load libcqhost instead of the three 32-bit libraries.
p = f"{stage}/smali/com/doublefine/mmoj/MoaiActivity.smali"
s = open(p).read()
load = "    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V"
for lib in ("fmodex", "fmodevent", "mmoj"):
    a = f'const-string v0, "{lib}"\n\n' + load
    assert s.count(a) == 1, lib
    s = s.replace(a, f'const-string v0, "{lib}"\n\n    invoke-static {{}}, Lcom/doublefine/cq/HostLoader;->load()V' if lib == "fmodex" else f'const-string v0, "{lib}"')
open(p, "w").write(s)
# Manifest: debuggable (adb run-as), native libs extracted to disk.
p = f"{stage}/AndroidManifest.xml"
s = open(p).read()
s = add_app_attrs(s, debuggable="true", extractNativeLibs="true")
# The game handles rotation itself (configChanges=orientation). With targetSdk >= 13 a rotation
# also changes screenSize, which would otherwise destroy and recreate MoaiActivity mid-game (and
# Moai's AKUFinalize crashes on that path).
m = re.search(r'<activity\b[^>]*android:name="com.doublefine.mmoj.MoaiActivity"[^>]*>', s)
tag = m.group(0)
tag = re.sub(r'android:configChanges="([^"]*)"',
             lambda c: 'android:configChanges="' + "|".join(dict.fromkeys(c.group(1).split("|") + ["screenSize", "smallestScreenSize", "screenLayout"])) + '"', tag)
assert "screenSize" in tag
s = s[:m.start()] + tag + s[m.end():]
open(p, "w").write(s)
# Manifest: categorize as a game (Game Dashboard, Game Mode) and declare Game Mode support.
s = open(p).read()
s = add_app_attrs(s, appCategory="game", isGame="true")
s = s.replace("</application>", '    <meta-data android:name="isGame" android:value="true"/>\n'
              '    <meta-data android:name="android.game_mode_config" android:resource="@xml/game_mode_config"/>\n    </application>', 1)
open(p, "w").write(s)
# DevTools (java_patch/mmoj, shipped as classes2.dex): full screen + Game Mode frame rates. Call it
# at the start of MoaiActivity.onResume and onWindowFocusChanged.
p = f"{stage}/smali/com/doublefine/mmoj/MoaiActivity.smali"
s = open(p).read()
def hook(s, header, call):
    i = s.index(header)
    j = s.index(".end method", i)
    body = s[i:j]
    # first instruction: a line that is not blank, a directive, a label or a comment
    lines = body.split("\n")
    for k in range(1, len(lines)):
        t = lines[k].strip()
        if t and not t.startswith((".", ":", "#")):
            lines.insert(k, "    " + call + "\n")
            break
    return s[:i] + "\n".join(lines) + s[j:]
s = hook(s, ".method protected onResume()V", "invoke-static {p0}, Lcom/doublefine/mmoj/DevTools;->onResume(Landroid/app/Activity;)V")
s = hook(s, ".method public onWindowFocusChanged(Z)V", "invoke-static {p0, p1}, Lcom/doublefine/mmoj/DevTools;->onFocus(Landroid/app/Activity;Z)V")
open(p, "w").write(s)
# Android 15+ refuses to install apps targeting SDK < 24.
p = f"{stage}/apktool.yml"
s = open(p).read()
s, n = re.subn(r"targetSdkVersion: '?(\d+)'?", lambda m: f"targetSdkVersion: {max(int(m.group(1)), 24)}", s)
assert n == 1
open(p, "w").write(s)
PY

for abi in "${ABIS[@]}"; do
    bdir=$HERE/build/mmoj-$abi
    d=$STAGE/lib/$abi
    mkdir -p "$d"
    "$STRIP" -o "$d/libcqhost.so" "$bdir/libcqhost.so"
    cp "$OUT_DIR/libguestrt.so" "$d/libguest_rt.so"
    for l in mmoj fmodex fmodevent; do cp "$DECOMP/lib/armeabi-v7a/lib$l.so" "$d/libguest_$l.so"; done
done

echo "== apktool"
UNSIGNED=$OUT_DIR/mmoj-unaligned.apk
java -jar "$TP/apktool.jar" b "$STAGE" -o "$UNSIGNED"
echo "== dev tools dex"
JBUILD=$OUT_DIR/java-mmoj
rm -rf "$JBUILD" && mkdir -p "$JBUILD/classes" "$JBUILD/dex"
javac --release 8 -Xlint:-options -classpath "$ANDROID_JAR" -d "$JBUILD/classes" \
    $(find "$HERE/java_patch/mmoj/src" -name '*.java')
"$BT/d8" --min-api 24 --lib "$ANDROID_JAR" --output "$JBUILD/dex" $(find "$JBUILD/classes" -name '*.class')
mv "$JBUILD/dex/classes.dex" "$JBUILD/classes2.dex"
(cd "$JBUILD" && zip -q "$UNSIGNED" classes2.dex)
"$BT/zipalign" -f -p 4 "$UNSIGNED" "$OUT_DIR/mmoj64.apk"
rm -f "$UNSIGNED"
KS=$HOME/.android/debug.keystore
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android "$OUT_DIR/mmoj64.apk"
ls -la "$OUT_DIR/mmoj64.apk"
