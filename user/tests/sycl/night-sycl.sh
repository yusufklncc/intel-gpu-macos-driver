# SPDX-License-Identifier: MIT
# night phase Y (sourced by the night script, which defines tmo, say and $O): SYCL on the B580 - Intel's DPC++ runtime
# (libsycl, the Unified Runtime loader, its Level Zero adapters v1 and v2, UMF; compute/build-sycl.sh) over our Level
# Zero (compute-runtime/NEO on xemac). Nothing is installed.
# Stage: compute/build-sycl.sh stage on the build Mac (13 Nov: ~/hackintosh/work/syclred/stage), copied to ~/b580/sycl
# (SYCL_STAGE overrides): lib/ (libsycl and the UR/UMF dylibs), bin/sycl_hello (SPIR-V + an ahead-of-time Battlemage
# binary), bin/sycl_hello_jit (SPIR-V only), bin/sycl-ls, SHA256SUMS. The Level Zero driver comes from the L0 stages:
# ~/l0 (no compiler: the AOT binary is used, and NEO's built-ins from ~/l0/builtins, so it runs in that directory) and
# ~/l0igc (IGC in the driver: SPIR-V compiled at run time; SYCL_L0 and SYCL_L0IGC override the two). Each run is a
# subshell with the variables exported (no env/perl wrapper: they would strip DYLD_*).
# Expected: Y0 nothing listed; Y1 one "[level_zero:gpu]" Intel(R) Arc(TM) B580 Graphics; Y2 "sycl_hello build": PASS
# device query, PASS free memory, PASS sysman (SKIP over an L0 stage without NEO patch 0013: zesInit fails there), PASS
# kernel build with 10 kernels (no GPU work: the program's kernels created from the native image); Y3/Y4 (the v2
# adapter, the default for Battlemage, and v1) "PASS: 0 failure(s)", exit=0, eight PASS lines (seven when sysman is
# SKIP); Y5 the same through IGC (SPIR-V JIT, ~/l0igc). The first B580 run (13 Nov, the stage before intel-llvm patch
# 0006) stopped Y3-Y5 with a null call in the reduction (DESIGN-sycl.md section 3.1). sycl_hello's source is
# user/tests/sycl/sycl_hello.cpp (13 Nov: the first one was never committed; this one was written again from
# DESIGN-sycl.md section 4 and adds the free-memory and sysman steps).
# Since 14 Nov (stage with sycl_libs, sycl-jit and XPTI; DESIGN-sycl.md section 7): Y6 "sycl_libs dpl", oneDPL with a
# device policy over ~/l0 (AOT image): seven PASS lines (sort, sort with a comparator, reduce, transform_reduce,
# inclusive_scan, count_if/find_if, copy_if) and "PASS: 0 failure(s)"; Y7 "sycl_libs rtc" over ~/l0igc: can_build
# "spirv 1, opencl 1, sycl 1", then PASS for the SPIR-V module, the OpenCL C source (ocloc) and the SYCL source
# (sycl-jit, host headers from the stage's rtc-sysroot), each run on the GPU; Y8 sycl-trace around "sycl_hello vector":
# "[UR]" call lines (about 100), "[SYCL]" queue events and the vector add's PASS; Y9 sycl-prof: a non-empty Chrome
# trace (9-prof.json.gz, about 200 events); Y10 (when the stage has oneDNN) onednn_hello over ~/l0igc: four PASS (the
# GPU engine, ReLU, matmul, convolution against host references), the implementations oneDNN chose, exit=0.
# On the build Mac the same phase ran against xemac's canned device (XEMAC_FAKE=1): Y1 and Y2 pass there, Y3-Y5 pass the
# device query and then wait for a completion that never comes (fake mode executes nothing) until tmo kills them.
phase_sycl() {
  local D=${SYCL_STAGE:-~/b580/sycl} L0=${SYCL_L0:-~/l0} L0IGC=${SYCL_L0IGC:-~/l0igc} R="$O/sycl"; mkdir -p "$R"
  local t='tmo 60'
  say "Y: SYCL (DPC++ over our Level Zero) - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && shasum -a 256 -c SHA256SUMS 2>&1 | grep -v ': OK$' > "$R/0-sums.txt" ); say "Y0 sums: $(grep -c . "$R/0-sums.txt") not OK"
  # $1 result name, $2 L0 stage, $3 directory to run in, $4... command (relative to $D/bin); extra variables in $YX
  yrun() {
    local n=$1 l0=$2 dir=$3; shift 3
    ( cd "$dir" && export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH="$D/lib:$l0/lib" ${YX:-}
      $t "$D/bin/$@" > "$R/$n.txt" 2>&1; echo "exit=$?" >> "$R/$n.txt" )
  }
  # the same run again with the runtime's, UR's and NEO's logs on, when it failed
  ydebug() {
    local n=$1 l0=$2 dir=$3; shift 3
    grep -q "exit=0" "$R/$n.txt" && return 0
    ( cd "$dir" && export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH="$D/lib:$l0/lib" ${YX:-} \
        SYCL_UR_TRACE=2 UR_LOG_LEVEL_ZERO="level:debug;flush:debug;output:stdout" UR_LOG_LOADER="level:debug;output:stdout" \
        NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1
      $t "$D/bin/$@" > "$R/$n-debug.full" 2>&1; echo "exit=$?" >> "$R/$n-debug.full"
      tail -120 "$R/$n-debug.full" > "$R/$n-debug.txt"; gzip -f "$R/$n-debug.full" )
  }
  yrun 1-sycl-ls $L0 $L0/builtins sycl-ls --verbose
  say "Y1 sycl-ls: $(grep -m2 -E '^\[level_zero|exit=' "$R/1-sycl-ls.txt" | tr '\n' ' ')"
  ydebug 1-sycl-ls $L0 $L0/builtins sycl-ls --verbose
  yrun 2-build $L0 $L0/builtins sycl_hello build
  say "Y2 sycl_hello build (kernels from the AOT bmg image, no GPU work): $(grep -E '^(PASS|FAIL)' "$R/2-build.txt" | tr '\n' ' ') $(tail -1 "$R/2-build.txt")"
  ydebug 2-build $L0 $L0/builtins sycl_hello build
  yrun 3-hello-v2 $L0 $L0/builtins sycl_hello
  say "Y3 sycl_hello (L0 v2 adapter, AOT bmg): $(grep -cE '^PASS ' "$R/3-hello-v2.txt") PASS, $(grep -m1 -E '^FAIL' "$R/3-hello-v2.txt") $(tail -2 "$R/3-hello-v2.txt" | tr '\n' ' ')"
  ydebug 3-hello-v2 $L0 $L0/builtins sycl_hello
  YX="UR_LOADER_USE_LEVEL_ZERO_V2=0" yrun 4-hello-v1 $L0 $L0/builtins sycl_hello
  say "Y4 sycl_hello (L0 v1 adapter, AOT bmg): $(grep -cE '^PASS ' "$R/4-hello-v1.txt") PASS, $(grep -m1 -E '^FAIL' "$R/4-hello-v1.txt") $(tail -2 "$R/4-hello-v1.txt" | tr '\n' ' ')"
  YX="UR_LOADER_USE_LEVEL_ZERO_V2=0" ydebug 4-hello-v1 $L0 $L0/builtins sycl_hello
  if [ -d "$L0IGC/lib" ]; then
    yrun 5-hello-jit $L0IGC $L0IGC sycl_hello_jit
    say "Y5 sycl_hello_jit (SPIR-V through IGC): $(grep -cE '^PASS ' "$R/5-hello-jit.txt") PASS, $(grep -m1 -E '^FAIL' "$R/5-hello-jit.txt") $(tail -2 "$R/5-hello-jit.txt" | tr '\n' ' ')"
    ydebug 5-hello-jit $L0IGC $L0IGC sycl_hello_jit
  else
    say "Y5 skipped: no $L0IGC (the IGC stage)"
  fi
  # what sits on top (14 Nov stage, DESIGN-sycl.md section 6): oneDPL with a device policy (AOT image, no IGC needed)
  if [ -x "$D/bin/sycl_libs" ]; then
    yrun 6-dpl $L0 $L0/builtins sycl_libs dpl
    say "Y6 oneDPL (device policy, AOT bmg): $(grep -cE '^PASS ' "$R/6-dpl.txt") PASS, $(grep -m1 -E '^FAIL' "$R/6-dpl.txt") $(tail -2 "$R/6-dpl.txt" | tr '\n' ' ')"
    ydebug 6-dpl $L0 $L0/builtins sycl_libs dpl
    # run-time compilation: SPIR-V and OpenCL C (ocloc) and SYCL source (sycl-jit), all through IGC; the SYCL source
    # takes its host headers from the stage's rtc-sysroot (the test machine has no SDK)
    if [ -d "$L0IGC/lib" ]; then
      YX="SYCL_RTC_SYSROOT=$D/rtc-sysroot" yrun 7-rtc $L0IGC $L0IGC sycl_libs rtc
      say "Y7 kernel_compiler (SPIR-V, OpenCL C, SYCL source; IGC): $(grep -E '^(PASS|FAIL|SKIP) rtc|can_build' "$R/7-rtc.txt" | tr '\n' ' ') $(tail -1 "$R/7-rtc.txt")"
      YX="SYCL_RTC_SYSROOT=$D/rtc-sysroot" ydebug 7-rtc $L0IGC $L0IGC sycl_libs rtc
    else
      say "Y7 skipped: no $L0IGC (the IGC stage)"
    fi
  else
    say "Y6/Y7 skipped: no sycl_libs in $D (a stage before 14 Nov)"
  fi
  # XPTI: sycl-trace (SYCL events and UR calls) and sycl-prof (a Chrome trace file) around the vector add
  if [ -x "$D/bin/sycl-trace" ]; then
    yrun 8-trace $L0 $L0/builtins sycl-trace --sycl --ur.call "$D/bin/sycl_hello" vector
    say "Y8 sycl-trace: $(grep -c '^\[UR\]' "$R/8-trace.txt") UR calls, $(grep -c '^\[SYCL\]' "$R/8-trace.txt") SYCL events, $(grep -E '^(PASS|FAIL) vector' "$R/8-trace.txt") $(tail -1 "$R/8-trace.txt")"
    ydebug 8-trace $L0 $L0/builtins sycl-trace --sycl --ur.call "$D/bin/sycl_hello" vector
    yrun 9-prof $L0 $L0/builtins sycl-prof -o "$R/9-prof.json" "$D/bin/sycl_hello" vector
    say "Y9 sycl-prof: $(wc -c < "$R/9-prof.json" 2>/dev/null || echo no) bytes of trace, $(grep -o '"name"' "$R/9-prof.json" 2>/dev/null | wc -l | tr -d ' ') events, $(tail -1 "$R/9-prof.txt")"
    [ -f "$R/9-prof.json" ] && gzip -f "$R/9-prof.json"
  else
    say "Y8/Y9 skipped: no sycl-trace in $D"
  fi
  # oneDNN's SYCL GPU engine: ReLU, matmul, convolution; nGEN's Xe2 kernels and OpenCL C ones compiled through NEO's
  # OpenCL (the device with the same UUID), so over ~/l0igc with its OpenCL ICD
  if [ -x "$D/bin/onednn_hello" ] && [ -d "$L0IGC/lib" ]; then
    YX="OCL_ICD_FILENAMES=$L0IGC/lib/libigdrcl.dylib" yrun 10-onednn $L0IGC $L0IGC onednn_hello
    say "Y10 oneDNN (GPU engine, ReLU, matmul, conv; IGC): $(grep -cE '^PASS ' "$R/10-onednn.txt") PASS, $(grep -m1 -E '^FAIL' "$R/10-onednn.txt") $(grep -m2 'implementation:' "$R/10-onednn.txt" | tr '\n' ' ') $(tail -2 "$R/10-onednn.txt" | tr '\n' ' ')"
    YX="OCL_ICD_FILENAMES=$L0IGC/lib/libigdrcl.dylib ONEDNN_VERBOSE=all" ydebug 10-onednn $L0IGC $L0IGC onednn_hello
  else
    say "Y10 skipped: no onednn_hello in $D or no $L0IGC"
  fi
  say "Y: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
