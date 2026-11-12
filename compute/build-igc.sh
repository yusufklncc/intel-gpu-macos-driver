#!/bin/bash
# SPDX-License-Identifier: MIT
# build-igc.sh [STEP...]: the Intel Graphics Compiler (IGC) for macOS x86_64: libigc.2.dylib (SPIR-V/LLVM to Gen ISA,
# the SIP kernels) and libigdfcl.2.dylib (the OpenCL C front end, over libopencl-clang), the compiler NEO loads at run
# time. IGC's own build ("Source" mode) builds LLVM 22.1.8 + Clang with IGC's LLVM patches, SPIRV-LLVM-Translator,
# opencl-clang, vc-intrinsics and SPIRV-Tools in one tree. Runs on the build Mac (Apple silicon or Intel) and builds
# x86_64; on Apple silicon the build's own tools (tablegen, clang for the built-in library, IGC's generators) run
# under Rosetta. See compute/README.md and docs/design/DESIGN-level-zero.md (section 4).
#   STEP: fetch patch configure llvm deps igc install (default: all, in that order)
#     llvm    LLVM + Clang (ninja clang)
#     deps    SPIRV-LLVM-Translator, opencl-clang, SPIRV-Tools, vc-intrinsics
#     igc     libigc and libigdfcl (and everything they need)
#     install the igc-core/igc-opencl(-devel) components and libopencl-clang into $IGC_PREFIX
#   IGC_WORK    work directory (default $HOME/intelxe-igc): src/ (the workspace IGC expects: igc, llvm-project with
#               projects/opencl-clang and projects/llvm-spirv, SPIRV-Tools, SPIRV-Headers, vc-intrinsics), build/,
#               venv/ (python3 with mako, ninja), ccache/ (when ccache is on PATH)
#   IGC_PREFIX  install prefix (default $IGC_WORK/prefix): what build-neo.sh takes (IGC_PREFIX there too)
#   JOBS        ninja -j (default: all cores)
# Needs git, cmake >= 3.21, python3, bison >= 3 (Homebrew's: brew install bison), flex, the Xcode command line tools.
# ~14 GB of sources and objects; ~46 min cold on 8 Apple silicon cores (M1 Pro), much less with a warm ccache (README).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${IGC_WORK:-$HOME/intelxe-igc}
SRC=$W/src
BLD=$W/build/igc
PREFIX=${IGC_PREFIX:-$W/prefix}
J=${JOBS:-$(/usr/sbin/sysctl -n hw.ncpu)}

# Pins: IGC at the commit compute-runtime 248eb54f8441's manifest names (API 2.42, 2026-10-20); LLVM 22.1.8 (IGC's
# DEFAULT_IGC_LLVM_VERSION) and the other dependencies at the revisions IGC v2.42.6's release notes list, except the
# SPIR-V translator: this IGC (v2.42.0+240 on master) uses TranslatorOpts::setErrorHandlingKind, which
# llvm_release_220 has from 2026-09-29, so the branch as of IGC's date (2026-10-15).
IGC_URL=https://github.com/intel/intel-graphics-compiler.git
IGC_REV=0f18b8dc2e5d75fa86ba9f1960eab87b5a89a4c6
LLVM_URL=https://github.com/llvm/llvm-project.git
LLVM_TAG=llvmorg-22.1.8
LLVM_REV=ca7933e47d3a3451d81e72ac174dcb5aa28b59d1
OCLCLANG_URL=https://github.com/intel/opencl-clang.git
OCLCLANG_REV=b953eceebb0abc6ea954a14545420e3f97540a77 # ocl-open-220
SPIRV_LLVM_URL=https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git
SPIRV_LLVM_REV=67b3fc338a3ce0ebce5d249cbc437e8b36ffdeaa # llvm_release_220 on 2026-10-15
SPIRV_TOOLS_URL=https://github.com/KhronosGroup/SPIRV-Tools.git
SPIRV_TOOLS_REV=f80351511e9c4672e284842c7b124315c511078a
SPIRV_HEADERS_URL=https://github.com/KhronosGroup/SPIRV-Headers.git
SPIRV_HEADERS_REV=575b6512579ebde466ed3dfc04e413439d14d95d
VCI_URL=https://github.com/intel/vc-intrinsics.git
VCI_TAG=v0.25.0
VCI_REV=27f7c4f34738f5eaf7a045b77edf8d9e034443d8

# bison >= 3 (macOS's /usr/bin/bison is 2.3) and a python3 with mako and pyyaml for IGC's generators
BISON=
for b in /opt/homebrew/opt/bison/bin/bison /usr/local/opt/bison/bin/bison "$(command -v bison || true)"; do
	[ -x "$b" ] && "$b" --version | head -1 | grep -qE ' [3-9]\.' && BISON=$b && break
done
[ -n "$BISON" ] || { echo "bison >= 3 not found (brew install bison)" >&2; exit 1; }
venv() {
	if [ ! -x "$W/venv/bin/python3" ]; then
		mkdir -p "$W"
		python3 -m venv "$W/venv"
		"$W/venv/bin/pip" -q install mako pyyaml ninja
	fi
	export PATH=$W/venv/bin:$PATH
}
venv
LAUNCHER=()
if command -v ccache >/dev/null; then
	export CCACHE_DIR=${CCACHE_DIR:-$W/ccache} CCACHE_BASEDIR=$W CCACHE_COMPILERCHECK=content
	export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-25G}
	LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

