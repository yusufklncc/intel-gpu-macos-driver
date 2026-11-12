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
#                                                              with windows/IntelXe-EFI.ps1 and its README)
#   intel-gpu-macos-driver-VERSION.sha256
#   intel-gpu-macos-driver-VERSION.sha256.sig  with SIGN_KEY=<Ed25519 private key file> (release/signing.swift): the
#                                              list's signature, which IntelXe Manager checks before it installs a download
# With ZINK_DIR set in the environment also intel-gpu-macos-driver-VERSION-zink-gl.zip (see below), with VULKAN_DIR
# intel-gpu-macos-driver-VERSION-vulkan.zip (native Vulkan for games, experimental), and with APP_DIR
# (the folder app/build.sh wrote) IntelXe-Manager-VERSION.zip, the app.
set -euo pipefail
[ $# = 4 ] || { sed -n '3,18p' "$0"; exit 2; }
VER=$1 T=$2 S=$3 OUT=$4
R=$(cd "$(dirname "$0")" && pwd)
N=intel-gpu-macos-driver-$VER
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# the OpenCore pieces must be identical in both, or the one -efi.zip would be wrong for one of them
for f in EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist \
         EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist \
         EFI/OC/Kexts/HybridPM.kext/Contents/Resources/LICENSE-Lilu.txt \
         EFI/OC/ACPI/SSDT-ARCOFF.aml EFI/OC/ACPI/SSDT-ARCOFF.dsl hpmstat; do
    [ -f "$T/$f" ] || { echo "missing in the Tahoe input: $f"; exit 1; }
    cmp -s "$T/$f" "$S/$f" || { echo "differs between the Tahoe and Sequoia inputs: $f"; exit 1; }
done

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
    cp "$R/nvmtl-allow.txt" "$d/Library/GPUBundles/nvmtl-allow.txt"   # the allow-list is kept in the repository
    fill "$R/templates/install.sh" "$d/install.sh" $c; chmod 755 "$d/install.sh"
    install -m 755 "$R/templates/efi.sh" "$d/efi.sh"   # no placeholders: the app carries the same file
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
cat > "$e/README.txt" <<EOF
intel-gpu-macos-driver $VER: the OpenCore pieces (the same for macOS 26 Tahoe and macOS 15 Sequoia)

EFI/OC/Kexts/IntelXe.kext       -> EFI/OC/Kexts/, config.plist Kernel > Add
EFI/OC/Kexts/HybridPM.kext      optional, for 12th-14th gen Core CPUs with P- and E-cores: -> EFI/OC/Kexts/,
                                Kernel > Add after Lilu.kext (it needs Lilu, which is not included); see
                                "Hybrid Intel CPUs" in README-INSTALL.md
EFI/OC/ACPI/SSDT-ARCOFF.aml     -> EFI/OC/ACPI/, config.plist ACPI > Add (check the ACPI path first)
EFI/OC/ACPI/SSDT-ARCOFF.dsl     its source, to adapt with iasl when your board's path differs
IntelXe-EFI.ps1                 on Windows: makes stages 2 and 4 in the EFI folder you choose, with a
                                backup and an undo (right-click > Run with PowerShell; README-Windows.md)

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
    rm -f "$OUT/IntelXe-Manager-$VER.zip"
    (cd "$a" && ditto -c -k --norsrc --noextattr --noacl --keepParent "IntelXe Manager.app" "$OUT/IntelXe-Manager-$VER.zip")
    ZIPS+=("IntelXe-Manager-$VER.zip")
fi
(cd "$OUT" && shasum -a 256 "${ZIPS[@]}" > "$N.sha256")
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
