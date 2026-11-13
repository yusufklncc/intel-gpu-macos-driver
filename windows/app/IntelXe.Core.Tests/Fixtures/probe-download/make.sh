#!/bin/sh
# SPDX-License-Identifier: MIT
# make.sh: remakes this folder's fixtures for ProbeDownloadTests (macOS: release/signing.swift uses CryptoKit).
#   - xe-probe-stick-0.1.0.img.zip: a 2 MiB image with the stick's layout (an MBR, a FAT32 boot sector labelled XEPROBE,
#     a pattern), zipped by tools/probe-stick/zip-image.py as release/package.sh zips the real one;
#   - intel-gpu-macos-driver-0.1.5.sha256: a release's list of hashes (shasum -a 256 lines) naming it, and its
#     .sha256.sig, signed by a key made here (test-key.txt holds its public key; the private key is thrown away);
#   - releases.json, releases-none.json: GitHub's answer to GET /repos/OWNER/REPO/releases, in the API's shape, for
#     releases with and without the image (a pre-release and a draft that have one must be passed over).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../../../../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 -I - "$T/xe-probe-stick-0.1.0.img" <<'PY'
import hashlib, os, struct, sys
p = sys.argv[1]
b = bytearray(2 << 20)
b[446] = 0x80; b[446 + 4] = 0x0C
struct.pack_into("<II", b, 446 + 8, 2048, len(b) // 512 - 2048)
b[510:512] = b"\x55\xaa"
bs = 2048 * 512
b[bs:bs + 3] = b"\xeb\x58\x90"
struct.pack_into("<H", b, bs + 11, 512)
b[bs + 71:bs + 82] = b"XEPROBE    "
b[bs + 82:bs + 90] = b"FAT32   "
b[bs + 510:bs + 512] = b"\x55\xaa"
for i in range(bs + 4096, len(b), 4096): b[i:i + 16] = hashlib.sha256(str(i).encode()).digest()[:16]
open(p, "wb").write(b)
open(p + ".sha256", "w").write(hashlib.sha256(b).hexdigest() + "  " + os.path.basename(p) + "\n")
PY
python3 -I "$R/tools/probe-stick/zip-image.py" zip "$T/xe-probe-stick-0.1.0.img" "$HERE" --version 0.1.0
N=intel-gpu-macos-driver-0.1.5
for f in "$N-tahoe.zip" "$N-sequoia.zip" "$N-efi.zip"; do printf '%s\n' "$f" > "$T/$f"; done
(cd "$T" && shasum -a 256 "$N-tahoe.zip" "$N-sequoia.zip" "$N-efi.zip") > "$HERE/$N.sha256"
(cd "$HERE" && shasum -a 256 xe-probe-stick-0.1.0.img.zip) >> "$HERE/$N.sha256"
swift "$R/release/signing.swift" keygen "$T/test.key" > "$HERE/test-key.txt"
rm -f "$HERE/$N.sha256.sig"
swift "$R/release/signing.swift" sign "$T/test.key" "$HERE/$N.sha256"
swift "$R/release/signing.swift" verify "$(cat "$HERE/test-key.txt")" "$HERE/$N.sha256" "$HERE/$N.sha256.sig"
python3 -I - "$HERE" <<'PY'
import json, os, sys
here = sys.argv[1]
repo = "yusufklncc/intel-gpu-macos-driver"
n = [0]
def asset(tag, name, size=None):
    n[0] += 1
    p = os.path.join(here, name)
    size = size if size is not None else (os.path.getsize(p) if os.path.exists(p) else 1000 + n[0])
    return {
        "url": f"https://api.github.com/repos/{repo}/releases/assets/{300000000 + n[0]}", "id": 300000000 + n[0],
        "node_id": f"RA_kwDOFixture{n[0]:04d}", "name": name, "label": "", "content_type": "application/zip" if name.endswith(".zip") else "application/octet-stream",
        "state": "uploaded", "size": size, "digest": None, "download_count": 0,
        "created_at": "2026-11-12T20:00:00Z", "updated_at": "2026-11-12T20:00:00Z",
        "browser_download_url": f"https://github.com/{repo}/releases/download/{tag}/{name}",
    }
def release(tag, names, draft=False, pre=False, rid=0):
    return {
        "url": f"https://api.github.com/repos/{repo}/releases/{rid}", "assets_url": f"https://api.github.com/repos/{repo}/releases/{rid}/assets",
        "upload_url": f"https://uploads.github.com/repos/{repo}/releases/{rid}/assets{{?name,label}}",
        "html_url": f"https://github.com/{repo}/releases/tag/{tag}", "id": rid, "node_id": f"RE_kwDOFixture{rid}",
        "tag_name": tag, "target_commitish": "main", "name": f"intel-gpu-macos-driver {tag[1:]}", "draft": draft, "immutable": False,
        "prerelease": pre, "created_at": "2026-11-12T19:00:00Z", "updated_at": "2026-11-12T20:00:00Z", "published_at": None if draft else "2026-11-12T20:00:00Z",
        "assets": [asset(tag, x) for x in names],
        "tarball_url": f"https://api.github.com/repos/{repo}/tarball/{tag}", "zipball_url": f"https://api.github.com/repos/{repo}/zipball/{tag}",
        "body": "notes",
    }
def pkg(v, probe=None):
    N = f"intel-gpu-macos-driver-{v}"
    a = [f"{N}-tahoe.zip", f"{N}-sequoia.zip", f"{N}-efi.zip", f"IntelXe-Manager-{v}.zip", f"IntelXeManager-{v}-win-x64.zip"]
    if probe: a.append(probe)
    return a + [f"{N}.sha256", f"{N}.sha256.sig"]
releases = [
    release("v0.1.7", pkg("0.1.7", "xe-probe-stick-0.3.0.img.zip"), draft=True, rid=7),
    release("v0.1.6", pkg("0.1.6", "xe-probe-stick-0.2.0.img.zip"), pre=True, rid=6),
    release("v0.1.5", pkg("0.1.5", "xe-probe-stick-0.1.0.img.zip"), rid=5),
    release("v0.1.4", pkg("0.1.4"), rid=4),
]
json.dump(releases, open(os.path.join(here, "releases.json"), "w"), indent=2)
json.dump([release("v0.1.4", pkg("0.1.4"), rid=4), release("v0.1.3", pkg("0.1.3")[:3] + pkg("0.1.3")[-2:], rid=3)],
          open(os.path.join(here, "releases-none.json"), "w"), indent=2)
PY
ls -l "$HERE"
