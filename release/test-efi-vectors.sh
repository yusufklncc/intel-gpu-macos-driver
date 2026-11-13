#!/bin/bash
# SPDX-License-Identifier: MIT
# test-efi-vectors.sh [VECTORS]: the shared test vectors of the EFI rules (release/efi-rules-vectors.json; the format
# is in docs/design/DESIGN-efi-rules.md section 5) against templates/efi.sh and its efi-rules.json, through efi.sh's
# command line, in a throwaway folder: no root, no EFI partition, no NVRAM, no python. For each vector: the EFI and
# the package it describes, the --status values it lists, the plan's steps (INTELXE_EFI_OPS) and notes, --apply with
# the plan's hash, the values, files, list of changes and --status values it must leave, then --undo back to the state
# before. Then each CPU name (hybridpm_needed
# of --status) and each PCI device (an ioreg listing with it below the shipped table's port). windows/test/
# Test-EfiVectors.ps1 runs the same vectors against IntelXe-EFI.ps1; test-efi-flow.sh runs this.
set -uo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
R=$(cd "$(dirname "$0")" && pwd)
S=$R/templates/efi.sh
VEC=${1:-$R/efi-rules-vectors.json}
ROOT=$(mktemp -d -t ixevec)
trap 'rm -rf -- "$ROOT"' EXIT
fails=0 count=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fails=$((fails + 1)); }

VP=$ROOT/vectors.plist
plutil -convert xml1 -o "$VP" "$VEC" || { echo "cannot read $VEC"; exit 1; }
v() { plutil -extract "$1" raw -o - "$VP" 2>/dev/null; }      # v KEYPATH: a value of the vectors; fails without one
n_of() { local n; n=$(v "$1") && [[ "$n" =~ ^[0-9]+$ ]] && echo "$n" || echo 0; }   # the length of a list, 0 without one
[ "$(v format)" = intelxe-efi-rules-vectors ] && [ "$(v format_version)" = 1 ] || { echo "$VEC is not intelxe-efi-rules-vectors 1"; exit 1; }
LEVELS=$(plutil -extract cards.port_levels_above_gpu raw -o - "$R/templates/efi-rules.json")
SAMPLE=$R/../$(v config)
BOOTPATH='PciRoot(0x0)/Pci(0x1B,0x4)/Pci(0x0,0x0)/NVMe(0x1,00-00-00-00-00-00-00-00)/HD(1,GPT,0b8f5a2c-0000-4000-8000-000000000001,0x28,0x64000)/\EFI\OC\OpenCore.efi%00'

hexfile() { printf "$(printf '%s' "$1" | sed 's/../\\x&/g')" > "$2"; }   # hexfile HEX FILE
js() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$s"; }     # a JSON string
jv() { case $1 in bool|integer) printf '%s' "$2";; *) js "$2";; esac; }   # jv TYPE VALUE: a JSON value

# edit PLIST KEYPATH: one edit of the vectors (a step, or remove) made with plutil
edit() {
    local op p t x
    op=$(v "$2.op"); p=$(v "$2.path"); t=$(v "$2.type")
    case $op in
        set) plutil -replace "$p" -"$t" "$(v "$2.value")" "$1";;
        remove) plutil -remove "$p" "$1";;
        append) if [ "$t" = dict ]; then plutil -insert "$p" -json "$(plutil -extract "$2.value" json -o - "$VP")" -append "$1"
                else plutil -insert "$p" -"$t" "$(v "$2.value")" -append "$1"; fi;;
        insert) plutil -insert "$p.$(v "$2.index")" -json "$(plutil -extract "$2.value" json -o - "$VP")" "$1";;
        *) echo "unknown edit $op"; return 1;;
    esac
}

