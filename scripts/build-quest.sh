#!/usr/bin/env bash
# Builds the Quest 3 app (quest/reuc) : the game cross-compiled for Android arm64 as libmain.so, SDL3, the Khronos OpenXR loader, the
# port's shaders as assets, then the APK with Gradle (as gse-recomp's quest/build_vulkan.py does for HOTD4).
#   scripts/build-quest.sh            -> quest/reuc/build/outputs/apk/debug/reuc-debug.apk
# Needs: the translated game in generated/ (scripts/build.sh --translate-only), the Android SDK + NDK 27.3, JDK 17, the OpenXR loader
# AAR unpacked (UC_OPENXR_DIR, default ~/Downloads/VR/deps/oxr), slangc (UC_SLANGC), Vulkan headers newer than the NDK's
# (UC_VULKAN_HEADERS, default Homebrew's vulkan-headers). Settings may come from local.env.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/local.sh"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.3.13750724}"
OXR="${UC_OPENXR_DIR:-$HOME/Downloads/VR/deps/oxr}"
VKH="${UC_VULKAN_HEADERS:-/opt/homebrew/opt/vulkan-headers/include}"
JDK="${UC_JAVA_HOME:-/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home}"
SLANGC="${UC_SLANGC:-$(command -v slangc || true)}"
BUILD="$ROOT/build-android"
APP="$ROOT/quest/reuc/src/main"
for need in "$NDK/build/cmake/android.toolchain.cmake" "$OXR/jni/arm64-v8a/libopenxr_loader.so" "$VKH/vulkan/vulkan_core.h" "$JDK/bin/java" "$SLANGC"; do
  [ -e "$need" ] || { echo "ERROR: missing $need (see the header of $0)" >&2; exit 1; }
done
[ -f "$ROOT/generated/game_sources.cmake" ] || { echo "ERROR: generated/ is empty: run scripts/build.sh --translate-only first" >&2; exit 1; }

echo "[1/4] Configure and build libmain.so (arm64-v8a)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DUC_SLANGC="$SLANGC" -DUC_THINLTO="${UC_THINLTO:-OFF}" \
  -DUC_TARGET_CPU="${UC_TARGET_CPU:-cortex-a78c}" -DVulkan_INCLUDE_DIR="$VKH" -DUC_OPENXR_DIR="$OXR" > "$BUILD.configure.log" 2>&1 \
  || { echo "ERROR: configure failed (see $BUILD.configure.log)" >&2; tail -20 "$BUILD.configure.log" >&2; exit 1; }
cmake --build "$BUILD" --target uc || { echo "ERROR: build failed" >&2; exit 1; }

echo "[2/4] Stage libraries, assets and SDL's Java sources"
rm -rf "$APP/libs" "$APP/assets" "$APP/sdl-java"
mkdir -p "$APP/libs/arm64-v8a" "$APP/assets"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" -o "$APP/libs/arm64-v8a/libmain.so" "$BUILD/game/libmain.so"
cp "$BUILD/game/libSDL3.so" "$OXR/jni/arm64-v8a/libopenxr_loader.so" "$APP/libs/arm64-v8a/"
cp -R "$BUILD/game/data" "$APP/assets/data"
( cd "$APP/assets" && find data -type f | LC_ALL=C sort > data.files && cat data.files | xargs shasum -a 256 | shasum -a 256 | cut -c1-16 > data.stamp )
cp -R "$BUILD/_deps/sdl3-src/android-project/app/src/main/java" "$APP/sdl-java"

echo "[3/4] Gradle (offline)"
echo "sdk.dir=$SDK" > "$ROOT/quest/local.properties"
DOL_SHA="$(shasum -a 256 "${UC_INSTALL_DIR:?set UC_INSTALL_DIR (local.env)}/game/DATA/sys/main.dol" | cut -d' ' -f1)"
# Debug by default, as gse-recomp's Vulkan Quest builds: run-as (the runner's private game folder) needs a debuggable app. The native code is
# the optimised build either way. UC_APK_TYPE=release for the signed release variant.
TYPE="${UC_APK_TYPE:-debug}"; TASK="assemble$(echo "${TYPE:0:1}" | tr a-z A-Z)${TYPE:1}"
( cd "$ROOT/quest" && JAVA_HOME="$JDK" ./gradlew --offline -q ":reuc:$TASK" -PdolSha256="$DOL_SHA" )

APK="$ROOT/quest/reuc/build/outputs/apk/$TYPE/reuc-$TYPE.apk"
echo "[4/4] $APK ($(du -h "$APK" | cut -f1))"
