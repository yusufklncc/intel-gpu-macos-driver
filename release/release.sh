#!/bin/bash
# SPDX-License-Identifier: MIT
# release.sh VERSION OUT_DIR [--publish]: the release of intel-gpu-macos-driver VERSION in one place.
#
#   release/release.sh 0.1.4 ~/github-staging/release/out-0.1.4             dry run: checks everything, publishes nothing
#   release/release.sh 0.1.4 ~/github-staging/release/out-0.1.4 --publish   tag, push, GitHub release (asks once more)
#
# Checks (all must pass, also for --publish):
#   - VERSION in the repository, the branch (main), a clean work tree (untracked files only warn);
#   - the tag vVERSION exists neither here nor on origin or gitlab; origin/main and gitlab/public are ancestors of HEAD
#     (fast-forwards);
#   - every copy of the supported-device list matches data/platforms.json (tools/parity/devices.py);
#   - OUT_DIR holds the package zips of package.sh and the app, built after the last commit to what goes into them
#     (code, scripts, templates, packager; not documentation), and its .sha256 matches them and is signed
#     (.sha256.sig) with a key IntelXe Manager accepts (app/Sources/Signing.swift);
#   - IntelXe Manager for Windows (IntelXeManager-VERSION-win-x64.zip, built on Windows, checked and copied by
#     package.sh from WINAPP_ZIP) is there and in the list, unless WINAPP_SKIP=1;
#   - the Xe probe stick's image (xe-probe-stick-SVER.img.zip, SVER from tools/probe-stick/VERSION, zipped by package.sh
#     from PROBE_IMG) is there and in the list, and holds the image and its matching .img.sha256, unless PROBE_SKIP=1;
#   - the versions inside: IntelXe.kext and HybridPM.kext in the -efi.zip, IntelXe Manager in its zip, install.sh in the
#     Tahoe and Sequoia zips; efi-rules.json beside efi.sh and IntelXe-EFI.ps1 in every zip, the repository's;
#   - the release notes release/notes-VERSION.md (the final file, not the -draft) exist, say VERSION and have no
#     "draft" or "TODO" left; release/install.sh and README-INSTALL*.md (package.sh's filled copies) are of VERSION;
#   - the machine-identity scan (tools/commit-plan-scan.py of the private repo, when present) finds nothing in the
#     notes and the filled guides.
# Publishing: git tag -a vVERSION; git push origin main vVERSION; to gitlab (the local GitLab mirror) main goes to its
# branch public (gitlab's main is a separate, private history: never pushed to);
# gh release create vVERSION <zips> --notes-file release/notes-VERSION.md. Each step stops the rest on failure.
set -uo pipefail
[ $# -ge 2 ] || { sed -n '3,25p' "$0"; exit 2; }
VER=$1 OUT=$2 PUBLISH=no
[ "${3:-}" = --publish ] && PUBLISH=yes
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
N=intel-gpu-macos-driver-$VER
fails=0
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; fails=$((fails + 1)); }
warn(){ printf '  warn  %s\n' "$*"; }
zipver() { # zip, path of an Info.plist inside: its CFBundleShortVersionString or CFBundleVersion
	local t; t=$(mktemp -d); unzip -q -o "$1" "$2" -d "$t" 2>/dev/null
	plutil -extract CFBundleShortVersionString raw "$t/$2" 2>/dev/null || plutil -extract CFBundleVersion raw "$t/$2" 2>/dev/null
	rm -rf "$t"
}

echo "== intel-gpu-macos-driver $VER: release checks ($([ $PUBLISH = yes ] && echo publish || echo dry run))"
# --- repository
[ "$(cat VERSION 2>/dev/null)" = "$VER" ] && ok "VERSION is $VER" || bad "VERSION says $(cat VERSION 2>/dev/null), not $VER"
br=$(git rev-parse --abbrev-ref HEAD)
[ "$br" = main ] && ok "on main" || bad "on $br, not main"
[ -z "$(git status --porcelain --untracked-files=no)" ] && ok "work tree clean" || bad "uncommitted changes: $(git status --porcelain --untracked-files=no | wc -l | tr -d ' ') files"
[ -n "$(git status --porcelain | grep '^??')" ] && warn "untracked files (not part of the release): $(git status --porcelain | grep -c '^??')"
if git rev-parse -q --verify "refs/tags/v$VER" >/dev/null; then bad "tag v$VER exists here"; else ok "tag v$VER is new here"; fi
for rem in origin gitlab; do
	git remote get-url "$rem" >/dev/null 2>&1 || { warn "no remote $rem"; continue; }
	if out=$(timeout 20 git ls-remote --tags "$rem" "refs/tags/v$VER" 2>/dev/null || git ls-remote --tags "$rem" "refs/tags/v$VER" 2>/dev/null); then
		[ -z "$out" ] && ok "tag v$VER is new on $rem" || bad "tag v$VER already on $rem"
	else warn "$rem not reachable (tag not checked there)"; fi
