#!/bin/bash
# SPDX-License-Identifier: MIT
# package.sh: build the release zips from staged binaries and this directory's templates (macOS, uses ditto).
#
#   release/package.sh VERSION TAHOE_DIR SEQUOIA_DIR OUT_DIR
#
# TAHOE_DIR / SEQUOIA_DIR hold the binaries in the package layout (EFI/, Library/, system-volume/, LICENSES/, sigcheck,
# hpmstat, SOURCES.md), e.g. the unpacked previous release. EFI/OC/Kexts/ has IntelXe.kext and HybridPM.kext
# (kernel/HybridPM/build.py writes HybridPM.kext and hpmstat), the same files in both. install.sh and README-INSTALL.md come from templates/ with @VER@,
# @MACOS@, @MAJOR@, @CODENAME@ and @TESTED@ / @TESTED_TR@ filled in, Library/GPUBundles/nvmtl-allow.txt comes from release/; SHA256SUMS is
# regenerated. The filled Tahoe copies are also
# written next to this script (release/install.sh, release/README-INSTALL.md), for reading in the repository.
# Writes to OUT_DIR:
#   intel-gpu-macos-driver-VERSION-tahoe.zip, -sequoia.zip   (unpack on macOS: ditto -x -k)
#   intel-gpu-macos-driver-VERSION-efi.zip                    (the OpenCore pieces, the same in both; any system;
#                                                              with windows/IntelXe-EFI.ps1, its README and the
#                                                              EFI rules, templates/efi-rules.json)
#   intel-gpu-macos-driver-VERSION.sha256
#   intel-gpu-macos-driver-VERSION.sha256.sig  with SIGN_KEY=<Ed25519 private key file> (release/signing.swift): the
#                                              list's signature, which IntelXe Manager checks before it installs a download
#   intel-gpu-macos-driver-VERSION.sha256.sigs during a key change, with SIGN_KEY_NEXT=<the new key's file> too: one
#                                              line per key (docs/RELEASE-KEYS.md; both keys must be in the apps' lists)
# With ZINK_DIR set in the environment also intel-gpu-macos-driver-VERSION-zink-gl.zip (see below), with VULKAN_DIR
# intel-gpu-macos-driver-VERSION-vulkan.zip (native Vulkan for games, experimental), and with APP_DIR
# (the folder app/build.sh wrote) IntelXe-Manager-VERSION.zip, the app.
# IntelXe Manager for Windows is required: WINAPP_ZIP=<IntelXeManager-VERSION-win-x64.zip> as windows/app/build.ps1 or
# the CI workflow (.github/workflows/windows-app.yml) made it, with its .sha256 beside it (or WINAPP_SHA256=<hash>).
# It is built on Windows and published as it is, as its own asset (see below). WINAPP_SKIP=1 packages without it.
# The Xe probe stick's image is required too: PROBE_IMG=<xe-probe-stick-SVER.img> as tools/probe-stick/build.sh made it
# (SVER: tools/probe-stick/VERSION), with its .img.sha256 beside it; package.sh checks both and writes
# xe-probe-stick-SVER.img.zip (the image and its .img.sha256; tools/probe-stick/zip-image.py), its own asset, listed in
# the signed list of hashes. IntelXe Manager for Windows downloads it from there. PROBE_SKIP=1 packages without it.
set -euo pipefail
[ $# = 4 ] || { sed -n '3,32p' "$0"; exit 2; }
VER=$1 T=$2 S=$3 OUT=$4
R=$(cd "$(dirname "$0")" && pwd)
N=intel-gpu-macos-driver-$VER
WIN=IntelXeManager-$VER-win-x64

# IntelXe Manager for Windows (windows/app): a WinUI app, so it is built on Windows (windows/app/build.ps1) or by the
# CI workflow, never here; package.sh takes the zip that build made and checks it before anything is written. It is
# its own release asset, IntelXeManager-VERSION-win-x64.zip, not a part of the -efi.zip: the -efi.zip stays the small
# OpenCore package for every system (the app itself takes it as its package and unpacks all of it, and efi.sh's and
# the script's users need no 66 MB of Windows runtime), and the app's zip is published byte for byte as built, so that
# its SHA-256 is the one build.ps1 printed. Both are in the signed list of hashes. Unpacked side by side, the app finds
# the unpacked -efi.zip in the folder above its own (EfiPackage.FindBeside).
winapp_check() { # the zip's name, version, files and hash; prints the reason and fails otherwise
    local z=$1 want got
    [ -f "$z" ] || { echo "WINAPP_ZIP: no file $z"; return 1; }
    [ "$(basename "$z")" = "$WIN.zip" ] || { echo "WINAPP_ZIP is $(basename "$z"), not $WIN.zip (build.ps1 names it by VERSION)"; return 1; }
    want=${WINAPP_SHA256:-}
    [ -n "$want" ] || { [ -f "$z.sha256" ] && want=$(awk 'NR == 1 { print $1 }' "$z.sha256"); }
    [ -n "$want" ] || { echo "no SHA-256 for $z: build.ps1 writes $(basename "$z").sha256 beside it; or set WINAPP_SHA256"; return 1; }
    got=$(shasum -a 256 "$z" | awk '{ print $1 }')
    [ "$(tr 'A-F' 'a-f' <<< "$want")" = "$got" ] || { echo "$z: SHA-256 $got, not the build's $want (damaged or another build)"; return 1; }
    # the zip as build.ps1's Compress-Archive writes it (Windows PowerShell 5.1 writes backslashes): one top folder
    # with the exe, the managed app of this version, the string tables, the report script of this repository
    python3 -I - "$z" "$WIN" "$VER" "$R/../tools/report/collect-windows.ps1" "$(bash "$R/release-keys.sh" list | paste -sd' ' -)" <<'PY' || return 1
import re, sys, zipfile
z, top, ver, script, keys = sys.argv[1:6]
def fail(m): print("WINAPP_ZIP: " + m); sys.exit(1)
try:
    zf = zipfile.ZipFile(z)
    bad = zf.testzip()
except Exception as e: fail(f"not a zip ({e})")
if bad: fail(f"damaged entry {bad}")
names = {i.filename.replace("\\", "/"): i for i in zf.infolist()}
for f in ("IntelXeManager.exe", "IntelXeManager.dll", "LICENSE.txt", "tools/collect-windows.ps1"):
    if f"{top}/{f}" not in names: fail(f"no {top}/{f}")
# the app's resource index: resources.pri from older Windows App SDK builds, IntelXeManager.pri (the assembly's name)
# from the SDK build.ps1 uses now (14 Nov); either is what the unpackaged app loads
if f"{top}/resources.pri" not in names and f"{top}/IntelXeManager.pri" not in names: fail(f"no {top}/resources.pri or IntelXeManager.pri")
if any(not n.startswith(top + "/") for n in names): fail(f"files outside its folder {top}/")
if zf.read(names[f"{top}/IntelXeManager.exe"])[:2] != b"MZ": fail("IntelXeManager.exe is not a Windows program")
# the ProductVersion of the app's version resource (Directory.Build.props: the repository's VERSION)
dll = zf.read(names[f"{top}/IntelXeManager.dll"])
m = re.search("ProductVersion".encode("utf-16-le") + rb"\0\0(?:\0\0)?((?:[^\0]\0)+)\0\0", dll)
v = m.group(1).decode("utf-16-le") if m else ""
if v != ver: fail(f"IntelXeManager.dll is version {v or 'unknown'}, not {ver}")
norm = lambda b: b.replace(b"\r\n", b"\n").lstrip(b"\xef\xbb\xbf")
if norm(zf.read(names[f"{top}/tools/collect-windows.ps1"])) != norm(open(script, "rb").read()):
    fail("tools/collect-windows.ps1 is not this repository's tools/report/collect-windows.ps1 (built from another tree?)")
# the release keys of app/Sources/Signing.swift, in the Windows app's list (a .NET string: UTF-16)
core = names.get(f"{top}/IntelXe.Core.dll")
if core is None: fail(f"no {top}/IntelXe.Core.dll")
core = zf.read(core)
for k in keys.split():
    if k.encode("utf-16-le") not in core: fail(f"IntelXe.Core.dll does not carry the release key {k} (built from another tree?)")
print(f"  ok    {top}.zip: IntelXeManager.exe, version {ver}, {len(names)} files")
PY
}
if [ -n "${WINAPP_ZIP:-}" ]; then
    winapp_check "$WINAPP_ZIP" || exit 1
elif [ "${WINAPP_SKIP:-0}" = 1 ]; then
    echo "note: packaging without IntelXe Manager for Windows (WINAPP_SKIP=1)"
else
    echo "no WINAPP_ZIP: IntelXe Manager for Windows ($WIN.zip from windows/app/build.ps1 or the windows-app workflow) is part of the release; WINAPP_SKIP=1 packages without it"
    exit 1
fi
# The Xe probe stick (tools/probe-stick/): built in Docker by build.sh, never here; shipped zipped (about 208 MB, mostly
# free FAT space; about 38 MB zipped, and Windows opens a zip by itself) as its own asset, with its .img.sha256 inside
# the zip rather than beside it: the macOS app finds a release's list of hashes by the ending .sha256, so the release
# has no other asset with that ending. The image is checked here (name and version, its .img.sha256, the stick's layout)
# before anything is written.
PROBE_VER=$(cat "$R/../tools/probe-stick/VERSION")
PROBE=xe-probe-stick-$PROBE_VER.img.zip
if [ -n "${PROBE_IMG:-}" ]; then
    python3 -I "$R/../tools/probe-stick/zip-image.py" check "$PROBE_IMG" --version "$PROBE_VER" || { echo "PROBE_IMG: not the probe stick image to ship"; exit 1; }
elif [ "${PROBE_SKIP:-0}" = 1 ]; then
    echo "note: packaging without the Xe probe stick image (PROBE_SKIP=1)"
else
    echo "no PROBE_IMG: the Xe probe stick image (xe-probe-stick-$PROBE_VER.img from tools/probe-stick/build.sh, with its .img.sha256) is part of the release; PROBE_SKIP=1 packages without it"
    exit 1
fi
# The signing keys (docs/RELEASE-KEYS.md), checked before anything is written: SIGN_KEY's public key must be one the
# app accepts (release.sh also checks it against the previous release's app), SIGN_KEY_NEXT's (a key change) must be in
# both apps' lists, which must be the same (release-keys.sh), or the transition release would ship apps that do not
# know the key that signs the next one.
if [ -n "${SIGN_KEY:-}" ]; then
    bash "$R/release-keys.sh" check >/dev/null || { echo "the macOS and Windows apps' release keys differ: release/release-keys.sh check"; exit 1; }
    for k in "$SIGN_KEY" ${SIGN_KEY_NEXT:+"$SIGN_KEY_NEXT"}; do
        pub=$(swift "$R/signing.swift" public "$k") || { echo "$k is not an Ed25519 private key"; exit 1; }
        bash "$R/release-keys.sh" list | grep -qxF "$pub" ||
            { echo "the public key of $k ($pub) is not in app/Sources/Signing.swift's releaseSigningKeys: the app would refuse this release (release/release-keys.sh add $pub)"; exit 1; }
    done
    [ -z "${SIGN_KEY_NEXT:-}" ] || [ "$(swift "$R/signing.swift" public "$SIGN_KEY")" != "$(swift "$R/signing.swift" public "$SIGN_KEY_NEXT")" ] ||
        { echo "SIGN_KEY_NEXT is the same key as SIGN_KEY"; exit 1; }
elif [ -n "${SIGN_KEY_NEXT:-}" ]; then
    echo "SIGN_KEY_NEXT without SIGN_KEY: the .sig every app reads is made with SIGN_KEY (the old key)"; exit 1
fi
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# the OpenCore pieces must be identical in both, or the one -efi.zip would be wrong for one of them
for f in EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist \
         EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist \
         EFI/OC/Kexts/HybridPM.kext/Contents/Resources/LICENSE-Lilu.txt \
         EFI/OC/ACPI/SSDT-ARCOFF.aml EFI/OC/ACPI/SSDT-ARCOFF.dsl hpmstat; do
    [ -f "$T/$f" ] || { echo "missing in the Tahoe input: $f"; exit 1; }
    cmp -s "$T/$f" "$S/$f" || { echo "differs between the Tahoe and Sequoia inputs: $f"; exit 1; }
done

# XeProbe's GuC images for the other families (stage 3 off the B580) are built into IntelXe.kext (decided 13 Nov:
# ship them, about 2.4 MB): kernel/IntelXe/build.py --release records them in the XeProbe personality. PROBE_GUC_SKIP=1
# packages a kext without them.
if [ "${PROBE_GUC_SKIP:-0}" != 1 ]; then
    pg=$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:XeProbe:XeProbeGuC' "$T/EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist" 2>/dev/null || true)
    for s in tgl adlp dg1 dg2 mtl lnl ptl; do
        case " $pg " in *" $s "*) ;; *) echo "IntelXe.kext has no XeProbe GuC image for $s (XeProbeGuC: '$pg'): build it after firmware/fetch.sh --probe, or PROBE_GUC_SKIP=1"; exit 1;; esac
    done
