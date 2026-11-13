#!/bin/bash
# SPDX-License-Identifier: MIT
# build-sycl.sh [STEP...]: SYCL on macOS x86_64 over our Level Zero: Intel's DPC++ (intel/llvm) - the SYCL runtime
# libsycl, the Unified Runtime loader and its Level Zero adapters, UMF, and the clang -fsycl compiler with its tools and
# device libraries - built from one LLVM tree for x86_64 on the build Mac (Apple silicon: the build tools and the
# compiler run under Rosetta, as in build-igc.sh). See docs/design/DESIGN-sycl.md.
#   STEP: fetch patch configure compiler runtime stage (default: all, in that order); onednn (optional: fetch, patch
#         and build oneDNN's SYCL GPU library with the built compiler, into $SYCL_WORK/onednn-build; stage then takes
#         it when it is there)
#   SYCL_WORK  work directory (default $HOME/intelxe-sycl): src/intel-llvm, deps/ (pinned upstream checkouts; patched
#              on branch intelxe), build/, stage/ (what goes to the test machine)
#   L0_WORK    compute/build-neo.sh's work directory (default $HOME/intelxe-l0): the Level Zero loader and headers it
#              installed in prefix/, NEO's ze_intel_gpu.h, and (OCL=1) the OpenCL headers and ICD loader sources
#   IGC_PREFIX optional: compute/build-igc.sh's install. Its ocloc (staged by build-neo.sh in $L0_WORK/stage/bin) lets
#              the test program carry a Battlemage binary next to its SPIR-V (ahead-of-time, spir64_gen)
#   JOBS       ninja -j (default: all cores)
# Needs git, cmake >= 3.21, ninja and python3 on PATH, the Xcode command line tools, Rosetta on Apple silicon.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
W=${SYCL_WORK:-$HOME/intelxe-sycl}
L0W=${L0_WORK:-$HOME/intelxe-l0}
SRC=$W/src/intel-llvm
DEPS=$W/deps
BLD=$W/build
STAGE=${OUT:-$W/stage}
J=${JOBS:-$(/usr/sbin/sysctl -n hw.ncpu)}

# Pins. intel/llvm at the DPC++ v7.2.0 tag; the dependencies its CMake would fetch, at the revisions it names there
# (FetchContent is run disconnected: nothing is downloaded during the build). TREE = SHA-256 of `git ls-tree -r <rev>`
# (every path, mode and blob id of the revision: the checkouts are partial clones, so this is what can be hashed
# without fetching every blob).
DPCPP_URL=https://github.com/intel/llvm.git
DPCPP_TAG=v7.2.0
DPCPP_REV=71767974d44acf0464ebf4b26c2059781eb7d6ee
DPCPP_TREE=08d6ade04888c35b48fa93c52d71365967fa91aee1d2d5c4b58b2009d95f1eb3
UMF_URL=https://github.com/oneapi-src/unified-memory-framework.git
UMF_TAG=v1.2.0
UMF_REV=d8763834d68f9545db30bea5d092edd78ad3d748
UMF_TREE=8c058107871176aca133e9fd0258271ef7f03fd5aa99577b7ebb89626c667754
EMHASH_URL=https://github.com/ktprime/emhash.git
EMHASH_REV=5e131ba09a5290823fe71099d9c35eb5df5345b6
EMHASH_TREE=ca8e7447efc4c0f9f1422f8886e1f0ab30dc7335b004069049b098cb1f0151ef
SPVH_URL=https://github.com/KhronosGroup/SPIRV-Headers.git
SPVH_REV=5c50cbd25a40f8b60e44a2ccc2f1ba3c9e0d0299
SPVH_TREE=96ec83a58480759f36e39667a2b593b27d5e8cc2e0f9e4af6155f6a1a771a53a
VCI_URL=https://github.com/intel/vc-intrinsics.git
VCI_REV=4fc83e12979096db72c129bd432238d5ca397e4d
VCI_TREE=7add8f3a5f54bed11ea8e5b262b5b8c5405575f3380994152ca9d4d0a29dd263
# the XPTI framework's hash map (xptifw/src/CMakeLists.txt names this commit, version 1.3.12)
PHMAP_URL=https://github.com/greg7mdp/parallel-hashmap.git
PHMAP_REV=8a889d3699b3c09ade435641fb034427f3fd12b6
PHMAP_TREE=130db94cb553e4340b1981ba94b4bcfd8e943088a8f7eb534cd5940de804ce37
# oneDPL (oneAPI's parallel STL over SYCL; header-only), release tag oneDPL-release-2022.14.0
ONEDPL_URL=https://github.com/uxlfoundation/oneDPL.git
ONEDPL_TAG=oneDPL-release-2022.14.0
ONEDPL_REV=66efa97efe014565b2f8dd9e579efd76dd8cd9ec
ONEDPL_TREE=72b4f8f925897021ec14706ff79879a4bc092762412211ceec1ecd21d49006dd
# oneDNN (oneAPI's deep-learning primitives), release tag v3.14.2: built by the optional step `onednn` only
ONEDNN_URL=https://github.com/uxlfoundation/oneDNN.git
ONEDNN_TAG=v3.14.2
ONEDNN_REV=4db284662a37cf46401655b5ee99391d9de52c00
ONEDNN_TREE=14290b4ad45880d036959f78ef9a3ecf4bfd49b80e290027d8dc98823e004c63
# the parts of intel/llvm the build reads (sparse checkout)
DPCPP_PATHS=(buildbot clang cmake libc libclc libdevice llvm llvm-spirv opencl sycl sycl-jit third-party unified-runtime xpti
	xptifw)

