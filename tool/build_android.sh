#!/bin/sh
# Build AndroidDeviceModule for Android arm64 into dist/android-arm64/:
#
#   1. the openDAQ SDK at the release in the file OPENDAQ_VERSION, with the
#      patch in patches/, cloned into
#      build/openDAQ from OPENDAQ_SOURCE (the openDAQ repository on GitHub
#      unless set; a local clone is faster),
#   2. the module, against that SDK.
#
#   tool/build_android.sh
#
# An app that loads the module builds it the same way, from these sources and
# against its own SDK build, as the openDAQ Device Server does. This script is
# for building and checking the module on its own. The first run takes about
# ten minutes and needs network access for the SDK's dependencies; later runs
# only rebuild what changed.
#
# Needs the Android NDK (ANDROID_NDK, or the newest NDK of the Android SDK in
# ANDROID_HOME or its default place), CMake 3.24 or newer, Ninja and git.
# CMAKE and NINJA name the tools when they are not on the PATH.

set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$HERE/build"
DIST="$HERE/dist/android-arm64"
SDK_SOURCE="${OPENDAQ_SOURCE:-https://github.com/openDAQ/openDAQ.git}"
OPENDAQ_VERSION="${OPENDAQ_VERSION:-$(tr -d ' \r\n' < "$HERE/OPENDAQ_VERSION")}"
SDK_REF="${SDK_REF:-v$OPENDAQ_VERSION}"
CMAKE="${CMAKE:-cmake}"
NINJA="${NINJA:-ninja}"

if [ -z "$ANDROID_NDK" ]; then
  case "$(uname -s)" in
    Darwin) ANDROID_SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}" ;;
    *) ANDROID_SDK="${ANDROID_HOME:-$HOME/Android/Sdk}" ;;
  esac
  ANDROID_NDK="$(ls -d "$ANDROID_SDK/ndk/"* 2> /dev/null | sort -V | tail -1)"
fi
if [ ! -f "$ANDROID_NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "No Android NDK found; install one with Android Studio's SDK Manager, or set ANDROID_NDK" >&2
  exit 1
fi
LLVM="$(ls -d "$ANDROID_NDK"/toolchains/llvm/prebuilt/* | head -1)"
for tool in git "$CMAKE" "$NINJA"; do
  command -v "$tool" > /dev/null 2>&1 || { echo "$tool is missing; see Requirements in README.md" >&2; exit 1; }
done

mkdir -p "$WORK"
"$HERE/tool/prepare_sdk.sh" "$WORK/openDAQ" "$SDK_SOURCE" "$SDK_REF"

COMMON="-G Ninja -DCMAKE_MAKE_PROGRAM=$NINJA -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
# A compiler cache: a file compiled before, in any build folder, with the
# same flags, comes from the cache instead of the compiler.
if command -v ccache > /dev/null 2>&1; then
  COMMON="$COMMON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  export CCACHE_BASEDIR="$HERE"
fi

# Configures a build folder only when it has none yet or its options changed.
# Ninja configures again by itself when a CMakeLists.txt changes.
configure() {
  source="$1"
  folder="$2"
  log="$3"
  shift 3
  options="$source $*"
  if [ -f "$folder/build.ninja" ] && [ "$(cat "$folder/.configure-options" 2> /dev/null)" = "$options" ]; then
    return
  fi
  "$CMAKE" -S "$source" -B "$folder" "$@" > "$log"
  printf '%s' "$options" > "$folder/.configure-options"
}

echo "== openDAQ SDK $SDK_REF for Android arm64"
# shellcheck disable=SC2086
configure "$WORK/openDAQ" "$WORK/sdk-android-arm64" "$WORK/sdk-configure.log" $COMMON \
  -DOPENDAQ_ENABLE_TESTS=OFF -DOPENDAQ_ENABLE_TEST_UTILS=OFF \
  -DOPENDAQ_ENABLE_NATIVE_STREAMING=OFF -DOPENDAQ_ENABLE_OPCUA=OFF \
  -DDAQMODULES_REF_DEVICE_MODULE=OFF -DDAQMODULES_REF_FB_MODULE=OFF
"$CMAKE" --build "$WORK/sdk-android-arm64" > "$WORK/sdk-build.log"
"$CMAKE" --install "$WORK/sdk-android-arm64" --prefix "$WORK/sdk-android-arm64-install" > "$WORK/sdk-install.log"

echo "== Android device module"
# shellcheck disable=SC2086
configure "$HERE" "$WORK/android-arm64" "$WORK/configure.log" $COMMON \
  -DCMAKE_PREFIX_PATH="$WORK/sdk-android-arm64-install" -DCMAKE_FIND_ROOT_PATH="$WORK/sdk-android-arm64-install"
"$CMAKE" --build "$WORK/android-arm64"

mkdir -p "$DIST"
"$LLVM/bin/llvm-strip" --strip-unneeded -o "$DIST/libandroid_device_module.module.so" \
  "$WORK/android-arm64/libandroid_device_module.module.so"
ls -l "$DIST"
