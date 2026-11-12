#!/bin/bash
# SPDX-License-Identifier: MIT
# build-neo.sh [STEP...]: Intel's compute runtime (NEO) for macOS x86_64 over IntelXe.kext: the Level Zero driver
# libze_intel_gpu.1.dylib (and, with OCL=1, the OpenCL ICD libigdrcl.dylib), with gmmlib and the Level Zero loader.
# Runs on the build Mac (Apple silicon or Intel) and cross-compiles to x86_64. See compute/README.md.
#   STEP: fetch patch gmmlib loader igc-headers neo stage (default: all, in that order)
#   L0_WORK   work directory (default $HOME/intelxe-l0): src/ (pinned upstream checkouts, patched on branch intelxe),
#             build/, prefix/ (gmmlib, loader, IGC headers), stage/ (what goes to the test machine)
#   OCL=1     also build the OpenCL ICD (libigdrcl)
#   JOBS      ninja -j (default: all cores)
# Needs git, cmake >= 3.21, ninja on PATH, the Xcode command line tools. ~1.5 GB of sources and builds; the NEO build
# is ~5 minutes on 8 Apple silicon cores. No IGC is built: kernels from source need it (docs/design/DESIGN-level-zero.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
W=${L0_WORK:-$HOME/intelxe-l0}
SRC=$W/src
BLD=$W/build
PREFIX=$W/prefix
STAGE=${OUT:-$W/stage}
J=${JOBS:-$(/usr/sbin/sysctl -n hw.ncpu)}

# Pins: NEO at the commit docs/parity reads (2026-10-30), and the gmmlib, loader and IGC revisions its
# manifests/manifest.yml names for that commit (IGC: headers only).
NEO_URL=https://github.com/intel/compute-runtime.git
NEO_REV=248eb54f84415da3498d9f4070ba09cc55efab94
GMM_URL=https://github.com/intel/gmmlib.git
GMM_TAG=intel-gmmlib-22.10.2
GMM_REV=733c91a19baa65c40f4b1cf95a3991cc8c84d866
LOADER_URL=https://github.com/oneapi-src/level-zero.git
LOADER_TAG=v1.35.0
LOADER_REV=9e1a8b2c07ed6ce1f00b0a195fa5efa0db8e3650
IGC_URL=https://github.com/intel/intel-graphics-compiler.git
IGC_REV=0f18b8dc2e5d75fa86ba9f1960eab87b5a89a4c6

X86=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0)
command -v ninja >/dev/null || { echo "ninja is not on PATH (pip install ninja, or brew install ninja)" >&2; exit 1; }

fetch() {
	mkdir -p "$SRC"
	[ -d "$SRC/neo/.git" ] || git clone -q --filter=blob:none --no-checkout "$NEO_URL" "$SRC/neo"
	[ -d "$SRC/gmmlib/.git" ] || git clone -q --depth 1 -b "$GMM_TAG" "$GMM_URL" "$SRC/gmmlib"
	[ -d "$SRC/level-zero/.git" ] || git clone -q --depth 1 -b "$LOADER_TAG" "$LOADER_URL" "$SRC/level-zero"
	if [ ! -d "$SRC/igc/.git" ]; then
		# the headers NEO compiles against (the igc-opencl-devel layout), not the compiler
		git clone -q --filter=blob:none --no-checkout --sparse "$IGC_URL" "$SRC/igc"
		git -C "$SRC/igc" sparse-checkout set IGC/AdaptorOCL IGC/common visa/include visa/iga/IGALibrary/api \
			IGC/ZEBinWriter/zebin/source
	fi
	git -C "$SRC/neo" fetch -q --filter=blob:none origin "$NEO_REV" 2>/dev/null || true
	git -C "$SRC/igc" checkout -q "$IGC_REV"
}

# component dir, pinned revision, patch directory: branch intelxe = the pin + our series (git am)
apply_series() {
	local dir=$1 rev=$2 pdir=$3
	[ "$(git -C "$dir" rev-parse "$rev^{commit}")" = "$rev" ] || { echo "$dir: $rev missing" >&2; exit 1; }
	git -C "$dir" checkout -q -f -B intelxe "$rev"
	local p
	while read -r p; do
		[ -n "$p" ] || continue
		git -C "$dir" -c user.name=intelxe-build -c user.email=build@localhost am -q --keep-cr "$pdir/$p"
	done < "$pdir/series"
}

