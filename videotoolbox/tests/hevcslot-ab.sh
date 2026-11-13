#!/bin/bash
# SPDX-License-Identifier: MIT
# hevcslot-ab.sh <kit>: on the B580 machine, the libavcodec path (INTELXEDEC_DIRECT=0) over the conformance streams on
# which libavcodec's DPB array index (FFmpeg 9.0.2's Vulkan slotIndex) reaches the driver's slot count, with the
# libraries of the kit staged one set at a time (nothing installed):
#   installed  the installed libavcodec/libavutil and libvulkan_intel.dylib (13 Nov, HEVC: SIGABRT, __stack_chk_fail
#              under anv_h265_decode_video: FFmpeg passed DPB slot 16, ANV wrote past its 16-entry stack array; the
#              H.264 streams were never run on the B580: expected the same, or worse, as 17-19 land past dpb_slots[17])
#   ffmpeg     libs-ffmpeg/: FFmpeg with ffmpeg-9.0.2-vulkan-decode-dpb-slots-below-maxDpbSlots.patch (compact slots)
#              and the installed ANV. Expected: every stream right (every picture equals the software reference)
#   anv        libs-anv/: the installed FFmpeg and ANV with mesa/patches/0106 (decodes with a slot outside the
#              session refused). Expected: no abort, no GPU reset; the pictures FFmpeg sends with a slot of 16 or more
#              fail (vkEndCommandBuffer: VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR), so those streams come out WRONG
#              (pictures missing or predicted from them), not lost; the controls right
#   both       libs-both/: both fixes. Expected: as ffmpeg
# A set is staged with DYLD_LIBRARY_PATH, exported here inside bash (a DYLD_* variable put in front of a protected
# binary such as perl, env or sh is dropped when it starts): dyld then takes a library of the same leaf name from that
# folder first, for the plug-in's linked libavcodec/libavutil and for its dlopen of
# /Library/GPUBundles/nvmtl/libvulkan_intel.dylib alike. Each run's loaded copies are listed (DYLD_PRINT_LIBRARIES) in
# out/<set>/<stream>.libs, so the log shows which libraries ran.
# Streams (with their software references <dir>/ref/<name>.md5, sorted, vtdec --md5's format):
#   conf-hevc/RPS_B_qualcomm_5, RPS_C_ericsson_5, RPS_E_qualcomm_5: the only JCT-VC HEVC_v1 streams whose index reaches
#     16 (on 4 / 1 / 14 pictures; ANV has 16 H.265 slots), RPS_A_docomo_5 as a control (at most 5);
#   conf-h264slot/HCBP2_HHI_A, HCMP1_HHI_A: JVT streams whose H.264 index reaches 19 (on 14 pictures each; ANV has 17
#     H.264 slots, dpb_slots[17] on the stack), MR3_TANDBERG_B as a control (at most 16).
# HEVCSLOT_SETS picks the sets (default all four, in the order above). Output lines: "^S <set> <stream>: ..." and
# "^SUMMARY".
K=${1:?usage: hevcslot-ab.sh <kit>}
cd "$K" || exit 2
tmo() {
  local s=$1; shift
  "$@" & local p=$!
  ( trap 'kill $sp 2>/dev/null; exit 0' TERM; sleep "$s" & sp=$!; wait $sp; kill -9 $p 2>/dev/null ) > /dev/null 2>&1 &
  local w=$!
  wait $p; local r=$?
  kill $w 2>/dev/null; wait $w 2>/dev/null
  return $r
}
export NVMTL_PROCESS_ENV=1 NVMTL_NO_INTELXEDEC=1 INTELXEDEC_DIRECT=0
O=${HEVCSLOT_OUT:-$PWD/out-hevcslot}; mkdir -p "$O"; : > "$O/summary.txt"
L=$PWD/libintelxeav1dec.dylib V=$PWD/vtdec
STREAMS="conf-hevc/RPS_B_qualcomm_5 conf-hevc/RPS_C_ericsson_5 conf-hevc/RPS_E_qualcomm_5 conf-hevc/RPS_A_docomo_5
  conf-h264slot/HCBP2_HHI_A conf-h264slot/HCMP1_HHI_A conf-h264slot/MR3_TANDBERG_B"
echo "stats before: $(sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
for set in ${HEVCSLOT_SETS:-installed ffmpeg anv both}; do
  case $set in installed) dir=;; ffmpeg|anv|both) dir=$PWD/libs-$set;; *) echo "unknown set $set"; continue;; esac
  mkdir -p "$O/$set"
  for p in $STREAMS; do
    b=${p##*/} c=$p.mp4 ref=$(dirname "$p")/ref/${p##*/}.md5 t=$O/$set/${p##*/}
    [ -f "$c" ] || { echo "S $set $b: missing $c"; continue; }
    (
      if [ -n "$dir" ]; then export DYLD_LIBRARY_PATH="$dir"; fi
      export DYLD_PRINT_LIBRARIES=1
      tmo 120 "$V" "$c" 1000000 --lib "$L" --md5 --quiet > "$t.txt" 2> "$t.err"
    )
    r=$?
    grep -oE '/[^ ]*(libvulkan_intel|libavcodec\.63|libavutil\.61)\.dylib' "$t.err" | sort -u > "$t.libs"
    grep -E '^ ?frame ' "$t.txt" | awk '{print $NF}' | sort > "$t.md5"
    n=$(wc -l < "$t.md5" | tr -d ' ')
    if [ -f "$ref" ]; then
      miss=$(comm -23 "$ref" "$t.md5" | wc -l | tr -d ' '); extra=$(comm -13 "$ref" "$t.md5" | wc -l | tr -d ' ')
      if [ "$miss" = 0 ] && [ "$extra" = 0 ]; then v=right; elif [ "$miss" = 0 ]; then v="right (+$extra)"
      else v="WRONG ($miss of $(wc -l < "$ref" | tr -d ' ') missing, $extra others)"; fi
    else v="no reference"; fi
    case $r in 134) why=" (SIGABRT)";; 137) why=" (killed: time box)";; 139) why=" (SIGSEGV)";; *) why=;; esac
    echo "S $set $b: exit $r$why, $n pictures, $v; ANV $(grep libvulkan_intel "$t.libs" | head -1), libavcodec $(grep libavcodec "$t.libs" | head -1)" | tee -a "$O/summary.txt"
  done
done
echo "stats after: $(sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
echo "SUMMARY $(grep -c ': exit 0, .*, right' "$O/summary.txt") of $(grep -c '^S ' "$O/summary.txt") runs right; aborted $(grep -c 'SIGABRT' "$O/summary.txt")"