fi

# The Metal plugin is rebuilt for every release, and its bundle's version is what the Manager and a bug report show.
# 0.1.4 shipped with the 0.1.2 Info.plist around new binaries (the inputs were assembled by copying binaries into an
# older bundle); the build (nullmoth/build/build-intelxe.sh) stamps VERSION. The Info.plist cannot be fixed here: it is
# sealed by the bundle's signature. PLUGIN_VERSION_ANY=1 lets an older version through on purpose.
for src in "$T" "$S"; do
    for pl in Library/GPUBundles/NVMTLDriverLib.bundle system-volume/system-layout/NVMTLDriver.bundle; do
        [ -f "$src/$pl/Contents/Info.plist" ] || continue
        v=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$src/$pl/Contents/Info.plist" 2>/dev/null || true)
        [ "$v" = "$VER" ] || [ "${PLUGIN_VERSION_ANY:-0}" = 1 ] ||
            { echo "$src/$pl says version '$v', not $VER: take the bundle from a build of this release (PLUGIN_VERSION_ANY=1 to allow)"; exit 1; }
    done
done

fill() { # fill SRC DST codename
    local macos major tested tested_tr
    case $3 in
        tahoe) macos="macOS 26 Tahoe"; major=26
               tested="Experimental. Tested on one machine only: an Arc B580 in a Z790 desktop, macOS 26.7.1, OpenCore."
               tested_tr="Deneysel bir sürüm. Yalnızca tek makinede denendi: Z790 masaüstü, Arc B580, macOS 26.7.1, OpenCore.";;
        sequoia) macos="macOS 15 Sequoia"; major=15
               tested="Experimental. **Not re-tested on Sequoia:** this package is built from the same sources as the Tahoe package, which was tested on one machine (an Arc B580 in a Z790 desktop, macOS 26.7.1, OpenCore); only NMXAccel is built for macOS 15. The same machine ran the previous build of this stack on macOS 15 until 4 November 2026."
               tested_tr="Deneysel bir sürüm. **Sequoia'da yeniden denenmedi:** Bu paket, tek makinede denenen Tahoe paketiyle (Z790 masaüstü, Arc B580, macOS 26.7.1, OpenCore) aynı kaynaktan derlendi; yalnızca NMXAccel macOS 15 için derlendi. Aynı makine, bu yığının bir önceki derlemesini 4 Kasım 2026'ya kadar macOS 15'te çalıştırdı.";;
    esac
    sed -e "s|@VER@|$VER|g" -e "s|@MACOS@|$macos|g" -e "s|@MAJOR@|$major|g" -e "s|@CODENAME@|$3|g" \
        -e "s|@TESTED@|$tested|g" -e "s|@TESTED_TR@|$tested_tr|g" "$1" > "$2"
    ! grep -n '@[A-Z_]*@' "$2" || { echo "unfilled placeholder in $2"; exit 1; }
}