patch() {
	apply_series "$SRC/neo" "$NEO_REV" "$HERE/patches/compute-runtime"
	apply_series "$SRC/gmmlib" "$GMM_REV" "$HERE/patches/gmmlib"
	apply_series "$SRC/level-zero" "$LOADER_REV" "$HERE/patches/level-zero"
}

gmmlib() {
	cmake -S "$SRC/gmmlib" -B "$BLD/gmmlib" -G Ninja "${X86[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DRUN_TEST_SUITE=OFF \
		-Wno-dev >/dev/null
	ninja -C "$BLD/gmmlib" -j "$J" install >/dev/null
}

loader() {
	cmake -S "$SRC/level-zero" -B "$BLD/level-zero" -G Ninja "${X86[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
	ninja -C "$BLD/level-zero" -j "$J" install >/dev/null
}

# IGC's igc-opencl-devel package layout, from the source: what NEO's find_igc.cmake (pkg-config igc-opencl) wants.
igc_headers() {
	local I=$SRC/igc P=$PREFIX/igc
	rm -rf "$P"
	mkdir -p "$P/include/igc/common" "$P/include/igc/zebin" "$P/include/visa" "$P/include/iga" "$P/lib/pkgconfig"
	cp -R "$I/IGC/AdaptorOCL/ocl_igc_shared" "$I/IGC/AdaptorOCL/ocl_igc_interface" "$I/IGC/AdaptorOCL/OCLAPI" \
		"$I/IGC/AdaptorOCL/cif" "$P/include/igc/"
	cp "$I/IGC/common/StateSaveAreaHeader.h" "$P/include/igc/common/"
	cp "$I/visa/include/RelocationInfo.h" "$P/include/visa/"
	(cd "$I/visa/iga/IGALibrary/api" && find . -name '*.h*' -exec cp {} "$P/include/iga/" \;)
	cp "$I/IGC/ZEBinWriter/zebin/source/ZEELF.h" "$P/include/igc/zebin/"
	sed "s|#cmakedefine IGC_REVISION \"\${IGC_REVISION}\"|#define IGC_REVISION \"$IGC_REV\"|" \
		"$I/IGC/AdaptorOCL/ocl_igc_interface/impl/version_in.h" > "$P/include/igc/ocl_igc_interface/impl/version.h"
	local maj min pat
	maj=$(sed -n 's/^  set(IGC_API_MAJOR_VERSION \([0-9]*\))/\1/p' "$I/IGC/CMakeLists.txt")
	min=$(sed -n 's/^  set(IGC_API_MINOR_VERSION \([0-9]*\))/\1/p' "$I/IGC/CMakeLists.txt")
	pat=$(sed -n 's/^  set(IGC_API_PATCH_VERSION \([0-9]*\))/\1/p' "$I/IGC/CMakeLists.txt")
	# igc.opencl.h.in has no Mach-O branch: the names NEO dlopen()s for the compiler (absent until IGC is ported)
	cat > "$P/include/igc/igc.opencl.h" <<EOF
// Generated by compute/build-neo.sh: IGC's library names on macOS.
#ifndef IGC_OPENCL_H
#define IGC_OPENCL_H
#define IGC_LIBRARY_NAME "libigc.$maj.dylib"
#define FCL_LIBRARY_NAME "libigdfcl.$maj.dylib"
#endif // IGC_OPENCL_H
EOF
	cat > "$P/lib/pkgconfig/igc-opencl.pc" <<EOF
prefix=$P
exec_prefix=\${prefix}
includedir=\${prefix}/include/igc
libdir=\${exec_prefix}/lib

Name: igc-opencl
Description: Intel(R) Graphics Compiler for OpenCL(TM), headers only (intel-gpu-macos-driver compute/build-neo.sh)
Version: $maj.$min.$pat
Cflags: -I\${includedir} -I\${includedir}/cif -I\${includedir}/ocl_igc_shared/executable_format -I\${includedir}/ocl_igc_shared/device_enqueue
Libs:
EOF
}

