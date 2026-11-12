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
#   ./efi.sh --bootlog-plan         what --bootlog-apply would change (opt-in; it slows every boot, turn it off once the
#                                   logs are collected): OpenCore logs to a file on the EFI, with the macOS boot loader's log,
#                                   and saves kernel panics there
#   sudo ./efi.sh --bootlog-apply   back up the whole EFI folder, then make that change
#   ./efi.sh --bootlog-off-plan / sudo ./efi.sh --bootlog-off-apply   boot logging off again (the log file, AppleDebug,
#                                   ApplePanic), whatever was changed after it was turned on; --status: boot_logging=
#   ./efi.sh --undo-plan            what --undo would put back
#   sudo ./efi.sh --undo            put back, from the last backup, the files the last change replaced
#   sudo ./efi.sh --mount           mount the EFI partition (macOS mounts it read-write; it stays mounted until restart)
#
# Which EFI: the partition in OpenCore's NVRAM variable 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path, which OpenCore
# writes when Misc > Security > ExposeSensitiveData includes 1 (--apply adds it). Without it, name the partition with
# --part UUID, its partition UUID (--status lists every EFI partition as "candidate="). Never a disk number: those change
# between boots. --efi DIR works on a folder holding EFI/OC instead (a copy, or an EFI you mounted yourself).
# --package DIR takes the files from that package (default: this script's folder). --expect SHA256 with --apply, the
# other --*-apply commands or --undo: change nothing unless the plan's SHA-256 is that one, the plan that was shown.
# Backups: /Library/Application Support/intel-gpu-macos-driver/efi-backups/<partition UUID>/<NNN-date>/EFI.
# The rules (entries, files, settings, cards, CPUs, what undo may put back) are in efi-rules.json beside this script, the
# same file IntelXe-EFI.ps1 reads on Windows (docs/design/DESIGN-efi-rules.md); this script finds the EFI and applies them.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
PKG=$(cd "$(dirname "$0")" && pwd)
STATE=${INTELXE_STATE_DIR:-/Library/Application Support/intel-gpu-macos-driver}   # tests point it at a throwaway folder
OCGUID=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
RULES=${INTELXE_EFI_RULES:-$PKG/efi-rules.json}   # beside this script (tests: another file)
CMD="" EFIDIR="" PART="" EXPECT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --status|--plan|--apply|--settings-plan|--settings-apply|--safari-plan|--safari-apply|--settings-safari-plan|--settings-safari-apply|--bootlog-plan|--bootlog-apply|--bootlog-off-plan|--bootlog-off-apply|--undo-plan|--undo|--mount) CMD=$1; shift;;
        --package) PKG=$(cd "$2" && pwd); shift 2;;
        --efi) EFIDIR=$(cd "$2" && pwd); shift 2;;
        --part) PART=$(printf '%s' "$2" | tr a-f A-F); shift 2;;
        --expect) EXPECT=$2; shift 2;;
        *) sed -n '3,32p' "$0"; exit 2;;
    esac
done
[ -n "$CMD" ] || { sed -n '3,32p' "$0"; exit 2; }
if [ -n "$PART" ] && ! [[ "$PART" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]]; then
    echo "--part takes a partition UUID (see --status, candidate=), not a disk name"; exit 2
fi
T=$(mktemp -d -t intelxe-efi)
trap 'rm -rf "$T"' EXIT

pget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }   # pget PLIST KEYPATH: the value, or nothing
upper() { tr a-z A-Z; }