zipdir() { # zipdir PARENT NAME: NAME.zip with Unix modes, no resource forks, no extended attributes
    rm -f "$OUT/$2.zip"
    (cd "$1" && ditto -c -k --norsrc --noextattr --noacl --keepParent "$2" "$OUT/$2.zip")
}

# every Mach-O that install.sh or ssv-install.sh copies must carry a valid signature (install.sh refuses it otherwise;
# 0.1.2 was first uploaded with an unsigned IntelXeFB, which the flow test, run without the real checks, did not see)
for src in "$T" "$S"; do
    while IFS= read -r f; do
        if file -b "$f" | grep -q Mach-O && ! codesign -v "$f" 2>/dev/null; then echo "not signed: ${f#$src/}"; exit 1; fi
    done < <(find "$src/Library" "$src/system-volume" "$src/sigcheck" "$src/hpmstat" -type f)
done

W=$(mktemp -d -t ixepkg)
trap 'rm -rf "$W"' EXIT
for c in tahoe sequoia; do
    src=$T; [ $c = sequoia ] && src=$S
    d=$W/$N-$c
    mkdir -p "$d"
    for x in EFI Library system-volume LICENSES sigcheck hpmstat SOURCES.md; do ditto "$src/$x" "$d/$x"; done
    cp "$R/../kernel/HybridPM/LICENSE.Lilu" "$d/LICENSES/Lilu-BSD-3-Clause.txt"   # Lilu's plugin_start.cpp is in HybridPM.kext
    cp "$R/../firmware/LICENSE.xe" "$d/LICENSES/Intel-firmware-LICENSE.xe.txt"
    cp "$R/../firmware/LICENSE.i915" "$d/LICENSES/Intel-firmware-LICENSE.i915.txt"   # XeProbe's tgl/adlp/dg1/dg2/mtl GuC images
    cp "$R/nvmtl-allow.txt" "$d/Library/GPUBundles/nvmtl-allow.txt"   # the allow-list is kept in the repository
    fill "$R/templates/install.sh" "$d/install.sh" $c; chmod 755 "$d/install.sh"
    install -m 755 "$R/templates/efi.sh" "$d/efi.sh"   # no placeholders: the app carries the same file
    install -m 644 "$R/templates/efi-rules.json" "$d/efi-rules.json"   # efi.sh's rules, beside it (the app's too)
    install -m 755 "$R/templates/hidpi.sh" "$d/hidpi.sh"   # the same: the app carries it too
    fill "$R/templates/README-INSTALL.md" "$d/README-INSTALL.md" $c
    fill "$R/templates/README-INSTALL.tr.md" "$d/README-INSTALL.tr.md" $c
    if [ $c = tahoe ]; then cp "$d/install.sh" "$R/install.sh"; cp "$d/README-INSTALL.md" "$R/README-INSTALL.md"; cp "$d/README-INSTALL.tr.md" "$R/README-INSTALL.tr.md"; fi
    xattr -cr "$d"
    (cd "$d" && find . -type f ! -name SHA256SUMS ! -path './system-volume/*' | sed 's#^\./##' | LC_ALL=C sort |
        while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS
     find system-volume -type f | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done >> SHA256SUMS)
    (cd "$d" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "SHA256SUMS check failed in $d"; exit 1; }
    (cd "$d/system-volume" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "system-volume/SHA256SUMS does not match"; exit 1; }
    zipdir "$W" "$N-$c"
