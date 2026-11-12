#!/bin/bash
# SPDX-License-Identifier: MIT
# efi.sh: the OpenCore part of intel-gpu-macos-driver, done on macOS: puts this package's IntelXe.kext and SSDT-ARCOFF.aml
# (adapted to the card's ACPI path) into the EFI OpenCore booted from, with their config.plist entries, and undoes it.
# On a 12th-14th gen Intel CPU with efficiency cores it adds HybridPM.kext too, below Lilu.kext.
#
#   ./efi.sh --status               the EFI compared with this package, one "key=value" per line. Changes nothing.
#   ./efi.sh --plan                 what --apply would change (stage 4 of README-INSTALL.md). Changes nothing.
#   sudo ./efi.sh --apply           back up the whole EFI folder, then make those changes
#   ./efi.sh --settings-plan        what --settings-apply would change (stage 2: SIP, AMFI and surfvram boot-args, quirks)
#   sudo ./efi.sh --settings-apply  back up the whole EFI folder, then make those changes
#   ./efi.sh --safari-plan          what --safari-apply would change: Misc > Security > SecureBootModel = Disabled, which
#                                   the optional system-volume copy (Safari on the GPU) needs. Changes nothing.
#   sudo ./efi.sh --safari-apply    back up the whole EFI folder, then make that change
#   ./efi.sh --settings-safari-plan / sudo ./efi.sh --settings-safari-apply   both of the above as one change (one backup,
#                                   one restart: the setup assistant's first step with Safari chosen)
#   ./efi.sh --undo-plan            what --undo would put back
#   sudo ./efi.sh --undo            put back, from the last backup, the files the last change replaced
#   sudo ./efi.sh --mount           mount the EFI partition (macOS mounts it read-write; it stays mounted until restart)
#
# Which EFI: the partition in OpenCore's NVRAM variable 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path, which OpenCore
# writes when Misc > Security > ExposeSensitiveData includes 1 (--apply adds it). Without it, name the partition with
# --part UUID, its partition UUID (--status lists every EFI partition as "candidate="). Never a disk number: those change
# between boots. --efi DIR works on a folder holding EFI/OC instead (a copy, or an EFI you mounted yourself).
# --package DIR takes the files from that package (default: this script's folder). --expect SHA256 with --apply,
# --settings-apply or --undo: change nothing unless the plan's SHA-256 is that one, the plan that was shown.
# Backups: /Library/Application Support/intel-gpu-macos-driver/efi-backups/<partition UUID>/<NNN-date>/EFI.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
PKG=$(cd "$(dirname "$0")" && pwd)
STATE=${INTELXE_STATE_DIR:-/Library/Application Support/intel-gpu-macos-driver}   # tests point it at a throwaway folder
OCGUID=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
APPLE=7C436110-AB2A-4BBB-A880-FE41995C9F82
KEXT=IntelXe.kext OLDKEXT=B580.kext SSDT=SSDT-ARCOFF.aml   # intelxe-rename: keep
HPM=HybridPM.kext LILU=Lilu.kext
CMD="" EFIDIR="" PART="" EXPECT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --status|--plan|--apply|--settings-plan|--settings-apply|--safari-plan|--safari-apply|--settings-safari-plan|--settings-safari-apply|--undo-plan|--undo|--mount) CMD=$1; shift;;
        --package) PKG=$(cd "$2" && pwd); shift 2;;
        --efi) EFIDIR=$(cd "$2" && pwd); shift 2;;
        --part) PART=$(printf '%s' "$2" | tr a-f A-F); shift 2;;
        --expect) EXPECT=$2; shift 2;;
        *) sed -n '3,26p' "$0"; exit 2;;
    esac
done
[ -n "$CMD" ] || { sed -n '3,26p' "$0"; exit 2; }
if [ -n "$PART" ] && ! [[ "$PART" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]]; then
    echo "--part takes a partition UUID (see --status, candidate=), not a disk name"; exit 2
fi
T=$(mktemp -d -t intelxe-efi)
trap 'rm -rf "$T"' EXIT

pget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }   # pget PLIST KEYPATH: the value, or nothing
upper() { tr a-z A-Z; }

# ---- which EFI ----------------------------------------------------------------------------------------------------
boot_path() {
    if [ -n "${INTELXE_BOOT_PATH+x}" ]; then printf '%s\n' "$INTELXE_BOOT_PATH"; else nvram "$OCGUID:boot-path" 2>/dev/null | cut -f2- || true; fi
}
# the GPT partition UUID in a device path such as PciRoot(0x0)/.../HD(1,GPT,<UUID>,0x28,0x64000)/\EFI\OC\OpenCore.efi
boot_uuid() { boot_path | sed -nE 's/.*HD\([0-9]+,GPT,([0-9A-Fa-f-]{36}),.*/\1/p' | head -1 | upper; }

SOURCE=none ROOTDIR="" DEV="" MODEL="" SIZE="" WHERE="" CONTENT="" MP="" KEY="" BOOTREC=no
[ -n "$(boot_uuid)" ] && BOOTREC=yes
if [ -n "$EFIDIR" ]; then
    SOURCE=folder ROOTDIR=$EFIDIR CONTENT=folder
    KEY=folder-$(printf '%s' "$EFIDIR" | shasum -a 256 | cut -c1-12)
else
    if [ -n "$PART" ]; then SOURCE=chosen; else PART=$(boot_uuid); [ -n "$PART" ] && SOURCE=boot-path; fi
    if [ -n "$PART" ]; then
        KEY=$PART
        if diskutil info -plist "$PART" > "$T/part.plist" 2>/dev/null; then
            DEV=$(pget "$T/part.plist" DeviceIdentifier)
            CONTENT=$(pget "$T/part.plist" Content)
            MP=$(pget "$T/part.plist" MountPoint)
            whole=$(pget "$T/part.plist" ParentWholeDisk)
            if [ -n "$whole" ] && diskutil info -plist "$whole" > "$T/disk.plist" 2>/dev/null; then
                MODEL=$(pget "$T/disk.plist" MediaName); [ -n "$MODEL" ] || MODEL=$(pget "$T/disk.plist" IORegistryEntryName)
                SIZE=$(pget "$T/disk.plist" Size)
                [ "$(pget "$T/disk.plist" Internal)" = true ] && WHERE=internal || WHERE=external
            fi
            [ -n "$MP" ] && ROOTDIR=$MP
        fi
    fi
