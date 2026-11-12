#!/bin/bash
# SPDX-License-Identifier: MIT
# make-expected.sh [--check]: on macOS, the reference results that windows/test/Test-IntelXe-EFI.ps1 compares the
# Windows tool with, and a check of the tool's engine against them here (PowerShell is not needed on the Mac).
#
#  1. stage1/config.plist: efi/sample-z790/config.plist set back to how stage 1 leaves it, as release/test-efi-flow.sh
#     does, but with text edits so that the sample's own layout stays (checked against plutil's result); the same
#     without the IntelXe/SSDT-ARCOFF entries and without the HybridPM entry; the 0.1.3 SSDT-ARCOFF.aml (for adapting).
#  2. expected/efi-sh/: what release/templates/efi.sh plans and writes for each step (plutil's output): the reference
#     for the keys and values; the tables it adapts to other ports; which CPU names it adds HybridPM.kext for.
#  3. The C# engine inside windows/IntelXe-EFI.ps1, built with dotnet (C# 5, as Windows PowerShell 5.1 compiles it;
#     offline: no package source), runs the same steps (EngineTest.cs) and must give the same plans and the same keys
#     and values. It writes expected/engine/: its own output, layout kept, which the Windows test compares byte for
#     byte (the same engine on .NET Framework and .NET). With --check those files must already be the same.
#     PACKAGE=DIR in the environment: also check an unpacked -efi.zip's SHA256SUMS and kext version with the engine.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin
W=$(cd "$(dirname "$0")/.." && pwd)        # windows/
R=$(cd "$W/.." && pwd)                      # the repository
S=$R/release/templates/efi.sh
X=$W/test/expected
CHECK=no; [ "${1:-}" = --check ] && CHECK=yes
T=$(mktemp -d -t ixewin)
trap 'rm -rf -- "$T"' EXIT
A=NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82

# ---- 1. stage 1, layout kept -------------------------------------------------------------------------------------
mkdir -p "$W/test/stage1" "$X/efi-sh" "$X/engine"
perl -0pe '
    s{(<key>ResizeAppleGpuBars</key>\s*<integer>)10(</integer>)}{${1}-1$2} or die "ResizeAppleGpuBars";
    s{(<key>DisableIoMapper</key>\s*)<true/>}{$1<false/>} or die "DisableIoMapper";
    s{(<key>ReconnectGraphicsOnConnect</key>\s*)<false/>}{$1<true/>} or die "ReconnectGraphicsOnConnect";
    s{(<key>boot-args</key>\s*<string>)[^<]*(</string>)}{${1}keepsyms=1 debug=0x100 darkwake=0 -btlfxboardid revcpu=1 ctrsmt=off$2} or die "boot-args";
    s{(<key>csr-active-config</key>\s*<data>\s*)AwgAAA==}{${1}AAAAAA==} or die "csr-active-config";
    s{(<key>Delete</key>\s*<dict>.*?<key>7C436110-AB2A-4BBB-A880-FE41995C9F82</key>\s*<array>)\s*<string>boot-args</string>}{$1}s or die "NVRAM Delete";
' "$R/efi/sample-z790/config.plist" > "$T/stage1.plist"
# the same as test-efi-flow.sh's plutil edits
cp "$R/efi/sample-z790/config.plist" "$T/ref.plist"
plutil -replace $A.csr-active-config -data AAAAAA== "$T/ref.plist"
plutil -replace $A.boot-args -string "keepsyms=1 debug=0x100 darkwake=0 -btlfxboardid revcpu=1 ctrsmt=off" "$T/ref.plist"
plutil -replace Booter.Quirks.ResizeAppleGpuBars -integer -1 "$T/ref.plist"
plutil -replace Kernel.Quirks.DisableIoMapper -bool false "$T/ref.plist"
plutil -replace UEFI.Output.ReconnectGraphicsOnConnect -bool true "$T/ref.plist"
plutil -remove NVRAM.Delete.7C436110-AB2A-4BBB-A880-FE41995C9F82.0 "$T/ref.plist"
[ "$(plutil -p "$T/stage1.plist")" = "$(plutil -p "$T/ref.plist")" ] || { echo "stage1: the text edits differ from plutil's"; diff <(plutil -p "$T/ref.plist") <(plutil -p "$T/stage1.plist"); exit 1; }
# and without the IntelXe.kext and SSDT-ARCOFF entries, for adding them
perl -0pe 's{\n\t\t\t<dict>(?:(?!<dict>).)*?<string>(?:IntelXe\.kext|SSDT-ARCOFF\.aml)</string>(?:(?!<dict>).)*?</dict>}{}sg' \
    "$T/stage1.plist" > "$T/noentries.plist"
