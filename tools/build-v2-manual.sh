#!/usr/bin/env bash
# Reproducible Android-native V2 build for ARM64-host environments where the
# official NDK Linux host tools cannot run.  It uses host clang/lld together
# with the official Android NDK r28b target sysroot and runtime archives.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK_ROOT=${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to the verified android-ndk-r28b directory.}
CLANGXX=${CLANGXX:-clang++}
STRIP=${STRIP:-llvm-strip-18}
API=${ANDROID_API:-26}
PREBUILT="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$PREBUILT/sysroot"
RUNTIME_ROOT="$PREBUILT/lib/clang/19/lib/linux"
OUT_ROOT="$ROOT/build/manual"
SOURCE_ROOT="$ROOT/module/src/main/cpp"

for required in "$SYSROOT/usr/include/jni.h" "$RUNTIME_ROOT" "$SOURCE_ROOT/main.cpp" "$SOURCE_ROOT/mainCore.cxx"; do
  [[ -e "$required" ]] || { echo "missing required build input: $required" >&2; exit 1; }
done
command -v "$CLANGXX" >/dev/null
command -v "$STRIP" >/dev/null
command -v readelf >/dev/null

build_one() {
  local abi=$1 target=$2 lib_triple=$3 runtime_arch=$4 xdl_abi=$5 expected_machine=$6
  local out="$OUT_ROOT/$abi"
  local runtime="$RUNTIME_ROOT/libclang_rt.builtins-${runtime_arch}-android.a"
  local unwind_arch=$runtime_arch
  [[ "$runtime_arch" = "i686" ]] && unwind_arch=i386
  local unwind="$RUNTIME_ROOT/$unwind_arch/libunwind.a"
  local xdl="$SOURCE_ROOT/xdl/libs/$xdl_abi/libxdl.a"
  local library="$out/libinline_hook_spoof.so"

  for required in "$runtime" "$unwind" "$xdl" "$SYSROOT/usr/lib/$lib_triple/$API/libc++.a"; do
    [[ -f "$required" ]] || { echo "missing $abi build input: $required" >&2; exit 1; }
  done
  mkdir -p "$out"

  "$CLANGXX" --target="${target}${API}" --sysroot="$SYSROOT" -fuse-ld=lld \
    -std=c++17 -fPIC -shared -O2 -DNDEBUG \
    -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections \
    -Wall -Wextra -Werror=return-type -Werror=format-security \
    -nostdinc++ -isystem "$SYSROOT/usr/include/c++/v1" \
    -nostdlib -nodefaultlibs \
    -I"$SOURCE_ROOT" -I"$SOURCE_ROOT/xdl" \
    "$SOURCE_ROOT/main.cpp" "$SOURCE_ROOT/mainCore.cxx" "$xdl" \
    -L"$SYSROOT/usr/lib/$lib_triple/$API" -L"$SYSROOT/usr/lib/$lib_triple" \
    -Wl,-shared,-soname,libinline_hook_spoof.so,-z,relro,-z,now,-z,noexecstack,--gc-sections,--no-undefined,--exclude-libs,ALL,--build-id=sha1 \
    -lc++_static -lc++abi "$unwind" -llog -ldl -lm -lc "$runtime" \
    -o "$library"

  "$STRIP" --strip-unneeded "$library"
  file "$library" | grep -F "$expected_machine" >/dev/null || { echo "$abi has unexpected ELF machine" >&2; exit 1; }
  readelf -Ws "$library" | awk '$7 == "UND" && $8 !~ /^$/ && $8 != "__android_log_print" && $8 !~ /@LIBC/ && $8 !~ /@LIBDL/ { exit 1 }'
  readelf -Ws "$library" | awk '$8 == "zygisk_module_entry" && $4 == "FUNC" { found=1 } END { exit !found }'
  local needed
  needed=$(readelf -d "$library" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' | sort | tr '\n' ' ')
  [[ "$needed" == "libc.so libdl.so liblog.so libm.so " ]] || { echo "$abi has unexpected shared dependencies: $needed" >&2; exit 1; }
  echo "PASS: $abi -> $library"
}

rm -rf "$OUT_ROOT"
build_one armeabi-v7a armv7a-linux-androideabi arm-linux-androideabi arm armeabi-v7a "ARM, EABI5"
build_one arm64-v8a aarch64-linux-android aarch64-linux-android aarch64 arm64-v8a "ARM aarch64"
build_one x86 i686-linux-android i686-linux-android i686 x86 "Intel 80386"
build_one x86_64 x86_64-linux-android x86_64-linux-android x86_64 x86_64 "x86-64"

echo "PASS: all Android ABI libraries built and verified"