# fixture DIR KEYPATH [PACKAGE]: the file a fixture of the vectors describes, at DIR/EFI/OC/<its file>
fixture() {
    local f d x id ver
    f=$(v "$2.file"); d=$1/EFI/OC/$f
    [ "$f" != SHA256SUMS ] || return 0   # the package's sums: written last (sums)
    rm -rf "$d"; mkdir -p "$(dirname "$d")"
    case $(v "$2.type") in
        kext)
            x=$(v "$2.executable"); mkdir -p "$d/$(dirname "$x")"; printf '%s\n' "$(v "$2.text")" > "$d/$x"
            plutil -create xml1 "$d/Contents/Info.plist"
            id=$(v "$2.bundle_id"); [ -z "$id" ] || plutil -insert CFBundleIdentifier -string "$id" "$d/Contents/Info.plist"
            ver=$(v "$2.version"); [ -z "$ver" ] || plutil -insert CFBundleVersion -string "$ver" "$d/Contents/Info.plist";;
        table) hexfile "$(v "tables.$(v "$2.table")")" "$d";;
        dir) mkdir -p "$d/Contents/MacOS";;
        package) ditto "$3/EFI/OC/$f" "$d"
            if x=$(v "$2.append"); then for id in "$d"/Contents/MacOS/*; do printf '%s\n' "$x" >> "$id"; done; fi;;
        none) ;;
        *) echo "unknown fixture type $(v "$2.type")"; return 1;;
    esac
}
sums() { (cd "$1" && find EFI -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS); }

# ops_json OPSFILE PACKAGE: efi.sh's steps (copy, set, append, insert, mkdict, mkarray; tab-separated) in the vectors' form
ops_json() {
    local a b c d out="[" sep="" item from
    while IFS=$'\t' read -r a b c d; do
        [ -n "$a" ] || continue
        case $a in
            copy) from=adapted; [ "$b" != "$2/EFI/OC/$c" ] || from=package
                  item="{\"op\":\"copy\",\"to\":$(js "$c"),\"from\":\"$from\"}";;
            set) item="{\"op\":\"set\",\"path\":$(js "$b"),\"type\":\"$c\",\"value\":$(jv "$c" "$d")}";;
            append) if [ "$c" = json ]; then item="{\"op\":\"append\",\"path\":$(js "$b"),\"type\":\"dict\",\"value\":$d}"
                    else item="{\"op\":\"append\",\"path\":$(js "$b"),\"type\":\"$c\",\"value\":$(jv "$c" "$d")}"; fi;;
            insert) item="{\"op\":\"insert\",\"path\":$(js "${b%.*}"),\"index\":${b##*.},\"type\":\"dict\",\"value\":$d}";;
            mkdict|mkarray) item="{\"op\":\"$a\",\"path\":$(js "$b")}";;
            *) item="{\"op\":$(js "$a")}";;
        esac
        out="$out$sep$item"; sep=,
    done < "$1"
    echo "$out]"
}

# ioreg_for VENDOR DEVICE PORT: an IOService listing with the PCI device VENDOR:DEVICE LEVELS levels below the bridge
# whose ACPI path is PORT ("none": a bridge without one), as ioreg -p IOService -w0 -l shows it
ioreg_for() {
    local i ind="  " acpi=""
    le() { printf '<%s%s0000>' "${1:2:2}" "${1:0:2}"; }   # e20b -> <0be20000>, as ioreg shows an id
    [ "$3" = none ] || acpi=IOACPIPlane:/$(printf '%s' "${3#\\}" | awk -F. '{ for (i = 1; i <= NF; i++) printf "%s%s%s", (i > 1 ? "/" : ""), $i, (i > 1 ? "@0" : "") }')
    echo "+-o Root  <class IORegistryEntry, id 0x100000100, retain 30>"
    echo "$ind+-o PORT@0  <class IOPCIDevice, id 0x100000200, registered, matched, active, busy 0 (1 ms), retain 12>"
    echo "$ind  {"; [ -z "$acpi" ] || echo "$ind    \"acpi-path\" = \"$acpi\""; echo "$ind    \"vendor-id\" = <86800000>"; echo "$ind    \"device-id\" = <e2e20000>"; echo "$ind  }"
    for ((i = 1; i < LEVELS; i++)); do
        ind="$ind  "; echo "$ind+-o bridge@0  <class IOPCIDevice, id 0x10000030$i, registered, matched, active, busy 0 (1 ms), retain 12>"
        echo "$ind  {"; echo "$ind    \"vendor-id\" = <86800000>"; echo "$ind    \"device-id\" = <e3e20000>"; echo "$ind  }"
    done
    ind="$ind  "; echo "$ind+-o display@0  <class IOPCIDevice, id 0x100000600, registered, matched, active, busy 0 (1 ms), retain 12>"
    echo "$ind  {"; echo "$ind    \"device-id\" = $(le "$2")"; echo "$ind    \"vendor-id\" = $(le "$1")"; echo "$ind  }"
}

# the package every vector starts from
P0=$ROOT/pkg0
mkdir -p "$P0/EFI/OC"
for ((j = 0; j < $(n_of package); j++)); do fixture "$P0" package.$j || exit 1; done
# config.plist as stage 1 leaves it
cp "$SAMPLE" "$ROOT/stage1.plist"
for ((j = 0; j < $(n_of stage1); j++)); do edit "$ROOT/stage1.plist" stage1.$j || { echo "stage1 edit $j failed"; exit 1; }; done

canon() { plutil -convert xml1 -o - "$1"; }   # a property list's content, keys sorted
# status_diff KEYPATH STATUSFILE: the --status values the vectors list at KEYPATH ({"key": "value"}) that differ, as
# " key=got;" (nothing when they all match, or when there is no such list)
status_diff() {
    local k got
    for k in $(plutil -extract "$1" xml1 -o - "$VP" 2>/dev/null | sed -nE 's/^[[:space:]]*<key>([a-z_]+)<\/key>$/\1/p'); do
        got=$(sed -n "s/^$k=//p" "$2")
        [ "$got" = "$(v "$1.$k")" ] || printf ' %s=%s;' "$k" "$got"
    done
}
N=$(n_of vectors)
for ((i = 0; i < N; i++)); do
    V=vectors.$i name=$(v $V.name) plan=$(v $V.plan)
    W=$ROOT/v$i P=$W/pkg F=$W/efi C=$W/efi/EFI/OC/config.plist
    mkdir -p "$W" "$F/EFI/OC/Kexts" "$F/EFI/OC/ACPI" "$F/EFI/BOOT"
    : > "$F/EFI/OC/OpenCore.efi"
    ditto "$P0" "$P"
    nosums=no
    for ((j = 0; j < $(n_of $V.package); j++)); do
        [ "$(v $V.package.$j.file)" = SHA256SUMS ] && [ "$(v $V.package.$j.type)" = none ] && nosums=yes
        fixture "$P" $V.package.$j || bad "$name: package fixture $j"
    done
    [ $nosums = yes ] || sums "$P"
    cp "$ROOT/stage1.plist" "$C"
    for ((j = 0; j < $(n_of $V.edits); j++)); do edit "$C" $V.edits.$j || bad "$name: edit $j"; done
    for ((j = 0; j < $(n_of $V.efi); j++)); do fixture "$F" $V.efi.$j "$P" || bad "$name: EFI fixture $j"; done
    ditto "$F" "$W/before"
    # the facts
    export INTELXE_STATE_DIR=$W/state INTELXE_EFI_OPS=$W/ops INTELXE_CPU_BRAND="$(v $V.facts.cpu)"
    unset INTELXE_CARD_ACPI_PATH
    if [ "$(v $V.facts.boot_path_recorded)" = true ]; then export INTELXE_BOOT_PATH=$BOOTPATH; else export INTELXE_BOOT_PATH=""; fi
    : > "$W/ioreg"
    if pci=$(v $V.facts.card.pci); then ioreg_for "${pci%%:*}" "${pci#*:}" "$(v $V.facts.card.port)" > "$W/ioreg"; fi
    export INTELXE_IOREG_FILE=$W/ioreg
    case $plan in driver) pa=--plan aa=--apply;; settings) pa=--settings-plan aa=--settings-apply;; safari) pa=--safari-plan aa=--safari-apply;;
        settings-safari) pa=--settings-safari-plan aa=--settings-safari-apply;; bootlog) pa=--bootlog-plan aa=--bootlog-apply;;
        bootlog-off) pa=--bootlog-off-plan aa=--bootlog-off-apply;; xeprobe) pa=--xeprobe-plan aa=--xeprobe-apply;;
        xeprobe-off) pa=--xeprobe-off-plan aa=--xeprobe-off-apply;;
        *) bad "$name: no plan $plan"; continue;; esac
    PD=$(v $V.facts.probe_device) || PD=""   # the GPU's PCI device id, for the XeProbe boot-arg
    efi() { "$S" --package "$P" --efi "$F" ${PD:+--probe-device "$PD"} "$@"; }
    count=$((count + 1))

    # the --status values before
    if v $V.expect.status > /dev/null; then
        efi --status > "$W/status" 2>&1; why=$(status_diff $V.expect.status "$W/status")
        if [ -z "$why" ]; then ok "$name: --status before"; else bad "$name: --status before:$why"; fi
    fi

    # the plan: its steps and notes
    if ! efi $pa > "$W/plan" 2>&1; then bad "$name: $pa failed:"; sed 's/^/        /' "$W/plan"; continue; fi
    ops_json "$W/ops" "$P" > "$W/got.json"
    plutil -extract $V.expect.ops xml1 -o "$W/want.xml" "$VP"
    if plutil -convert xml1 -o "$W/got.xml" "$W/got.json" 2>/dev/null && cmp -s "$W/got.xml" "$W/want.xml"; then ok "$name: the plan's $(n_of $V.expect.ops) steps"
    else bad "$name: the plan's steps differ"; echo "      efi.sh:"; plutil -p "$W/got.json" 2>&1 | sed 's/^/        /'; echo "      the vector:"; plutil -p "$W/want.xml" | sed 's/^/        /'; fi
    grep '^note     ' "$W/plan" | sed 's/^note     //' > "$W/notes"
    m=$(n_of $V.expect.notes); good=yes
    [ "$(wc -l < "$W/notes" | tr -d ' ')" = "$m" ] || good=no
    for ((j = 0; j < m; j++)); do grep -qF -- "$(v $V.expect.notes.$j)" <<< "$(sed -n "$((j + 1))p" "$W/notes")" || good=no; done
    if [ $good = yes ]; then ok "$name: the $m note(s)"; else bad "$name: the notes:"; sed 's/^/        /' "$W/notes"; fi

    # apply it, and what it must leave
    [ "$(n_of $V.expect.ops)" -gt 0 ] || continue
    if ! efi $aa --expect "$(shasum -a 256 < "$W/plan" | cut -d' ' -f1)" > "$W/out" 2>&1; then bad "$name: $aa failed:"; sed 's/^/        /' "$W/out"; continue; fi
    good=yes why=""
    for ((j = 0; j < $(n_of $V.expect.after.config); j++)); do
        A=$V.expect.after.config.$j p=$(v $A.path)
        if [ "$(v $A.absent)" = true ]; then plutil -extract "$p" raw -o /dev/null "$C" 2>/dev/null && { good=no; why="$why $p is there;"; }
        else got=$(plutil -extract "$p" raw -o - "$C" 2>/dev/null); [ "$got" = "$(v $A.value)" ] || { good=no; why="$why $p=$got;"; }; fi
    done
    for ((j = 0; j < $(n_of $V.expect.after.files); j++)); do
        A=$V.expect.after.files.$j f=$(v $A.file); e=$F/EFI/OC/$f
        if st=$(v $A.state); then
            case $st in package) diff -rq "$P/EFI/OC/$f" "$e" > /dev/null 2>&1 || { good=no; why="$why $f is not the package's;"; };;
                absent) [ ! -e "$e" ] || { good=no; why="$why $f is there;"; };; esac
        elif h=$(v $A.sha256); then [ "$(shasum -a 256 < "$e" 2>/dev/null | cut -d' ' -f1)" = "$h" ] || { good=no; why="$why $f's SHA-256;"; }
        elif t=$(v $A.table); then hexfile "$(v tables.$t)" "$W/table"; cmp -s "$W/table" "$e" || { good=no; why="$why $f is not $t;"; }; fi
    done
    if [ "$(n_of $V.expect.after.changed)" -gt 0 ]; then
        b=$(ls -d "$W"/state/efi-backups/*/[0-9][0-9][0-9]-* 2>/dev/null | LC_ALL=C sort | tail -1)
        want=$(for ((j = 0; j < $(n_of $V.expect.after.changed); j++)); do v $V.expect.after.changed.$j; done)
        [ "$(cat "$b/changed" 2>/dev/null)" = "$want" ] || { good=no; why="$why the backup lists $(tr '\n' ' ' < "$b/changed" 2>/dev/null);"; }
    fi
    efi --status > "$W/status" 2>&1; s=$(status_diff $V.expect.after.status "$W/status")
    [ -z "$s" ] || { good=no; why="$why --status$s"; }
    if [ $good = yes ]; then ok "$name: applied, the values, files, list of changes and --status"; else bad "$name: after --apply:$why"; fi

    # undo: back to the state before
    [ "$(v $V.expect.undo)" = true ] || continue
    efi --undo-plan > "$W/uplan" 2>&1
    if efi --undo --expect "$(shasum -a 256 < "$W/uplan" | cut -d' ' -f1)" > "$W/out" 2>&1 && [ "$(canon "$C")" = "$(canon "$W/before/EFI/OC/config.plist")" ] \
        && diff -rq -x config.plist "$W/before/EFI" "$F/EFI" > /dev/null; then ok "$name: undo, config.plist and the files as before"
    else bad "$name: undo:"; sed 's/^/        /' "$W/out"; diff -rq -x config.plist "$W/before/EFI" "$F/EFI" | sed 's/^/        /'; fi