plutil -lint -s "$T/noentries.plist"
[ "$(plutil -extract Kernel.Add raw "$T/noentries.plist")" = 15 ] && [ "$(plutil -extract ACPI.Add raw "$T/noentries.plist")" = 8 ] || { echo "noentries: not one entry less in each"; exit 1; }
# and without the HybridPM.kext entry, for adding it below Lilu.kext (item 0)
perl -0pe 's{\n\t\t\t<dict>(?:(?!<dict>).)*?<string>HybridPM\.kext</string>(?:(?!<dict>).)*?</dict>}{}sg' \
    "$T/stage1.plist" > "$T/nohybridpm.plist"
plutil -lint -s "$T/nohybridpm.plist"
[ "$(plutil -extract Kernel.Add raw "$T/nohybridpm.plist")" = 15 ] && [ "$(plutil -extract Kernel.Add.0.BundlePath raw "$T/nohybridpm.plist")" = Lilu.kext ] \
    || { echo "nohybridpm: not one entry less, or Lilu.kext not item 0"; exit 1; }
# the 0.1.3 SSDT-ARCOFF.aml (iasl 20260408 from efi/SSDT-ARCOFF.dsl), as release/test-efi-flow.sh has it
h=$(sed -nE 's/^    h=(\$\{h\})?([0-9a-f]+)$/\2/p' "$R/release/test-efi-flow.sh" | tr -d '\n')
printf "$(printf '%s' "$h" | sed 's/../\\x&/g')" > "$T/aml"
[ "$(head -c 4 "$T/aml")" = SSDT ] && [ "$(od -An -v -tu4 -j4 -N4 "$T/aml" | tr -d ' ')" = "$(wc -c < "$T/aml" | tr -d ' ')" ] \
    || { echo "the 0.1.3 SSDT-ARCOFF.aml from test-efi-flow.sh is not a table"; exit 1; }
for n in config.plist config-noentries.plist config-nohybridpm.plist SSDT-ARCOFF-0.1.3.aml; do
    case $n in config.plist) src=$T/stage1.plist;; config-noentries.plist) src=$T/noentries.plist;;
        config-nohybridpm.plist) src=$T/nohybridpm.plist;; *) src=$T/aml;; esac
    if [ $CHECK = yes ]; then cmp -s "$src" "$W/test/stage1/$n" || { echo "stage1/$n is out of date"; exit 1; }
    else cp "$src" "$W/test/stage1/$n"; fi
done
echo "ok    stage1/config.plist: the sample as stage 1 leaves it, its layout kept; config-noentries.plist without the two entries;"
echo "      config-nohybridpm.plist without the HybridPM entry; SSDT-ARCOFF-0.1.3.aml"