done
if out=$(python3 tools/parity/devices.py 2>&1); then ok "supported-device lists match data/platforms.json ($(tail -1 <<< "$out" | sed 's/^devices: //'))"
else bad "supported-device lists differ from data/platforms.json: $(grep -c '^FAIL' <<< "$out") (python3 tools/parity/devices.py)"; fi
git fetch -q origin main 2>/dev/null && { git merge-base --is-ancestor origin/main HEAD && ok "origin/main is an ancestor of HEAD ($(git rev-list --count origin/main..HEAD) commits to push)" || bad "origin/main is not an ancestor of HEAD: not a fast-forward"; } || warn "origin not fetched"
if git remote get-url gitlab >/dev/null 2>&1; then
	git fetch -q gitlab public 2>/dev/null && { git merge-base --is-ancestor gitlab/public HEAD && ok "gitlab/public is an ancestor of HEAD" || bad "gitlab/public is not an ancestor of HEAD: not a fast-forward"; } || warn "gitlab not fetched"
fi

# --- packages
for f in "$N-tahoe.zip" "$N-sequoia.zip" "$N-efi.zip" "$N.sha256" "IntelXe-Manager-$VER.zip"; do
	[ -f "$OUT/$f" ] && ok "$f" || bad "missing $OUT/$f"
done
[ -f "$OUT/$N-zink-gl.zip" ] && ok "$N-zink-gl.zip (optional)" || warn "no $N-zink-gl.zip (optional)"
[ -f "$OUT/$N-vulkan.zip" ] && ok "$N-vulkan.zip (optional, experimental)" || warn "no $N-vulkan.zip (optional, experimental)"
# IntelXe Manager for Windows: built on Windows, checked by package.sh (WINAPP_ZIP), its own asset
WIN=IntelXeManager-$VER-win-x64.zip
if [ -f "$OUT/$WIN" ]; then
	grep -q "  $WIN\$" "$OUT/$N.sha256" 2>/dev/null && ok "$WIN, in $N.sha256" || bad "$WIN is not in $N.sha256 (package.sh with WINAPP_ZIP lists it)"
	unzip -l "$OUT/$WIN" 2>/dev/null | grep -qE "IntelXeManager-$VER-win-x64[/\\]IntelXeManager\.exe$" && ok "$WIN holds IntelXeManager.exe" || bad "$WIN has no IntelXeManager.exe"
elif [ "${WINAPP_SKIP:-0}" = 1 ]; then warn "no $WIN (WINAPP_SKIP=1: a release without the Windows app)"
else bad "missing $OUT/$WIN (IntelXe Manager for Windows: package.sh with WINAPP_ZIP; WINAPP_SKIP=1 to release without it)"; fi
# the Xe probe stick's image: zipped by package.sh (PROBE_IMG), its own asset; IntelXe Manager for Windows downloads it
# and checks it against the signed list
PVER=$(cat tools/probe-stick/VERSION 2>/dev/null)
PZ=xe-probe-stick-$PVER.img.zip
others=$(cd "$OUT" 2>/dev/null && ls xe-probe-stick-* 2>/dev/null | grep -vx "$PZ" | tr '\n' ' ')
[ -z "$others" ] || bad "other probe stick files in $OUT: $others(only $PZ, tools/probe-stick/VERSION $PVER, is published)"
if [ -f "$OUT/$PZ" ]; then
	grep -q "  $PZ\$" "$OUT/$N.sha256" 2>/dev/null && ok "$PZ, in $N.sha256" || bad "$PZ is not in $N.sha256 (package.sh with PROBE_IMG lists it)"
	if out=$(python3 -I tools/probe-stick/zip-image.py verify "$OUT/$PZ" --version "$PVER" 2>&1); then ok "${out#  ok    }"; else bad "$PZ: ${out#error: }"; fi
