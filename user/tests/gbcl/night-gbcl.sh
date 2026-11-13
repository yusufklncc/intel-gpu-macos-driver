# night-gbcl.sh: phase_gbclmicro, sourced by a night script that defines tmo (the bash timeout function: never perl or
# env wrappers, which strip DYLD_*), say and $O. Runs cl_gbmicro (user/tests/gbcl), the Geekbench-OpenCL-shaped
# microbenchmark, through the installed plugin: every mode 3 times, the modes interleaved and the plugin's host-pointer
# import (NVMTL_IMPORT_UNALIGNED=1) against copy alternating, first one then the other, so drift spreads over both.
# Out: $O/gbcl/<mode>-<copy|import>-<run>.log, $O/gbcl/results.txt (one RESULT line per run), $O/gbcl/table.txt.
# ~16 s per run with GBCL_T=15: 5 modes x 2 x 3 = 30 runs, about 9 minutes.
GBCL=${GBCL:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cl_gbmicro}
GBCL_T=${GBCL_T:-15}
gbcl_resets() { sysctl -n debug.intelxe.stats 2>/dev/null | sed -nE 's/.*GuC context resets ([0-9]+); GT resets ([0-9]+).*/\1 \2/p'; }
phase_gbclmicro() {
  local d="$O/gbcl" r m v vs tag r0 r1 g0 t0 g1 t1
  mkdir -p "$d"; : > "$d/results.txt"
  [ -x "$GBCL" ] || { say "GBCL: no binary at $GBCL"; return 0; }
  say "GBCL: $GBCL, $GBCL_T s per mode; kernel $(sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'rb8[a-z]' | sort -u | tr '\n' ' ')"
  for r in 1 2 3; do
    vs="copy import"; [ $((r % 2)) = 0 ] && vs="import copy"
    for m in chain churn import mixed live; do
      for v in $vs; do
        tag="$m-$v-$r"; r0=$(gbcl_resets)
        ( export NVMTL_PROCESS_ENV=1 NVMTL_ENABLE=1
          if [ "$v" = import ]; then export NVMTL_IMPORT_UNALIGNED=1; else unset NVMTL_IMPORT_UNALIGNED; fi
          tmo $((GBCL_T * 3 + 60)) "$GBCL" -q -t "$GBCL_T" -m "$m" ) > "$d/$tag.log" 2>&1
        r1=$(gbcl_resets); read -r g0 t0 <<< "${r0:-0 0}"; read -r g1 t1 <<< "${r1:-0 0}"
        sed -n "s/^RESULT /RESULT run=$r plugin=$v /p" "$d/$tag.log" >> "$d/results.txt"
        grep -qE '^(RESULT|  kstat)' "$d/$tag.log" || say "GBCL $tag: no result: $(tail -2 "$d/$tag.log" | tr '\n' ' ')"
        grep -qE '^(CHECK|FAIL)' "$d/$tag.log" && say "GBCL $tag: $(grep -E '^(CHECK|FAIL)' "$d/$tag.log" | head -2 | tr '\n' ' ')"
        [ $((g1 - g0 + t1 - t0)) -gt 0 ] && say "GBCL $tag: guc+$((g1 - g0)) gt+$((t1 - t0))"
      done
    done
  done
  # table: per mode and plugin setting, the three runs' ops/s, their median, and the median p50 / p99
  awk '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
         k = f["mode"] " " f["plugin"]; if (!(k in seen)) { seen[k] = 1; order[++n] = k }
         ops[k] = ops[k] " " f["ops_s"]; p50[k] = p50[k] " " f["p50_us"]; p99[k] = p99[k] " " f["p99_us"] }
       function med(s,   a, c, i, j, t) { c = split(s, a, " "); for (i = 1; i <= c; i++) for (j = i + 1; j <= c; j++) if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
                                        return c ? a[int((c + 1) / 2)] : "-" }
       END { printf "%-10s %-6s %-28s %10s %9s %9s\n", "mode", "plugin", "ops/s (runs)", "ops/s med", "p50 us", "p99 us"
             for (i = 1; i <= n; i++) { k = order[i]; split(k, mk, " ")
               printf "%-10s %-6s %-28s %10s %9s %9s\n", mk[1], mk[2], ops[k], med(ops[k]), med(p50[k]), med(p99[k]) } }' \
    "$d/results.txt" > "$d/table.txt"
  # the kernel counters that moved in run 1, per log (<mode>-<copy|import>-1: counter, delta, per op)
  grep -H '  kstat' "$d"/*-1.log 2>/dev/null | sed -E "s|^$d/||; s|\.log:  kstat [^:]*:|:|" > "$d/kstat-run1.txt"
  say "GBCL table:"; while IFS= read -r l; do say "  $l"; done < "$d/table.txt"
}
