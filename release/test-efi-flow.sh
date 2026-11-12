#!/bin/bash
# SPDX-License-Identifier: MIT
# test-efi-flow.sh [PACKAGE_DIR]: runs templates/efi.sh against a fake EFI folder made from efi/sample-z790/config.plist,
# set back to how stage 1 leaves it, in a throwaway folder: no root, no EFI partition, no NVRAM. Without PACKAGE_DIR the
# package's IntelXe.kext and SSDT-ARCOFF.aml are stand-ins (and the 0.1.3 table, for adapting it). Checks status, plan,
# apply, settings, undo, the plan hash, a different kext, the ACPI path check, SSDT-ARCOFF adapted to other ports (with
# iasl, when there is one, to disassemble and compare), an earlier B580.kext entry, the boot-path and the ioreg parsing;
# HybridPM.kext by CPU (hybrid with and without Lilu, an existing entry, an earlier build, not hybrid), and its undo;
# last, tools/parity/devices.py (efi.sh's CARD_IDS and the other copies of the supported-device list).
set -uo pipefail
R=$(cd "$(dirname "$0")" && pwd)
S=$R/templates/efi.sh
ROOT=$(mktemp -d -t ixeefi)
trap 'rm -rf -- "$ROOT"' EXIT
fails=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fails=$((fails + 1)); }