done

e=$W/$N-efi
mkdir -p "$e"
ditto "$T/EFI" "$e/EFI"
cp "$R/../windows/IntelXe-EFI.ps1" "$e/IntelXe-EFI.ps1"   # stages 2 and 4 on Windows, in the EFI folder the user picks
cp "$R/../windows/README.md" "$e/README-Windows.md"
cp "$R/templates/efi-rules.json" "$e/efi-rules.json"   # its rules: the file efi.sh reads on macOS
cat > "$e/README.txt" <<EOF
intel-gpu-macos-driver $VER: the OpenCore pieces (the same for macOS 26 Tahoe and macOS 15 Sequoia)

EFI/OC/Kexts/IntelXe.kext       -> EFI/OC/Kexts/, config.plist Kernel > Add
EFI/OC/Kexts/HybridPM.kext      optional, for 12th-14th gen Core CPUs with P- and E-cores: -> EFI/OC/Kexts/,
                                Kernel > Add after Lilu.kext (it needs Lilu, which is not included); see
                                "Hybrid Intel CPUs" in README-INSTALL.md
EFI/OC/ACPI/SSDT-ARCOFF.aml     -> EFI/OC/ACPI/, config.plist ACPI > Add (check the ACPI path first)
EFI/OC/ACPI/SSDT-ARCOFF.dsl     its source, to adapt with iasl when your board's path differs
IntelXe-EFI.ps1                 on Windows, the scripted way: makes stages 2 and 4 in the EFI folder you
                                choose, with a backup and an undo (right-click > Run with PowerShell;
                                README-Windows.md)