# ---- 2. efi.sh's results -----------------------------------------------------------------------------------------
P=$T/pkg
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
F=$T/efi
mkdir -p "$F/EFI/OC/Kexts" "$F/EFI/OC/ACPI" "$F/EFI/BOOT"
: > "$F/EFI/OC/OpenCore.efi"
C=$F/EFI/OC/config.plist
cp "$T/stage1.plist" "$C"
export INTELXE_STATE_DIR=$T/state INTELXE_BOOT_PATH="" INTELXE_CARD_ACPI_PATH=""   # no ioreg: the card is given per step
NOHYBRID="Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz" HYBRID="13th Gen Intel(R) Core(TM) i5-13400F"
export INTELXE_CPU_BRAND=$NOHYBRID   # no HybridPM.kext, but in the HybridPM steps below
PK=""                                # the package: $P, or another one
efi() { "$S" --package "${PK:-$P}" --efi "$F" "$@"; }
hash() { shasum -a 256 < "$1" | cut -d' ' -f1; }
step() { # step PLANARG APPLYARG NAME CARD: the plan without its "EFI:" line, then apply it and keep config.plist
    INTELXE_CARD_ACPI_PATH=$4 efi "$1" > "$T/plan"
    tail -n +2 "$T/plan" > "$X/efi-sh/$3.plan"
    INTELXE_CARD_ACPI_PATH=$4 efi "$2" --expect "$(hash "$T/plan")" > "$T/out" || { cat "$T/out"; exit 1; }
}
undo() { efi --undo-plan > "$T/plan"; efi --undo --expect "$(hash "$T/plan")" > "$T/out" || { cat "$T/out"; exit 1; }; }
step --settings-plan --settings-apply settings ''
cp "$C" "$X/efi-sh/after-settings.plist"
step --plan --apply driver-nossdt ''
cp "$C" "$X/efi-sh/after-settings-driver-nossdt.plist"
undo
step --plan --apply driver-ssdt '\_SB.PC00.PEG1.PEGP'
cp "$C" "$X/efi-sh/after-settings-driver-ssdt.plist"
undo; undo
cmp -s "$C" "$T/stage1.plist" || { echo "efi.sh's undo did not give stage 1 back"; exit 1; }
step --plan --apply driver-stage1 ''
cp "$C" "$X/efi-sh/after-driver-nossdt.plist"
undo
cp "$T/noentries.plist" "$C"
step --plan --apply noentries-driver-ssdt '\_SB.PC00.PEG1.PEGP'
cp "$C" "$X/efi-sh/after-noentries-driver-ssdt.plist"
undo
# HybridPM.kext on a hybrid CPU, Lilu.kext in the EFI: the sample's disabled entry turned on; without an entry, one
# inserted right below Lilu.kext's
mkdir -p "$F/EFI/OC/Kexts/Lilu.kext/Contents/MacOS"
export INTELXE_CPU_BRAND=$HYBRID
cp "$T/stage1.plist" "$C"
step --plan --apply hybrid-driver ''
cp "$C" "$X/efi-sh/after-hybrid-driver.plist"
undo
cmp -s "$C" "$T/stage1.plist" && [ ! -e "$F/EFI/OC/Kexts/HybridPM.kext" ] || { echo "efi.sh's undo (hybrid) did not give stage 1 back"; exit 1; }
cp "$T/nohybridpm.plist" "$C"
step --plan --apply nohybridpm-driver ''
cp "$C" "$X/efi-sh/after-nohybridpm-driver.plist"
undo
export INTELXE_CPU_BRAND=$NOHYBRID
rm -rf "$F/EFI/OC/Kexts/Lilu.kext"
# which CPU names efi.sh adds HybridPM.kext for: "yes|name" or "no|name"
: > "$X/efi-sh/hybrid-cpus.txt"
for cpu in "$NOHYBRID" "$HYBRID" '12th Gen Intel(R) Core(TM) i9-12900K' '12th Gen Intel(R) Core(TM) i5-12400F' \
    '12th Gen Intel(R) Core(TM) i5-12500' '12th Gen Intel(R) Core(TM) i5-12500H' '12th Gen Intel(R) Core(TM) i5-12600' \
    '12th Gen Intel(R) Core(TM) i5-12600K' '12th Gen Intel(R) Core(TM) i5-12600H' '12th Gen Intel(R) Core(TM) i5-12490F' \
    '12th Gen Intel(R) Core(TM) i3-12100F' '12th Gen Intel(R) Core(TM) i3-12300' '13th Gen Intel(R) Core(TM) i3-13100' \
    '13th Gen Intel(R) Core(TM) i7-13620H' '14th Gen Intel(R) Core(TM) i9-14900K' '14th Gen Intel(R) Core(TM) i5-14400F' \
    'Intel(R) Core(TM) i3-14100' 'Intel(R) Core(TM) Ultra 7 155H' '12th Gen Intel(R) Core(TM) i7-1255U' \
    '13th Gen Intel(R) Core(TM) i7-1360P'; do
    printf '%s|%s\n' "$(INTELXE_CPU_BRAND=$cpu efi --status | sed -n 's/^hybridpm_needed=//p')" "$cpu" >> "$X/efi-sh/hybrid-cpus.txt"