command -v ninja >/dev/null || { echo "ninja is not on PATH (pip install ninja, or brew install ninja)" >&2; exit 1; }

tree_sum() { git -C "$1" ls-tree -r "$2" | shasum -a 256 | cut -c1-64; }

# dir url rev tree-sha256 [sparse paths...]
get() {
	local dir=$1 url=$2 rev=$3 sum=$4
	shift 4
	if [ ! -d "$dir/.git" ]; then
		mkdir -p "$dir"
		git -C "$dir" init -q
		git -C "$dir" remote add origin "$url"
		[ $# -eq 0 ] || git -C "$dir" sparse-checkout set "$@"
		git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$rev"
		git -C "$dir" checkout -q FETCH_HEAD
	fi
	[ "$(git -C "$dir" rev-parse "$rev^{commit}")" = "$rev" ] || { echo "$dir: $rev missing" >&2; exit 1; }
	local got
	got=$(tree_sum "$dir" "$rev")
	[ "$got" = "$sum" ] || { echo "$dir: tree SHA-256 $got, pinned $sum" >&2; exit 1; }
	echo "$(basename "$dir") $rev ok"
}

fetch() {
	get "$SRC" "$DPCPP_URL" "$DPCPP_REV" "$DPCPP_TREE" "${DPCPP_PATHS[@]}"
	get "$DEPS/umf" "$UMF_URL" "$UMF_REV" "$UMF_TREE"
	get "$DEPS/emhash" "$EMHASH_URL" "$EMHASH_REV" "$EMHASH_TREE"
	get "$DEPS/spirv-headers" "$SPVH_URL" "$SPVH_REV" "$SPVH_TREE"
	get "$DEPS/vc-intrinsics" "$VCI_URL" "$VCI_REV" "$VCI_TREE"
	get "$DEPS/parallel-hashmap" "$PHMAP_URL" "$PHMAP_REV" "$PHMAP_TREE"
	get "$DEPS/onedpl" "$ONEDPL_URL" "$ONEDPL_REV" "$ONEDPL_TREE" include LICENSE.txt
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
	apply_series "$SRC" "$DPCPP_REV" "$HERE/patches/intel-llvm"
	apply_series "$DEPS/umf" "$UMF_REV" "$HERE/patches/umf"
}

configure() {
	local L0P=$L0W/prefix
	[ -e "$L0P/lib/libze_loader.dylib" ] || { echo "no Level Zero loader in $L0P (compute/build-neo.sh loader)" >&2; exit 1; }
	[ -e "$L0W/src/neo/level_zero/include/level_zero/ze_intel_gpu.h" ] || { echo "no NEO source in $L0W/src/neo" >&2; exit 1; }
	[ -d "$L0W/src/ocl-headers/CL" ] && [ -d "$L0W/src/ocl-icd-loader" ] ||
		{ echo "no OpenCL headers/ICD loader in $L0W/src (OCL=1 compute/build-neo.sh fetch)" >&2; exit 1; }
	# the adapters include <level_zero/ze_api.h> (the loader's) and <level_zero/ze_intel_gpu.h> (NEO's): one directory
	# with both, the loader's headers from build-neo.sh's prefix and NEO's from the pinned NEO source
	rm -rf "$W/l0-include"
	mkdir -p "$W/l0-include/level_zero"
	cp -R "$L0P/include/level_zero/." "$L0W/src/neo/level_zero/include/level_zero/." "$W/l0-include/level_zero/"
	# x86_64 on any build Mac: LLVM_HOST_TRIPLE and no CMAKE_SYSTEM_NAME, so the build's own tools (tablegen, the
	# just-built clang compiling libdevice) are x86_64 and run under Rosetta (compute/build-igc.sh does the same).
	# DEFAULT_SYSROOT: the built clang finds the macOS SDK (libc++, libSystem) without -isysroot, for libdevice's host
	# objects and for SYCL programs.
	# Level Zero only (the OpenCL adapter is not built), no tests. XPTI tracing (libsycl's and the UR loader's trace
	# points, the xptifw dispatcher, sycl-trace/sycl-prof and their collectors) and sycl-jit (kernel_compiler's
	# in-process compilation of SYCL source; upstream's configure.py leaves both it and libdevice out on Darwin).
	PKG_CONFIG_PATH=$L0P/lib/pkgconfig cmake -S "$SRC/llvm" -B "$BLD" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
		-DLLVM_HOST_TRIPLE=x86_64-apple-darwin -DLLVM_TARGETS_TO_BUILD="X86;SPIRV" \
		-DDEFAULT_SYSROOT="$(xcrun --show-sdk-path)" \
		-DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
		-DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
		-DLLVM_ENABLE_PROJECTS="clang;sycl;llvm-spirv;opencl;xpti;xptifw;libdevice;sycl-jit" \
		-DLLVM_EXTERNAL_PROJECTS="sycl;llvm-spirv;opencl;xpti;xptifw;libdevice;sycl-jit" \
		-DLLVM_EXTERNAL_SYCL_SOURCE_DIR="$SRC/sycl" -DLLVM_EXTERNAL_LLVM_SPIRV_SOURCE_DIR="$SRC/llvm-spirv" \
		-DLLVM_EXTERNAL_XPTI_SOURCE_DIR="$SRC/xpti" -DXPTI_SOURCE_DIR="$SRC/xpti" \
		-DLLVM_EXTERNAL_XPTIFW_SOURCE_DIR="$SRC/xptifw" -DLLVM_EXTERNAL_SYCL_JIT_SOURCE_DIR="$SRC/sycl-jit" \
		-DLLVM_EXTERNAL_LIBDEVICE_SOURCE_DIR="$SRC/libdevice" -DLLVM_EXTERNAL_OPENCL_SOURCE_DIR="$SRC/opencl" \
		-DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$DEPS/spirv-headers" \
		-DSYCL_ENABLE_BACKENDS="level_zero;level_zero_v2" -DSYCL_ENABLE_XPTI_TRACING=ON \
		-DSYCL_ENABLE_EXTENSION_JIT=ON -DSYCL_JIT_ENABLE_WERROR=OFF -DSYCL_INCLUDE_TESTS=OFF \
		-DSYCL_ENABLE_MAJOR_RELEASE_PREVIEW_LIB=OFF \
		-DSYCL_ENABLE_WERROR=OFF -DXPTI_ENABLE_WERROR=OFF -DXPTI_ENABLE_TESTS=OFF -DSYCL_UR_BUILD_TESTS=OFF \
		-DUR_ENABLE_TRACING=ON -DUR_ENABLE_SANITIZER=OFF -DUR_ENABLE_SYMBOLIZER=OFF -DUR_USE_EXTERNAL_UMF=OFF \
		-DUMF_HWLOC_STANDIN=ON -DUMF_BUILD_CUDA_PROVIDER=OFF -DUMF_BUILD_LIBUMF_POOL_JEMALLOC=OFF \
		-DL0_COMPUTE_RUNTIME_HEADERS="$W/l0-include" \
		-DFETCHCONTENT_FULLY_DISCONNECTED=ON \
		-DFETCHCONTENT_SOURCE_DIR_UNIFIED-MEMORY-FRAMEWORK="$DEPS/umf" \
		-DFETCHCONTENT_SOURCE_DIR_EMHASH="$DEPS/emhash" \
		-DFETCHCONTENT_SOURCE_DIR_VC-INTRINSICS="$DEPS/vc-intrinsics" \
		-DFETCHCONTENT_SOURCE_DIR_PARALLEL-HASHMAP="$DEPS/parallel-hashmap" \
		-DFETCHCONTENT_SOURCE_DIR_OCL-HEADERS="$L0W/src/ocl-headers" \
		-DFETCHCONTENT_SOURCE_DIR_OCL-ICD="$L0W/src/ocl-icd-loader" \
		-DCMAKE_INSTALL_PREFIX="$W/prefix"
}

# the compiler: clang (-fsycl), the offload tools the driver runs (sycl-post-link, llvm-spirv, clang-offload-wrapper,
# ...), and libdevice (the device libraries, compiled by the just-built clang)
compiler() {
	ninja -C "$BLD" -j "$J" sycl-compiler libsycldevice
}

# the runtime: libsycl (with the UR loader linked in), both Level Zero adapters, UMF, sycl-ls; sycl-jit (in-process
# SYCL compiler for kernel_compiler); XPTI: the xptifw dispatcher, sycl-trace and sycl-prof with their collectors
runtime() {
	ninja -C "$BLD" -j "$J" sycl-runtime-libraries ur_adapter_level_zero ur_adapter_level_zero_v2 umf sycl-ls \
		sycl-jit xptifw sycl-trace sycl-prof
}

# $1 output, $2 source, $3... extra flags: a test program with the built compiler for macOS 15 x86_64. The linker is
# named by path: /usr/bin/ld is a shim that loads libxcrun, which has no x86_64 slice, and the x86_64 clang runs it
# from a Rosetta process on an Apple-silicon build Mac.
sycl_cc() {
	local out=$1 src=$2
	shift 2
	"$BLD/bin/clang++" -fsycl "$@" -O2 -mmacosx-version-min=15.0 --ld-path="$(xcrun -f ld)" \
		-o "$out" "$src" -Wl,-rpath,@executable_path/../lib
}

# $1 output, $2 source, $3... extra flags: with a Battlemage binary next to the SPIR-V when an ocloc is there
# (compute/build-neo.sh with IGC_PREFIX stages one): the driver runs `ocloc` from PATH; the runtime takes the native
# image on a matching device and the SPIR-V elsewhere
sycl_cc_aot() {
	local out=$1 src=$2 oc=${OCLOC:-$L0W/stage/bin/ocloc}
	shift 2
	if [ -x "$oc" ]; then
		PATH=$(dirname "$oc"):$PATH sycl_cc "$out" "$src" -fsycl-targets=spir64_gen,spir64 \
			-Xsycl-target-backend=spir64_gen "-device bmg" "$@"
	else
		echo "no ocloc ($oc): $(basename "$out") carries SPIR-V only"
		sycl_cc "$out" "$src" -fsycl-targets=spir64 "$@"
	fi
}

# The host headers sycl_libs' run-time SYCL compilation reads (libc++ and the C library's, from the build Mac's SDK),
# copied into $1 with the SDK's layout: sycl-jit compiles SYCL source as the driver's device-only compile does, with
# the program's host target as the auxiliary triple, so it needs the platform's C++ headers; the test machine has no
# SDK. The list is what a device-only compile of the same source includes from the SDK.
rtc_sysroot() {
	local dst=$1 sdk tmp
	sdk=$(xcrun --show-sdk-path)
	tmp=$(mktemp -d)
	awk '/R"SYCL\(/{p=1; next} /\)SYCL"/{p=0} p' "$REPO/user/tests/sycl/sycl_libs.cpp" > "$tmp/rtc.cpp"
	"$BLD/bin/clang++" -fsycl -fsycl-device-only -fsyntax-only -isysroot "$sdk" -MD -MF "$tmp/rtc.d" \
		-o /dev/null "$tmp/rtc.cpp"
	mkdir -p "$dst"
	tr ' \\' '\n\n' < "$tmp/rtc.d" | grep "^$sdk/" | sort -u | while read -r f; do
		mkdir -p "$dst/$(dirname "${f#"$sdk"/}")"
		cp "$f" "$dst/${f#"$sdk"/}"
	done
	cp "$sdk/SDKSettings.json" "$dst/"
	rm -rf "$tmp"
	echo "rtc-sysroot: $(find "$dst" -type f | wc -l | tr -d ' ') files from $sdk"
}

# The test machine's layout: bin/ sycl_hello (SPIR-V and, with ocloc, a Battlemage binary), sycl_hello_jit (SPIR-V
# only), sycl_libs (oneDPL and the kernel compiler; SPIR-V + Battlemage), sycl-ls, sycl-trace, sycl-prof; lib/ libsycl,
# the UR loader and adapters, UMF, sycl-jit, the XPTI dispatcher and the trace collectors (the Level Zero loader and
# driver come from the L0 stage); share/rtc_kernels.spv; rtc-sysroot/ (the SDK headers the run-time SYCL compile
# reads); night-sycl.sh; ad-hoc signed; SHA256SUMS over everything.
stage() {
	rm -rf "$STAGE"
	mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share"
	cp -a "$BLD"/lib/libsycl.*dylib "$BLD"/lib/libur_adapter_level_zero*.dylib "$BLD"/lib/libumf.*dylib \
		"$BLD"/lib/libsycl-jit*.dylib "$BLD"/lib/libxptifw*.dylib "$BLD"/lib/libsycl_ur_trace_collector.dylib \
		"$BLD"/lib/libze_trace_collector.dylib "$BLD"/lib/libsycl_profiler_collector.dylib "$STAGE/lib/"
	cp "$BLD/bin/sycl-ls" "$BLD/bin/sycl-trace" "$BLD/bin/sycl-prof" "$STAGE/bin/"
	# the build Mac's library directories as rpaths: @loader_path instead (UMF, xptifw beside them; the Level Zero
	# loader from DYLD_LIBRARY_PATH, the L0 stage's lib/)
	local f r
	for f in "$STAGE"/lib/*.dylib; do
		[ -L "$f" ] && continue
		for r in $(otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
			case $r in /*) install_name_tool -delete_rpath "$r" "$f" ;; esac
		done
		otool -l "$f" | grep -q "path @loader_path " || install_name_tool -add_rpath @loader_path "$f"
	done
	sycl_cc_aot "$STAGE/bin/sycl_hello" "$REPO/user/tests/sycl/sycl_hello.cpp"
	sycl_cc "$STAGE/bin/sycl_hello_jit" "$REPO/user/tests/sycl/sycl_hello.cpp" -fsycl-targets=spir64
	sycl_cc_aot "$STAGE/bin/sycl_libs" "$REPO/user/tests/sycl/sycl_libs.cpp" -I"$DEPS/onedpl/include"
	# the SPIR-V kernel sycl_libs loads: OpenCL C through this clang and the SPIR-V translator
	"$BLD/bin/clang" --target=spir64 -cl-std=CL2.0 -Xclang -finclude-default-header -O2 -c -emit-llvm \
		-o "$STAGE/share/rtc_kernels.bc" "$REPO/user/tests/sycl/kernels/rtc_kernels.cl"
	"$BLD/bin/llvm-spirv" "$STAGE/share/rtc_kernels.bc" -o "$STAGE/share/rtc_kernels.spv"
	rm "$STAGE/share/rtc_kernels.bc"
	rtc_sysroot "$STAGE/rtc-sysroot"
	# oneDNN, when the onednn step was run: lib/libdnnl and bin/onednn_hello (SPIR-V only: oneDNN's own kernels are
	# compiled at run time, so it runs over the IGC stage anyway)
	local ob=$W/onednn-build
	if [ -e "$ob/src/libdnnl.dylib" ]; then
		cp -a "$ob"/src/libdnnl*.dylib "$STAGE/lib/"
		for r in $(otool -l "$STAGE"/lib/libdnnl.3.*.dylib | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
			case $r in /*) install_name_tool -delete_rpath "$r" "$STAGE"/lib/libdnnl.3.*.dylib ;; esac
		done
		otool -l "$STAGE"/lib/libdnnl.3.*.dylib | grep -q "path @loader_path " ||
			install_name_tool -add_rpath @loader_path "$STAGE"/lib/libdnnl.3.*.dylib
		sycl_cc "$STAGE/bin/onednn_hello" "$REPO/user/tests/sycl/onednn_hello.cpp" -fsycl-targets=spir64 \
			-I"$DEPS/onednn/include" -I"$ob/include" -L"$ob/src" -ldnnl
	else
		echo "no oneDNN ($ob): run the onednn step for lib/libdnnl and bin/onednn_hello"
	fi
	cp "$REPO/user/tests/sycl/night-sycl.sh" "$STAGE/"
	for f in "$STAGE"/bin/* "$STAGE"/lib/*.dylib; do
		[ -L "$f" ] || codesign -s - -f "$f" 2>/dev/null
	done
	(cd "$STAGE" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
	echo "staged in $STAGE ($(du -sh "$STAGE" | cut -f1)):"
	(cd "$STAGE" && ls -l bin lib)
}

# oneDNN for Intel GPUs through SYCL (Level Zero), no CPU engine, Xe2 kernels only (the B580), no graph API, tests or
# examples; compiled by the built clang (-fsycl objects need intel-llvm 0009 on Mach-O, and every link of them -fsycl:
# such an object is a bundler container, not a Mach-O file, so ld64 alone cannot read it). Patches onednn/: the Level
# Zero and OpenCL loaders by their macOS names, size_t != uint64_t. nGEN generates most kernels as Xe2 binaries; the
# OpenCL C ones are compiled at run time through the OpenCL device with the same UUID (NEO's OpenCL ICD), so a run
# needs the IGC stage and OCL_ICD_FILENAMES (oneDNN's other route, through kernel_compiler and ocloc, is its
# experimental ONEDNN_EXPERIMENTAL_SYCL_KERNEL_COMPILER, not used).
onednn() {
	local ob=$W/onednn-build ld
	get "$DEPS/onednn" "$ONEDNN_URL" "$ONEDNN_REV" "$ONEDNN_TREE"
	apply_series "$DEPS/onednn" "$ONEDNN_REV" "$HERE/patches/onednn"
	ld=$(xcrun -f ld)
	# a link-only -fsycl command ignores -mmacosx-version-min (the result would require the build Mac's macOS); the
	# environment variable is honoured (DESIGN-sycl.md section 7.3)
	export MACOSX_DEPLOYMENT_TARGET=15.0
	cmake -S "$DEPS/onednn" -B "$ob" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER="$BLD/bin/clang" -DCMAKE_CXX_COMPILER="$BLD/bin/clang++" \
		-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
		-DCMAKE_EXE_LINKER_FLAGS="-fsycl --ld-path=$ld" -DCMAKE_SHARED_LINKER_FLAGS="-fsycl --ld-path=$ld" \
		-DDNNL_TARGET_ARCH=X64 -DDNNL_CPU_RUNTIME=NONE -DDNNL_GPU_RUNTIME=SYCL -DDNNL_GPU_VENDOR=INTEL \
		-DONEDNN_ENABLE_PRIMITIVE_GPU_ISA=XE2 -DONEDNN_BUILD_GRAPH=OFF -DDNNL_BUILD_TESTS=OFF \
		-DDNNL_BUILD_EXAMPLES=OFF -DONEDNN_WERROR=OFF
	ninja -C "$ob" -j "$J" dnnl
}

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(fetch patch configure compiler runtime stage)
for s in "${steps[@]}"; do
	echo "== $s"
	case $s in
	fetch | patch | configure | compiler | runtime | stage | onednn) "$s" ;;
	*) echo "unknown step $s" >&2; exit 2 ;;
	esac
done
