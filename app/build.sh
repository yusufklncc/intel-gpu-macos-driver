#!/bin/bash
# SPDX-License-Identifier: MIT
# build.sh OUTDIR: "OUTDIR/IntelXe Manager.app", universal (x86_64 for the machines it serves, arm64 for working on it), ad-hoc signed.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd); O=$1; A="$O/IntelXe Manager.app"
VER=$(cat "$D/../VERSION")
mkdir -p "$O/obj" "$A/Contents/MacOS" "$A/Contents/Resources"
for arch in x86_64 arm64; do
    swiftc -O -swift-version 5 -target $arch-apple-macos15.0 "$D"/Sources/*.swift -module-name IntelXeManager -o "$O/obj/IntelXeManager-$arch"
done
lipo -create "$O/obj/IntelXeManager-x86_64" "$O/obj/IntelXeManager-arm64" -output "$A/Contents/MacOS/IntelXeManager"
sed "s/@VER@/$VER/g" "$D/Info.plist" > "$A/Contents/Info.plist"
ditto "$D/Resources" "$A/Contents/Resources"
install -m 755 "$D/../release/templates/efi.sh" "$A/Contents/Resources/efi.sh"   # for packages made before efi.sh
install -m 644 "$D/../release/templates/efi-rules.json" "$A/Contents/Resources/efi-rules.json"   # its rules, beside it
install -m 755 "$D/../release/templates/hidpi.sh" "$A/Contents/Resources/hidpi.sh"   # for packages made before hidpi.sh
install -m 755 "$D/../tools/report/collect-macos.sh" "$A/Contents/Resources/collect-macos.sh"   # the diagnostics report
codesign -f -s - --deep "$A" 2>/dev/null
echo "$A"