# the package: the real one, or stand-ins
P=$ROOT/pkg
if [ $# -ge 1 ]; then
    mkdir -p "$P"; ditto "$1/EFI" "$P/EFI"; [ -f "$1/SHA256SUMS" ] && cp "$1/SHA256SUMS" "$P/"
else
    mkdir -p "$P/EFI/OC/Kexts/IntelXe.kext/Contents/MacOS" "$P/EFI/OC/ACPI"
    printf 'stand-in IntelXe executable\n' > "$P/EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe"
    plutil -create xml1 "$P/EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist"
    plutil -insert CFBundleVersion -string 9.9.9 "$P/EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist"
    printf 'SSDT\x2f\x04_SB_PC00PEG1PEGP stand-in' > "$P/EFI/OC/ACPI/SSDT-ARCOFF.aml"
    mkdir -p "$P/EFI/OC/Kexts/HybridPM.kext/Contents/MacOS"
    printf 'stand-in HybridPM executable\n' > "$P/EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM"
    plutil -create xml1 "$P/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    plutil -insert CFBundleIdentifier -string io.github.yusufklncc.HybridPM "$P/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    plutil -insert CFBundleVersion -string 9.9.8 "$P/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    (cd "$P" && find EFI -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
fi

# the EFI as stage 1 leaves it: IntelXe.kext and SSDT-ARCOFF disabled (as in the sample), no SIP/AMFI settings, the quirks
# not set, boot-args not in NVRAM > Delete
F=$ROOT/efi
mkdir -p "$F/EFI/OC/Kexts" "$F/EFI/OC/ACPI" "$F/EFI/BOOT"
: > "$F/EFI/OC/OpenCore.efi"
C=$F/EFI/OC/config.plist
cp "$R/../efi/sample-z790/config.plist" "$C"
A=NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82
plutil -replace $A.csr-active-config -data AAAAAA== "$C"
plutil -replace $A.boot-args -string "keepsyms=1 debug=0x100 darkwake=0 -btlfxboardid revcpu=1 ctrsmt=off" "$C"
plutil -replace Booter.Quirks.ResizeAppleGpuBars -integer -1 "$C"
plutil -replace Kernel.Quirks.DisableIoMapper -bool false "$C"
plutil -replace UEFI.Output.ReconnectGraphicsOnConnect -bool true "$C"
plutil -remove NVRAM.Delete.7C436110-AB2A-4BBB-A880-FE41995C9F82.0 "$C"   # boot-args
cp "$C" "$ROOT/config.stage1"

export INTELXE_STATE_DIR=$ROOT/state INTELXE_BOOT_PATH="" INTELXE_CARD_ACPI_PATH='\_SB.PC00.PEG1.PEGP'
export INTELXE_CPU_BRAND='Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz'   # no efficiency cores: no HybridPM.kext (tested below)
efi() { "$S" --package "$P" --efi "$F" "$@"; }
state() { efi --status | sed -n "s/^$1=//p"; }
expect() { # expect WHAT KEY VALUE
    local got; got=$(state "$2")
    if [ "$got" = "$3" ]; then ok "$1: $2=$got"; else bad "$1: $2=$got, expected $3"; fi
}
has() { # has WHAT FILE TEXT
    if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1: no '$3' in:"; sed 's/^/        /' "$2"; fi
}
hasnt() { if grep -qF -- "$3" "$2"; then bad "$1: '$3' in:"; sed 's/^/        /' "$2"; else ok "$1"; fi; }
hash() { shasum -a 256 < "$1" | cut -d' ' -f1; }
pp() { plutil -p "$1"; }   # the parsed contents, for comparing two config.plists

expect "stage 1" opencore yes
expect "stage 1" efi_source folder
expect "stage 1" kext_file absent
expect "stage 1" kext_entry disabled
expect "stage 1" ssdt_file absent
expect "stage 1" ssdt_entry disabled
expect "stage 1" ssdt_package_names_card yes
expect "stage 1" plan_changes 5
expect "stage 1" settings_changes 6
expect "stage 1" backup none
expect "stage 1, a CPU without efficiency cores" hybridpm_needed no

efi --plan > "$ROOT/plan" 2>&1 || bad "--plan exited with an error"
has "plan copies the kext" "$ROOT/plan" "copy     EFI/OC/Kexts/IntelXe.kext"
has "plan enables the kext entry" "$ROOT/plan" "Kernel > Add, item 15 (IntelXe.kext): Enabled false -> true"
has "plan copies the SSDT" "$ROOT/plan" "copy     EFI/OC/ACPI/SSDT-ARCOFF.aml"
has "plan enables the SSDT entry" "$ROOT/plan" "ACPI > Add, item 5 (SSDT-ARCOFF.aml): Enabled false -> true"
has "plan records the boot path from now on" "$ROOT/plan" "ExposeSensitiveData: 6 -> 7"
has "plan names the EFI" "$ROOT/plan" "EFI: the folder $F"

efi --apply --expect 0000 > "$ROOT/out" 2>&1 && bad "--apply with another plan's hash went ahead" || ok "--apply with another plan's hash refuses"
cmp -s "$C" "$ROOT/config.stage1" && [ ! -e "$F/EFI/OC/Kexts/IntelXe.kext" ] && ok "... and changes nothing" || bad "it changed the EFI"

efi --apply --expect "$(hash "$ROOT/plan")" > "$ROOT/out" 2>&1 || { bad "--apply exited with an error"; cat "$ROOT/out"; }
expect "after --apply" kext_file same
expect "after --apply" kext_entry enabled
expect "after --apply" ssdt_file same
expect "after --apply" ssdt_entry enabled
expect "after --apply" plan_changes 0
expect "after --apply" backup_kind "IntelXe files"
cp "$ROOT/config.stage1" "$ROOT/want"
plutil -replace Kernel.Add.15.Enabled -bool true "$ROOT/want"
plutil -replace ACPI.Add.5.Enabled -bool true "$ROOT/want"
plutil -replace Misc.Security.ExposeSensitiveData -integer 7 "$ROOT/want"
[ "$(pp "$C")" = "$(pp "$ROOT/want")" ] && ok "config.plist: those three values changed, the rest is the same" || { bad "config.plist differs from the expected edit:"; diff <(pp "$ROOT/want") <(pp "$C") | head -20; }
head -c 5 "$C" | grep -q '<?xml' && plutil -lint -s "$C" && ok "config.plist stays an XML property list" || bad "config.plist is not XML"
[ -z "$(find "$F" -name '._*')" ] && ok "no AppleDouble files on the EFI" || bad "AppleDouble files on the EFI"
b=$(state backup); [ -f "$b/EFI/OC/config.plist" ] && cmp -s "$b/EFI/OC/config.plist" "$ROOT/config.stage1" && ok "the backup holds the EFI as it was" || bad "backup $b is not the EFI as it was"
cp "$C" "$ROOT/config.files"
efi --plan > "$ROOT/plan" 2>&1; has "plan after --apply" "$ROOT/plan" "nothing to change"

efi --settings-plan > "$ROOT/splan" 2>&1 || bad "--settings-plan exited with an error"
has "settings: ResizeAppleGpuBars" "$ROOT/splan" "ResizeAppleGpuBars: -1 -> 10"
has "settings: DisableIoMapper" "$ROOT/splan" "DisableIoMapper: false -> true"
has "settings: ReconnectGraphicsOnConnect" "$ROOT/splan" "ReconnectGraphicsOnConnect: true -> false"
has "settings: SIP" "$ROOT/splan" "csr-active-config: 0x0 -> 0x803"
has "settings: boot-args" "$ROOT/splan" "add amfi_get_out_of_my_way=0x1 intelxe_surfvram=2"
has "settings: NVRAM > Delete" "$ROOT/splan" "add boot-args (so that"
efi --settings-apply --expect "$(hash "$ROOT/splan")" > "$ROOT/out" 2>&1 || { bad "--settings-apply exited with an error"; cat "$ROOT/out"; }
expect "after --settings-apply" settings_changes 0
[ "$(plutil -extract $A.csr-active-config raw -o - "$C")" = AwgAAA== ] && ok "csr-active-config is 03080000" || bad "csr-active-config"
[ "$(plutil -extract $A.boot-args raw -o - "$C")" = "keepsyms=1 debug=0x100 darkwake=0 -btlfxboardid revcpu=1 ctrsmt=off amfi_get_out_of_my_way=0x1 intelxe_surfvram=2" ] \
    && ok "boot-args: added to, the rest kept" || bad "boot-args: $(plutil -extract $A.boot-args raw -o - "$C")"
plutil -replace $A.csr-active-config -data 7w8AAA== "$C"
expect "a wider SIP setting (0xfef) is kept" settings_changes 0

efi --undo-plan > "$ROOT/uplan" 2>&1
has "undo plan puts config.plist back" "$ROOT/uplan" "restore  EFI/OC/config.plist"
has "undo plan warns about the later edit" "$ROOT/uplan" "config.plist was changed after that change"
efi --undo --expect "$(hash "$ROOT/uplan")" > "$ROOT/out" 2>&1 || { bad "--undo exited with an error"; cat "$ROOT/out"; }
cmp -s "$C" "$ROOT/config.files" && ok "--undo of the settings: config.plist as after --apply" || bad "--undo of the settings"
efi --undo-plan > "$ROOT/uplan" 2>&1
has "second undo removes the kext" "$ROOT/uplan" "remove   EFI/OC/Kexts/IntelXe.kext (it was not there before)"
efi --undo > "$ROOT/out" 2>&1 || { bad "second --undo exited with an error"; cat "$ROOT/out"; }
cmp -s "$C" "$ROOT/config.stage1" && ok "--undo of the files: config.plist byte for byte as in stage 1" || bad "config.plist not back"
expect "after both --undo" kext_file absent
expect "after both --undo" ssdt_file absent
efi --undo > "$ROOT/out" 2>&1 && bad "a third --undo went ahead" || ok "nothing left to undo"
has "... and says so" "$ROOT/out" "no backup to put back"

# a config.plist without NVRAM > Delete: stage 2 adds it (plutil -insert makes no missing parent: before, --settings-apply
# stopped there after the backup)
plutil -remove NVRAM.Delete "$C"; cp "$C" "$ROOT/config.nodel"
efi --settings-plan > "$ROOT/splan" 2>&1
has "no NVRAM > Delete: the plan adds it" "$ROOT/splan" "config   NVRAM > Delete: add (missing)"
efi --settings-apply --expect "$(hash "$ROOT/splan")" > "$ROOT/out" 2>&1 && ok "--settings-apply without NVRAM > Delete" || { bad "--settings-apply without NVRAM > Delete:"; cat "$ROOT/out"; }
[ "$(plutil -extract NVRAM.Delete.7C436110-AB2A-4BBB-A880-FE41995C9F82 xml1 -o - "$C" | grep -c '<string>')" = 2 ] && ok "... with csr-active-config and boot-args in it" || bad "NVRAM > Delete after --settings-apply"
expect "... and nothing left to change" settings_changes 0
efi --undo > /dev/null 2>&1; cmp -s "$C" "$ROOT/config.nodel" && ok "--undo: config.plist as before" || bad "--undo after adding NVRAM > Delete"
cp "$ROOT/config.stage1" "$C"

# a change that stopped before its list of changes was written (efi.sh wrote "changed" last until 0.1.4; now each step
# is listed before it is made): --undo-plan and --undo pass over it to the change before
efi --apply > /dev/null 2>&1; b=$(state backup)
s=$(dirname "$b")/999-20991231-235959; mkdir "$s"; ditto "$b/EFI" "$s/EFI"; echo settings > "$s/kind"; echo '2099-12-31 23:59:59' > "$s/date"
efi --undo-plan > "$ROOT/uplan" 2> "$ROOT/err"; r=$?
[ $r = 0 ] && [ ! -s "$ROOT/err" ] && grep -qF "from     $s (settings" "$ROOT/uplan" && ok "a backup without its list: the undo plan names it, no error" || { bad "undo plan for a backup without its list ($r):"; cat "$ROOT/err" "$ROOT/uplan"; }
efi --undo --expect "$(hash "$ROOT/uplan")" > "$ROOT/out" 2>&1 && [ -f "$s/restored" ] && [ "$(state backup)" = "$b" ] && ok "... --undo marks it put back; the next undo is the change before" || { bad "--undo of a backup without its list"; cat "$ROOT/out"; }
[ "$(cat "$b/changed")" = "$(printf 'Kexts/IntelXe.kext\nACPI/SSDT-ARCOFF.aml\nconfig.plist')" ] && ok "the list names each step in order: $(tr '\n' ' ' < "$b/changed")" || bad "the list of changes: $(cat "$b/changed")"
efi --undo > /dev/null 2>&1; cmp -s "$C" "$ROOT/config.stage1" && [ ! -e "$F/EFI/OC/Kexts/IntelXe.kext" ] && ok "... and that undo gives stage 1 back" || bad "undo after passing over a backup without its list"

# a package whose EFI files differ from its SHA256SUMS is not copied
if [ -f "$P/SHA256SUMS" ]; then
    cp -p "$P/EFI/OC/ACPI/SSDT-ARCOFF.aml" "$ROOT/aml"; printf 'x' >> "$P/EFI/OC/ACPI/SSDT-ARCOFF.aml"
    efi --apply > "$ROOT/out" 2>&1 && bad "--apply copied from a damaged package" || has "a damaged package is refused" "$ROOT/out" "differ from its SHA256SUMS"
    cmp -s "$C" "$ROOT/config.stage1" && ok "... and nothing changed" || bad "the EFI changed"
    cp -p "$ROOT/aml" "$P/EFI/OC/ACPI/SSDT-ARCOFF.aml"
fi

# a different IntelXe.kext in the EFI is replaced; the same one is left alone
efi --apply > /dev/null 2>&1
printf 'older build\n' >> "$F/EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe"
expect "an older kext in the EFI" kext_file different
efi --plan > "$ROOT/plan" 2>&1; has "plan replaces it" "$ROOT/plan" "replace  EFI/OC/Kexts/IntelXe.kext"
efi --apply > /dev/null 2>&1; expect "after --apply" kext_file same
efi --undo > /dev/null 2>&1; efi --undo > /dev/null 2>&1

# the ACPI path: SSDT-ARCOFF only when a table names the card's port (or the package's can be adapted to it, below)
PX=$ROOT/pkg-stand-in; ditto "$P/EFI" "$PX/EFI"; printf 'SSDT\x2f\x04_SB_PC00PEG1PEGP stand-in' > "$PX/EFI/OC/ACPI/SSDT-ARCOFF.aml"
INTELXE_CARD_ACPI_PATH='\_SB.PC00.PEG0.PEGP' "$S" --package "$PX" --efi "$F" --plan > "$ROOT/plan" 2>&1
hasnt "another port, a table it cannot adapt: the SSDT is not copied" "$ROOT/plan" "EFI/OC/ACPI/SSDT-ARCOFF.aml"
has "another port: says why" "$ROOT/plan" "the card's port here is \\_SB.PC00.PEG0.PEGP, and the package's SSDT-ARCOFF.aml is not laid out"
INTELXE_CARD_ACPI_PATH='\_SB.PC00.PEG1' "$S" --package "$PX" --efi "$F" --plan > "$ROOT/plan" 2>&1
hasnt "a shorter path inside the package's does not count as named" "$ROOT/plan" "EFI/OC/ACPI/SSDT-ARCOFF.aml"
INTELXE_CARD_ACPI_PATH=none efi --plan > "$ROOT/plan" 2>&1; has "no ACPI name: says so" "$ROOT/plan" "has no ACPI name"
INTELXE_CARD_ACPI_PATH= efi --plan > "$ROOT/plan" 2>&1; has "no card: says so" "$ROOT/plan" "No Arc B580 found"
printf 'SSDT\x2f\x04_SB_PC00PEG0PEGP my own build' > "$F/EFI/OC/ACPI/SSDT-ARCOFF.aml"
INTELXE_CARD_ACPI_PATH='\_SB.PC00.PEG0.PEGP' efi --plan > "$ROOT/plan" 2>&1
has "your own table for this port is kept" "$ROOT/plan" "kept: EFI/OC/ACPI/SSDT-ARCOFF.aml differs"
has "... and its entry enabled" "$ROOT/plan" "ACPI > Add, item 5 (SSDT-ARCOFF.aml): Enabled false -> true"
rm "$F/EFI/OC/ACPI/SSDT-ARCOFF.aml"

# an earlier B580.kext entry is turned off   # intelxe-rename: keep
plutil -insert Kernel.Add -json '{"BundlePath":"B580.kext","Enabled":true,"ExecutablePath":"Contents/MacOS/B580","PlistPath":"Contents/Info.plist"}' -append "$C"   # intelxe-rename: keep
efi --plan > "$ROOT/plan" 2>&1; has "the earlier name is turned off" "$ROOT/plan" "item 16 (B580.kext, the earlier name of IntelXe.kext): Enabled true -> false"   # intelxe-rename: keep
cp "$ROOT/config.stage1" "$C"

# a recorded boot path: no ExposeSensitiveData change
INTELXE_BOOT_PATH='PciRoot(0x0)/Pci(0x1B,0x4)/Pci(0x0,0x0)/NVMe(0x1,00-00-00-00-00-00-00-00)/HD(1,GPT,0b8f5a2c-0000-4000-8000-000000000001,0x28,0x64000)/\EFI\OC\OpenCore.efi%00' efi --plan > "$ROOT/plan" 2>&1
hasnt "with a recorded boot path ExposeSensitiveData stays" "$ROOT/plan" "ExposeSensitiveData"

# OpenCore's boot-path, without --efi: the partition UUID is read from it (this one does not exist)
INTELXE_BOOT_PATH='PciRoot(0x0)/Pci(0x1B,0x4)/Pci(0x0,0x0)/NVMe(0x1,00-00-00-00-00-00-00-00)/HD(1,GPT,0b8f5a2c-0000-4000-8000-000000000001,0x28,0x64000)/\EFI\OC\OpenCore.efi%00' \
    "$S" --package "$P" --status > "$ROOT/out" 2>&1
has "boot-path: source" "$ROOT/out" "efi_source=boot-path"
has "boot-path: partition UUID" "$ROOT/out" "efi_partition=0B8F5A2C-0000-4000-8000-000000000001"
has "a partition that is not there: not checked" "$ROOT/out" "kext_file=unknown"
INTELXE_BOOT_PATH='PciRoot(0x0)/HD(1,MBR,0x1234,0x800,0x1000)' "$S" --package "$P" --status > "$ROOT/out" 2>&1
has "no GPT partition in the boot path: none" "$ROOT/out" "efi_source=none"
"$S" --package "$P" --part disk0s1 --status > "$ROOT/out" 2>&1 && bad "--part took a disk number" || ok "--part takes no disk number"
if [ "$(id -u)" != 0 ]; then
    "$S" --package "$P" --part 0B8F5A2C-0000-4000-8000-000000000001 --apply > "$ROOT/out" 2>&1
    has "--apply on a partition needs root" "$ROOT/out" "needs root"
fi

# the card's ACPI path from ioreg's IOService plane: the bridge two PCI levels above the GPU
cat > "$ROOT/ioreg" <<'EOF'
+-o Root  <class IORegistryEntry, id 0x100000100, retain 30>
  +-o PC00@0  <class IOPCIDevice, id 0x100000200, registered, matched, active, busy 0 (1 ms), retain 20>
  | | {
  | |   "acpi-path" = "IOACPIPlane:/_SB/PC00@0"
  | | }
  | +-o PEG1@1  <class IOPCIDevice, id 0x100000300, registered, matched, active, busy 0 (1 ms), retain 12>
  | | | {
  | | |   "acpi-path" = "IOACPIPlane:/_SB/PC00@0/PEG1@10000"
  | | | }
  | | +-o IOPP  <class IOPCI2PCIBridge, id 0x100000310, registered, matched, active, busy 0 (1 ms), retain 8>
  | |   +-o PEGP@0  <class IOPCIDevice, id 0x100000400, registered, matched, active, busy 0 (1 ms), retain 12>
  | |   | | {
  | |   | |   "vendor-id" = <86800000>
  | |   | |   "device-id" = <e2e20000>
  | |   | |   "acpi-path" = "IOACPIPlane:/_SB/PC00@0/PEG1@10000/PEGP@0"
  | |   | | }
  | |   | +-o IOPP  <class IOPCI2PCIBridge, id 0x100000410, registered, matched, active, busy 0 (1 ms), retain 8>
  | |   |   +-o pci-bridge@1  <class IOPCIDevice, id 0x100000500, registered, matched, active, busy 0 (1 ms), retain 12>
  | |   |   | | {
  | |   |   | |   "vendor-id" = <86800000>
  | |   |   | |   "device-id" = <e3e20000>
  | |   |   | | }
  | |   |   | +-o IOPP  <class IOPCI2PCIBridge, id 0x100000510, registered, matched, active, busy 0 (1 ms), retain 8>
  | |   |   |   +-o display@0  <class IOPCIDevice, id 0x100000600, registered, matched, active, busy 0 (1 ms), retain 12>
  | |   |   |   | {
  | |   |   |   |   "device-id" = <0be20000>
  | |   |   |   |   "vendor-id" = <86800000>
  | |   |   |   | }
  | |   |   |   +-o pci8086,e2f7@0,1  <class IOPCIDevice, id 0x100000700, registered, matched, active, busy 0 (1 ms), retain 9>
  | +-o RP01@1C  <class IOPCIDevice, id 0x100000800, registered, matched, active, busy 0 (1 ms), retain 12>
EOF
got=$(env -u INTELXE_CARD_ACPI_PATH INTELXE_IOREG_FILE="$ROOT/ioreg" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
[ "$got" = '\_SB.PC00.PEG1.PEGP' ] && ok "ioreg: the card's port is $got" || bad "ioreg: card_acpi_path=$got"
sed '/PEG1@10000\/PEGP@0/d' "$ROOT/ioreg" > "$ROOT/ioreg2"
got=$(env -u INTELXE_CARD_ACPI_PATH INTELXE_IOREG_FILE="$ROOT/ioreg2" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
[ "$got" = none ] && ok "ioreg: a port without an ACPI name is 'none'" || bad "ioreg without the port's name: $got"
sed 's/<0be20000>/<0ce20000>/' "$ROOT/ioreg" > "$ROOT/ioreg4"   # an Arc B570 (8086:e20c) in the same slot
got=$(env -u INTELXE_CARD_ACPI_PATH INTELXE_IOREG_FILE="$ROOT/ioreg4" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
[ "$got" = '\_SB.PC00.PEG1.PEGP' ] && ok "ioreg: a B570's port is $got" || bad "ioreg with a B570: card_acpi_path=$got"
grep -v 0be20000 "$ROOT/ioreg" > "$ROOT/ioreg3"
got=$(env -u INTELXE_CARD_ACPI_PATH INTELXE_IOREG_FILE="$ROOT/ioreg3" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
[ -z "$got" ] && ok "ioreg: no card, no path" || bad "ioreg without a card: $got"
sed 's/<0be20000>/<0de20000>/' "$ROOT/ioreg" > "$ROOT/ioreg5"   # 8086:e20d, a BMG-G21 id outside CARD_IDS
got=$(env -u INTELXE_CARD_ACPI_PATH INTELXE_IOREG_FILE="$ROOT/ioreg5" "$S" --package "$P" --efi "$F" --status | sed -n 's/^card_acpi_path=//p')
[ -z "$got" ] && ok "ioreg: an id efi.sh does not support, no path" || bad "ioreg with 8086:e20d: $got"

# SSDT-ARCOFF adapted to another port. The table: the package's, or (with stand-ins) the 0.1.3 SSDT-ARCOFF.aml, which
# iasl 20260408 builds from efi/SSDT-ARCOFF.dsl
PA=$ROOT/pkg-aml; ditto "$P/EFI" "$PA/EFI"
if [ $# = 0 ]; then
    h=53534454da000000024c5745580000004152434f4646000000100000494e544c08042620a01800155c2f045f53425f5043303050454731
    h=${h}504547500600a04c09905b125c5f4f5349605f4f53490d44617277696e001044085c2f045f53425f5043303050454731504547505b824d
    h=${h}0642524731085f4144520c000001005b824b0547465830085f41445200144e045f44534d04a009936a00a411030103a4123b060d6e616d
    h=${h}6500110c0a0923646973706c6179000d494f4e616d65000d23646973706c6179000d636c6173732d636f64650011070a04ffffffff
    printf "$(printf '%s' "$h" | sed 's/../\\x&/g')" > "$PA/EFI/OC/ACPI/SSDT-ARCOFF.aml"
fi
PAML=$PA/EFI/OC/ACPI/SSDT-ARCOFF.aml
eval "$(sed -n '/^hexof()/p; /^namepath_hex()/,/^}/p; /^aml_names_card()/,/^}/p' "$S")"   # efi.sh's own check
names() { CARD=$1 aml_names_card "$2"; }
sum_ok() { [ $(( $(od -An -v -tu1 "$1" | tr -s ' \n' '+' | sed 's/^+//; s/+$//') & 255 )) = 0 ]; }
len_ok() { [ "$(od -An -v -tu4 -j4 -N4 "$1" | tr -d ' ')" = "$(wc -c < "$1" | tr -d ' ')" ]; }
IASL=$(command -v iasl || ls /opt/homebrew/bin/iasl /usr/local/bin/iasl 2>/dev/null | head -1 || true)
F2=$ROOT/efi2
fresh() { rm -rf "$F2"; mkdir -p "$F2/EFI/OC/ACPI" "$F2/EFI/OC/Kexts"; : > "$F2/EFI/OC/OpenCore.efi"; cp "$ROOT/config.stage1" "$F2/EFI/OC/config.plist"; }
efi2() { "$S" --package "$PA" --efi "$F2" "$@"; }
state2() { efi2 --status | sed -n "s/^$1=//p"; }
PKGPATH_T='\_SB.PC00.PEG1.PEGP'
names "$PKGPATH_T" "$PAML" && sum_ok "$PAML" && len_ok "$PAML" && ok "the package's table names $PKGPATH_T, checksum and length valid" || bad "the package's table"

# the same depth, the card found through ioreg: \_SB.PC00.RP05.PXSX
sed -e 's|PEG1@10000/PEGP@0|RP05@1c0004/PXSX@0|' -e 's|PEG1@10000"|RP05@1c0004"|' -e 's|PEG1@1 |RP05@1C,4 |' -e 's|PEGP@0 |PXSX@0 |' "$ROOT/ioreg" > "$ROOT/ioreg5"
export INTELXE_IOREG_FILE=$ROOT/ioreg5; unset INTELXE_CARD_ACPI_PATH
fresh
[ "$(state2 card_acpi_path)" = '\_SB.PC00.RP05.PXSX' ] && ok "ioreg: the card at RP05/PXSX" || bad "ioreg5: card_acpi_path=$(state2 card_acpi_path)"
[ "$(state2 ssdt_package_names_card)" = adaptable ] && ok "status: the table is adaptable" || bad "status: ssdt_package_names_card=$(state2 ssdt_package_names_card)"
efi2 --plan > "$ROOT/plan2" 2>&1 || bad "--plan (RP05) exited with an error"
has "plan: the table adapted to RP05" "$ROOT/plan2" "copy     EFI/OC/ACPI/SSDT-ARCOFF.aml (the package's table adapted to this card's port: names \\_SB.PC00.RP05.PXSX instead of \\_SB.PC00.PEG1.PEGP)"
has "plan: its ACPI > Add entry" "$ROOT/plan2" "ACPI > Add, item 5 (SSDT-ARCOFF.aml): Enabled false -> true"
hasnt "plan: no note about editing the table" "$ROOT/plan2" "not added"
efi2 --plan > "$ROOT/plan2b" 2>&1; cmp -s "$ROOT/plan2" "$ROOT/plan2b" && ok "the plan is the same every time" || bad "the plan changes between runs"
efi2 --apply --expect "$(hash "$ROOT/plan2")" > "$ROOT/out" 2>&1 || { bad "--apply (RP05) exited with an error"; cat "$ROOT/out"; }
AD=$F2/EFI/OC/ACPI/SSDT-ARCOFF.aml
names '\_SB.PC00.RP05.PXSX' "$AD" && ok "the EFI's table names \\_SB.PC00.RP05.PXSX" || bad "the EFI's table does not name RP05"
names "$PKGPATH_T" "$AD" && bad "the EFI's table still names PEG1" || ok "... and no longer \\_SB.PC00.PEG1.PEGP"
sum_ok "$AD" && len_ok "$AD" && ok "its checksum and length are valid" || bad "its checksum or length"
a=$(hexof "$AD"); p=$(hexof "$PAML")
a=${a//" 52 50 30 35 50 58 53 58"/" 50 45 47 31 50 45 47 50"}   # RP05PXSX -> PEG1PEGP
[ "${#a}" = "${#p}" ] && [ "${a:0:27}${a:30}" = "${p:0:27}${p:30}" ] && [ "${a:27:3}" != "${p:27:3}" ] \
    && ok "it differs from the package's only in those NameSegs and the checksum" || { bad "it differs elsewhere:"; cmp -l "$AD" "$PAML" | head; }
[ "$(cmp -l "$AD" "$PAML" | wc -l | tr -d ' ')" -le 17 ] && ok "($(cmp -l "$AD" "$PAML" | wc -l | tr -d ' ') bytes differ)" || bad "too many bytes differ"
[ "$(state2 ssdt_file)" = same ] && [ "$(state2 ssdt_entry)" = enabled ] && [ "$(state2 plan_changes)" = 0 ] \
    && ok "after --apply: the adapted table counts as the same, nothing left to change" || bad "after --apply (RP05): $(efi2 --status | grep -E '^(ssdt|plan)')"
if [ -n "$IASL" ]; then
    rm -rf "$ROOT/dis"; mkdir "$ROOT/dis"; cp "$AD" "$ROOT/dis/a.aml"
    "$IASL" -d "$ROOT/dis/a.aml" > "$ROOT/dis/log" 2>&1 || bad "iasl -d failed: $(tail -3 "$ROOT/dis/log")"
    has "iasl: External names RP05" "$ROOT/dis/a.dsl" "External (_SB_.PC00.RP05.PXSX, DeviceObj)"
    has "iasl: Scope names RP05" "$ROOT/dis/a.dsl" "Scope (\\_SB.PC00.RP05.PXSX)"
    has "iasl: the devices below are kept" "$ROOT/dis/a.dsl" "Device (GFX0)"
else echo "skip  iasl not found: no disassembly"; fi
efi2 --undo > /dev/null 2>&1; [ ! -e "$AD" ] && ok "--undo removes the adapted table" || bad "--undo left the adapted table"
unset INTELXE_IOREG_FILE

# another depth: the PkgLengths and the length change; the result must be what iasl builds from the .dsl with that path
for cpath in '\_SB.PCI0.GPP8' '\_SB.PCI0.GPP8.UP00.DP40.UP00' '\PEGP'; do
    fresh
    INTELXE_CARD_ACPI_PATH=$cpath efi2 --plan > "$ROOT/plan3" 2>&1
    has "depth $cpath: plan adapts the table" "$ROOT/plan3" "adapted to this card's port: names $cpath instead of"
    INTELXE_CARD_ACPI_PATH=$cpath efi2 --apply --expect "$(hash "$ROOT/plan3")" > "$ROOT/out" 2>&1 || { bad "--apply ($cpath) exited with an error"; cat "$ROOT/out"; }
    names "$cpath" "$AD" && ! names "$PKGPATH_T" "$AD" && sum_ok "$AD" && len_ok "$AD" \
        && ok "depth $cpath: names it, checksum and length valid" || bad "depth $cpath: the table is wrong"
    if [ -n "$IASL" ]; then
        rm -rf "$ROOT/dis"; mkdir "$ROOT/dis"
        sed -e "s/_SB_\\.PC00\\.PEG1\\.PEGP/${cpath//\\/\\\\}/" -e "s/\\\\_SB\\.PC00\\.PEG1\\.PEGP/${cpath//\\/\\\\}/" "$R/../efi/SSDT-ARCOFF.dsl" > "$ROOT/dis/x.dsl"
        "$IASL" -p "$ROOT/dis/x" "$ROOT/dis/x.dsl" > "$ROOT/dis/log" 2>&1 || bad "iasl could not compile the .dsl for $cpath"
        # the same bytes but the checksum (9) and iasl's version (32-35)
        x=$(hexof "$ROOT/dis/x.aml"); a=$(hexof "$AD")
        [ "${x:0:27}${x:30:66}${x:108}" = "${a:0:27}${a:30:66}${a:108}" ] && ok "depth $cpath: the bytes iasl builds from the .dsl" || { bad "depth $cpath: differs from iasl's"; cmp -l "$ROOT/dis/x.aml" "$AD" | head; }
    fi
    INTELXE_CARD_ACPI_PATH=$cpath efi2 --undo > /dev/null 2>&1
done
export INTELXE_CARD_ACPI_PATH='\_SB.PC00.PEG1.PEGP'

# the Safari setting (system-volume copy): SecureBootModel to Disabled, apart from stage 2, and back
plutil -replace Misc.Security.SecureBootModel -string Default "$C"; cp "$C" "$ROOT/config.sbm"
expect "SecureBootModel Default" secure_boot_model Default
efi --safari-plan > "$ROOT/fplan" 2>&1 || bad "--safari-plan exited with an error"
has "safari plan: SecureBootModel" "$ROOT/fplan" "SecureBootModel: Default -> Disabled"
efi --settings-plan > "$ROOT/out" 2>&1; hasnt "stage 2 leaves SecureBootModel alone" "$ROOT/out" "SecureBootModel"
efi --safari-apply --expect "$(hash "$ROOT/fplan")" > "$ROOT/out" 2>&1 || { bad "--safari-apply exited with an error"; cat "$ROOT/out"; }
expect "after --safari-apply" secure_boot_model Disabled
efi --safari-plan > "$ROOT/out" 2>&1; hasnt "nothing left for Safari" "$ROOT/out" "SecureBootModel:"
efi --undo > "$ROOT/out" 2>&1 || { bad "--undo of the Safari setting exited with an error"; cat "$ROOT/out"; }
cmp -s "$C" "$ROOT/config.sbm" && ok "--undo of the Safari setting: config.plist back" || bad "--undo of the Safari setting"

# stage 2 and the Safari setting as one change (the setup assistant's first step with Safari chosen): one backup, one undo
cp "$ROOT/config.stage1" "$C"; plutil -replace Misc.Security.SecureBootModel -string Default "$C"; cp "$C" "$ROOT/config.both"
efi --settings-safari-plan > "$ROOT/bplan" 2>&1 || bad "--settings-safari-plan exited with an error"
has "settings+safari plan: SIP" "$ROOT/bplan" "csr-active-config: 0x0 -> 0x803"
has "settings+safari plan: SecureBootModel" "$ROOT/bplan" "SecureBootModel: Default -> Disabled"
efi --settings-safari-apply --expect "$(hash "$ROOT/bplan")" > "$ROOT/out" 2>&1 || { bad "--settings-safari-apply exited with an error"; cat "$ROOT/out"; }
expect "after --settings-safari-apply: settings" settings_changes 0
expect "after --settings-safari-apply: SecureBootModel" secure_boot_model Disabled
efi --undo > "$ROOT/out" 2>&1 || { bad "--undo of settings+Safari exited with an error"; cat "$ROOT/out"; }
cmp -s "$C" "$ROOT/config.both" && ok "--undo of settings+Safari: config.plist back in one step" || bad "--undo of settings+Safari"

# HybridPM.kext: by the CPU's name, below Lilu.kext, only when Lilu loads
F3=$ROOT/efi3 C3=$F3/EFI/OC/config.plist
fresh3() { rm -rf "$F3"; mkdir -p "$F3/EFI/OC/ACPI" "$F3/EFI/OC/Kexts/Lilu.kext/Contents/MacOS"; : > "$F3/EFI/OC/OpenCore.efi"; cp "$ROOT/config.stage1" "$C3"; }
efi3() { "$S" --package "$P" --efi "$F3" "$@"; }
state3() { efi3 --status | sed -n "s/^$1=//p"; }
expect3() { local got; got=$(state3 "$2"); if [ "$got" = "$3" ]; then ok "$1: $2=$got"; else bad "$1: $2=$got, expected $3"; fi; }
bp() { plutil -extract Kernel.Add.$1.BundlePath raw -o - "$2" 2>/dev/null; }
en() { plutil -extract Kernel.Add.$1.Enabled raw -o - "$2" 2>/dev/null; }
PH=$P/EFI/OC/Kexts/HybridPM.kext
for cpu in 'Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz' '12th Gen Intel(R) Core(TM) i5-12400F' 'Intel(R) Core(TM) Ultra 7 155H'; do
    fresh3
    INTELXE_CPU_BRAND=$cpu efi3 --plan > "$ROOT/plan" 2>&1
    hasnt "not hybrid ($cpu): no HybridPM in the plan" "$ROOT/plan" "HybridPM"
    [ "$(INTELXE_CPU_BRAND=$cpu state3 hybridpm_needed)" = no ] && ok "... and hybridpm_needed=no" || bad "$cpu: hybridpm_needed=$(INTELXE_CPU_BRAND=$cpu state3 hybridpm_needed)"
done
export INTELXE_CPU_BRAND='13th Gen Intel(R) Core(TM) i5-13400F'
if [ -f "$PH/Contents/MacOS/HybridPM" ]; then
    # the sample's disabled HybridPM entry (item 8), Lilu.kext as item 0
    fresh3
    expect3 "hybrid CPU" hybridpm_needed yes
    expect3 "hybrid CPU" hybridpm_package yes
    expect3 "hybrid CPU" hybridpm_file absent
    expect3 "hybrid CPU" hybridpm_entry disabled
    expect3 "hybrid CPU" lilu_entry enabled
    expect3 "hybrid CPU" plan_changes 7
    efi3 --plan > "$ROOT/hplan" 2>&1 || bad "--plan (hybrid) exited with an error"
    has "hybrid: plan copies HybridPM.kext" "$ROOT/hplan" "copy     EFI/OC/Kexts/HybridPM.kext (version $(plutil -extract CFBundleVersion raw -o - "$PH/Contents/Info.plist"); this CPU has efficiency cores"
    has "hybrid: plan enables its disabled entry" "$ROOT/hplan" "Kernel > Add, item 8 (HybridPM.kext): Enabled false -> true"
    hasnt "hybrid: no second entry" "$ROOT/hplan" "add HybridPM.kext"
    efi3 --plan > "$ROOT/hplan2" 2>&1; cmp -s "$ROOT/hplan" "$ROOT/hplan2" && ok "hybrid: the plan is the same every time" || bad "hybrid: the plan changes between runs"
    efi3 --apply --expect "$(hash "$ROOT/hplan")" > "$ROOT/out" 2>&1 || { bad "--apply (hybrid) exited with an error"; cat "$ROOT/out"; }
    expect3 "hybrid, after --apply" hybridpm_file same
    expect3 "hybrid, after --apply" hybridpm_entry enabled
    expect3 "hybrid, after --apply" hybridpm_version_efi "$(plutil -extract CFBundleVersion raw -o - "$PH/Contents/Info.plist")"
    expect3 "hybrid, after --apply" plan_changes 0
    cp "$ROOT/config.stage1" "$ROOT/want"
    for k in Kernel.Add.8.Enabled Kernel.Add.15.Enabled ACPI.Add.5.Enabled; do plutil -replace $k -bool true "$ROOT/want"; done
    plutil -replace Misc.Security.ExposeSensitiveData -integer 7 "$ROOT/want"
    [ "$(pp "$C3")" = "$(pp "$ROOT/want")" ] && ok "hybrid: config.plist has those four values changed, the rest the same" || { bad "hybrid: config.plist differs:"; diff <(pp "$ROOT/want") <(pp "$C3") | head -20; }
    efi3 --undo > "$ROOT/out" 2>&1 || { bad "--undo (hybrid) exited with an error"; cat "$ROOT/out"; }
    cmp -s "$C3" "$ROOT/config.stage1" && [ ! -e "$F3/EFI/OC/Kexts/HybridPM.kext" ] && ok "hybrid: --undo puts config.plist back and removes HybridPM.kext" || bad "hybrid: --undo"

    # no HybridPM entry: one is added right below Lilu.kext; the IntelXe entry below it still gets its change
    fresh3; plutil -remove Kernel.Add.8 "$C3"; cp "$C3" "$ROOT/config.nohpm"
    expect3 "no HybridPM entry" hybridpm_entry absent
    efi3 --plan > "$ROOT/hplan" 2>&1
    has "plan adds the entry below Lilu" "$ROOT/hplan" "Kernel > Add: add HybridPM.kext below Lilu.kext, as item 1"
    has "plan still enables IntelXe.kext's entry" "$ROOT/hplan" "Kernel > Add, item 14 (IntelXe.kext): Enabled false -> true"
    efi3 --apply --expect "$(hash "$ROOT/hplan")" > "$ROOT/out" 2>&1 || { bad "--apply (no entry) exited with an error"; cat "$ROOT/out"; }
    [ "$(bp 0 "$C3")" = Lilu.kext ] && [ "$(bp 1 "$C3")" = HybridPM.kext ] && [ "$(en 1 "$C3")" = true ] && [ "$(bp 2 "$C3")" = VirtualSMC.kext ] \
        && ok "HybridPM.kext is item 1, right below Lilu.kext, on" || bad "Kernel > Add: $(bp 0 "$C3"), $(bp 1 "$C3"), $(bp 2 "$C3")"
    [ "$(plutil -extract Kernel.Add.1.ExecutablePath raw -o - "$C3")" = Contents/MacOS/HybridPM ] && [ "$(plutil -extract Kernel.Add.1.PlistPath raw -o - "$C3")" = Contents/Info.plist ] \
        && ok "... with its ExecutablePath and PlistPath" || bad "the new entry's paths"
    [ "$(bp 15 "$C3")" = IntelXe.kext ] && [ "$(en 15 "$C3")" = true ] && [ "$(plutil -extract Kernel.Add raw -o - "$C3")" = 16 ] \
        && ok "IntelXe.kext's entry moved to item 15 and is on; 16 items" || bad "IntelXe.kext's entry: item 15 is $(bp 15 "$C3") ($(en 15 "$C3"))"
    expect3 "after --apply (no entry)" plan_changes 0
    efi3 --undo > /dev/null 2>&1; cmp -s "$C3" "$ROOT/config.nohpm" && ok "--undo takes the new entry back out" || bad "--undo (no entry)"

    # Lilu not loaded: a note, nothing of HybridPM in the plan
    fresh3; plutil -replace Kernel.Add.0.Enabled -bool false "$C3"
    efi3 --plan > "$ROOT/hplan" 2>&1
    has "Lilu off: a note says why" "$ROOT/hplan" "not added: HybridPM.kext. This CPU (13th Gen Intel(R) Core(TM) i5-13400F) has efficiency cores"
    has "... naming Lilu" "$ROOT/hplan" "this EFI does not load Lilu.kext"
    hasnt "Lilu off: HybridPM.kext not copied" "$ROOT/hplan" "EFI/OC/Kexts/HybridPM.kext"
    hasnt "Lilu off: its entry left alone" "$ROOT/hplan" "item 8 (HybridPM.kext)"
    expect3 "Lilu off" lilu_entry disabled
    fresh3; rm -rf "$F3/EFI/OC/Kexts/Lilu.kext"
    efi3 --plan > "$ROOT/hplan" 2>&1; has "no Lilu.kext file: the same note" "$ROOT/hplan" "this EFI does not load Lilu.kext"
    hasnt "... and no HybridPM change" "$ROOT/hplan" "EFI/OC/Kexts/HybridPM.kext"

    # the test machine's earlier build under the same name: replaced, and put back by --undo
    fresh3; mkdir -p "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/MacOS"
    printf 'org.hackintosh build\n' > "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM"
    plutil -create xml1 "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    plutil -insert CFBundleIdentifier -string org.hackintosh.HybridPM "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    plutil -insert CFBundleVersion -string 1.0.0 "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"
    plutil -replace Kernel.Add.8.Enabled -bool true "$C3"
    expect3 "an earlier HybridPM build" hybridpm_file different
    expect3 "an earlier HybridPM build" hybridpm_version_efi 1.0.0
    efi3 --plan > "$ROOT/hplan" 2>&1
    has "plan replaces the earlier build" "$ROOT/hplan" "replace  EFI/OC/Kexts/HybridPM.kext (an earlier build, org.hackintosh.HybridPM 1.0.0, by the package's"
    hasnt "its enabled entry stays as it is" "$ROOT/hplan" "item 8 (HybridPM.kext)"
    efi3 --apply --expect "$(hash "$ROOT/hplan")" > "$ROOT/out" 2>&1 || { bad "--apply (earlier build) exited with an error"; cat "$ROOT/out"; }
    expect3 "after replacing the earlier build" hybridpm_file same
    efi3 --undo > /dev/null 2>&1
    grep -q 'org.hackintosh build' "$F3/EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM" 2>/dev/null && ok "--undo puts the earlier build back" || bad "--undo did not put the earlier build back"

    # an entry above Lilu's: a note
    fresh3; plutil -insert Kernel.Add.0 -json '{"BundlePath":"HybridPM.kext","Enabled":true,"ExecutablePath":"Contents/MacOS/HybridPM","PlistPath":"Contents/Info.plist"}' "$C3"
    efi3 --plan > "$ROOT/hplan" 2>&1; has "an entry above Lilu's: a note" "$ROOT/hplan" "HybridPM.kext is item 0 of Kernel > Add, above Lilu.kext (item 1)"

    # a damaged HybridPM.kext in the package is refused like IntelXe.kext
    if [ -f "$P/SHA256SUMS" ] && grep -q 'Kexts/HybridPM.kext/' "$P/SHA256SUMS"; then
        fresh3; cp -p "$PH/Contents/MacOS/HybridPM" "$ROOT/hpmexe"; printf 'x' >> "$PH/Contents/MacOS/HybridPM"
        efi3 --apply > "$ROOT/out" 2>&1 && bad "--apply copied a damaged HybridPM.kext" || has "a damaged HybridPM.kext is refused" "$ROOT/out" "differ from its SHA256SUMS"
        cmp -s "$C3" "$ROOT/config.stage1" && ok "... and nothing changed" || bad "the EFI changed"
        cp -p "$ROOT/hpmexe" "$PH/Contents/MacOS/HybridPM"
    fi
fi
# a package without HybridPM.kext (0.1.3 and earlier): a note
PN=$ROOT/pkg-nohpm; ditto "$P/EFI" "$PN/EFI"; rm -rf "$PN/EFI/OC/Kexts/HybridPM.kext"
fresh3; "$S" --package "$PN" --efi "$F3" --plan > "$ROOT/hplan" 2>&1
has "a package without HybridPM.kext: a note" "$ROOT/hplan" "this package has no HybridPM.kext"
hasnt "... and no HybridPM change" "$ROOT/hplan" "EFI/OC/Kexts/HybridPM.kext"
[ "$("$S" --package "$PN" --efi "$F3" --status | sed -n 's/^hybridpm_package=//p')" = no ] && ok "... and hybridpm_package=no" || bad "hybridpm_package for a package without it"
export INTELXE_CPU_BRAND='Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz'

# efi.sh's CARD_IDS and every other copy of the supported-device list = data/platforms.json "supported"
if out=$(python3 "$R/../tools/parity/devices.py" 2>&1); then ok "supported-device lists: $(tail -1 <<< "$out")"
else bad "supported-device lists differ from data/platforms.json:"; grep -v '^ok' <<< "$out"; fi

echo "== $fails failure(s)"
exit $fails