neo() {
	local ocl=FALSE targets=(ze_intel_gpu)
	[ "${OCL:-0}" = 1 ] && ocl=TRUE && targets+=(igdrcl_dll)
	# Battlemage and Lunar Lake (Xe2) only; no ULTs; built-in kernels are compiled at run time by IGC (none yet) since
	# Intel's prebuilt kernels_bin repository is not public; long-running VM probe and small BAR: see patches 0006.
	cmake -S "$SRC/neo" -B "$BLD/neo" -G Ninja "${X86[@]}" -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
		-DNEO_XEMAC_DIR="$REPO/user/xemac" -DIGC_DIR="$PREFIX/igc" -DGMM_DIR="$PREFIX" -DLEVEL_ZERO_ROOT="$PREFIX" \
		-DNEO_SKIP_UNIT_TESTS=1 -DNEO_SKIP_AUB_TESTS=1 -DCOMPILE_BUILT_INS=FALSE -DNEO_DISABLE_BUILTINS_COMPILATION=TRUE \
		-DNEO_BUILD_WITH_OCL=$ocl -DBUILD_WITH_L0=TRUE -DDISABLE_WDDM_LINUX=TRUE -DNEO_DISABLE_LTO=TRUE \
		-DNEO_DISABLE_LD_GOLD=1 -DNEO_DISABLE_LD_LLD=1 \
		-DSUPPORT_GEN12LP=0 -DSUPPORT_XE_HPG_CORE=0 -DSUPPORT_XE_HPC_CORE=0 -DSUPPORT_XE3_CORE=0 -DSUPPORT_XE3P_CORE=0 \
		-DSUPPORT_XE2_HPG_CORE=1 -DSUPPORT_BMG=1 -DSUPPORT_LNL=1 -Wno-dev >/dev/null
	ninja -C "$BLD/neo" -j "$J" "${targets[@]}"
}

# The test machine's layout: bin/ze_hello, lib/ with the driver, gmmlib and the loader. Every image finds the others
# through @rpath (@executable_path/../lib, @loader_path); ad-hoc signed.
stage() {
	rm -rf "$STAGE"
	mkdir -p "$STAGE/bin" "$STAGE/lib"
	cp -a "$PREFIX"/lib/libze_loader*.dylib "$PREFIX"/lib/libigdgmm*.dylib "$BLD"/neo/bin/libze_intel_gpu*.dylib "$STAGE/lib/"
	[ -e "$BLD/neo/bin/libigdrcl.dylib" ] && cp -a "$BLD"/neo/bin/libigdrcl*.dylib "$STAGE/lib/"
	local f
	for f in "$STAGE"/lib/libze_intel_gpu.1.*.dylib "$STAGE"/lib/libigdrcl.dylib; do
		[ -f "$f" ] && [ ! -L "$f" ] && install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true
	done
	clang -arch x86_64 -mmacosx-version-min=14.0 -O1 -Wall -I"$PREFIX/include" -o "$STAGE/bin/ze_hello" \
		"$REPO/user/tests/compute/ze_hello.c" -L"$PREFIX/lib" -lze_loader -Wl,-rpath,@executable_path/../lib
	for f in "$STAGE"/bin/* "$STAGE"/lib/*.dylib; do
		[ -L "$f" ] || codesign -s - -f "$f" 2>/dev/null
	done
	(cd "$STAGE" && find . -type f | sort | xargs shasum -a 256 > SHA256SUMS)
	echo "staged in $STAGE:"
	(cd "$STAGE" && ls -l bin lib)
}

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(fetch patch gmmlib loader igc-headers neo stage)
for s in "${steps[@]}"; do
	echo "== $s"
	case $s in
	fetch | patch | gmmlib | loader | neo | stage) "$s" ;;
	igc-headers) igc_headers ;;
	*) echo "unknown step $s" >&2; exit 2 ;;
	esac
done
