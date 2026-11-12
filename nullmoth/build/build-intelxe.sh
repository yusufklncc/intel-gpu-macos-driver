#!/bin/bash
# SPDX-License-Identifier: MIT
# build-intelxe.sh: the modified nullmoth Metal stack in this directory, built for Intel Xe GPUs (x86_64 macOS; tested on
# the Arc B580).
# (This script is ours, MIT; what it builds is nullmoth's PolyForm Noncommercial code and the LGPL translator.)
#
#   nullmoth/build/build-intelxe.sh [all|kexts|plugin|translator|tests]     default: all
#   RELEASE=1 ...   the plugin without diagnostics (NVMTL_RELEASE: logs only failures to syslog)
#
# Products ($OUT, default <repo>/build/nullmoth):
#   NVMTLDriver.bundle/   -> /Library/GPUBundles/NVMTLDriver.bundle   (Metal driver plugin)
#     Contents/MacOS/NVMTLDriver, libnvmtl_translate.dylib (AIR -> SPIR-V), nvmtl-air-dis (out-of-process AIR decoder),
#     air-runtime/libLLVM.dylib + libzstd.1.dylib
#   NMXAccel.kext/        -> /Library/Extensions/NMXAccel.kext         (IOGraphicsAccelerator2 for 8086:e20b)
#   NMXAGDC.kext/         -> /Library/Extensions/NMXAGDC.kext          (AppleGraphicsDeviceControl listing IntelXeFB)
#   nvmtl/                -> /Library/GPUBundles/nvmtl/                (intel_icd.json + ANV, only if ANV_DYLIB is set)
#   nvmtl-allow.txt       -> /Library/GPUBundles/nvmtl-allow.txt
#   mtl_smoke, xlate_check, mtl_cube_read, mtl_props, mtl_f32filter, mtl_timestamps, mtl_gpu_icb, mtl_gpu_icb2, mtl_icb_cost, mtl_lod_pull, mtl_m4_inherit, mtl_sparse   tests (user/tests/metal)
#
# Nothing here installs or loads anything. Inputs (environment):
#   MACKERNELSDK   MacKernelSDK checkout (default <repo>/external/MacKernelSDK)
#   MESA_INCLUDE   a Mesa source tree's include/ (Vulkan headers 1.4.363 were used)
#   ANV_DYLIB      libvulkan_intel.dylib built from mesa/ (optional: the nvmtl/ step is skipped without it)
#   MACOS_SDK      macOS SDK path (default: the 26.5 Command Line Tools SDK if present, else xcrun's)
#   cargo/rustc    1.99.1 with the x86_64-apple-darwin target in PATH (RUST_TOOLCHAIN=<dir>/bin is prepended if set)
#   air-runtime    libLLVM (Homebrew LLVM 21.1.8, x86_64, Apache-2.0) is taken from nullmoth's 2.1.0 release package,
#                  downloaded into <repo>/external/ and checked by sha256 (the repository does not carry it)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
IXVER=$(cat "$REPO/VERSION")   # the release that last rebuilt a binary
NM=$REPO/nullmoth
OUT=${OUT:-$REPO/build/nullmoth}
WHAT=${1:-all}
SDK=${MACOS_SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk}
[ -d "$SDK" ] || SDK=$(xcrun --show-sdk-path)
KSDK=${MACKERNELSDK:-$REPO/external/MacKernelSDK}
PKG=$REPO/external/nullmoth-nvidia-2.1.0.tar.gz
PKG_SHA=ee52792d8ae3fbeb0fca733627febb9580dc06a6ceeb3b444d8e96e6493e2831
PKG_URL=https://github.com/nullmoth/nvidia-macos-driver/releases/download/v2.1.0/nullmoth-nvidia-2.1.0.tar.gz
[ -n "${RUST_TOOLCHAIN:-}" ] && export PATH=$RUST_TOOLCHAIN:$PATH
mkdir -p "$OUT"
want() { [ "$WHAT" = all ] || [ "$WHAT" = "$1" ]; }

# 1. Shader translator (translator/wrapper -> libnvmtl_translate.dylib), as nullmoth's build_xlate_release.sh
XL=$OUT/libnvmtl_translate.dylib
if want translator || want plugin; then
  echo "== translator"
  ( cd "$NM/translator"
    # rustc applies the last matching remap: the checkout's own path last, so panic locations read /src/... wherever the
    # repository is (crates from CARGO_HOME under $HOME read /build/...)
    RUSTFLAGS="--remap-path-prefix=$HOME=/build --remap-path-prefix=$NM/translator=/src" \
    MACOSX_DEPLOYMENT_TARGET=15.0 \
    cargo build --release --target x86_64-apple-darwin --manifest-path wrapper/Cargo.toml --target-dir "$OUT/xlate-target" \
      > "$OUT/cargo.log" 2>&1 ) || { grep -A6 '^error' "$OUT/cargo.log" | head -30; exit 1; }
  cp "$OUT/xlate-target/x86_64-apple-darwin/release/libnvmtl_translate.dylib" "$XL"
  strip -S "$XL"; install_name_tool -id @rpath/libnvmtl_translate.dylib "$XL" 2>/dev/null; codesign -f -s - "$XL"