done
unset INTELXE_EFI_OPS

# which CPUs need HybridPM.kext, and which PCI devices are cards (each with efi.sh's own check)
F=$ROOT/v0/efi P=$ROOT/v0/pkg
export INTELXE_STATE_DIR=$ROOT/state INTELXE_BOOT_PATH="" INTELXE_IOREG_FILE=$ROOT/ioreg-none
: > "$ROOT/ioreg-none"
good=0 m=$(n_of cpus)
for ((j = 0; j < m; j++)); do
    want=no; [ "$(v cpus.$j.hybridpm)" = true ] && want=yes
    got=$(INTELXE_CPU_BRAND="$(v cpus.$j.cpu)" "$S" --package "$P" --efi "$F" --status | sed -n 's/^hybridpm_needed=//p')
    if [ "$got" = "$want" ]; then good=$((good + 1)); else bad "CPU '$(v cpus.$j.cpu)': hybridpm_needed=$got, the vectors say $want"; fi
done
[ $good = "$m" ] && ok "the $m CPU names: efi.sh adds HybridPM.kext for the ones the vectors say"
good=0 m=$(n_of cards)
for ((j = 0; j < m; j++)); do
    pci=$(v cards.$j.pci); ioreg_for "${pci%%:*}" "${pci#*:}" '\_SB.PC00.PEG1.PEGP' > "$ROOT/ioreg"
    got=$(INTELXE_IOREG_FILE=$ROOT/ioreg INTELXE_CPU_BRAND="" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
    want=false; [ -n "$got" ] && want=true
    if [ "$want" = "$(v cards.$j.card)" ]; then good=$((good + 1)); else bad "PCI $pci: card_acpi_path='$got', the vectors say card=$(v cards.$j.card)"; fi
done
[ $good = "$m" ] && ok "the $m PCI devices: efi.sh finds the ones the vectors call cards"

echo "== $count vectors, $fails failure(s)"
exit $fails