fi
E=${ROOTDIR:+$ROOTDIR/EFI/OC}
CFG=${E:+$E/config.plist}
READABLE=no; [ -n "$CFG" ] && [ -r "$CFG" ] && READABLE=yes
OPENCORE=unknown
if [ -n "$ROOTDIR" ]; then
    if [ -f "$E/OpenCore.efi" ] && [ -f "$CFG" ]; then OPENCORE=yes; elif [ -d "$ROOTDIR/EFI" ]; then OPENCORE=no; fi
fi
gb() { [ -n "$1" ] && awk -v b="$1" 'BEGIN { printf "%.0f GB", b / 1e9 }'; }
describe() { # one line naming the EFI: the disk's model first, so that it can be recognised
    case $SOURCE in
        folder) echo "the folder $ROOTDIR";;
        none) echo "no EFI partition found";;
        *) local how="found through OpenCore's boot-path"; [ $SOURCE = chosen ] && how="chosen by partition UUID"
           echo "${MODEL:-unknown disk}${WHERE:+, $WHERE}${SIZE:+, $(gb "$SIZE")}; ${DEV:-not found} (partition $PART), $how";;
    esac
}

# ---- the package's files and the EFI's ----------------------------------------------------------------------------
PK=$PKG/EFI/OC/Kexts/$KEXT PS=$PKG/EFI/OC/ACPI/$SSDT
[ -f "$PK/Contents/MacOS/IntelXe" ] && [ -f "$PS" ] || { echo "no EFI/OC/Kexts/$KEXT and EFI/OC/ACPI/$SSDT in $PKG"; exit 1; }
kext_state() { # same|different|absent|unknown: the executable and Info.plist compared byte for byte
    [ "$READABLE" = yes ] || { echo unknown; return; }
    local k=$E/Kexts/$KEXT
    if [ ! -e "$k" ]; then echo absent
    elif cmp -s "$k/Contents/MacOS/IntelXe" "$PK/Contents/MacOS/IntelXe" && cmp -s "$k/Contents/Info.plist" "$PK/Contents/Info.plist"; then echo same
    else echo different; fi
}
ssdt_state() { # against the table this card needs: the package's, or the package's adapted to the card's port (SSDTSRC)
    [ "$READABLE" = yes ] || { echo unknown; return; }
    if [ ! -e "$E/ACPI/$SSDT" ]; then echo absent; elif cmp -s "$E/ACPI/$SSDT" "$SSDTSRC"; then echo same; else echo different; fi
}
kext_version() { pget "$1/Contents/Info.plist" CFBundleVersion; }

# ---- HybridPM.kext (kernel/HybridPM): only for Alder Lake / Raptor Lake CPUs with efficiency cores ------------------
# By the CPU's name: machdep.cpu.model is what OpenCore's Cpuid1Data makes it (Comet Lake on the test machine). Core
# Ultra is left out (HybridPM leaves it alone), as are the desktop models with P-cores only. INTELXE_CPU_BRAND: tests.
HPK=$PKG/EFI/OC/Kexts/$HPM
BRAND=${INTELXE_CPU_BRAND-$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)}
case "$BRAND" in
    *Ultra*|*i3-1[234]100*|*i3-12300*|*i5-12400*|*i5-12490*|*i5-12500|*i5-12500[!H]*|*i5-12600|*i5-12600[!KH]*) HPMNEED=no;;
    *i[3579]-1[2-4][0-9][0-9][0-9]*) HPMNEED=yes;;
    *i[3579]-1[2-4][0-9][0-9][UP]*) HPMNEED=yes;;   # the U and P laptop models (i7-1255U, i7-1360P): all with E-cores
    *) HPMNEED=no;;
esac
HPMPKG=no; [ -f "$HPK/Contents/MacOS/HybridPM" ] && [ -f "$HPK/Contents/Info.plist" ] && HPMPKG=yes
hpm_state() { # same|different|absent|unknown, as kext_state; "different" also for an earlier build (org.hackintosh.HybridPM)
    [ "$READABLE" = yes ] || { echo unknown; return; }
    local k=$E/Kexts/$HPM
    if [ ! -e "$k" ]; then echo absent
    elif [ $HPMPKG = yes ] && cmp -s "$k/Contents/MacOS/HybridPM" "$HPK/Contents/MacOS/HybridPM" && cmp -s "$k/Contents/Info.plist" "$HPK/Contents/Info.plist"; then echo same
    else echo different; fi
}

