#!/bin/bash
# Packs the DBZ3 Android APK without Gradle:
#   NDK clang (probe) -> aapt2 -> javac (app + SDL3 Java glue) -> d8 -> zipalign -> apksigner
#
# Usage: android/build_apk.sh <cmake-build-dir> <rexglue-sdk-dir> <out.apk>
# Env:   ANDROID_HOME (SDK with build-tools;35.0.0, platforms;android-35),
#        ANDROID_NDK, KEYSTORE (optional, default android/debug.keystore)
#
# The resulting APK contains code derived from YOUR .xex: personal use only,
# never redistribute it.
set -euo pipefail
BUILD=$(realpath "$1"); SDKSRC=$(realpath "$2"); OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
BT=$ANDROID_HOME/build-tools/35.0.0
JAR=$ANDROID_HOME/platforms/android-35/android.jar
TC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64
KEYSTORE=${KEYSTORE:-$HERE/debug.keystore}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p $W/lib/arm64-v8a $W/gen $W/classes $W/dex $W/java

# 1) native libraries: libmain.so + every shared lib it (transitively) needs
#    that the build produced, the GPU plugin, libc++_shared and the probe.
# The SDK writes its shared libs to <sdk>/out/<platform>/, the game to the build dir.
# Only AArch64 ELF files qualify: <sdk>/out/ may also hold the host (x86-64) build.
find_lib() {
  local f
  for f in $(find "$BUILD" "$SDKSRC/out" -name "$1" -type f 2>/dev/null); do
    if $TC/bin/llvm-readelf -h "$f" 2>/dev/null | grep -q "AArch64"; then echo "$f"; return; fi
  done
}
copy_with_deps() {
  local f=$1 base; base=$(basename "$f")
  [ -f "$W/lib/arm64-v8a/$base" ] && return
  cp "$f" "$W/lib/arm64-v8a/"
  for dep in $($TC/bin/llvm-readelf -d "$f" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
    local p; p=$(find_lib "$dep")
    if [ -n "$p" ]; then copy_with_deps "$p"; fi
  done
}
copy_with_deps "$(find_lib libmain.so)"
copy_with_deps "$(find_lib librexgpu-xenos.so)"
cp $TC/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so $W/lib/arm64-v8a/
$TC/bin/aarch64-linux-android29-clang++ -O2 -std=c++17 -shared -fPIC -Wl,-z,max-page-size=16384 \
  -static-libstdc++ "$HERE/app/probe.cpp" -o $W/lib/arm64-v8a/libdbz3probe.so -landroid -lvulkan -llog
$TC/bin/llvm-strip --strip-unneeded $W/lib/arm64-v8a/*.so
for so in $W/lib/arm64-v8a/*.so; do
  $TC/bin/llvm-readelf -h "$so" | grep -q "AArch64" || { echo "ERROR: $so no es AArch64" >&2; exit 1; }
done
ls -la $W/lib/arm64-v8a

# 2) resources + manifest
$BT/aapt2 compile --dir "$HERE/app/res" -o $W/res.zip
mkdir -p $W/assets
cp "$HERE/../gamecontrollerdb.txt" $W/assets/   # SDL pad mappings (zlib), installed into DBZ3/
$BT/aapt2 link -o $W/base.apk -I $JAR --manifest "$HERE/app/AndroidManifest.xml" -A $W/assets \
  --java $W/gen $W/res.zip --min-sdk-version 29 --target-sdk-version 35

# 3) Java: app + SDL3's own Java glue (zlib licence, taken from the SDK checkout)
cp -r "$SDKSRC/thirdparty/sdl3/android-project/app/src/main/java/." $W/java/
cp -r "$HERE/app/src/." $W/java/
javac -source 17 -target 17 -nowarn -Xlint:none -classpath $JAR -d $W/classes \
  $(find $W/java $W/gen -name '*.java') 2>&1 | grep -v "^Picked up\|^Note:" || true
$BT/d8 --release --min-api 29 --lib $JAR --output $W/dex $(find $W/classes -name '*.class')

# 4) assemble (libs deflated: extractNativeLibs=true keeps the APK small), align, sign
cp $W/base.apk $W/unsigned.apk
(cd $W && zip -q -j unsigned.apk dex/classes.dex && zip -q -9 -r unsigned.apk lib)
$BT/zipalign -f -P 16 4 $W/unsigned.apk $W/aligned.apk
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias dbz3 \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=DBZ3 Android" 2>&1 | grep -v "^Picked up" || true
fi
$BT/apksigner sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias dbz3 --out "$OUT" $W/aligned.apk
$BT/apksigner verify "$OUT" && ls -la "$OUT"