fi

if want plugin; then
  # 2. Metal plugin (plugin/NVMTLDevice.m is one translation unit), as nullmoth's build_plugin_release.sh
  echo "== plugin"
  : "${MESA_INCLUDE:?set MESA_INCLUDE to the include/ directory of a Mesa source tree}"
  RDEF=(); [ -n "${RELEASE:-}" ] && RDEF=(-DNVMTL_RELEASE=1)
  xcrun --sdk "$SDK" clang -isysroot "$SDK" -arch x86_64 -mmacosx-version-min=15.0 -bundle -fobjc-arc -O1 ${RDEF[@]+"${RDEF[@]}"} \
    -Wno-protocol -Wno-incomplete-implementation -Werror=unguarded-availability-new -I"$MESA_INCLUDE" \
    -o "$OUT/NVMTLDriver" "$NM/plugin/NVMTLDevice.m" \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>"$OUT/plugin.err" \
    || { grep -A2 ': error:' "$OUT/plugin.err" | head -30; exit 1; }

  # 2b. nvmtl-air-dis (plugin/air_dis.c, nullmoth 2.9): the out-of-process AIR decoder the plugin runs beside itself when the
  #     in-process reader cannot take a library; it loads the same air-runtime/libLLVM.dylib
  xcrun --sdk "$SDK" clang -isysroot "$SDK" -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Werror \
    -o "$OUT/nvmtl-air-dis" "$NM/plugin/air_dis.c" 2>"$OUT/air-dis.err" || { cat "$OUT/air-dis.err"; exit 1; }

  # 3. air-runtime (the in-process AIR bitcode reader the plugin dlopens from Contents/MacOS/air-runtime)
  echo "== air-runtime"
  AR=$OUT/air-runtime
  if [ ! -f "$AR/libLLVM.dylib" ]; then
    [ -f "$PKG" ] || { mkdir -p "$(dirname "$PKG")"; curl -fL -o "$PKG" "$PKG_URL"; }
    [ "$(shasum -a 256 "$PKG" | cut -d' ' -f1)" = "$PKG_SHA" ] || { echo "STOP: $PKG checksum"; exit 1; }
    T=$(mktemp -d "$OUT/.pkg.XXXXXX")
    tar -xzf "$PKG" -C "$T" pkgroot/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime \
                            pkgroot/Library/GPUBundles/NVMTLDriver.bundle/Contents/Resources/air-runtime
    rm -rf "$AR" "$OUT/air-runtime-licenses"
    mv "$T/pkgroot/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime" "$AR"
    mv "$T/pkgroot/Library/GPUBundles/NVMTLDriver.bundle/Contents/Resources/air-runtime" "$OUT/air-runtime-licenses"
    rm -rf "$T"
  fi

  # 4. the bundle (Info.plist as in nullmoth's package: no principal class; NMXAccel's MetalPluginClassName names NVMTLDevice)
  echo "== NVMTLDriver.bundle"
  B=$OUT/NVMTLDriver.bundle/Contents
  rm -rf "$OUT/NVMTLDriver.bundle"; mkdir -p "$B/MacOS" "$B/Resources"
  cat > "$B/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>NVMTLDriver</string>
	<key>CFBundleIdentifier</key><string>com.nullmoth.NVMTLDriver</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>NVMTLDriver</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>@IXVER@</string>
	<key>CFBundleVersion</key><string>@IXVER@</string>
</dict>
</plist>
EOF
  sed -i '' "s/@IXVER@/$IXVER/g" "$B/Info.plist"
  cp "$OUT/NVMTLDriver" "$B/MacOS/"; cp "$XL" "$B/MacOS/"; cp "$OUT/nvmtl-air-dis" "$B/MacOS/"; cp -R "$AR" "$B/MacOS/air-runtime"
  codesign -f -s - "$B/MacOS/nvmtl-air-dis"
  cp -R "$OUT/air-runtime-licenses" "$B/Resources/air-runtime"
  codesign -f -s - "$B/MacOS/libnvmtl_translate.dylib"
  codesign -f -s - "$OUT/NVMTLDriver.bundle"
fi

