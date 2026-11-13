#!/bin/bash
# SPDX-License-Identifier: MIT
# run.sh OUTDIR: layertest on Apple's GL, then through libcglzink with the stand-in EGL (fakeegl.c), then with the stand-in
# refusing what zink does not resolve, then also reporting zink's version and extensions (4.6, GL_ARB_buffer_storage).
# What layertest prints (stdout: pixel hashes, versions, extensions) must be the same in every run; the layer's and the
# stand-in's reports go to stderr. No GPU of ours needed: this checks the layer's plumbing, not zink.
# LAYERTEST_VIEW=1 in the environment adds a view in a window that is never shown.
set -uo pipefail
O=${1:?usage: run.sh OUTDIR (from build.sh)}
cd "$O" || exit 1
./layertest > apple.txt 2> apple.err
CGLZINK_LOG=1 CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > layer.txt 2> layer.err
CGLZINK_LOG=1 FAKEEGL_ZINK_NAMES=1 CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > zinknames.txt 2> zinknames.err
CGLZINK_LOG=1 FAKEEGL_ZINK_NAMES=1 FAKEEGL_ZINK_STRINGS=1 CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > zinkstrings.txt 2> zinkstrings.err
# CGLZINK_SURF_WRITEBACK: dirty (the default, also the runs above) and flush (a copy at every flush) must give Apple's
# pixels; =present skips the copy at glFlush, so only the IOSurface lines may differ there (a reader that syncs on
# glFlush sees the previous frame: why it is opt-in)
CGLZINK_LOG=1 CGLZINK_SURF_WRITEBACK=flush CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > flush.txt 2> flush.err
CGLZINK_LOG=1 CGLZINK_SURF_WRITEBACK=dirty CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > dirty.txt 2> dirty.err
CGLZINK_LOG=1 CGLZINK_SURF_WRITEBACK=present CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > present.txt 2> present.err
CGLZINK_OFF=1 DYLD_INSERT_LIBRARIES="$O/libcglzink.dylib" ./layertest > off.txt 2> off.err
# the "every OpenGL application" switch: libcglzink-boot inserted, libcglzink loaded by it (layertest is no application
# bundle: CGLZINK_GLOBAL_ANY lets the boot library take part anyway); then refused by the environment
CGLZINK_LOG=1 CGLZINK_GLOBAL_ANY=1 CGLZINK_LIB="$O/libcglzink.dylib" CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink-boot.dylib" ./layertest > boot.txt 2> boot.err
CGLZINK_LOG=1 CGLZINK_LIB="$O/libcglzink.dylib" CGLZINK_EGL="$O/libfakeegl.dylib" DYLD_INSERT_LIBRARIES="$O/libcglzink-boot.dylib" ./layertest > bootout.txt 2> bootout.err
cat apple.txt
r=0
for f in layer dirty flush zinknames zinkstrings off boot bootout; do
    if cmp -s apple.txt $f.txt; then echo "$f: same as Apple's GL"; else echo "$f: DIFFERENT"; diff apple.txt $f.txt; r=1; fi
done
if diff <(grep -v '^iosurface \(re\)\{0,1\}draw' apple.txt) <(grep -v '^iosurface \(re\)\{0,1\}draw' present.txt) >/dev/null; then
    echo "present: same as Apple's GL outside the IOSurface lines ($(grep -c '^iosurface \(re\)\{0,1\}draw' present.txt) lines left out)"
else echo "present: DIFFERENT outside the IOSurface lines"; r=1; fi
grep -h -E "^(layertest|fakeegl)|EGL [0-9]|used on two|swap interval|layer scale|stays|went|is gone|extensions:|told" layer.err zinknames.err zinkstrings.err | sort | uniq -c
# the boot library must have taken part (the layer adopted contexts) only in the first of its two runs
if grep -q "taking part" boot.err && grep -q "adopted" boot.err; then echo "boot: took part, contexts adopted"; else echo "boot: did NOT take part"; r=1; fi
if grep -q "taking part\|adopted" bootout.err; then echo "bootout: took part outside an application bundle"; r=1; else echo "bootout: stayed out (not an application)"; fi
exit $r