pin() { # dir url rev [branch/tag for a shallow clone]
	local dir=$1 url=$2 rev=$3 ref=${4:-}
	if [ ! -d "$dir/.git" ]; then
		if [ -n "$ref" ]; then
			git clone -q --depth 1 -b "$ref" "$url" "$dir"
		else
			git clone -q --filter=blob:none "$url" "$dir"
		fi
	fi
	git -C "$dir" cat-file -e "$rev^{commit}" 2>/dev/null || git -C "$dir" fetch -q --filter=blob:none origin "$rev"
	[ "$(git -C "$dir" rev-parse "$rev^{commit}")" = "$rev" ] || { echo "$dir: $rev missing" >&2; exit 1; }
}

fetch() {
	mkdir -p "$SRC"
	pin "$SRC/igc" "$IGC_URL" "$IGC_REV"
	pin "$SRC/llvm-project" "$LLVM_URL" "$LLVM_REV" "$LLVM_TAG"
	pin "$SRC/llvm-project/llvm/projects/opencl-clang" "$OCLCLANG_URL" "$OCLCLANG_REV"
	pin "$SRC/llvm-project/llvm/projects/llvm-spirv" "$SPIRV_LLVM_URL" "$SPIRV_LLVM_REV"
	pin "$SRC/SPIRV-Tools" "$SPIRV_TOOLS_URL" "$SPIRV_TOOLS_REV"
	pin "$SRC/SPIRV-Headers" "$SPIRV_HEADERS_URL" "$SPIRV_HEADERS_REV"
	pin "$SRC/vc-intrinsics" "$VCI_URL" "$VCI_REV" "$VCI_TAG"
	# the unpatched ones at their pins (igc and opencl-clang: the patch step)
	git -C "$SRC/llvm-project/llvm/projects/llvm-spirv" checkout -q "$SPIRV_LLVM_REV"
	git -C "$SRC/SPIRV-Tools" checkout -q "$SPIRV_TOOLS_REV"
	git -C "$SRC/SPIRV-Headers" checkout -q "$SPIRV_HEADERS_REV"
}

# component dir, pinned revision, patch directory: branch intelxe = the pin + our series (git am)
apply_series() {
	local dir=$1 rev=$2 pdir=$3
	git -C "$dir" checkout -q -f -B intelxe "$rev"
	local p
	while read -r p; do
		[ -n "$p" ] || continue
		git -C "$dir" -c user.name=intelxe-build -c user.email=build@localhost am -q --keep-cr "$pdir/$p"
	done < "$pdir/series"
}

patch() {
	apply_series "$SRC/igc" "$IGC_REV" "$HERE/patches/igc"
	apply_series "$SRC/llvm-project/llvm/projects/opencl-clang" "$OCLCLANG_REV" "$HERE/patches/opencl-clang"
	# IGC copies llvm-project/llvm (with projects/) into its build tree and patches the copy; a changed series is
	# only picked up by a fresh copy
	rm -rf "$BLD/IGC/llvm-deps-22.1.8/src" "$BLD/IGC/llvm-deps-22.1.8/llvm_patches.stamp"
}

configure() {
	# No CMAKE_SYSTEM_NAME/PROCESSOR: CMake would then cross-compile, and LLVM would build a second, native tablegen.
	# The x86_64 build tools run under Rosetta instead; LLVM_HOST_TRIPLE tells LLVM what it is built for. No zstd,
	# libxml2, libedit: LLVM would find Homebrew's arm64 libraries on Apple silicon (and IGC needs none of them); no
	# static analyzer or ARC migrator in the clang that compiles the built-ins. opencl-clang always from the tree: IGC
	# looks for a "system" libopencl-clang first and would find the one install put into $IGC_PREFIX/lib.
	cmake -S "$SRC/igc" -B "$BLD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 "${LAUNCHER[@]}" \
		-DLLVM_HOST_TRIPLE=x86_64-apple-darwin -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-apple-darwin \
		-DIGC_OPTION__LLVM_MODE=Source -DIGC_OPTION__LLVM_PREFERRED_VERSION=22.1.8 \
		-DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
		-DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
		-DCCLANG_BUILD_INTREE_LLVM=TRUE -DCCLANG_FROM_SYSTEM=FALSE \
		-DPython3_EXECUTABLE="$W/venv/bin/python3" -DBISON_EXECUTABLE="$BISON" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -Wno-dev >/dev/null
}

timed() { # label target...
	local label=$1 s
	shift
	s=$(date +%s)
	ninja -C "$BLD" -j "$J" "$@"
	echo "== $label: $(($(date +%s) - s)) s"
}

llvm() { timed llvm clang; }
deps() { timed deps LLVMSPIRVLib opencl-clang SPIRV-Tools-static LLVMGenXIntrinsics; }
igc() { timed igc igc_dll fcl_dll IGA_EXE; }

install_() {
	rm -rf "$PREFIX"
	local c
	for c in igc-core igc-core-devel igc-opencl igc-opencl-devel; do
		cmake --install "$BLD" --component "$c" >/dev/null
	done
	# libigdfcl links libopencl-clang (@rpath): next to it, with @loader_path in the rpath of both libraries
	cp -a "$BLD"/IGC/llvm-deps-22.1.8/build/src/lib/libopencl-clang*.dylib "$PREFIX/lib/"
	local f r
	for f in "$PREFIX"/lib/*.dylib; do
		[ -L "$f" ] && continue
		# the build tree's absolute rpaths (LLVM's) out, @loader_path in
		for r in $(otool -l "$f" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
			case $r in /*) install_name_tool -delete_rpath "$r" "$f" 2>/dev/null ;; esac
		done
		install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true
	done
	echo "installed in $PREFIX:"
	/bin/ls -l "$PREFIX/lib" "$PREFIX/bin"
}

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(fetch patch configure llvm deps igc install)
for s in "${steps[@]}"; do
	echo "== $s"
	case $s in
	fetch | patch | configure | llvm | deps | igc) "$s" ;;
	install) install_ ;;
	*) echo "unknown step $s" >&2; exit 2 ;;
	esac
done