elif [ "${PROBE_SKIP:-0}" = 1 ]; then warn "no $PZ (PROBE_SKIP=1: a release without the probe stick image)"
else bad "missing $OUT/$PZ (the Xe probe stick image: package.sh with PROBE_IMG; PROBE_SKIP=1 to release without it)"; fi
# packages older than the source that goes into them: built before a later commit to the code, the scripts, the
# templates or the packager (documentation-only commits outside release/templates do not count)
t_code=$(git log -1 --format=%ct -- kernel nullmoth user app zink-gl vulkan windows ':(exclude)*.md' 2>/dev/null)
t_pkg=$(git log -1 --format=%ct -- release/templates release/package.sh release/nvmtl-allow.txt VERSION 2>/dev/null)
t_src=$(( ${t_code:-0} > ${t_pkg:-0} ? ${t_code:-0} : ${t_pkg:-0} ))
for f in "$N-tahoe.zip" "$N-sequoia.zip" "$N-efi.zip" "$N-zink-gl.zip" "$N-vulkan.zip" "IntelXe-Manager-$VER.zip"; do
	[ -f "$OUT/$f" ] || continue
	m=$(stat -f %m "$OUT/$f")
	[ "$m" -ge "$t_src" ] || bad "$f was built $(date -r "$m" '+%d %b %H:%M'), before the source commit of $(date -r "$t_src" '+%d %b %H:%M') ($(git log -1 --format=%h -- kernel nullmoth user app zink-gl vulkan windows release/templates release/package.sh release/nvmtl-allow.txt VERSION ':(exclude)*.md')): package again"
done
if [ -f "$OUT/$N.sha256" ]; then
	(cd "$OUT" && shasum -a 256 -c "$N.sha256" >/dev/null 2>&1) && ok "$N.sha256 matches" || bad "$N.sha256 does not match the files"
	# the app refuses a download whose list is not signed with one of its keys
	if [ -f "$OUT/$N.sha256.sig" ]; then
		signed=no
		for key in $(sed -n 's/^let releaseSigningKeys = \[\(.*\)\]/\1/p' app/Sources/Signing.swift | tr -d '",'); do
			swift release/signing.swift verify "$key" "$OUT/$N.sha256" "$OUT/$N.sha256.sig" >/dev/null 2>&1 && signed=yes
		done
		[ $signed = yes ] && ok "$N.sha256.sig is a signature IntelXe Manager accepts" || bad "$N.sha256.sig does not verify with app/Sources/Signing.swift's keys"
	else bad "no $N.sha256.sig: IntelXe Manager refuses unsigned releases (package with SIGN_KEY=<key file>)"; fi
	# a key change's .sigs: every line must verify with one of the app's keys (docs/RELEASE-KEYS.md)
	if [ -f "$OUT/$N.sha256.sigs" ]; then
		n=0; good=0
		while IFS= read -r line; do
			[ -n "$line" ] || continue; n=$((n + 1)); printf '%s\n' "$line" > "$OUT/.sigline"
			for key in $(sed -n 's/^let releaseSigningKeys = \[\(.*\)\]/\1/p' app/Sources/Signing.swift | tr -d '",'); do
				swift release/signing.swift verify "$key" "$OUT/$N.sha256" "$OUT/.sigline" >/dev/null 2>&1 && { good=$((good + 1)); break; }
			done
		done < "$OUT/$N.sha256.sigs"; rm -f "$OUT/.sigline"
		[ $n -gt 0 ] && [ $good = $n ] && ok "$N.sha256.sigs: $n signatures, each from a key the app accepts" || bad "$N.sha256.sigs: $good of $n lines verify with the app's keys"
	fi
fi
if [ -f "$OUT/$N-efi.zip" ]; then
	v=$(zipver "$OUT/$N-efi.zip" "$N-efi/EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist"); [ "$v" = "$VER" ] && ok "IntelXe.kext $v" || bad "IntelXe.kext in the -efi.zip is ${v:-unreadable}"
	v=$(zipver "$OUT/$N-efi.zip" "$N-efi/EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist"); [ "$v" = "$VER" ] && ok "HybridPM.kext $v" || bad "HybridPM.kext in the -efi.zip is ${v:-unreadable}"
fi
# the EFI rules beside each of their readers, as in the repository: efi.sh (Tahoe, Sequoia, the app) and IntelXe-EFI.ps1
for z in "$N-tahoe.zip|$N-tahoe/efi-rules.json" "$N-sequoia.zip|$N-sequoia/efi-rules.json" "$N-efi.zip|$N-efi/efi-rules.json" \
	"IntelXe-Manager-$VER.zip|IntelXe Manager.app/Contents/Resources/efi-rules.json"; do
	[ -f "$OUT/${z%%|*}" ] || continue
	unzip -p "$OUT/${z%%|*}" "${z#*|}" 2>/dev/null | cmp -s - release/templates/efi-rules.json && ok "${z#*|} is release/templates/efi-rules.json" || bad "${z#*|} in ${z%%|*} is missing or not release/templates/efi-rules.json"