efi-rules.json                  what IntelXe-EFI.ps1 changes (the same rules as efi.sh's on macOS): keep it
                                next to the script

On Windows the recommended way is IntelXe Manager for Windows, a separate download of the same release
($WIN.zip): it makes the same changes by the same rules, shows each plan first and
takes this package (this zip, or the folder you unpacked it to; unpacked next to the app's folder it is
found by itself).

Add them in stage 4 of README-INSTALL.md (in the macOS package, and at
https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v$VER/release/README-INSTALL.md), after
"sudo ./install.sh --apply" on macOS and before the reboot. Not earlier: SSDT-ARCOFF hides the B580 from
the display driver that shows the macOS installer.
EOF
xattr -cr "$e"
(cd "$e" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
zipdir "$W" "$N-efi"

# ZINK_DIR: built zink-gl binaries (lib/, vulkan/, bin/, sigcheck): adds the optional -zink-gl.zip from zink-gl/ in this
# repository. Without it no zink package is made.
ZIPS=("$N-tahoe.zip" "$N-sequoia.zip" "$N-efi.zip")
if [ -n "${ZINK_DIR:-}" ]; then
    z=$W/$N-zink-gl
    mkdir -p "$z"
    ditto "$R/../zink-gl" "$z"
    for x in lib vulkan bin sigcheck; do ditto "$ZINK_DIR/$x" "$z/$x"; done
    chmod 755 "$z/install.sh" "$z/zink-gl-version.sh" "$z/sigcheck"
    xattr -cr "$z"
    (cd "$z" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
    zipdir "$W" "$N-zink-gl"
    ZIPS+=("$N-zink-gl.zip")
fi
# VULKAN_DIR: the output folder of vulkan/build.sh (lib/, bin/, icd.d/): adds the optional -vulkan.zip from vulkan/ in
# this repository, with the Tahoe input's sigcheck. Experimental and off unless installed and used (vulkan/README.md).
if [ -n "${VULKAN_DIR:-}" ]; then
    v=$W/$N-vulkan
    mkdir -p "$v"
    ditto "$R/../vulkan" "$v"
    for x in lib bin icd.d; do ditto "$VULKAN_DIR/$x" "$v/$x"; done
    cp "$T/sigcheck" "$v/sigcheck"
    for f in "$v/lib/libintelxe-vk.dylib" "$v/bin/vksurf"; do codesign -v "$f" 2>/dev/null || { echo "not signed: ${f#$v/}"; exit 1; }; done
    chmod 755 "$v/install.sh" "$v/intelxe-vk" "$v/build.sh" "$v/test/run.sh" "$v/sigcheck"
    xattr -cr "$v"
    (cd "$v" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
    zipdir "$W" "$N-vulkan"
    ZIPS+=("$N-vulkan.zip")
fi
# APP_DIR: the output folder of app/build.sh. The app's name must not end like a package (-tahoe.zip, -sequoia.zip,
# -zink-gl.zip, -vulkan.zip): the app finds a release's packages by those endings.
if [ -n "${APP_DIR:-}" ]; then
    a=$W/app; mkdir -p "$a"
    ditto "$APP_DIR/IntelXe Manager.app" "$a/IntelXe Manager.app"
    [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$a/IntelXe Manager.app/Contents/Info.plist")" = "$VER" ] ||
        { echo "the app was built for another version than $VER"; exit 1; }
    codesign -v "$a/IntelXe Manager.app" || { echo "the app's signature does not verify"; exit 1; }
    # the app must know every key of app/Sources/Signing.swift (built from this tree, not an older one): during a key
    # change the new key, which signs the next release
    for k in $(bash "$R/release-keys.sh" list); do
        grep -a -q -F "$k" "$a/IntelXe Manager.app/Contents/MacOS/IntelXeManager" ||
            { echo "the app does not carry the release key $k of app/Sources/Signing.swift: build it again (app/build.sh)"; exit 1; }
    done
    rm -f "$OUT/IntelXe-Manager-$VER.zip"
    (cd "$a" && ditto -c -k --norsrc --noextattr --noacl --keepParent "IntelXe Manager.app" "$OUT/IntelXe-Manager-$VER.zip")
    ZIPS+=("IntelXe-Manager-$VER.zip")
fi
if [ -n "${WINAPP_ZIP:-}" ]; then
    [ "$(cd "$(dirname "$WINAPP_ZIP")" && pwd)/$WIN.zip" = "$OUT/$WIN.zip" ] || cp "$WINAPP_ZIP" "$OUT/$WIN.zip"
    cmp -s "$WINAPP_ZIP" "$OUT/$WIN.zip" || { echo "the copy of $WIN.zip in $OUT differs from $WINAPP_ZIP"; exit 1; }
    ZIPS+=("$WIN.zip")
fi
rm -f "$OUT"/xe-probe-stick-*.img.zip
if [ -n "${PROBE_IMG:-}" ]; then
    python3 -I "$R/../tools/probe-stick/zip-image.py" zip "$PROBE_IMG" "$OUT" --version "$PROBE_VER" || { echo "zipping the probe stick image failed"; exit 1; }
    ZIPS+=("$PROBE")
fi
(cd "$OUT" && shasum -a 256 "${ZIPS[@]}" > "$N.sha256")
(cd "$OUT" && shasum -a 256 -c "$N.sha256" >/dev/null) || { echo "$N.sha256 does not match the zips"; exit 1; }
rm -f "$OUT/$N.sha256.sig" "$OUT/$N.sha256.sigs"
if [ -n "${SIGN_KEY:-}" ]; then
    swift "$R/signing.swift" sign "$SIGN_KEY" "$OUT/$N.sha256" || { echo "signing failed"; exit 1; }
    echo "signed: $N.sha256.sig"
    # during a key change (docs/RELEASE-KEYS.md): SIGN_KEY_NEXT=<new key file> also writes .sha256.sigs, one line per key;
    # apps from before the change read .sig (the old key), apps that know the new key read .sigs
    if [ -n "${SIGN_KEY_NEXT:-}" ]; then
        for k in "$SIGN_KEY" "$SIGN_KEY_NEXT"; do
            swift "$R/signing.swift" sign "$k" "$OUT/$N.sha256" "$OUT/$N.sha256.sigs" || { echo "signing failed"; exit 1; }
        done
        echo "signed: $N.sha256.sigs (two keys)"
    fi
fi
cat "$OUT/$N.sha256"
