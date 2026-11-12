#!/bin/sh
# SPDX-License-Identifier: MIT
# disk-rules-test.sh: every vector of stick-disk-vectors.json through write-stick.sh's rules (write-stick.sh --rules),
# with each POSIX shell found here (sh, dash, busybox sh). IntelXe Manager's StickRules runs the same vectors in
# windows/app/IntelXe.Core.Tests (StickDiskVectorTests). Needs python3 (to read the JSON); no Docker, no disk.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
SHELLS=""
for s in sh dash bash "busybox sh"; do
	set -- $s
	command -v "$1" >/dev/null 2>&1 && SHELLS="$SHELLS|$s"
done
exec python3 -I -c '
import json, subprocess, sys
vectors_file, script, shells = sys.argv[1], sys.argv[2], [s for s in sys.argv[3].split("|") if s]
data = json.load(open(vectors_file))
assert data["maxBytes"] == 128 * 10**9, "write-stick.sh has MAXGB=128"
bad = 0
for shell in shells:
    for v in data["vectors"]:
        b = lambda k: "1" if v[k] else "0"
        args = shell.split() + [script, "--rules", v["bus"], b("removable"), b("whole"), b("system"), str(v["size"]), str(v["minimum"]), b("allowLarge")]
        got = subprocess.run(args, capture_output=True, text=True)
        codes = sorted(got.stdout.split())
        if got.returncode != 0 or codes != sorted(v["refuse"]) or any(c not in data["codes"] for c in codes):
            bad += 1
            print("FAIL [%s] %s: got %s (exit %d, %s), expected %s" % (shell, v["name"], codes, got.returncode, got.stderr.strip(), sorted(v["refuse"])))
    print("%-10s %d vectors" % (shell, len(data["vectors"])))
print("disk rules: %s" % ("FAILED (%d)" % bad if bad else "all vectors match"))
sys.exit(1 if bad else 0)
' "$HERE/stick-disk-vectors.json" "$HERE/write-stick.sh" "$SHELLS"