# TARGET_MACOS=26 (default) builds NMXAccel for macOS 26 Tahoe (upstream's NM_TAHOE path: tahoe_fwd.h forwarders through
# the base vtables, plus the no-Metal fallback); TARGET_MACOS=15 builds it for macOS 15 Sequoia.
TARGET_MACOS=${TARGET_MACOS:-26}
kext() { # name source-dir source define [extra defines...]
  local name=$1 dir=$2 src=$3 def=$4 K=$OUT/$1.kext/Contents; shift 4; local extra=("$@")
  echo "== $name.kext"
  [ -f "$KSDK/Library/x86_64/libkmod.a" ] || { echo "STOP: MacKernelSDK not found at $KSDK (set MACKERNELSDK)"; exit 1; }
  rm -rf "$OUT/$name.kext"; mkdir -p "$K/MacOS"
  xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -fapple-kext -mkernel -nostdinc -I"$KSDK/Headers" \
    -I"$NM/kexts/NVRM" -I"$dir" -I"$dir/iofam" -I"$dir/re" -I"$REPO/kernel/IntelXe/uapi" \
    -D"$def" ${extra[@]+"${extra[@]}"} -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
    -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
    -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
    -c "$dir/$src" -o "$OUT/$name.o" 2>"$OUT/$name.err" || { grep -A3 error "$OUT/$name.err" | head -30; exit 1; }
  xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -fapple-kext -nostdlib -Xlinker -kext -Xlinker -no_fixup_chains \
    "$OUT/$name.o" -L"$KSDK/Library/x86_64" -lkmod -o "$K/MacOS/$name"
  sed "s#<string>0\.1\.0</string>#<string>$IXVER</string>#g" "$dir/Info-NMX-IntelXe.plist" > "$K/Info.plist"
  codesign -f -s - "$OUT/$name.kext"
}
if want kexts; then
  # 5. NMXAccel.kext (kexts/NVRM/accel/nvrm-accel.cpp with -DNMX_ACCEL)
  if [ "$TARGET_MACOS" = 26 ]; then kext NMXAccel "$NM/kexts/NVRM/accel" nvrm-accel.cpp NMX_ACCEL -DNM_TAHOE -DNMX_NOMETAL_FALLBACK
  else kext NMXAccel "$NM/kexts/NVRM/accel" nvrm-accel.cpp NMX_ACCEL; fi
  # 5b. NMXAGDC.kext (kexts/NVRMAGDC/nvrm-agdc.cpp with -DNMX_AGDC): the AppleGraphicsDeviceControl WindowServer asks
  # (command 0x980) for the framebuffers it builds display pipes for; lists IntelXeFB.
  kext NMXAGDC "$NM/kexts/NVRMAGDC" nvrm-agdc.cpp NMX_AGDC
fi

if want plugin; then
  # 6. /Library/GPUBundles/nvmtl: the ICD the plugin dlopens for vendor 0x8086 and the allow-list
  if [ -n "${ANV_DYLIB:-}" ]; then
    echo "== nvmtl/"
    rm -rf "$OUT/nvmtl"; mkdir -p "$OUT/nvmtl"
    cp "$ANV_DYLIB" "$OUT/nvmtl/libvulkan_intel.dylib"
    install_name_tool -id /Library/GPUBundles/nvmtl/libvulkan_intel.dylib "$OUT/nvmtl/libvulkan_intel.dylib" 2>/dev/null
    codesign -f -s - "$OUT/nvmtl/libvulkan_intel.dylib"
    cat > "$OUT/nvmtl/intel_icd.json" <<'EOF'
{
    "ICD": {
        "api_version": "1.4.363",
        "library_arch": "64",
        "library_path": "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
    },
    "file_format_version": "1.0.1"
}
EOF
  fi
  # the test programs always get a device; WindowServer and every other process only once the display pipe is armed
  # ("!": debug.nmxaccelfb reads 1). An accelerated desktop needs the apps too.
  printf '# B580: which processes get the B580 Metal device (-Name denies, !Name = only when armed; NVMTL_ENABLE=1 bypasses)\nmtl_smoke\n!WindowServer\n!*\n' > "$OUT/nvmtl-allow.txt"
fi

if want tests || [ "$WHAT" = all ]; then
  echo "== tests"
  # host check of NMXAccel's head rule (kexts/NVRM/accel/nmx_heads.h): which IntelXeFB heads it arms, and when
  xcrun --sdk "$SDK" clang++ -isysroot "$SDK" -std=c++17 -Wall -Wextra -Werror -I"$NM/kexts/NVRM/accel" \
    -o "$OUT/nmx_heads_test" "$NM/kexts/NVRM/accel/tests/nmx_heads_test.cpp" && "$OUT/nmx_heads_test"
  for t in mtl_smoke xlate_check mtl_cube_read mtl_props mtl_f32filter mtl_timestamps mtl_gpu_icb mtl_gpu_icb2 mtl_icb_cost mtl_lod_pull mtl_m4_inherit mtl_sparse; do
    xcrun --sdk "$SDK" clang -isysroot "$SDK" -arch x86_64 -mmacosx-version-min=15.0 -fobjc-arc -O1 -g -Wall \
      -Werror=unguarded-availability-new -o "$OUT/$t" "$REPO/user/tests/metal/$t.m" -framework Foundation -framework Metal -framework IOKit
  done
fi

echo "built into $OUT:"
for f in NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib \
         NMXAccel.kext/Contents/MacOS/NMXAccel NMXAGDC.kext/Contents/MacOS/NMXAGDC nvmtl/libvulkan_intel.dylib mtl_smoke; do
  [ -f "$OUT/$f" ] && printf '  %-58s %s\n' "$f" "$(shasum -a 256 "$OUT/$f" | cut -c1-12)"
done
exit 0