done
if [ -f "$OUT/IntelXe-Manager-$VER.zip" ]; then
	v=$(zipver "$OUT/IntelXe-Manager-$VER.zip" "IntelXe Manager.app/Contents/Info.plist"); [ "$v" = "$VER" ] && ok "IntelXe Manager $v" || bad "IntelXe Manager is ${v:-unreadable}"
fi
for m in tahoe sequoia; do
	[ -f "$OUT/$N-$m.zip" ] || continue
	unzip -p "$OUT/$N-$m.zip" "$N-$m/install.sh" 2>/dev/null | grep -q "^VER=$VER$" && ok "install.sh in -$m.zip is $VER" || bad "install.sh in -$m.zip is not $VER"
done

# --- notes and filled copies
NOTES=release/notes-$VER.md
if [ -f "$NOTES" ]; then
	ok "$NOTES"
	grep -q "$VER" "$NOTES" || bad "$NOTES does not mention $VER"
	grep -qiE '\bdraft\b|TODO|TBD' "$NOTES" && bad "$NOTES still says draft/TODO/TBD" || ok "no draft/TODO left in the notes"
else
	bad "no $NOTES (finish release/notes-$VER-draft.md and rename it)"
fi
grep -q "^VER=$VER$" release/install.sh && ok "release/install.sh is $VER" || bad "release/install.sh is not $VER (run package.sh, commit its filled copies)"
grep -q "intel-gpu-macos-driver-$VER-" release/README-INSTALL.md && ok "release/README-INSTALL.md is $VER" || bad "release/README-INSTALL.md is not $VER"
# the site's install guide (site/guide, written by hand) must still say what the templates say: commands, file names,
# boot-args and versions (tools/site-guide.py --check lists what is missing); a release must not leave it behind
python3 tools/site-guide.py --check >/dev/null 2>&1 && ok "site/guide has every command and name of the install guide templates" || bad "site/guide drifted from the install guide templates: python3 tools/site-guide.py --check, fix the pages, then --index"
SCAN=$(cd "$R/.." && pwd)/../hackintosh/tools/commit-plan-scan.py
[ -f "$SCAN" ] || SCAN=$HOME/hackintosh/tools/commit-plan-scan.py
if [ -f "$SCAN" ]; then
	files=$(ls "$NOTES" release/README-INSTALL.md release/README-INSTALL.tr.md 2>/dev/null)
	h=$(python3 "$SCAN" $files 2>&1 | sed -n 's/^scan: \([0-9]*\) hit(s)$/\1/p')
	[ "${h:-1}" = 0 ] && ok "identity scan: 0 hits" || bad "identity scan: ${h:-?} hit(s) in the notes or guides"
else
	warn "identity scan not found (tools/commit-plan-scan.py of the private repo)"
fi

echo "== $fails failure(s)"
CMDS=(
	"git tag -a v$VER -m 'intel-gpu-macos-driver $VER'"
	"git push origin main v$VER"
	"git push gitlab main:public v$VER"
	"gh release create v$VER $(cd "$OUT" 2>/dev/null && ls | grep -E "^($N|IntelXe-Manager-$VER|IntelXeManager-$VER-win-x64\.zip$|xe-probe-stick-${PVER//./\\.}\.img\.zip$)" | sed "s#^#'$OUT/#; s#\$#'#" | tr '\n' ' ')--title 'intel-gpu-macos-driver $VER' --notes-file $NOTES"
)
echo "== the publishing steps:"
for c in "${CMDS[@]}"; do echo "  $c"; done
[ $PUBLISH = yes ] || { echo "dry run: nothing was published"; exit $([ $fails = 0 ] && echo 0 || echo 1); }
[ $fails = 0 ] || { echo "not publishing: fix the failures first"; exit 1; }
printf 'Publish intel-gpu-macos-driver %s now (tag, push to origin and gitlab, GitHub release)? Type the version to go on: ' "$VER"
read -r answer
[ "$answer" = "$VER" ] || { echo "not published"; exit 1; }
for c in "${CMDS[@]}"; do
	echo "== $c"
	eval "$c" || { echo "stopped: that step failed; the steps before it are done"; exit 1; }
done
echo "== published $VER"