# the cards efi.sh looks for: PCI device ids (vendor 8086), the same list as data/platforms.json "supported"
# (tools/parity/devices.py checks every copy in the repository)
CARD_IDS="e20b e20c"
# the ACPI path of the B580's upstream port (the bridge two PCI levels above the GPU), e.g. \_SB.PC00.PEG1.PEGP;
# "none" when that port has no ACPI name, nothing when there is no B580
card_acpi_path() {
    if [ -n "${INTELXE_CARD_ACPI_PATH+x}" ]; then printf '%s\n' "$INTELXE_CARD_ACPI_PATH"; return 0; fi
    { if [ -n "${INTELXE_IOREG_FILE:-}" ]; then cat "$INTELXE_IOREG_FILE"; else ioreg -p IOService -w0 -l 2>/dev/null || true; fi; } | awk -v ids="$CARD_IDS" '
        BEGIN {   # ioreg shows a device id little-endian: e20b is <0be20000>
            k = split(ids, a, " "); re = ""
            for (i = 1; i <= k; i++) re = re (i > 1 ? "|" : "") substr(a[i], 3, 2) substr(a[i], 1, 2) "0000"
            re = "\"device-id\" = <(" re ")>"
        }
        /\+-o / {
            c = index($0, "+-o")
            while (n > 0 && col[n] >= c) n--
            n++; col[n] = c; pci[n] = ($0 ~ /<class IOPCIDevice[,>]/); acpi[n] = ""; did[n] = 0; vid[n] = 0
            next
        }
        n == 0 || done { next }
        /"acpi-path" = "/ { s = $0; sub(/.*"acpi-path" = "/, "", s); sub(/".*/, "", s); acpi[n] = s }
        $0 ~ re { did[n] = 1 }   # one of CARD_IDS
        /"vendor-id" = <86800000>/ { vid[n] = 1 }
        pci[n] && did[n] && vid[n] {
            done = 1; up = 0
            for (k = n - 1; k > 0; k--) if (pci[k] && ++up == 2) break
            s = k > 0 ? acpi[k] : ""
            if (s == "") { print "none"; exit }
            sub(/^IOACPIPlane:\//, "", s); m = split(s, parts, "/"); out = ""
            for (i = 1; i <= m; i++) { p = parts[i]; sub(/@.*/, "", p); out = out (i > 1 ? "." : "") p }
            print "\\" out; exit
        }'
}
CARD=$(card_acpi_path)
# an AML table names \_SB.PC00.PEG1.PEGP as the bytes 2f 04 "_SB_PC00PEG1PEGP" (after "\" or not; namepath_hex below):
# do the table's bytes name the card's port? The count keeps a shorter path inside a longer one from matching.
aml_names_card() {
    local np
    np=$(namepath_hex "$CARD") || return 1
    case $np in " 5c 2e"*|" 5c 2f"*) np=${np# 5c};; esac
    [[ "$(hexof "$1")" == *"$np"* ]]
}

# ---- SSDT-ARCOFF adapted to the card's port -------------------------------------------------------------------------
# The package's table (efi/SSDT-ARCOFF.dsl) names the port twice, both times as an absolute NamePath: "\" (5c), then for
# one NameSeg nothing, for two DualNamePrefix (2e), for more MultiNamePrefix (2f) and the count, then the 4-byte NameSegs
# padded with "_": 5c 2f 04 "_SB_PC00PEG1PEGP". The table is
#   header (36 bytes; the length at 4, the checksum at 9)
#   If (a0) PkgLength Zero (00)  External (15) NamePath ObjectType ArgCount      (iasl wraps External in If (Zero))
#   If (a0) PkgLength  CondRefOf (\_OSI, Local0) && _OSI ("Darwin")
#       Scope (10) PkgLength NamePath  Device (BRG1) { ... }
# and ends with the Scope; nothing inside the Scope names the port. adapt_ssdt finds those parts, builds the table again
# from them with the package's path and goes on only when that gives the package's table byte for byte; then it builds
# the table with the card's path. With as many NameSegs that only swaps NameSegs and the checksum; a longer or shorter
# path also changes the three PkgLengths (both Ifs, the Scope) and the header's length, which the build computes.
PKGPATH='\_SB.PC00.PEG1.PEGP'
hexof() { od -An -v -tx1 "$@" | tr -s ' \n' '  ' | sed 's/ $//'; }   # a file (or stdin) as " hh" bytes
namepath_hex() { # the NamePath of an absolute ACPI path, " hh" bytes; fails for anything that is not one
    local re='^\\[A-Z_][A-Z0-9_]{0,3}(\.[A-Z_][A-Z0-9_]{0,3})*$' s n=0 out=""
    [[ "$1" =~ $re ]] || return 1
    for s in $(printf '%s' "${1#\\}" | tr . ' '); do out=$out$(printf '%-4s' "$s" | tr ' ' _ | hexof); n=$((n + 1)); done
    [ $n -le 255 ] || return 1
    if [ $n = 1 ]; then printf ' 5c%s' "$out"; elif [ $n = 2 ]; then printf ' 5c 2e%s' "$out"; else printf ' 5c 2f %02x%s' $n "$out"; fi
}
pkglen_hex() { # the PkgLength for N bytes that follow it, in as few bytes as it fits (as iasl writes it)
    local n=$1 k j v
    [ $((n + 1)) -le 63 ] && { printf ' %02x' $((n + 1)); return 0; }
    for k in 1 2 3; do
        v=$((n + 1 + k))
        if [ $v -lt $((1 << (4 + 8 * k))) ]; then
            printf ' %02x' $(((k << 6) | (v & 15)))
            for ((j = 0; j < k; j++)); do printf ' %02x' $(((v >> (4 + 8 * j)) & 255)); done
            return 0
        fi
    done
    return 1
}
pkglen_at() { # pkglen_at I: "VALUE SIZE" of the PkgLength at byte I of AB
    local lead=$((16#${AB[$1]})) k j v
    k=$((lead >> 6))
    if [ $k = 0 ]; then v=$((lead & 63)); else v=$((lead & 15)); for ((j = 1; j <= k; j++)); do v=$((v | 16#${AB[$1 + j]} << (4 + 8 * (j - 1)))); done; fi
    echo "$v $((k + 1))"
}
hex_sum() { local b s=0; for b in $1; do s=$((s + 16#$b)); done; echo $((s & 255)); }
build_ssdt() { # build_ssdt NAMEPATH: the table from the parts adapt_ssdt found, naming NAMEPATH; " hh" bytes
    local ext scope ifb body len hd="" i
    ext=" 00 15$1$AXT"; ext=" a0$(pkglen_hex $((${#ext} / 3)))$ext"
    scope="$1$ABODY"; scope=" 10$(pkglen_hex $((${#scope} / 3)))$scope"
    ifb="$APRED$scope"; ifb=" a0$(pkglen_hex $((${#ifb} / 3)))$ifb"
    body=$ext$ifb; len=$((36 + ${#body} / 3))
    for ((i = 0; i < 36; i++)); do
        case $i in 4|5|6|7) hd="$hd $(printf %02x $(((len >> (8 * (i - 4))) & 255)))";; 9) hd="$hd 00";; *) hd="$hd ${AB[$i]}";; esac
    done
    printf '%s %02x%s%s' "${hd:0:27}" $(((256 - $(hex_sum "$hd$body")) & 255)) "${hd:30}" "$body"
}
ADAPTWHY=""
adapt_ssdt() { # adapt_ssdt OUT: the package's SSDT-ARCOFF naming the card's port, written to OUT and read back; ADAPTWHY when not
    local ep ec h n i q k v s rest p1 sc=0 got
    ec=$(namepath_hex "$CARD") || { ADAPTWHY="$CARD is not an ACPI path this script can write into the table"; return 1; }
    ep=$(namepath_hex "$PKGPATH")
    ADAPTWHY="the package's $SSDT is not laid out as SSDT-ARCOFF.dsl builds it"
    h=$(hexof "$PS"); read -r -a AB <<< "$h"; n=${#AB[@]}
    [ $n -gt 40 ] && [ "${AB[0]}${AB[1]}${AB[2]}${AB[3]}" = 53534454 ] || return 1
    [ $((16#${AB[4]} | 16#${AB[5]} << 8 | 16#${AB[6]} << 16 | 16#${AB[7]} << 24)) = $n ] && [ "$(hex_sum "$h")" = 0 ] || return 1
    # the port's NamePath twice: the External's right after the header, then the Scope's
    [ "${AB[36]}" = a0 ] || return 1
    read -r v k <<< "$(pkglen_at 37)"; i=$((37 + k))
    [ "${AB[$i]}${AB[$i + 1]}" = 0015 ] && [ "${h:$(((i + 2) * 3)):${#ep}}" = "$ep" ] || return 1
    i=$((i + 2 + ${#ep} / 3)); AXT=" ${AB[$i]} ${AB[$i + 1]}"   # the External's ObjectType and ArgCount
    i=$((i + 2)); [ $((37 + v)) = $i ] || return 1               # the If (Zero) holds the External only
    rest=${h#*"$ep"}; s=${rest#*"$ep"}
    [ "${h%"$rest"}" = "${h:0:$(((i - 2) * 3))}" ] && [ "$s" != "$rest" ] && [ "${s#*"$ep"}" = "$s" ] || return 1
    q=$(((${#h} - ${#s} - ${#ep}) / 3))                       # the byte where the Scope's NamePath starts
    [ "${AB[$i]}" = a0 ] || return 1
    read -r v k <<< "$(pkglen_at $((i + 1)))"; [ $((i + 1 + v)) = $n ] || return 1   # the If ends with the table
    p1=$((i + 1 + k))                                         # the If's predicate starts here
    for k in 1 2 3 4; do                                      # the Scope opcode and its PkgLength before the NamePath
        [ $((q - 1 - k)) -ge $p1 ] && [ "${AB[$q - 1 - k]}" = 10 ] && [ $((16#${AB[$q - k]} >> 6)) = $((k - 1)) ] || continue
        read -r v got <<< "$(pkglen_at $((q - k)))"
        [ "$got" = $k ] && [ $((q - k + v)) = $n ] && { sc=$k; break; }   # the Scope ends with the table too
    done
    [ $sc -gt 0 ] || return 1
    APRED=""; for ((i = p1; i < q - 1 - sc; i++)); do APRED="$APRED ${AB[$i]}"; done
    ABODY=${s}
    [ "$(build_ssdt "$ep")" = "$h" ] || return 1             # the parts make the package's table again
    ADAPTWHY="the table built for $CARD did not read back as built"
    s=$(build_ssdt "$ec")
    printf "$(printf '%s' "$s" | sed 's/ /\\x/g')" > "$1"
    got=$(hexof "$1")
    [ "$got" = "$s" ] && [ "$(hex_sum "$got")" = 0 ] && aml_names_card "$1" || return 1
    ADAPTWHY=""
}
# SSDTSRC: the table this card needs, the package's or one adapted from it. ADAPT: yes (the package's names the card's
# port), adapted, or no
SSDTSRC=$PS ADAPT=no
if aml_names_card "$PS"; then ADAPT=yes
elif [ -n "$CARD" ] && [ "$CARD" != none ] && adapt_ssdt "$T/$SSDT"; then ADAPT=adapted SSDTSRC=$T/$SSDT; fi

# ---- config.plist ---------------------------------------------------------------------------------------------------
count() { local n; n=$(pget "$1" "$2"); [[ "$n" =~ ^[0-9]+$ ]] && echo "$n" || echo 0; }
find_entry() { # find_entry PLIST ARRAY FIELD VALUE: the index of the first entry whose FIELD is VALUE (any case), or nothing
    local n i v want; n=$(count "$1" "$2"); want=$(printf '%s' "$4" | upper)
    for ((i = 0; i < n; i++)); do
        v=$(pget "$1" "$2.$i.$3" | upper)
        [ "$v" = "$want" ] && { echo $i; return 0; }
    done
    return 0
}
entry_state() { # enabled|disabled|absent|unknown
    [ "$READABLE" = yes ] || { echo unknown; return; }
    local i; i=$(find_entry "$CFG" "$1" "$2" "$3")
    if [ -z "$i" ]; then echo absent; elif [ "$(pget "$CFG" "$1.$i.Enabled")" = true ]; then echo enabled; else echo disabled; fi
}

# A plan is two lists: PLAN, the lines shown, and OPS, what --apply does (tab-separated):
#   copy SRC REL           replace EFI/OC/REL with a copy of SRC
#   set KEYPATH TYPE VALUE plutil -replace in config.plist
#   append KEYPATH TYPE VALUE   append to the array at KEYPATH
#   insert ARRAY.N TYPE VALUE   insert at index N of that array (the items from N on move up by one)
#   mkdict KEYPATH / mkarray KEYPATH
PLAN=() OPS=() NOTES=()
TAB=$'\t'
op() { local IFS=$TAB; OPS+=("$*"); }

plan_files() {
    local i ks ss pkgok efiok cur
    ks=$(kext_state)
    case $ks in
        absent) PLAN+=("copy     EFI/OC/Kexts/$KEXT (version $(kext_version "$PK"))"); op copy "$PK" "Kexts/$KEXT";;
        different) PLAN+=("replace  EFI/OC/Kexts/$KEXT (version $(kext_version "$E/Kexts/$KEXT") in the EFI, $(kext_version "$PK") in the package; the files differ)")
                   op copy "$PK" "Kexts/$KEXT";;
    esac
    i=$(find_entry "$CFG" Kernel.Add BundlePath "$KEXT")
    if [ -z "$i" ]; then
        PLAN+=("config   Kernel > Add: add $KEXT (ExecutablePath Contents/MacOS/IntelXe, PlistPath Contents/Info.plist, Enabled)")
        op append Kernel.Add json '{"Arch":"Any","BundlePath":"IntelXe.kext","Comment":"IntelXe.kext: intel-gpu-macos-driver","Enabled":true,"ExecutablePath":"Contents/MacOS/IntelXe","MaxKernel":"","MinKernel":"","PlistPath":"Contents/Info.plist"}'
    else
        [ "$(pget "$CFG" Kernel.Add.$i.Enabled)" = true ] || { PLAN+=("config   Kernel > Add, item $i ($KEXT): Enabled false -> true"); op set Kernel.Add.$i.Enabled bool true; }
        cur=$(pget "$CFG" Kernel.Add.$i.ExecutablePath)
        [ "$cur" = Contents/MacOS/IntelXe ] || { PLAN+=("config   Kernel > Add, item $i ($KEXT): ExecutablePath '$cur' -> 'Contents/MacOS/IntelXe'"); op set Kernel.Add.$i.ExecutablePath string Contents/MacOS/IntelXe; }
        cur=$(pget "$CFG" Kernel.Add.$i.PlistPath)
        [ "$cur" = Contents/Info.plist ] || { PLAN+=("config   Kernel > Add, item $i ($KEXT): PlistPath '$cur' -> 'Contents/Info.plist'"); op set Kernel.Add.$i.PlistPath string Contents/Info.plist; }
    fi
    i=$(find_entry "$CFG" Kernel.Add BundlePath "$OLDKEXT")
    if [ -n "$i" ] && [ "$(pget "$CFG" Kernel.Add.$i.Enabled)" = true ]; then
        PLAN+=("config   Kernel > Add, item $i ($OLDKEXT, the earlier name of $KEXT): Enabled true -> false (only one of the two may load)")
        op set Kernel.Add.$i.Enabled bool false
    fi

    # SSDT-ARCOFF only when a table names the card's upstream port: the shipped one names \_SB.PC00.PEG1.PEGP; for
    # another port, the shipped one adapted to it (adapt_ssdt)
    ss=$(ssdt_state)
    efiok=no; [ "$ss" = absent ] || ! aml_names_card "$E/ACPI/$SSDT" || efiok=yes
    if [ -z "$CARD" ]; then
        NOTES+=("not added: $SSDT. No Arc B580 found on this machine, so its ACPI path cannot be checked (stage 4, step 2 of README-INSTALL.md)")
    elif [ "$CARD" = none ]; then
        NOTES+=("not added: $SSDT. The B580's upstream port has no ACPI name on this board; the table needs one more bridge level (stage 4, step 2 of README-INSTALL.md; please open an issue)")
    elif [ $efiok = no ] && [ $ADAPT = no ]; then
        NOTES+=("not added: $SSDT. The package's table is for \\_SB.PC00.PEG1.PEGP; the card's port here is $CARD, and $ADAPTWHY. Put that path into SSDT-ARCOFF.dsl, compile it with iasl and copy it to EFI/OC/ACPI (stage 4, step 2 of README-INSTALL.md); then this adds its entry")
    else
        if [ $efiok = yes ] && [ "$ss" = different ]; then
            if [ $ADAPT = adapted ]; then cur="the package's adapted to this card's port"; else cur="the package's"; fi
            NOTES+=("kept: EFI/OC/ACPI/$SSDT differs from $cur but names this card's port ($CARD): yours stays")
        elif [ "$ss" != same ]; then
            if [ "$ss" = absent ]; then cur=copy; else cur=replace; fi
            if [ $ADAPT = adapted ]; then
                PLAN+=("$(printf '%-8s' $cur) EFI/OC/ACPI/$SSDT (the package's table adapted to this card's port: names $CARD instead of $PKGPATH)")
            else
                PLAN+=("$(printf '%-8s' $cur) EFI/OC/ACPI/$SSDT (names $CARD, this card's port)")
            fi
            op copy "$SSDTSRC" "ACPI/$SSDT"
        fi
        i=$(find_entry "$CFG" ACPI.Add Path "$SSDT")
        if [ -z "$i" ]; then
            PLAN+=("config   ACPI > Add: add $SSDT (Enabled)")
            op append ACPI.Add json '{"Comment":"SSDT-ARCOFF: hides the Arc B580/B570 from the generic display driver (intel-gpu-macos-driver)","Enabled":true,"Path":"SSDT-ARCOFF.aml"}'
        elif [ "$(pget "$CFG" ACPI.Add.$i.Enabled)" != true ]; then
            PLAN+=("config   ACPI > Add, item $i ($SSDT): Enabled false -> true"); op set ACPI.Add.$i.Enabled bool true
        fi
    fi

    plan_hybridpm   # after every change above that names an item by its index: it may insert one

    # so that OpenCore records which EFI it booted from, and the next run finds it without being told
    cur=$(pget "$CFG" Misc.Security.ExposeSensitiveData)
    if [ "$BOOTREC" = no ] && [[ "$cur" =~ ^[0-9]+$ ]] && [ $((cur & 1)) = 0 ]; then
        PLAN+=("config   Misc > Security > ExposeSensitiveData: $cur -> $((cur | 1)) (OpenCore then records which EFI it booted from, for this script)")
        op set Misc.Security.ExposeSensitiveData integer $((cur | 1))
    fi
    return 0
}

# HybridPM.kext with an entry right below Lilu.kext's (a Lilu plugin loads after Lilu), only where it is needed and Lilu
# is loaded; an earlier build under the same name (org.hackintosh.HybridPM) is replaced
plan_hybridpm() {
    [ $HPMNEED = yes ] || return 0
    local l h hs cur id
    if [ $HPMPKG = no ]; then
        NOTES+=("not added: $HPM. This CPU ($BRAND) has efficiency cores and macOS holds it below its clocks, but this package has no $HPM (it comes with 0.1.4 and later)")
        return 0
    fi
    l=$(find_entry "$CFG" Kernel.Add BundlePath "$LILU")
    if [ -z "$l" ] || [ "$(pget "$CFG" Kernel.Add.$l.Enabled)" != true ] || [ ! -e "$E/Kexts/$LILU" ]; then
        NOTES+=("not added: $HPM. This CPU ($BRAND) has efficiency cores and $HPM corrects its clocks, but it is a Lilu plugin and this EFI does not load $LILU (no file in EFI/OC/Kexts, or its Kernel > Add entry is missing or off). Add Lilu (github.com/acidanthera/Lilu), then run this again (README-INSTALL.md, Hybrid Intel CPUs)")
        return 0
    fi
    hs=$(hpm_state)
    case $hs in
        absent) PLAN+=("copy     EFI/OC/Kexts/$HPM (version $(kext_version "$HPK"); this CPU has efficiency cores: it corrects their clocks)"); op copy "$HPK" "Kexts/$HPM";;
        different)
            id=$(pget "$E/Kexts/$HPM/Contents/Info.plist" CFBundleIdentifier)
            if [ "$id" = org.hackintosh.HybridPM ]; then
                PLAN+=("replace  EFI/OC/Kexts/$HPM (an earlier build, org.hackintosh.HybridPM $(kext_version "$E/Kexts/$HPM"), by the package's $(kext_version "$HPK"))")
            else
                PLAN+=("replace  EFI/OC/Kexts/$HPM (version $(kext_version "$E/Kexts/$HPM") in the EFI, $(kext_version "$HPK") in the package; the files differ)")
            fi
            op copy "$HPK" "Kexts/$HPM";;
    esac
    h=$(find_entry "$CFG" Kernel.Add BundlePath "$HPM")
    if [ -z "$h" ]; then
        PLAN+=("config   Kernel > Add: add $HPM below $LILU, as item $((l + 1)) (ExecutablePath Contents/MacOS/HybridPM, PlistPath Contents/Info.plist, Enabled)")
        op insert Kernel.Add.$((l + 1)) json '{"Arch":"Any","BundlePath":"HybridPM.kext","Comment":"HybridPM.kext: hybrid-CPU power management (intel-gpu-macos-driver)","Enabled":true,"ExecutablePath":"Contents/MacOS/HybridPM","MaxKernel":"","MinKernel":"","PlistPath":"Contents/Info.plist"}'
    else
        [ "$(pget "$CFG" Kernel.Add.$h.Enabled)" = true ] || { PLAN+=("config   Kernel > Add, item $h ($HPM): Enabled false -> true"); op set Kernel.Add.$h.Enabled bool true; }
        cur=$(pget "$CFG" Kernel.Add.$h.ExecutablePath)
        [ "$cur" = Contents/MacOS/HybridPM ] || { PLAN+=("config   Kernel > Add, item $h ($HPM): ExecutablePath '$cur' -> 'Contents/MacOS/HybridPM'"); op set Kernel.Add.$h.ExecutablePath string Contents/MacOS/HybridPM; }
        cur=$(pget "$CFG" Kernel.Add.$h.PlistPath)
        [ "$cur" = Contents/Info.plist ] || { PLAN+=("config   Kernel > Add, item $h ($HPM): PlistPath '$cur' -> 'Contents/Info.plist'"); op set Kernel.Add.$h.PlistPath string Contents/Info.plist; }
        [ "$h" -gt "$l" ] || NOTES+=("$HPM is item $h of Kernel > Add, above $LILU (item $l): OpenCore loads it before Lilu and it does not start. Move its entry below $LILU's")
    fi
    return 0
}

# csr-active-config as a number (little-endian data)
csr_value() { local b; b=$(printf '%s' "$1" | base64 -D 2>/dev/null | od -An -tu1 | tr -s ' \n' ' '); set -- $b; echo $(( ${1:-0} | ${2:-0} << 8 | ${3:-0} << 16 | ${4:-0} << 24 )); }
csr_data() { printf "\\x$(printf %02x $(($1 & 255)))\\x$(printf %02x $(($1 >> 8 & 255)))\\x$(printf %02x $(($1 >> 16 & 255)))\\x$(printf %02x $(($1 >> 24 & 255)))" | base64; }

plan_settings() {
    local cur v args add=() del re=' (amfi_get_out_of_my_way=(1|0x1)|amfi=(0x80|128)) '
    cur=$(pget "$CFG" Booter.Quirks.ResizeAppleGpuBars)
    [ "$cur" = 10 ] || { PLAN+=("config   Booter > Quirks > ResizeAppleGpuBars: ${cur:-missing} -> 10 (a 1 GB window on the B580's VRAM)"); op set Booter.Quirks.ResizeAppleGpuBars integer 10; }
    cur=$(pget "$CFG" Kernel.Quirks.DisableIoMapper)
    [ "$cur" = true ] || { PLAN+=("config   Kernel > Quirks > DisableIoMapper: ${cur:-missing} -> true"); op set Kernel.Quirks.DisableIoMapper bool true; }
    cur=$(pget "$CFG" UEFI.Output.ReconnectGraphicsOnConnect)
    [ "$cur" = true ] && { PLAN+=("config   UEFI > Output > ReconnectGraphicsOnConnect: true -> false (with true, OpenCore's RELEASE build boots without a console and watchdogd panics)"); op set UEFI.Output.ReconnectGraphicsOnConnect bool false; }
    [ -n "$(pget "$CFG" NVRAM.Add)" ] || { NOTES+=("config.plist has no NVRAM > Add: not changed"); return 0; }
    if [ -z "$(pget "$CFG" NVRAM.Add.$APPLE)" ] && ! plutil -extract NVRAM.Add.$APPLE xml1 -o /dev/null "$CFG" 2>/dev/null; then
        PLAN+=("config   NVRAM > Add > $APPLE: add (missing)"); op mkdict NVRAM.Add.$APPLE
    fi
    cur=$(pget "$CFG" NVRAM.Add.$APPLE.csr-active-config); v=$(csr_value "$cur")
    if [ $((v & 0x803)) != $((0x803)) ]; then
        PLAN+=("config   NVRAM > Add > $APPLE > csr-active-config: $(printf '0x%x' "$v") -> $(printf '0x%x' $((v | 0x803))) (SIP lets unsigned kexts load from the Auxiliary Kernel Collection)")
        op set NVRAM.Add.$APPLE.csr-active-config data "$(csr_data $((v | 0x803)))"
    fi
    args=$(pget "$CFG" NVRAM.Add.$APPLE.boot-args)
    [[ " $args " =~ $re ]] || add+=(amfi_get_out_of_my_way=0x1)
    [[ " $args " == *" intelxe_surfvram="* ]] || add+=(intelxe_surfvram=2)
    if [ ${#add[@]} -gt 0 ]; then
        v=$(printf '%s %s' "$args" "${add[*]}" | sed -E 's/^ +//')
        PLAN+=("config   NVRAM > Add > $APPLE > boot-args: add ${add[*]} ('$args' -> '$v')")
        op set NVRAM.Add.$APPLE.boot-args string "$v"
    fi
    if ! plutil -extract NVRAM.Delete xml1 -o /dev/null "$CFG" 2>/dev/null; then   # plutil makes no missing parent
        PLAN+=("config   NVRAM > Delete: add (missing)"); op mkdict NVRAM.Delete
    fi
    if ! plutil -extract NVRAM.Delete.$APPLE xml1 -o /dev/null "$CFG" 2>/dev/null; then
        PLAN+=("config   NVRAM > Delete > $APPLE: add (missing)"); op mkarray NVRAM.Delete.$APPLE; del=""
    else del=$(plutil -extract NVRAM.Delete.$APPLE xml1 -o - "$CFG" 2>/dev/null); fi
    for v in csr-active-config boot-args; do
        [[ "$del" == *"<string>$v</string>"* ]] || { PLAN+=("config   NVRAM > Delete > $APPLE: add $v (so that the new value replaces the old one at boot)"); op append NVRAM.Delete.$APPLE string $v; }
    done
    return 0
}

# The system-volume copy (system-volume/ssv-install.sh) boots from a snapshot whose seal no longer matches Apple's:
# Apple Secure Boot (any SecureBootModel but Disabled) refuses that, and the Mac does not boot. Kept apart from stage 2:
# only those who install the system-volume copy need it, and it turns a macOS security feature off.
plan_safari() {
    local cur
    if ! plutil -extract Misc.Security xml1 -o /dev/null "$CFG" 2>/dev/null; then NOTES+=("config.plist has no Misc > Security: not changed"); return 0; fi
    cur=$(pget "$CFG" Misc.Security.SecureBootModel)
    [ "$cur" = Disabled ] || { PLAN+=("config   Misc > Security > SecureBootModel: ${cur:-missing} -> Disabled (Apple Secure Boot refuses a system volume with a changed seal: needed for the system-volume copy, Safari on the GPU)"); op set Misc.Security.SecureBootModel string Disabled; }
    return 0
}

# ---- backups --------------------------------------------------------------------------------------------------------
BKD=$STATE/efi-backups/$KEY
last_backup() { # the newest backup not yet put back
    [ -n "$KEY" ] && [ -d "$BKD" ] || return 0
    local b; for b in $(ls -1 "$BKD" 2>/dev/null | grep -E '^[0-9]{3}-' | LC_ALL=C sort -r); do
        [ -e "$BKD/$b/restored" ] || { echo "$BKD/$b"; return 0; }
    done
    return 0
}
plan_undo() {
    local B rel; B=$(last_backup)
    [ -n "$B" ] || { NOTES+=("no backup to put back for this EFI"); return 0; }
    PLAN+=("from     $B ($(cat "$B/kind" 2>/dev/null || echo change), $(cat "$B/date" 2>/dev/null))")
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        if [ -e "$B/EFI/OC/$rel" ]; then PLAN+=("restore  EFI/OC/$rel"); else PLAN+=("remove   EFI/OC/$rel (it was not there before)"); fi
    done < <(changed_list "$B")
    if [ -f "$B/config-after.sha256" ] && [ "$(shasum -a 256 < "$CFG" | cut -d' ' -f1)" != "$(cat "$B/config-after.sha256")" ]; then
        NOTES+=("config.plist was changed after that change: putting it back loses those later edits")
    fi
    return 0
}

changed_list() { # the files a change touched, from its backup ("changed"; none when the change stopped before the first)
    cat "$1/changed" 2>/dev/null || true
}

print_plan() {
    echo "EFI: $(describe)"
    local l
    if [ ${#PLAN[@]} = 0 ]; then echo "nothing to change"; else for l in "${PLAN[@]}"; do echo "$l"; done; fi
    for l in ${NOTES[@]+"${NOTES[@]}"}; do echo "note     $l"; done
}

# ---- doing it -------------------------------------------------------------------------------------------------------
need_root() {
    [ "$SOURCE" = folder ] || [ "$(id -u)" = 0 ] || { echo "$CMD needs root: sudo $0 $CMD"; exit 1; }
}
need_efi() {
    [ "$SOURCE" != none ] || { echo "OpenCore did not record which EFI it booted from; name it: --part UUID (see --status, candidate=)"; exit 1; }
    [ "$SOURCE" = folder ] || [ -n "$DEV" ] || { echo "partition $PART not found"; exit 1; }
    [ "$SOURCE" = folder ] || [ "$CONTENT" = EFI ] || { echo "$DEV is not an EFI partition ($CONTENT)"; exit 1; }
    [ -n "$ROOTDIR" ] || { echo "the EFI partition is not mounted: sudo $0 --mount${PART:+ --part $PART}"; exit 1; }
    [ "$OPENCORE" = yes ] || { echo "no EFI/OC/OpenCore.efi and EFI/OC/config.plist in $ROOTDIR: not an OpenCore EFI"; exit 1; }
    [ "$READABLE" = yes ] || { echo "cannot read $CFG"; exit 1; }
}
check_expect() {
    local h; h=$(print_plan | shasum -a 256 | cut -d' ' -f1)
    [ -z "$EXPECT" ] || [ "$EXPECT" = "$h" ] || { echo "the EFI changed since the plan was shown (plan $h, expected $EXPECT): nothing changed"; exit 1; }
}
backup() { # backup KIND: the whole EFI folder, and the list of what this change touches
    mkdir -p "$BKD"
    local n; n=$(ls -1 "$BKD" | grep -cE '^[0-9]{3}-' || true)
    B=$BKD/$(printf %03d $((n + 1)))-$(date +%Y%m%d-%H%M%S)
    mkdir -p "$B"
    ditto --norsrc --noextattr --noacl "$ROOTDIR/EFI" "$B/EFI"
    echo "$1" > "$B/kind"; date '+%Y-%m-%d %H:%M:%S' > "$B/date"; describe > "$B/where"; : > "$B/changed"
    echo "== backup of the whole EFI folder: $B"
}
put_copy() { # put_copy SRC DST: DST replaced by a copy of SRC, without resource forks or extended attributes (FAT)
    rm -rf "$2.new"; ditto --norsrc --noextattr --noacl "$1" "$2.new"; rm -rf "$2"; mv "$2.new" "$2"
}
run_ops() {
    local o a b c d edited=no
    cp "$CFG" "$T/config.plist"
    for o in "${OPS[@]}"; do
        IFS=$TAB read -r a b c d <<< "$o"
        case $a in
            copy) echo "$c" >> "$B/changed"   # before the copy: an interrupted copy is undone too
                  mkdir -p "$(dirname "$E/$c")"; put_copy "$b" "$E/$c"; echo "  ok    EFI/OC/$c";;
            set) plutil -replace "$b" -"$c" "$d" "$T/config.plist"; edited=yes;;
            append) plutil -insert "$b" -"$c" "$d" -append "$T/config.plist"; edited=yes;;
            insert) plutil -insert "$b" -"$c" "$d" "$T/config.plist"; edited=yes;;
            mkdict) plutil -insert "$b" -dictionary "$T/config.plist"; edited=yes;;
            mkarray) plutil -insert "$b" -array "$T/config.plist"; edited=yes;;
        esac
    done
    if [ $edited = yes ]; then
        plutil -convert xml1 "$T/config.plist"   # OpenCore reads XML only
        plutil -lint -s "$T/config.plist" || { echo "  FAIL  the edited config.plist does not parse; the EFI's is unchanged"; exit 1; }
        echo config.plist >> "$B/changed"
        cp "$T/config.plist" "$CFG.new"; mv -f "$CFG.new" "$CFG"
        echo "  ok    EFI/OC/config.plist"
        shasum -a 256 < "$CFG" | cut -d' ' -f1 > "$B/config-after.sha256"
    fi
    sync
}
verify_package() { # the EFI files of the package against its SHA256SUMS, when it has one
    [ -f "$PKG/SHA256SUMS" ] || return 0
    (cd "$PKG" && grep -E '  EFI/OC/(Kexts/(IntelXe|HybridPM)\.kext/|ACPI/SSDT-ARCOFF\.aml$)' SHA256SUMS | shasum -a 256 -c -s) \
        || { echo "the package's EFI files differ from its SHA256SUMS (damaged or changed): nothing changed"; exit 1; }
}

status() {
    local i
    echo "efi_source=$SOURCE"
    echo "efi_partition=$PART"
    echo "efi_device=$DEV"
    echo "efi_disk=$MODEL"
    echo "efi_disk_size=$SIZE"
    echo "efi_disk_location=$WHERE"
    echo "efi_content=$CONTENT"
    [ -n "$ROOTDIR" ] && echo "efi_mounted=yes" || echo "efi_mounted=no"
    echo "efi_mount_point=$ROOTDIR"
    echo "efi_readable=$READABLE"
    echo "efi_description=$(describe)"
    echo "opencore=$OPENCORE"
    echo "boot_path_recorded=$BOOTREC"
    echo "kext_file=$(kext_state)"
    echo "kext_entry=$(entry_state Kernel.Add BundlePath "$KEXT")"
    echo "kext_version_package=$(kext_version "$PK")"
    [ "$READABLE" = yes ] && [ -e "$E/Kexts/$KEXT" ] && echo "kext_version_efi=$(kext_version "$E/Kexts/$KEXT")" || echo "kext_version_efi="
    echo "old_kext_entry=$(entry_state Kernel.Add BundlePath "$OLDKEXT")"
    echo "ssdt_file=$(ssdt_state)"
    echo "ssdt_entry=$(entry_state ACPI.Add Path "$SSDT")"
    echo "card_acpi_path=$CARD"
    echo "hybridpm_needed=$HPMNEED"
    echo "hybridpm_cpu=$BRAND"
    echo "hybridpm_package=$HPMPKG"
    [ $HPMPKG = yes ] && echo "hybridpm_version_package=$(kext_version "$HPK")" || echo "hybridpm_version_package="
    echo "hybridpm_file=$(hpm_state)"
    echo "hybridpm_entry=$(entry_state Kernel.Add BundlePath "$HPM")"
    [ "$READABLE" = yes ] && [ -e "$E/Kexts/$HPM" ] && echo "hybridpm_version_efi=$(kext_version "$E/Kexts/$HPM")" || echo "hybridpm_version_efi="
    echo "lilu_entry=$(entry_state Kernel.Add BundlePath "$LILU")"
    case $ADAPT in yes) echo "ssdt_package_names_card=yes";; adapted) echo "ssdt_package_names_card=adaptable";; *) echo "ssdt_package_names_card=no";; esac
    if [ "$READABLE" = yes ]; then
        PLAN=() OPS=() NOTES=(); plan_files; echo "plan_changes=${#PLAN[@]}"
        PLAN=() OPS=() NOTES=(); plan_settings; echo "settings_changes=${#PLAN[@]}"
        echo "secure_boot_model=$(pget "$CFG" Misc.Security.SecureBootModel)"
    else echo "plan_changes="; echo "settings_changes="; echo "secure_boot_model="; fi
    i=$(last_backup); echo "backup=${i:-none}"; echo "backup_kind=$( [ -n "$i" ] && cat "$i/kind" 2>/dev/null || true)"
    candidates
}
candidates() { # every EFI partition, "candidate=UUID|device|disk|size|mount point|OpenCore"
    [ "$SOURCE" = folder ] && return 0
    diskutil list -plist > "$T/list.plist" 2>/dev/null || return 0
    local d p n m uuid dev mp disk model oc
    n=$(count "$T/list.plist" AllDisksAndPartitions)
    for ((d = 0; d < n; d++)); do
        m=$(count "$T/list.plist" AllDisksAndPartitions.$d.Partitions)
        for ((p = 0; p < m; p++)); do
            [ "$(pget "$T/list.plist" AllDisksAndPartitions.$d.Partitions.$p.Content)" = EFI ] || continue
            uuid=$(pget "$T/list.plist" AllDisksAndPartitions.$d.Partitions.$p.DiskUUID)
            dev=$(pget "$T/list.plist" AllDisksAndPartitions.$d.Partitions.$p.DeviceIdentifier)
            mp=$(pget "$T/list.plist" AllDisksAndPartitions.$d.Partitions.$p.MountPoint)
            disk=$(pget "$T/list.plist" AllDisksAndPartitions.$d.DeviceIdentifier)
            diskutil info -plist "$disk" > "$T/c.plist" 2>/dev/null || true
            model=$(pget "$T/c.plist" MediaName); [ -n "$model" ] || model=$(pget "$T/c.plist" IORegistryEntryName)
            oc=unknown; [ -n "$mp" ] && { [ -f "$mp/EFI/OC/OpenCore.efi" ] && oc=yes || oc=no; }
            echo "candidate=$uuid|$dev|$model|$(gb "$(pget "$T/c.plist" Size)")|$mp|$oc"
        done
    done
}

case $CMD in
    --status) status;;
    --plan|--settings-plan|--safari-plan|--settings-safari-plan|--undo-plan)
        need_efi
        case $CMD in --plan) plan_files;; --settings-plan) plan_settings;; --safari-plan) plan_safari;;
            --settings-safari-plan) plan_settings; plan_safari;; --undo-plan) plan_undo;; esac
        print_plan;;
    --apply|--settings-apply|--safari-apply|--settings-safari-apply)
        need_root; need_efi
        case $CMD in --apply) plan_files; verify_package;; --settings-apply) plan_settings;; --safari-apply) plan_safari;;
            --settings-safari-apply) plan_settings; plan_safari;; esac
        check_expect
        print_plan
        [ ${#OPS[@]} -gt 0 ] || { echo "== nothing to change"; exit 0; }
        B=""; case $CMD in --apply) backup "IntelXe files";; --settings-apply) backup settings;; --safari-apply) backup "Safari setting";;
            --settings-safari-apply) backup "settings and Safari setting";; esac
        run_ops
        echo "== done. Restart to use it. To go back: sudo $0 --undo${PART:+ --part $PART}"
        ;;
    --undo)
        need_root; need_efi
        plan_undo; check_expect; print_plan
        B=$(last_backup); [ -n "$B" ] || exit 1
        while IFS= read -r rel; do
            [ -n "$rel" ] || continue
            if [ -e "$B/EFI/OC/$rel" ]; then put_copy "$B/EFI/OC/$rel" "$E/$rel"; echo "  ok    restored EFI/OC/$rel"
            else rm -rf "${E:?}/$rel"; echo "  ok    removed EFI/OC/$rel"; fi
        done < <(changed_list "$B")
        date '+%Y-%m-%d %H:%M:%S' > "$B/restored"
        sync
        echo "== done. The backup stays in $B."
        ;;
    --mount)
        need_root
        [ "$SOURCE" != folder ] || { echo "--mount is for a partition, not --efi"; exit 1; }
        [ "$SOURCE" != none ] || { echo "OpenCore did not record which EFI it booted from; name it: --part UUID (see --status, candidate=)"; exit 1; }
        [ "$CONTENT" = EFI ] || { echo "partition $PART is not an EFI partition (${CONTENT:-not found})"; exit 1; }
        echo "== mounting $(describe)"
        [ -n "$MP" ] && { echo "  ok    already mounted at $MP"; exit 0; }
        diskutil mount "$PART"
        ;;
esac
