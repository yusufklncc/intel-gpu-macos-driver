#!/bin/bash
# SPDX-License-Identifier: MIT
# run-fixtures.sh: builds an EFI for every fixture of fixtures/fixtures.json and checks it, on macOS.
#
#   PYTHON=/path/to/venv/bin/python IASL=/path/to/iasl tests/run-fixtures.sh WORK_DIR [PACKAGE_DIR]
#
# WORK_DIR (outside the repository) receives the fixtures, the EFIs, logs and a copy per EFI for stage 4. Nothing it
# writes may be committed: the EFIs hold generated serial numbers. PACKAGE_DIR is an unpacked
# intel-gpu-macos-driver-*-efi.zip (IntelXe.kext, SSDT-ARCOFF.aml, HybridPM.kext, SHA256SUMS); without it the stage-4
# part is skipped.
#
# Per fixture:
#   1. efibuild.py build (OpCore Simplify with the answer rules, stage 2, ocvalidate): ok, or the expected stop;
#   2. efi.sh (release/templates) on the EFI: --settings-safari-plan has nothing to change (stage 2 is done through
#      the same efi-rules.json), --status shows no IntelXe.kext / SSDT-ARCOFF.aml yet;
#   3. stage 4 on a copy, with the fixture's CPU and card port as facts: efi.sh --plan, --apply, then
#      efibuild.py validate --after-stage4 (ocvalidate again, IntelXe.kext, SSDT-ARCOFF.aml, HybridPM below Lilu);
#   4. no SMBIOS value (serial, MLB, UUID, ROM) in the build log, the result or anything printed.
# Before them: the stage-2/Safari vectors of release/efi-rules-vectors.json through the builder's rules reader.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EB=$(cd "$HERE/.." && pwd)
REPO=$(cd "$EB/../.." && pwd)
[ $# -ge 1 ] || { sed -n '3,20p' "$0"; exit 2; }
mkdir -p "$1"; W=$(cd "$1" && pwd)
PKG=${2:+$(cd "$2" && pwd)}
PY=${PYTHON:-python3}
IASL=${IASL:-$(command -v iasl || true)}
[ -x "$IASL" ] || { echo "no iasl: set IASL"; exit 2; }
S=$REPO/release/templates/efi.sh
export INTELXE_EFI_RULES=$REPO/release/templates/efi-rules.json
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok    $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $*"; }

echo "== the answer rules (tests/test_answers.py)"
if out=$("$PY" -I "$HERE/test_answers.py" 2>&1); then ok "$(tail -1 <<< "$out"): $(grep -o 'Ran [0-9]* tests' <<< "$out")"; else bad "$out"; fi

echo "== the stage-2/Safari vectors through rules.py"
if out=$("$PY" -I "$EB/efibuild.py" vectors 2>&1); then ok "$out"; else bad "$out"; fi

echo "== fixtures into $W/fixtures"
"$PY" -I "$HERE/fetch_fixtures.py" "$W/fixtures" > "$W/fixtures.list" || { echo "fetching the fixtures failed"; exit 1; }

field() { "$PY" -I -c 'import json,sys; d=json.load(open(sys.argv[1])); f=[x for x in d["fixtures"] if x["name"]==sys.argv[2]][0]
v=f
for k in sys.argv[3].split("."): v=v.get(k) if isinstance(v,dict) else None
print("" if v is None else (str(v).lower() if isinstance(v,bool) else v))' "$HERE/fixtures/fixtures.json" "$1" "$2"; }
jget() { "$PY" -I -c 'import json,sys; v=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): v=v.get(k) if isinstance(v,dict) else None
print("" if v is None else (str(v).lower() if isinstance(v,bool) else v))' "$1" "$2"; }
secrets_check() {   # secrets_check CONFIG FILE...: none of the config's SMBIOS values in the files
    "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1])
from intelxe_efibuild import validate
s = validate.read_secrets(sys.argv[2])
hits = [f for f in sys.argv[3:] if validate.find_secrets(open(f, encoding="utf-8", errors="replace").read(), s)]
print("{} SMBIOS strings; found in: {}".format(len(s), ", ".join(hits) or "none")); sys.exit(1 if hits or not s else 0)' "$EB" "$@"
}

while IFS=$'\t' read -r name macos dir; do
    echo "== $name (macOS $macos)"
    out=$W/out/$name res=$W/out/$name.result.json log=$W/out/$name.build.txt
    mkdir -p "$W/out"
    "$PY" -I "$EB/efibuild.py" build --report "$dir/Report.json" --acpi "$dir/ACPI" --out "$out" --macos "$macos" \
        --iasl "$IASL" --result "$res" > "$log" 2>&1
    rc=$?
    want_ok=$(field "$name" expect.ok); want_err=$(field "$name" expect.error)
    if [ "$want_ok" = false ]; then
        err=$(jget "$res" error)
        if [ $rc -ne 0 ] && [[ "$err" == *"$want_err"* ]]; then ok "stops as expected: ${err%%$'\n'*}"; else bad "expected a stop with '$want_err' (exit $rc): ${err%%$'\n'*}"; fi
        continue
    fi
    if [ $rc -eq 0 ]; then ok "built: $(jget "$res" checks.checks.ocvalidate.found) ocvalidate finding(s), all accepted; opencore $(jget "$res" opencore); smbios $(jget "$res" facts.smbios)"
    else bad "build failed: $(jget "$res" error | head -5)"; continue; fi
    C=$out/EFI/OC/config.plist

    # stage 2 through efi.sh and the same rules
    export INTELXE_STATE_DIR=$W/state/$name INTELXE_BOOT_PATH="" INTELXE_IOREG_FILE=/dev/null
    export INTELXE_CPU_BRAND="$("$PY" -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["CPU"]["Processor Name"])' "$dir/Report.json")"
    port=$(field "$name" card_port); export INTELXE_CARD_ACPI_PATH="$port"
    p=$(bash "$S" ${PKG:+--package "$PKG"} --efi "$out" --settings-safari-plan 2>&1)
    if [[ "$p" == *"nothing to change"* ]]; then ok "efi.sh --settings-safari-plan: nothing to change"; else bad "efi.sh still plans stage 2: $p"; fi
    st=$(bash "$S" ${PKG:+--package "$PKG"} --efi "$out" --status 2>&1)
    for kv in settings_changes=0 kext_file=absent kext_entry=absent ssdt_file=absent ssdt_entry=absent secure_boot_model=Disabled; do
        grep -qx "$kv" <<< "$st" || bad "efi.sh --status: no $kv ($(grep "^${kv%%=*}=" <<< "$st"))"
    done
    ok "efi.sh --status: $(grep -E '^(settings_changes|kext_file|ssdt_file|lilu_entry|hybridpm_needed)=' <<< "$st" | tr '\n' ' ')"

    # stage 4 on a copy
    if [ -n "$PKG" ]; then
        s4=$W/stage4/$name; rm -rf "$s4"; mkdir -p "$W/stage4"; ditto "$out" "$s4"
        bash "$S" --package "$PKG" --efi "$s4" --plan > "$W/stage4/$name.plan" 2>&1 || bad "efi.sh --plan failed: $(cat "$W/stage4/$name.plan")"
        if bash "$S" --package "$PKG" --efi "$s4" --apply --expect "$(shasum -a 256 < "$W/stage4/$name.plan" | cut -d' ' -f1)" > "$W/stage4/$name.apply" 2>&1; then
            ok "stage 4 applied by efi.sh: $(grep -c '^copy\|^config' "$W/stage4/$name.plan") step(s) ($(grep -o 'IntelXe.kext\|SSDT-ARCOFF.aml\|HybridPM.kext' "$W/stage4/$name.plan" | sort -u | tr '\n' ' '))"
        else bad "efi.sh --apply failed: $(tail -5 "$W/stage4/$name.apply")"; fi
        if v=$("$PY" -I "$EB/efibuild.py" validate --efi "$s4" --report "$dir/Report.json" --after-stage4 2> "$W/stage4/$name.err"); then
            ok "after stage 4: ocvalidate and the stage-4 checks pass"
        else bad "after stage 4: $(jget <(echo "$v") failures)"; fi
        printf '%s\n' "$v" > "$W/stage4/$name.validate.json"
    fi

    # no SMBIOS value anywhere but config.plist
    files=("$log" "$res" "$out/build-log.txt")
    [ -n "$PKG" ] && files+=("$W/stage4/$name.plan" "$W/stage4/$name.apply" "$W/stage4/$name.validate.json")
    if r=$(secrets_check "$C" "${files[@]}"); then ok "secrets: $r"; else bad "secrets: $r"; fi
done < "$W/fixtures.list"

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
