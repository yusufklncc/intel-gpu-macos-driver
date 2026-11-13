#!/bin/bash
# SPDX-License-Identifier: MIT
# hevcslot-ab.sh <kit>: on the B580 machine, the libavcodec path (INTELXEDEC_DIRECT=0) over the conformance streams on
# which libavcodec's DPB array index (FFmpeg 9.0.2's Vulkan slotIndex) reaches the driver's slot count, with the
# libraries of the kit staged one set at a time (nothing installed):
#   installed  the installed libavcodec/libavutil and libvulkan_intel.dylib. 13 Nov B580: RPS_B/C/E SIGABRT
#              (__stack_chk_fail under anv_h265_decode_video: slot 16 past ANV's 16-entry stack array); HCBP2/HCMP1
#              right (slots 17-19 wrote past dpb_slots[17] without hitting the canary); MR3_TANDBERG_B WRONG (90 of
#              300: FFmpeg's long-term lookup among the first 16 DPB entries gave a reference at entry 16 slot 0)
#   ffmpeg     libs-ffmpeg/: FFmpeg with the DPB slot patch (compact slots, the long-term lookup fixed) and the
#              exec fence patch, the installed ANV. Expected: all 7 right (13 Nov, slot patch only: 7 of 7)
#   anv        libs-anv/: the installed FFmpeg, ANV with mesa/patches/0106 (slot indices are keys up to 255 for
#              H.264/H.265; nothing refused, the command buffer always ends VK_SUCCESS). Expected: no abort, no hang;
#              RPS_B/C/E, HCBP2, HCMP1, RPS_A right; MR3_TANDBERG_B WRONG as installed (FFmpeg's slot 0 collision,
#              not a driver matter). 13 Nov with the first 0106 (refusing the decode): all five hung (exit 137)
#   both       libs-both/: both. Expected: all 7 right
#   fence      libs-fence/: the first 0106 (refuses the decode: vkEndCommandBuffer fails) with FFmpeg carrying only the
#              exec fence patch (old slot numbers). Shows that the 13 Nov hang was FFmpeg's reset fence: expected no
#              hang (no exit 137), the refused pictures lost, so RPS_B/C/E, HCBP2, HCMP1 WRONG; RPS_A right; MR3 WRONG
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
# HEVCSLOT_SETS picks the sets (default all five, in the order above). Output lines: "^S <set> <stream>: ..." and
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
for set in ${HEVCSLOT_SETS:-installed ffmpeg anv both fence}; do
  case $set in installed) dir=;; ffmpeg|anv|both|fence) dir=$PWD/libs-$set;; *) echo "unknown set $set"; continue;; esac
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
