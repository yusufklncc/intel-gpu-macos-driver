# SPDX-License-Identifier: MIT
# night phase Y (sourced by the night script, which defines tmo, say and $O): SYCL on the B580 - Intel's DPC++ runtime
# (libsycl, the Unified Runtime loader, its Level Zero adapters v1 and v2, UMF; compute/build-sycl.sh) over our Level
# Zero (compute-runtime/NEO on xemac). Nothing is installed.
# Stage: ~/hackintosh/work/night1214/sycl on the build Mac, copied to ~/b580/sycl (SYCL_STAGE overrides): lib/ (libsycl
# and the UR/UMF dylibs), bin/sycl_hello (SPIR-V + an ahead-of-time Battlemage binary), bin/sycl_hello_jit (SPIR-V only),
# bin/sycl-ls, SHA256SUMS. The Level Zero driver comes from the L0 stages: ~/l0 (no compiler: the AOT binary is used, and
# NEO's built-ins from ~/l0/builtins, so it runs in that directory) and ~/l0igc (IGC in the driver: SPIR-V compiled at
# run time). Each run is a subshell with the variables exported (no env/perl wrapper: they would strip DYLD_*).
# Expected: Y0 nothing listed; Y1 one "[level_zero:gpu]" Intel(R) Arc(TM) B580 Graphics; Y2/Y3 (v2 adapter, the default
# for Battlemage, and v1) "PASS: 0 failure(s)", exit=0, six PASS lines; Y4 the same through IGC (JIT, ~/l0igc).
phase_sycl() {
  local D=${SYCL_STAGE:-~/b580/sycl} R="$O/sycl"; mkdir -p "$R"
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
        SYCL_UR_TRACE=-1 UR_LOG_LEVEL_ZERO="level:debug;flush:debug;output:stdout" UR_LOG_LOADER="level:debug;output:stdout" \
        NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1
      $t "$D/bin/$@" > "$R/$n-debug.full" 2>&1; echo "exit=$?" >> "$R/$n-debug.full"
      tail -120 "$R/$n-debug.full" > "$R/$n-debug.txt"; gzip -f "$R/$n-debug.full" )
  }
  yrun 1-sycl-ls ~/l0 ~/l0/builtins sycl-ls --verbose
  say "Y1 sycl-ls: $(grep -m2 -E 'level_zero|exit=' "$R/1-sycl-ls.txt" | tr '\n' ' ')"
  ydebug 1-sycl-ls ~/l0 ~/l0/builtins sycl-ls --verbose
  yrun 2-hello-v2 ~/l0 ~/l0/builtins sycl_hello
  say "Y2 sycl_hello (L0 v2 adapter, AOT bmg, ~/l0): $(grep -cE '^PASS ' "$R/2-hello-v2.txt") PASS, $(grep -m1 -E '^FAIL' "$R/2-hello-v2.txt") $(tail -2 "$R/2-hello-v2.txt" | tr '\n' ' ')"
  ydebug 2-hello-v2 ~/l0 ~/l0/builtins sycl_hello
  YX="UR_LOADER_USE_LEVEL_ZERO_V2=0" yrun 3-hello-v1 ~/l0 ~/l0/builtins sycl_hello
  say "Y3 sycl_hello (L0 v1 adapter, AOT bmg, ~/l0): $(grep -cE '^PASS ' "$R/3-hello-v1.txt") PASS, $(grep -m1 -E '^FAIL' "$R/3-hello-v1.txt") $(tail -2 "$R/3-hello-v1.txt" | tr '\n' ' ')"
  YX="UR_LOADER_USE_LEVEL_ZERO_V2=0" ydebug 3-hello-v1 ~/l0 ~/l0/builtins sycl_hello
  if [ -d ~/l0igc/lib ]; then
    yrun 4-hello-jit ~/l0igc ~/l0igc sycl_hello_jit
    say "Y4 sycl_hello_jit (SPIR-V through IGC, ~/l0igc): $(grep -cE '^PASS ' "$R/4-hello-jit.txt") PASS, $(grep -m1 -E '^FAIL' "$R/4-hello-jit.txt") $(tail -2 "$R/4-hello-jit.txt" | tr '\n' ' ')"
    ydebug 4-hello-jit ~/l0igc ~/l0igc sycl_hello_jit
  else
    say "Y4 skipped: no ~/l0igc (the IGC stage)"
  fi
  say "Y: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
