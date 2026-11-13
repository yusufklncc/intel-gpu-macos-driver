#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""inband-hvc1.py <file.mp4>: renames the HEVC sample entry of an MP4 that FFmpeg wrote with -tag:v hev1 to 'hvc1', in
place, nothing else changed. FFmpeg's MP4 muxer drops every in-band VPS/SPS/PPS from an 'hvc1' track (the sample entry
is to hold them all), so a conformance stream that sends other parameter sets mid-stream (a PPS per picture: INITQP_*,
SLIST_*, TILES_*, DELTAQP_*) loses them; 'hev1' keeps them, but VideoToolbox finds no decoder for 'hev1' (Apple's:
kVTCouldNotFindVideoDecoderErr). An 'hvc1' entry with the parameter sets also in band is what the decoders under test
(and Apple's) take, so the kit's streams decode as published with any of them. Walks the boxes (moov/trak/mdia/minf/
stbl/stsd); exit 1 if there is no 'hev1' entry."""
import struct
import sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl'}


def walk(f, start, end):
    """the offset of the first 'hev1' sample entry between start and end, or None"""
    pos = start
    while pos + 8 <= end:
        f.seek(pos)
        size, kind = struct.unpack('>I4s', f.read(8))
        hdr = 8
        if size == 1:
            size = struct.unpack('>Q', f.read(8))[0]; hdr = 16
        elif size == 0:
            size = end - pos
        if size < hdr or pos + size > end:
            return None
        if kind in CONTAINERS:
            r = walk(f, pos + hdr, pos + size)
            if r is not None:
                return r
        elif kind == b'stsd':   # FullBox: version/flags, entry_count, then the sample entries (boxes)
            f.seek(pos + hdr + 4)
            n = struct.unpack('>I', f.read(4))[0]
            e = pos + hdr + 8
            for _ in range(n):
                f.seek(e)
                esize, ekind = struct.unpack('>I4s', f.read(8))
                if ekind == b'hev1':
                    return e + 4
                if esize < 8:
                    break
                e += esize
        pos += size
    return None


with open(sys.argv[1], 'r+b') as f:
    f.seek(0, 2)
    at = walk(f, 0, f.tell())
    if at is None:
        sys.exit(f'{sys.argv[1]}: no hev1 sample entry')
    f.seek(at)
    f.write(b'hvc1')