done
# SSDT-ARCOFF adapted to the card's port: the package with the 0.1.3 table; a port of the same depth, a shorter and a
# longer one (their PkgLengths and the table's length change)
P2=$T/pkg-aml
cp -R "$P" "$P2"
cp "$T/aml" "$P2/EFI/OC/ACPI/SSDT-ARCOFF.aml"
(cd "$P2" && find EFI -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
PK=$P2
for c in 'rp05|\_SB.PC00.RP05.PXSX' 'gpp8|\_SB.PCI0.GPP8' 'deep|\_SB.PCI0.GPP8.UP00.DP40.UP00'; do
    n=${c%%|*} cpath=${c#*|}
    cp "$T/stage1.plist" "$C"
    step --plan --apply adapt-$n "$cpath"
    grep -qF "adapted to this card's port: names $cpath instead of" "$X/efi-sh/adapt-$n.plan" || { echo "efi.sh did not adapt the table to $cpath"; exit 1; }
    cp "$F/EFI/OC/ACPI/SSDT-ARCOFF.aml" "$X/efi-sh/adapt-$n.aml"
    if [ $n = rp05 ]; then cp "$C" "$X/efi-sh/after-adapt-rp05.plist"; fi
    undo
done
PK=""
cmp -s "$C" "$T/stage1.plist" && [ ! -e "$F/EFI/OC/ACPI/SSDT-ARCOFF.aml" ] || { echo "efi.sh's undo (adapted table) did not give stage 1 back"; exit 1; }
echo "ok    expected/efi-sh/: efi.sh's plans and config.plists, adapted tables, hybrid CPU names"

# ---- 3. the engine -----------------------------------------------------------------------------------------------
B=$T/build
mkdir -p "$B/src" "$T/nosource"
cp "$W/test/macos/EngineTest.csproj" "$W/test/macos/EngineTest.cs" "$B/src/"
sed -n '/^\/\/ BEGIN ENGINE$/,/^\/\/ END ENGINE$/p' "$W/IntelXe-EFI.ps1" > "$B/src/Engine.cs"
[ -s "$B/src/Engine.cs" ] || { echo "no engine between // BEGIN ENGINE and // END ENGINE in IntelXe-EFI.ps1"; exit 1; }
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
dotnet build "$B/src/EngineTest.csproj" -o "$B/out" --source "$T/nosource" > "$T/build.log" 2>&1 || { cat "$T/build.log"; exit 1; }
O=$X/engine; [ $CHECK = yes ] && { O=$T/engine; mkdir -p "$O"; }
dotnet "$B/out/EngineTest.dll" "$W/test/stage1/config.plist" "$R/efi/sample-z790/config.plist" "$X" "$O" "$T/run" \
    "$R/release/templates/efi-rules.json" ${PACKAGE:+"$PACKAGE"}
if [ $CHECK = yes ]; then
    for f in "$O"/*; do cmp -s "$f" "$X/engine/$(basename "$f")" || { echo "expected/engine/$(basename "$f") is out of date"; exit 1; }; done
fi
