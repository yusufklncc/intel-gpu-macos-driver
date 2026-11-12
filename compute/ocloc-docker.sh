#!/bin/bash
# SPDX-License-Identifier: MIT
# ocloc-docker.sh: Intel's offline compiler (ocloc, with IGC) in a Linux x86_64 container, for the kernels the Mac
# cannot compile yet (IGC is not ported; docs/design/DESIGN-level-zero.md). The output is native Battlemage code
# (zebin), which the macOS Level Zero driver loads without a compiler.
#   compute/ocloc-docker.sh kernel FILE.cl OUTDIR      OUTDIR/<name>_bmg.bin, for zeModuleCreate(ZE_MODULE_FORMAT_NATIVE)
#   compute/ocloc-docker.sh builtins NEO_SRC OUTDIR    NEO's buffer built-in kernels (fill, copy, copy rect, kernel
#                                                      timestamps) for Battlemage in its four addressing modes, named as
#                                                      the driver's FileStorage looks them up in the working directory:
#                                                      20_1_0_<mode>_<kernel>.builtin_kernel.bin (IP= overrides 20_1_0)
#   compute/ocloc-docker.sh shell                      a shell in the container
# Needs docker that runs linux/amd64 images (OrbStack, or Docker Desktop with Rosetta). The packages are the compute-
# runtime 26.40.40196.8 release (ocloc) and IGC v2.42.6, checked against the sha256 sums the releases publish; they are
# kept in $L0_WORK/ocloc-debs and baked into the image intelxe-ocloc:26.40.40196.8 once.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${L0_WORK:-$HOME/intelxe-l0}
DEBS=$W/ocloc-debs
IMAGE=intelxe-ocloc:26.40.40196.8
CR=https://github.com/intel/compute-runtime/releases/download/26.40.40196.8
IGC=https://github.com/intel/intel-graphics-compiler/releases/download/v2.42.6
PKGS=(
	"$CR/intel-ocloc_26.40.40196.8-0_amd64.deb b2868d6d7958cc5ca28fa9b48a7d39961760ee39ef07673d5b58894dd53d6b4e"
	"$CR/libigdgmm12_22.10.2_amd64.deb 053fceadb84a1d2f9a30a0d1bc9b16424b3f1a90cc49900fcd80aadcf25c8a4d"
	"$IGC/intel-igc-core-2_2.42.6+23070_amd64.deb e20539632f2d99875d8f8b225d5da58b31f98baa54bb47c6328aa98ec473ab9d"
	"$IGC/intel-igc-opencl-2_2.42.6+23070_amd64.deb 7f5593cf828503ffb8450967768bde9209e22753bc576296e846b5a751011fa3"
)

image() {
	docker image inspect "$IMAGE" >/dev/null 2>&1 && return
	mkdir -p "$DEBS"
	local p url sum f
	for p in "${PKGS[@]}"; do
		url=${p% *} sum=${p#* } f=$DEBS/$(basename "$url")
		[ -f "$f" ] || curl -sSfL -o "$f" "$url"
		echo "$sum  $f" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $f" >&2; rm -f "$f"; exit 1; }
	done
	local ctx
	ctx=$(mktemp -d)
	cp "$DEBS"/*.deb "$ctx/"
	cat > "$ctx/Dockerfile" <<'EOF'
FROM --platform=linux/amd64 ubuntu:24.04
COPY *.deb /tmp/debs/
RUN apt-get -qq update && apt-get -qq install -y --no-install-recommends /tmp/debs/*.deb && rm -rf /tmp/debs /var/lib/apt/lists/*
EOF
	docker build -q --platform linux/amd64 -t "$IMAGE" "$ctx" >/dev/null
	rm -rf "$ctx"
}

run() { docker run --rm --platform linux/amd64 "$@"; }

kernel() {
	local src out name
	src=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
	mkdir -p "$2"
	out=$(cd "$2" && pwd)
	name=$(basename "$src" .cl)
	run -v "$(dirname "$src")":/in:ro -v "$out":/out -w /out "$IMAGE" \
		ocloc compile -q -file "/in/$(basename "$src")" -device bmg -output "$name"
	rm -f "$out/${name}_bmg.spv"
	ls -l "$out/${name}_bmg.bin"
}

# The options are those of shared/source/built_ins/kernels/CMakeLists.txt for Battlemage (XE2_HPG_CORE's builtin
# addressing modes in cmake/setup_platform_flags.cmake, the 20.1.0 release), source first to SPIR-V, then to the binary.
builtins() {
	local neo out
	neo=$(cd "$1" && pwd)
	mkdir -p "$2"
	out=$(cd "$2" && pwd)
	run -v "$neo/shared/source/built_ins/kernels":/in:ro -v "$out":/out -w /out -e IP="${IP:-20_1_0}" "$IMAGE" bash -c '
set -e
types="-Didx_t=uint -Dcoord2_t=uint2 -Dcoord4_t=uint4 -Doffset_t=uint"
wide="-Didx_t=ulong -Dcoord2_t=ulong2 -Dcoord4_t=ulong4 -Doffset_t=ulong"
lsc="-DLSC_STCC_DEFAULT=0 -DLSC_STCC_L1UC_L3UC=1 -DLSC_STCC_L1UC_L3WB=2 -DLSC_STCC_L1WT_L3UC=3 -DLSC_STCC_L1WT_L3WB=4 -DLSC_STCC_L1S_L3UC=5 -DLSC_STCC_L1S_L3WB=6 -DLSC_STCC_L1WB_L3WB=7 -DUSE_LSC_INTRINSICS_WB"
mkdir -p /tmp/w
for mode in bindful_image_stateless_buffer bindful_image_stateless_buffer_wide bindless_image_stateless_buffer bindless_image_stateless_buffer_wide; do
  t=$types; case $mode in *_wide) t=$wide;; esac
  addr=bindful; internal=()
  case $mode in bindless_*) addr=bindless; internal=(-internal_options "-cl-intel-use-bindless-mode -cl-intel-use-bindless-advanced-mode");; esac
  for k in copy_buffer_rect copy_buffer_to_buffer copy_kernel_timestamps fill_buffer; do
    ocloc -q -file /in/$k.builtin_kernel -spv_only -device bmg -heapless_mode disable -cl-intel-greater-than-4GB-buffer-required -64 \
      -output ${mode}_$k -out_dir /tmp/w "${internal[@]}" -options "$t $lsc -cl-kernel-arg-info"
    ocloc -q -file /tmp/w/${mode}_${k}_bmg.spv -spirv_input -device 20.1.0 -heapless_mode disable -cl-intel-greater-than-4GB-buffer-required \
      -64 -stateful_address_mode $addr -output ${IP}_${mode}_$k.builtin_kernel -output_no_suffix -out_dir /out "${internal[@]}" \
      -options "$t $lsc -cl-kernel-arg-info"
  done
done
rm -f /out/*.spv'
	ls "$out" | wc -l | xargs echo "built-in binaries in $out:"
}

case ${1:-} in
kernel) image; kernel "${2:?FILE.cl}" "${3:?OUTDIR}" ;;
builtins) image; builtins "${2:?NEO_SRC}" "${3:?OUTDIR}" ;;
shell) image; run -it "$IMAGE" bash ;;
*) sed -n '3,15p' "$0"; exit 2 ;;
esac
