#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""published-md5.py <zip>: the MD5s of the decoded pictures a JCT-VC conformance zip publishes (its *_yuv.md5, *.md5 or
*md5*.txt files; *_bin.md5 files and lines that name the bitstream, .bit / .bin without "yuv", are the bitstream's own
and skipped), on one line, the yuv ones first; nothing if it publishes none. Reads the zip as data (python3 -I)."""
import re
import sys
import zipfile

found = []
with zipfile.ZipFile(sys.argv[1]) as z:
    for name in z.namelist():
        low = name.lower()
        if '__macosx' in low or 'md5' not in low or '_bin' in low:   # (SLIST_D_Sony_9_bin.md5: the bitstream's)
            continue
        for line in z.read(name).decode('latin-1').splitlines():
            if re.search(r'\.(bit|bin)\b', line, re.I) and 'yuv' not in line.lower():
                continue
            m = re.search(r'\b([0-9a-fA-F]{32})\b', line)
            if m:
                found.append((0 if 'yuv' in low or 'yuv' in line.lower() else 1, m.group(1).lower()))
print(' '.join(h for _, h in sorted(found)))