# ---- the rules: efi-rules.json, read once: plutil turns the JSON into XML, awk lists every value by its key path ------
# (RK: "kext.entry.Enabled", "settings.3.kind", ...; RT: string, integer, bool, dict or array; RV: the value, or the
# number of items of a dict or an array). Rule strings hold no tab and no line break.
RK=() RT=() RV=()
while IFS=$'\t' read -r k t v; do RK+=("$k"); RT+=("$t"); RV+=("$v"); done < <(
    plutil -convert xml1 -o - "$RULES" 2>/dev/null | awk '
        function ent(s) { gsub(/&lt;/, "<", s); gsub(/&gt;/, ">", s); gsub(/&quot;/, "\"", s); gsub(/&apos;/, "\047", s); gsub(/&amp;/, "\\&", s); return s }
        function here() { return n == 0 ? "" : (ty[n] == "dict" ? (pa[n] == "" ? key : pa[n] "." key) : (pa[n] == "" ? "" : pa[n] ".") cnt[n]++) }
        function val(t, v,  p) { if (n > 0 && ty[n] == "dict") cnt[n]++; p = here(); print p "\t" t "\t" v }
        { sub(/^[ \t]+/, "") }
        /^<key>/ { key = ent(substr($0, 6, length($0) - 11)); next }
        /^<(dict|array)>$/ { t = substr($0, 2, length($0) - 2); p = (n == 0 ? "" : here()); if (n > 0 && ty[n] == "dict") cnt[n]++; n++; ty[n] = t; pa[n] = p; cnt[n] = 0; next }
        /^<(dict|array)\/>$/ { val(substr($0, 2, length($0) - 3), 0); next }
        /^<\/(dict|array)>$/ { if (n > 1) print pa[n] "\t" ty[n] "\t" cnt[n]; n--; next }
        /^<string>.*<\/string>$/ { val("string", ent(substr($0, 9, length($0) - 17))); next }
        /^<string\/>$/ { val("string", ""); next }
        /^<integer>/ { val("integer", substr($0, 10, length($0) - 19)); next }
        /^<true\/>$/ { val("bool", "true"); next }
        /^<false\/>$/ { val("bool", "false"); next }
        /^<(real|date|data)>/ { val("other", ""); next }')
[ ${#RK[@]} -gt 0 ] || { echo "cannot read $RULES: efi.sh needs the efi-rules.json it came with"; exit 1; }
rule_at() { # rule_at KEYPATH: RI set to its index in RK; fails when the rules have no such value
    for ((RI = 0; RI < ${#RK[@]}; RI++)); do [ "${RK[$RI]}" != "$1" ] || return 0; done; return 1
}
rule() { rule_at "$1" && printf '%s\n' "${RV[$RI]}"; }        # rule KEYPATH: a value of the rules; fails without one
rget() { rule_at "$2" && printf -v "$1" '%s' "${RV[$RI]}"; }   # rget VAR KEYPATH: the same into VAR, without a subshell
rule_type() { rule_at "$1" && printf '%s\n' "${RT[$RI]}"; }   # bool, integer, string, dict or array
rule_json() { # rule_json KEYPATH: a dictionary of strings, booleans and integers, as JSON (for plutil -insert -json)
    local i k v out="" sep=""
    for ((i = 0; i < ${#RK[@]}; i++)); do
        k=${RK[$i]#"$1."}; [ "$k" != "${RK[$i]}" ] && [ "${k#*.}" = "$k" ] || continue
        v=${RV[$i]}
        case ${RT[$i]} in
            string) v=${v//\\/\\\\}; v=\"${v//\"/\\\"}\";;
            bool|integer) ;;
            *) echo "$RULES: $1.$k is a ${RT[$i]}: an entry holds strings, booleans and integers" >&2; return 1;;
        esac
        out="$out$sep\"$k\":$v"; sep=,
    done
    printf '{%s}\n' "$out"
}
need() { # need VAR KEYPATH: VAR set to that value of the rules; stops when they have none
    rule_at "$2" || { echo "$RULES has no $2"; exit 1; }
    printf -v "$1" '%s' "${RV[$RI]}"
}
need_list() { # need_list ARRAY KEYPATH: ARRAY set to the strings of that list of the rules
    local n i
    rule_at "$2" && [ "${RT[$RI]}" = array ] || { echo "$RULES has no list $2"; exit 1; }
    n=${RV[$RI]}; eval "$1=()"
    for ((i = 0; i < n; i++)); do rule_at "$2.$i" || { echo "$RULES has no $2.$i"; exit 1; }; eval "$1+=(\"\${RV[$RI]}\")"; done
}
need RFORMAT format; need RVERSION format_version
[ "$RFORMAT" = intelxe-efi-rules ] && [ "$RVERSION" = 1 ] || { echo "$RULES is $RFORMAT $RVERSION; this efi.sh reads intelxe-efi-rules 1"; exit 1; }
need KEXT kext.bundle; need KEXTDIR kext.folder; need KEXE kext.executable; need KINFO kext.info_plist
need_list OLDKEXTS kext.earlier_bundles
need SSDT ssdt.file; need SSDTDIR ssdt.folder; need PKGPATH ssdt.shipped_port; need PORTRE ssdt.port_pattern
need HPM hybridpm.bundle; need HPMDIR hybridpm.folder; need HEXE hybridpm.executable; need HINFO hybridpm.info_plist
need LILU hybridpm.after; need_list HPMOLD hybridpm.earlier_builds
need_list CPUNO hybridpm.cpu_exclude; need_list CPUYES hybridpm.cpu_include
need_list CARD_IDS cards.device_ids; need LEVELS cards.port_levels_above_gpu
need BPPATH boot_path.path; need BPBITS boot_path.bits
need_list UNDOPATHS undo_paths

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
PK=$PKG/EFI/OC/$KEXTDIR/$KEXT PS=$PKG/EFI/OC/$SSDTDIR/$SSDT
[ -f "$PK/$KEXE" ] && [ -f "$PS" ] || { echo "no EFI/OC/$KEXTDIR/$KEXT and EFI/OC/$SSDTDIR/$SSDT in $PKG"; exit 1; }
kext_state() { # same|different|absent|unknown: the executable and Info.plist compared byte for byte
    [ "$READABLE" = yes ] || { echo unknown; return; }
    local k=$E/$KEXTDIR/$KEXT
    if [ ! -e "$k" ]; then echo absent
    elif cmp -s "$k/$KEXE" "$PK/$KEXE" && cmp -s "$k/$KINFO" "$PK/$KINFO"; then echo same
    else echo different; fi
}
ssdt_state() { # against the table this card needs: the package's, or the package's adapted to the card's port (SSDTSRC)
    [ "$READABLE" = yes ] || { echo unknown; return; }
    if [ ! -e "$E/$SSDTDIR/$SSDT" ]; then echo absent; elif cmp -s "$E/$SSDTDIR/$SSDT" "$SSDTSRC"; then echo same; else echo different; fi
}
kext_version() { pget "$1/Contents/Info.plist" CFBundleVersion; }

# ---- HybridPM.kext (kernel/HybridPM): only for Alder Lake / Raptor Lake CPUs with efficiency cores ------------------
# By the CPU's name: machdep.cpu.model is what OpenCore's Cpuid1Data makes it (Comet Lake on the test machine). The
# rules list the names (regular expressions) that have efficiency cores and, of those, the ones that have not: Core
# Ultra (HybridPM leaves it alone) and the desktop models with P-cores only. INTELXE_CPU_BRAND: tests.
HPK=$PKG/EFI/OC/$HPMDIR/$HPM
BRAND=${INTELXE_CPU_BRAND-$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)}
HPMNEED=no
for re in ${CPUYES[@]+"${CPUYES[@]}"}; do if [[ "$BRAND" =~ $re ]]; then HPMNEED=yes; fi; done
for re in ${CPUNO[@]+"${CPUNO[@]}"}; do if [[ "$BRAND" =~ $re ]]; then HPMNEED=no; fi; done
HPMPKG=no; [ -f "$HPK/$HEXE" ] && [ -f "$HPK/$HINFO" ] && HPMPKG=yes
hpm_state() { # same|different|absent|unknown, as kext_state; "different" also for an earlier build (org.hackintosh.HybridPM)
    [ "$READABLE" = yes ] || { echo unknown; return; }
    local k=$E/$HPMDIR/$HPM
    if [ ! -e "$k" ]; then echo absent
    elif [ $HPMPKG = yes ] && cmp -s "$k/$HEXE" "$HPK/$HEXE" && cmp -s "$k/$HINFO" "$HPK/$HINFO"; then echo same
    else echo different; fi
}

# the cards efi.sh looks for: PCI device ids (vendor 8086), cards.device_ids of the rules, the same list as
# data/platforms.json "supported" (tools/parity/devices.py checks it). The ACPI path of the B580's upstream port (the
# bridge cards.port_levels_above_gpu PCI levels above the GPU, two), e.g. \_SB.PC00.PEG1.PEGP; "none" when that port
# has no ACPI name, nothing when there is no B580
card_acpi_path() {
    if [ -n "${INTELXE_CARD_ACPI_PATH+x}" ]; then printf '%s\n' "$INTELXE_CARD_ACPI_PATH"; return 0; fi
    { if [ -n "${INTELXE_IOREG_FILE:-}" ]; then cat "$INTELXE_IOREG_FILE"; else ioreg -p IOService -w0 -l 2>/dev/null || true; fi; } | awk -v ids="${CARD_IDS[*]}" -v levels="$LEVELS" '
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
            for (k = n - 1; k > 0; k--) if (pci[k] && ++up == levels) break
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
hexof() { od -An -v -tx1 "$@" | tr -s ' \n' '  ' | sed 's/ $//'; }   # a file (or stdin) as " hh" bytes
namepath_hex() { # the NamePath of an absolute ACPI path, " hh" bytes; fails for anything that is not one
    local re=$PORTRE s n=0 out=""   # ssdt.port_pattern: \NAME(.NAME)*, each 1-4 of A-Z 0-9 _
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
PLAN=() OPS=() NOTES=() UNMET=""
TAB=$'\t'
op() { local IFS=$TAB; OPS+=("$*"); }
show() { printf '%s' "${1//./ > }"; }   # a key path as the plan shows it: Kernel > Add
exists() { plutil -extract "$1" xml1 -o /dev/null "$CFG" 2>/dev/null; }   # is there a value at KEYPATH in config.plist?

# what a new entry sets, for the plan: (ExecutablePath ..., PlistPath ..., Enabled)
entry_summary() { local e p; rget e "$1.entry.ExecutablePath"; rget p "$1.entry.PlistPath"; echo "ExecutablePath $e, PlistPath $p, Enabled"; }
# plan_fields RULE ARRAY INDEX NAME: item INDEX of ARRAY brought to the rule's entry in each of the rule's "fields"
plan_fields() {
    local fl f v cur
    need_list fl "$1.fields"
    for f in ${fl[@]+"${fl[@]}"}; do
        rget v "$1.entry.$f" || { echo "$RULES has no $1.entry.$f"; exit 1; }
        cur=$(pget "$CFG" "$2.$3.$f")
        [ "$cur" != "$v" ] || continue
        if [ "${RT[$RI]}" = bool ]; then
            PLAN+=("config   $(show "$2"), item $3 ($4): $f $([ "$v" = true ] && echo false || echo true) -> $v"); op set "$2.$3.$f" bool "$v"
        else
            PLAN+=("config   $(show "$2"), item $3 ($4): $f '$cur' -> '$v'"); op set "$2.$3.$f" string "$v"
        fi
    done
}

plan_files() {
    local i ks ss efiok cur old
    [ -f "$PKG/SHA256SUMS" ] || NOTES+=("the package has no SHA256SUMS: its files are not checked")
    ks=$(kext_state)
    case $ks in
        absent) PLAN+=("copy     EFI/OC/$KEXTDIR/$KEXT (version $(kext_version "$PK"))"); op copy "$PK" "$KEXTDIR/$KEXT";;
        different) PLAN+=("replace  EFI/OC/$KEXTDIR/$KEXT (version $(kext_version "$E/$KEXTDIR/$KEXT") in the EFI, $(kext_version "$PK") in the package; the files differ)")
                   op copy "$PK" "$KEXTDIR/$KEXT";;
    esac
    i=$(find_entry "$CFG" Kernel.Add BundlePath "$KEXT")
    if [ -z "$i" ]; then
        PLAN+=("config   Kernel > Add: add $KEXT ($(entry_summary kext))")
        op append Kernel.Add json "$(rule_json kext.entry)"
    else
        plan_fields kext Kernel.Add "$i" "$KEXT"
    fi
    for old in ${OLDKEXTS[@]+"${OLDKEXTS[@]}"}; do
        i=$(find_entry "$CFG" Kernel.Add BundlePath "$old")
        if [ -n "$i" ] && [ "$(pget "$CFG" Kernel.Add.$i.Enabled)" = true ]; then
            PLAN+=("config   Kernel > Add, item $i ($old, the earlier name of $KEXT): Enabled true -> false (only one of the two may load)")
            op set Kernel.Add.$i.Enabled bool false
        fi
    done

    # SSDT-ARCOFF only when a table names the card's upstream port: the shipped one names ssdt.shipped_port; for
    # another port, the shipped one adapted to it (adapt_ssdt)
    ss=$(ssdt_state)
    efiok=no; [ "$ss" = absent ] || ! aml_names_card "$E/$SSDTDIR/$SSDT" || efiok=yes
    if [ -z "$CARD" ]; then
        NOTES+=("not added: $SSDT. No Arc B580 found on this machine, so its ACPI path cannot be checked (stage 4, step 2 of README-INSTALL.md)")
    elif [ "$CARD" = none ]; then
        NOTES+=("not added: $SSDT. The B580's upstream port has no ACPI name on this board; the table needs one more bridge level (stage 4, step 2 of README-INSTALL.md; please open an issue)")
    elif [ $efiok = no ] && [ $ADAPT = no ]; then
        NOTES+=("not added: $SSDT. The package's table is for $PKGPATH; the card's port here is $CARD, and $ADAPTWHY. Put that path into SSDT-ARCOFF.dsl, compile it with iasl and copy it to EFI/OC/$SSDTDIR (stage 4, step 2 of README-INSTALL.md); then this adds its entry")
    else
        if [ $efiok = yes ] && [ "$ss" = different ]; then
            if [ $ADAPT = adapted ]; then cur="the package's adapted to this card's port"; else cur="the package's"; fi
            NOTES+=("kept: EFI/OC/$SSDTDIR/$SSDT differs from $cur but names this card's port ($CARD): yours stays")
        elif [ "$ss" != same ]; then
            if [ "$ss" = absent ]; then cur=copy; else cur=replace; fi
            if [ $ADAPT = adapted ]; then
                PLAN+=("$(printf '%-8s' $cur) EFI/OC/$SSDTDIR/$SSDT (the package's table adapted to this card's port: names $CARD instead of $PKGPATH)")
            else
                PLAN+=("$(printf '%-8s' $cur) EFI/OC/$SSDTDIR/$SSDT (names $CARD, this card's port)")
            fi
            op copy "$SSDTSRC" "$SSDTDIR/$SSDT"
        fi
        i=$(find_entry "$CFG" ACPI.Add Path "$SSDT")
        if [ -z "$i" ]; then
            PLAN+=("config   ACPI > Add: add $SSDT (Enabled)")
            op append ACPI.Add json "$(rule_json ssdt.entry)"
        else
            plan_fields ssdt ACPI.Add "$i" "$SSDT"
        fi
    fi

    plan_hybridpm   # after every change above that names an item by its index: it may insert one

    # so that OpenCore records which EFI it booted from (boot_path of the rules), and the next run finds it untold
    cur=$(pget "$CFG" "$BPPATH")
    if [ "$BOOTREC" = no ] && [[ "$cur" =~ ^[0-9]+$ ]] && [ $((cur & BPBITS)) != $((BPBITS)) ]; then
        PLAN+=("config   $(show "$BPPATH"): $cur -> $((cur | BPBITS)) (OpenCore then records which EFI it booted from, for this script)")
        op set "$BPPATH" integer $((cur | BPBITS))
    fi
    return 0
}

# HybridPM.kext with an entry right below Lilu.kext's (hybridpm.after: a Lilu plugin loads after Lilu), only where it is
# needed and Lilu is loaded; an earlier build under the same name (hybridpm.earlier_builds) is replaced
plan_hybridpm() {
    [ $HPMNEED = yes ] || return 0
    local l h hs id b old=no
    if [ $HPMPKG = no ]; then
        NOTES+=("not added: $HPM. This CPU ($BRAND) has efficiency cores and macOS holds it below its clocks, but this package has no $HPM (it comes with 0.1.4 and later)")
        return 0
    fi
    l=$(find_entry "$CFG" Kernel.Add BundlePath "$LILU")
    if [ -z "$l" ] || [ "$(pget "$CFG" Kernel.Add.$l.Enabled)" != true ] || [ ! -e "$E/$HPMDIR/$LILU" ]; then
        NOTES+=("not added: $HPM. This CPU ($BRAND) has efficiency cores and $HPM corrects its clocks, but it is a Lilu plugin and this EFI does not load $LILU (no file in EFI/OC/$HPMDIR, or its Kernel > Add entry is missing or off). Add Lilu (github.com/acidanthera/Lilu), then run this again (README-INSTALL.md, Hybrid Intel CPUs)")
        return 0
    fi
    hs=$(hpm_state)
    case $hs in
        absent) PLAN+=("copy     EFI/OC/$HPMDIR/$HPM (version $(kext_version "$HPK"); this CPU has efficiency cores: it corrects their clocks)"); op copy "$HPK" "$HPMDIR/$HPM";;
        different)
            id=$(pget "$E/$HPMDIR/$HPM/$HINFO" CFBundleIdentifier)
            for b in ${HPMOLD[@]+"${HPMOLD[@]}"}; do [ "$id" != "$b" ] || old=yes; done
            if [ $old = yes ]; then
                PLAN+=("replace  EFI/OC/$HPMDIR/$HPM (an earlier build, $id $(kext_version "$E/$HPMDIR/$HPM"), by the package's $(kext_version "$HPK"))")
            else
                PLAN+=("replace  EFI/OC/$HPMDIR/$HPM (version $(kext_version "$E/$HPMDIR/$HPM") in the EFI, $(kext_version "$HPK") in the package; the files differ)")
            fi
            op copy "$HPK" "$HPMDIR/$HPM";;
    esac
    h=$(find_entry "$CFG" Kernel.Add BundlePath "$HPM")
    if [ -z "$h" ]; then
        PLAN+=("config   Kernel > Add: add $HPM below $LILU, as item $((l + 1)) ($(entry_summary hybridpm))")
        op insert Kernel.Add.$((l + 1)) json "$(rule_json hybridpm.entry)"
    else
        plan_fields hybridpm Kernel.Add "$h" "$HPM"
        [ "$h" -gt "$l" ] || NOTES+=("$HPM is item $h of Kernel > Add, above $LILU (item $l): OpenCore loads it before Lilu and it does not start. Move its entry below $LILU's")
    fi
    return 0
}

# csr-active-config as a number (little-endian data)
csr_value() { local b; b=$(printf '%s' "$1" | base64 -D 2>/dev/null | od -An -tu1 | tr -s ' \n' ' '); set -- $b; echo $(( ${1:-0} | ${2:-0} << 8 | ${3:-0} << 16 | ${4:-0} << 24 )); }
csr_data() { printf "\\x$(printf %02x $(($1 & 255)))\\x$(printf %02x $(($1 >> 8 & 255)))\\x$(printf %02x $(($1 >> 16 & 255)))\\x$(printf %02x $(($1 >> 24 & 255)))" | base64; }

# plan_items LIST: the items of the rules' "settings" (stage 2: SIP, AMFI and surfvram boot-args, quirks), "safari"
# (SecureBootModel for the system-volume copy, kept apart from stage 2: only those who install that copy need it, and
# it turns a macOS security feature off), "bootlog" (OpenCore's log to a file) or "bootlog-off" list (that log off again),
# in their order. The kinds are described in docs/design/DESIGN-efi-rules.md. A "require" that is not met sets UNMET.
plan_items() {
    local n i j m k p type v why cur bits del s re if note a
    local -a add
    rget n "$1" && [[ "$n" =~ ^[0-9]+$ ]] || { echo "$RULES has no list $1"; exit 1; }
    for ((i = 0; i < n; i++)); do
        rget k "$1.$i.kind" && rget p "$1.$i.path" || { echo "$RULES: $1 item $i has no kind or path"; exit 1; }
        rget why "$1.$i.why" || why=""
        case $k in
            value|value_if)
                rget type "$1.$i.type" && rget v "$1.$i.value" || { echo "$RULES: $1 item $i has no type or value"; exit 1; }
                cur=$(pget "$CFG" "$p")
                if [ $k = value_if ]; then rget if "$1.$i.if"; [ "$cur" = "$if" ] || continue; else [ "$cur" != "$v" ] || continue; fi
                PLAN+=("config   $(show "$p"): ${cur:-missing} -> $v${why:+ ($why)}"); op set "$p" "$type" "$v";;
            require)
                exists "$p" || { rget note "$1.$i.note"; NOTES+=("$note"); UNMET=$p; return 0; };;
            note)
                rget note "$1.$i.note"; NOTES+=("$note");;
            dict)
                exists "$p" || { PLAN+=("config   $(show "$p"): add (missing)"); op mkdict "$p"; };;
            bits|clear_bits)
                rget type "$1.$i.type" && rget bits "$1.$i.bits" || { echo "$RULES: $1 item $i has no type or bits"; exit 1; }
                bits=$((bits)); cur=$(pget "$CFG" "$p")
                case $type in data) v=$(csr_value "$cur");; integer) [[ "$cur" =~ ^[0-9]+$ ]] && v=$cur || v=0;;
                    *) echo "$RULES: $1 item $i: bits of a $type"; exit 1;; esac
                if [ $k = bits ]; then [ $((v & bits)) != $bits ] || continue; m=$((v | bits))
                else [ $((v & bits)) != 0 ] || continue; m=$((v & ~bits)); fi
                PLAN+=("config   $(show "$p"): $(printf '0x%x' "$v") -> $(printf '0x%x' $m)${why:+ ($why)}")
                if [ $type = data ]; then op set "$p" data "$(csr_data $m)"; else op set "$p" integer $m; fi;;
            boot_args)
                cur=$(pget "$CFG" "$p"); add=()
                rget m "$1.$i.args" || m=0
                for ((j = 0; j < m; j++)); do
                    rget re "$1.$i.args.$j.unless" && rget a "$1.$i.args.$j.add" || { echo "$RULES: $1 item $i, args $j"; exit 1; }
                    [[ " $cur " =~ $re ]] || add+=("$a")
                done
                [ ${#add[@]} -gt 0 ] || continue
                v=$(printf '%s %s' "$cur" "${add[*]}" | sed -E 's/^ +//')
                PLAN+=("config   $(show "$p"): add ${add[*]} ('$cur' -> '$v')"); op set "$p" string "$v";;
            strings)
                if exists "$p"; then del=$(plutil -extract "$p" xml1 -o - "$CFG" 2>/dev/null)
                else PLAN+=("config   $(show "$p"): add (missing)"); op mkarray "$p"; del=""; fi
                rget m "$1.$i.strings" || m=0
                for ((j = 0; j < m; j++)); do
                    rget s "$1.$i.strings.$j"
                    [[ "$del" == *"<string>$s</string>"* ]] || { PLAN+=("config   $(show "$p"): add $s${why:+ ($why)}"); op append "$p" string "$s"; }
                done;;
            *) echo "$RULES: $1 item $i is of the kind '$k', which this efi.sh does not know (a newer rules file?)"; exit 1;;
        esac
    done
    return 0
}
plan_settings() { plan_items settings; }
# The system-volume copy (system-volume/ssv-install.sh) boots from a snapshot whose seal no longer matches Apple's:
# Apple Secure Boot (any SecureBootModel but Disabled) refuses that, and the Mac does not boot.
plan_safari() { plan_items safari; }
# Boot logging (opt-in, "bootlog"): OpenCore's log written to a file in the EFI volume's root, the macOS boot loader's
# log (AppleDebug) in it, a kernel panic saved there at the next boot (ApplePanic), for when macOS does not start. It
# slows every boot; --bootlog-off-apply takes it back.
plan_bootlog() { plan_items bootlog; }
# Boot logging off ("bootlog-off"): the log file, AppleDebug and ApplePanic off, the other Misc > Debug values kept. Its
# own change, not --undo: undo puts back the newest backup's whole config.plist, which is boot logging's only while
# nothing was changed after it.
plan_bootlog_off() { plan_items bootlog-off; }
# boot_logging of --status: on (nothing left for --bootlog-apply), off (nothing for --bootlog-off-apply), partial, or
# missing (no Misc > Debug)
bootlog_state() {
    local on
    PLAN=() OPS=() NOTES=() UNMET=""; plan_bootlog; on=${#PLAN[@]}
    [ -z "$UNMET" ] || { echo missing; return 0; }
    [ "$on" != 0 ] || { echo on; return 0; }
    PLAN=() OPS=() NOTES=(); plan_bootlog_off
    [ ${#PLAN[@]} != 0 ] && echo partial || echo off
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
        if ! undo_path "$rel"; then NOTES+=("the backup lists EFI/OC/$rel, which these rules never change: left as it is")
        elif [ -e "$B/EFI/OC/$rel" ]; then PLAN+=("restore  EFI/OC/$rel"); else PLAN+=("remove   EFI/OC/$rel (it was not there before)"); fi
    done < <(changed_list "$B")
    if [ -f "$B/config-after.sha256" ] && [ "$(shasum -a 256 < "$CFG" | cut -d' ' -f1)" != "$(cat "$B/config-after.sha256")" ]; then
        NOTES+=("config.plist was changed after that change: putting it back loses those later edits")
    fi
    return 0
}

changed_list() { # the files a change touched, from its backup ("changed"; none when the change stopped before the first)
    cat "$1/changed" 2>/dev/null || true
}
undo_path() { # may undo put REL back? Only what the rules change (undo_paths): a list never reaches further
    local u; for u in ${UNDOPATHS[@]+"${UNDOPATHS[@]}"}; do [ "$u" != "$1" ] || return 0; done; return 1
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
verify_package() { # the EFI files of the package against its SHA256SUMS, when it has one: every file of the kexts and
    # the table must be listed (a file added after packaging would be copied unchecked), and every one listed must match
    [ -f "$PKG/SHA256SUMS" ] || return 0
    local f listed bad="" k=EFI/OC/$KEXTDIR/$KEXT h=EFI/OC/$HPMDIR/$HPM a=EFI/OC/$SSDTDIR/$SSDT
    # the lines for those files, as "HASH  PATH"
    listed=$(sed -nE 's/^([0-9a-fA-F]{64}) [ *](\.\/)?/\1  /p' "$PKG/SHA256SUMS" |
        awk -v k="$k/" -v h="$h/" -v a="$a" '{ p = substr($0, 67) } index(p, k) == 1 || index(p, h) == 1 || p == a')
    while IFS= read -r f; do
        cut -c67- <<< "$listed" | grep -qxF -- "$f" || { bad="not listed in SHA256SUMS: $f"; break; }
    done < <(cd "$PKG" && { find "$k" "$h" -type f 2>/dev/null; echo "$a"; } | LC_ALL=C sort)
    [ -n "$bad" ] || (cd "$PKG" && printf '%s\n' "$listed" | shasum -a 256 -c -s) 2>/dev/null || bad=${bad:-a file differs or is missing}
    [ -z "$bad" ] || { echo "the package's EFI files differ from its SHA256SUMS (damaged or changed): nothing changed ($bad)"; exit 1; }
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
    [ "$READABLE" = yes ] && [ -e "$E/$KEXTDIR/$KEXT" ] && echo "kext_version_efi=$(kext_version "$E/$KEXTDIR/$KEXT")" || echo "kext_version_efi="
    if [ ${#OLDKEXTS[@]} -gt 0 ]; then echo "old_kext_entry=$(entry_state Kernel.Add BundlePath "${OLDKEXTS[0]}")"; else echo "old_kext_entry=absent"; fi
    echo "ssdt_file=$(ssdt_state)"
    echo "ssdt_entry=$(entry_state ACPI.Add Path "$SSDT")"
    echo "card_acpi_path=$CARD"
    echo "hybridpm_needed=$HPMNEED"
    echo "hybridpm_cpu=$BRAND"
    echo "hybridpm_package=$HPMPKG"
    [ $HPMPKG = yes ] && echo "hybridpm_version_package=$(kext_version "$HPK")" || echo "hybridpm_version_package="
    echo "hybridpm_file=$(hpm_state)"
    echo "hybridpm_entry=$(entry_state Kernel.Add BundlePath "$HPM")"
    [ "$READABLE" = yes ] && [ -e "$E/$HPMDIR/$HPM" ] && echo "hybridpm_version_efi=$(kext_version "$E/$HPMDIR/$HPM")" || echo "hybridpm_version_efi="
    echo "lilu_entry=$(entry_state Kernel.Add BundlePath "$LILU")"
    case $ADAPT in yes) echo "ssdt_package_names_card=yes";; adapted) echo "ssdt_package_names_card=adaptable";; *) echo "ssdt_package_names_card=no";; esac
    if [ "$READABLE" = yes ]; then
        PLAN=() OPS=() NOTES=(); plan_files; echo "plan_changes=${#PLAN[@]}"
        PLAN=() OPS=() NOTES=(); plan_settings; echo "settings_changes=${#PLAN[@]}"
        echo "secure_boot_model=$(pget "$CFG" Misc.Security.SecureBootModel)"
        echo "boot_logging=$(bootlog_state)"
    else echo "plan_changes="; echo "settings_changes="; echo "secure_boot_model="; echo "boot_logging="; fi
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
    --plan|--settings-plan|--safari-plan|--settings-safari-plan|--bootlog-plan|--bootlog-off-plan|--undo-plan)
        need_efi
        case $CMD in --plan) plan_files;; --settings-plan) plan_settings;; --safari-plan) plan_safari;;
            --settings-safari-plan) plan_settings; plan_safari;; --bootlog-plan) plan_bootlog;;
            --bootlog-off-plan) plan_bootlog_off;; --undo-plan) plan_undo;; esac
        print_plan
        # the steps too, for the shared test vectors (release/test-efi-vectors.sh)
        [ -z "${INTELXE_EFI_OPS:-}" ] || { [ ${#OPS[@]} = 0 ] || printf '%s\n' "${OPS[@]}"; } > "$INTELXE_EFI_OPS";;
    --apply|--settings-apply|--safari-apply|--settings-safari-apply|--bootlog-apply|--bootlog-off-apply)
        need_root; need_efi
        case $CMD in --apply) plan_files; verify_package;; --settings-apply) plan_settings;; --safari-apply) plan_safari;;
            --settings-safari-apply) plan_settings; plan_safari;; --bootlog-apply) plan_bootlog;;
            --bootlog-off-apply) plan_bootlog_off;; esac
        check_expect
        print_plan
        [ ${#OPS[@]} -gt 0 ] || { echo "== nothing to change"; exit 0; }
        B=""; case $CMD in --apply) k=driver;; --settings-apply) k=settings;; --safari-apply) k=safari;; --settings-safari-apply) k=settings-safari;; --bootlog-apply) k=bootlog;;
            --bootlog-off-apply) k=bootlog-off;; esac
        k=$(rule backup_kinds.$k) || { echo "$RULES has no backup_kinds.$k"; exit 1; }
        backup "$k"
        run_ops
        echo "== done. Restart to use it. To go back: sudo $0 --undo${PART:+ --part $PART}"
        ;;
    --undo)
        need_root; need_efi
        plan_undo; check_expect; print_plan
        B=$(last_backup); [ -n "$B" ] || exit 1
        while IFS= read -r rel; do
            [ -n "$rel" ] && undo_path "$rel" || continue
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
