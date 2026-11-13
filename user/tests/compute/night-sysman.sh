# SPDX-License-Identifier: MIT
# night phase Z (sourced by the night script, which defines tmo, say and $O): Level Zero sysman (zes*) over
# IntelXe.kext - compute-runtime/NEO with patches 0013 (the device's sysfs answered from the kext: sysfs_macos.cpp) and
# 0014 (frequency range control, engine activity through an emulated xe PMU) and the kext's debug.intelxe.sensors_raw
# (uapi/intelxe_sensors.h; version 2 with engine counters) and live slpc_min/slpc_max (sysman2, DESIGN-level-zero.md
# 10.6). Nothing is installed by this phase.
# Stages (the build Mac, docs/design/DESIGN-level-zero.md section 10): ~/l0sm = compute/build-neo.sh's stage with the
# patch (bin/zes_info, ze_hello, cl_hello; lib/; builtins/; SHA256SUMS), SYSMAN_L0 overrides; ~/l0smigc = the same
# with IGC (SYSMAN_L0IGC); ~/b580/sycl = build-sycl.sh's stage with the new sycl_hello (SYCL_STAGE).
# The kext: sensors_raw exists only in an IntelXe.kext built from this branch. With an older kext zesInit still works,
# the frequency domain reads -1 and there are no power or temperature handles (Z0 says which kext is loaded).
# Each run is a subshell with the variables exported (no env/perl wrapper: they would strip DYLD_*); <= 60 s each.
# Expected with the new kext: Z1 "PASS: 0 failed", frequency 0 (gpu) with a hardware range RPn-RP0 (400-2850 on the
# B580), a range of slpc_min (1200) to RP0, actual/request in MHz (actual 0 while the GT sleeps in RC6), two power
# domains (card, package) with energy counters and a few W idle, PL1/PL2/crit limits, two temperatures (gpu =
# package, memory = VRAM) in the 30-60 C range, memory 12 GB with free < size; Z2 five samples, each power > 0;
# Z3 as the user: "can control 0", "NONE zesFrequencySetRange" (not root); Z3r (only with passwordless sudo) as root:
# "can control 1", "asked 1500-2400 MHz, reads 1500-2400", the old range restored; Z3k (sudo) the kext directly:
# slpc_max 2000 then 0, sensors_raw max_freq follows at once, stats "range" line counts the writes and max sends;
# Z10 engine activity: zes_info busy idle (render/copy/compute groups near 0 %, every group "PASS ... busy"),
# then with ze_hello compute running: the compute (or all) group above 0 %; the kext log's "engine activity: buffer
# at GGTT ... given" for the primary and media GT; Z4 the same values as
# `sysctl debug.intelxe.sensors` (the text from the same sample path); Z5 ze_hello init and cl_hello lifetime still
# pass; Z6 sycl-ls lists ext_intel_power_limits; Z7 sycl_hello build: 4 PASS (device, free memory, sysman, kernels);
# Z8 sycl_hello on the B580 (all steps); Z9 zes_info over the IGC stage.
phase_sysman() {
  local L0=${SYSMAN_L0:-~/l0sm} L0IGC=${SYSMAN_L0IGC:-~/l0smigc} SY=${SYCL_STAGE:-~/b580/sycl} R="$O/sysman"; mkdir -p "$R"
  local t='tmo 60'
  say "Z: Level Zero sysman - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$L0" && shasum -a 256 -c SHA256SUMS 2>&1 | grep -v ': OK$' > "$R/0-sums.txt" ); say "Z0 sums: $(grep -c . "$R/0-sums.txt") not OK"
  if sysctl debug.intelxe.sensors_raw > /dev/null 2>&1; then say "Z0 kext: sensors_raw present"; else say "Z0 kext: NO sensors_raw (old kext: expect no power/temperature handles)"; fi
  sysctl -n debug.intelxe.sensors > "$R/0-sensors-before.txt" 2>&1
  # $1 result name, $2 L0 stage, $3... zes_info arguments; a failure is repeated with the sysfs trace and NEO's messages
  zrun() {
    local n=$1 l0=$2; shift 2
    ( cd "$l0" && export DYLD_LIBRARY_PATH="$l0/lib"
      $t ./bin/zes_info "$@" > "$R/$n.txt" 2>&1; echo "exit=$?" >> "$R/$n.txt" )
    grep -q "exit=0" "$R/$n.txt" && return 0
    ( cd "$l0" && export DYLD_LIBRARY_PATH="$l0/lib" NEO_MACOS_SYSFS_TRACE=1 NEOReadDebugKeys=1 PrintDebugMessages=1
      $t ./bin/zes_info "$@" > "$R/$n-debug.txt" 2>&1; echo "exit=$?" >> "$R/$n-debug.txt" )
  }
  zrun 1-info "$L0"
  say "Z1 zes_info: $(grep -E '^(PASS|FAIL)[:]' "$R/1-info.txt" | tail -1) $(grep -E '^FAIL ' "$R/1-info.txt" | head -3 | tr '\n' ' ') $(tail -1 "$R/1-info.txt")"
  say "Z1 values: $(grep -E '^PASS (frequency 0 state|power 0:|power 1:|temperature|memory 0 state)' "$R/1-info.txt" | sed 's/^PASS //' | tr '\n' ';')"
  zrun 2-watch "$L0" watch 5
  say "Z2 watch: $(grep -E '^sample' "$R/2-watch.txt" | tail -2 | tr '\n' ' ') $(tail -1 "$R/2-watch.txt")"
  zrun 3-setfreq "$L0" set-freq 1500 2400
  say "Z3 set-freq (user): $(grep -E 'can control|SetRange|frequency set' "$R/3-setfreq.txt" | tr '\n' ' ') $(tail -1 "$R/3-setfreq.txt")"
  if sudo -n true 2>/dev/null; then
    # as root: sudo strips DYLD_*, so the variable is exported inside the root bash, and tmo comes along as text
    sudo -n bash -c "$(declare -f tmo); cd '$L0' && export DYLD_LIBRARY_PATH='$L0/lib'; tmo 60 ./bin/zes_info set-freq 1500 2400" \
      > "$R/3r-setfreq-root.txt" 2>&1; echo "exit=$?" >> "$R/3r-setfreq-root.txt"
    say "Z3r set-freq (root): $(grep -E 'can control|SetRange|frequency set|after set' "$R/3r-setfreq-root.txt" | tr '\n' ' ') $(tail -1 "$R/3r-setfreq-root.txt")"
    { sysctl debug.intelxe.slpc_min debug.intelxe.slpc_max; sudo -n sysctl -w debug.intelxe.slpc_max=2000
      sleep 1; sysctl -n debug.intelxe.sensors; sysctl -n debug.intelxe.stats | grep -E '^slpc boost'
      sudo -n sysctl -w debug.intelxe.slpc_max=0; sleep 1; sysctl -n debug.intelxe.stats | grep -E '^slpc boost'
    } > "$R/3k-kext-range.txt" 2>&1
    say "Z3k kext range: $(grep -oE 'range \(slpc_min/max\).*' "$R/3k-kext-range.txt" | tr '\n' ' ')"
  else
    say "Z3r/Z3k skipped: no passwordless sudo (range control needs root)"
  fi
  sysctl -n debug.intelxe.sensors > "$R/4-sensors-after.txt" 2>&1
  say "Z4 sensors text: $(head -c 240 "$R/4-sensors-after.txt" | tr '\n' ' ')"
  ( cd "$L0" && export DYLD_LIBRARY_PATH="$L0/lib" OCL_ICD_FILENAMES="$L0/lib/libigdrcl.dylib"
    $t ./bin/ze_hello init > "$R/5-ze-init.txt" 2>&1; echo "exit=$?" >> "$R/5-ze-init.txt"
    $t ./bin/cl_hello lifetime > "$R/5-cl-lifetime.txt" 2>&1; echo "exit=$?" >> "$R/5-cl-lifetime.txt" )
  say "Z5 ze_hello init: $(tail -2 "$R/5-ze-init.txt" | tr '\n' ' '); cl_hello lifetime: $(tail -2 "$R/5-cl-lifetime.txt" | tr '\n' ' ')"
  if [ -x "$SY/bin/sycl_hello" ]; then
    ( cd "$L0/builtins" && export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH="$SY/lib:$L0/lib"
      $t "$SY/bin/sycl-ls" --verbose > "$R/6-sycl-ls.txt" 2>&1; echo "exit=$?" >> "$R/6-sycl-ls.txt"
      $t "$SY/bin/sycl_hello" build > "$R/7-sycl-build.txt" 2>&1; echo "exit=$?" >> "$R/7-sycl-build.txt"
      $t "$SY/bin/sycl_hello" > "$R/8-sycl-run.txt" 2>&1; echo "exit=$?" >> "$R/8-sycl-run.txt" )
    say "Z6 sycl-ls: power limits aspect $(grep -c ext_intel_power_limits "$R/6-sycl-ls.txt"), $(tail -1 "$R/6-sycl-ls.txt")"
    say "Z7 sycl_hello build: $(grep -E '^(PASS|FAIL|SKIP) ' "$R/7-sycl-build.txt" | cut -c1-40 | tr '\n' ';') $(tail -1 "$R/7-sycl-build.txt")"
    say "Z8 sycl_hello: $(grep -cE '^PASS ' "$R/8-sycl-run.txt") PASS, $(grep -m1 -E '^FAIL' "$R/8-sycl-run.txt") $(tail -2 "$R/8-sycl-run.txt" | tr '\n' ' ')"
  else
    say "Z6-Z8 skipped: no $SY/bin/sycl_hello"
  fi
  # Z10 engine activity: idle, then with a compute workload running beside it
  zrun 10-busy-idle "$L0" busy 1000
  say "Z10 busy idle: $(grep -E '^(PASS|FAIL|NONE) engine' "$R/10-busy-idle.txt" | sed 's/^PASS engine //' | tr '\n' ';') $(tail -1 "$R/10-busy-idle.txt")"
  ( cd "$L0" && export DYLD_LIBRARY_PATH="$L0/lib"
    ( for i in 1 2 3 4 5 6; do $t ./bin/ze_hello compute > /dev/null 2>&1; done ) &
    local bg=$!
    sleep 1; $t ./bin/zes_info busy 2000 > "$R/10-busy-load.txt" 2>&1; echo "exit=$?" >> "$R/10-busy-load.txt"
    kill $bg 2>/dev/null; wait $bg 2>/dev/null )
  say "Z10 busy under ze_hello compute: $(grep -E '^(PASS|FAIL) engine' "$R/10-busy-load.txt" | sed 's/^PASS engine //' | tr '\n' ';') $(tail -1 "$R/10-busy-load.txt")"
  sysctl -n debug.intelxe.state 2>/dev/null | grep -E 'engine activity' > "$R/10-kext-log.txt"
  say "Z10 kext: $(tr '\n' ';' < "$R/10-kext-log.txt" | cut -c1-300)"
  if [ -x "$L0IGC/bin/zes_info" ]; then
    zrun 9-info-igc "$L0IGC"
    say "Z9 zes_info (IGC stage): $(tail -2 "$R/9-info-igc.txt" | tr '\n' ' ')"
  else
    say "Z9 skipped: no $L0IGC"
  fi
  say "Z: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
