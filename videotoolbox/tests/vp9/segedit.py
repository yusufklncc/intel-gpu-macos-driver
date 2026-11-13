#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""segedit.py <in.ivf> <out.ivf>: a VP9 stream with the same pictures, whose segmented frames code the segment map
without segmentation_update_data wherever the data they code equals what the decoder already holds (the feature data
and abs_or_delta_update persist until a frame updates them, 7.2 / libvpx setup_segmentation): a valid stream, the
same decode, but one that libvpx's own encoders never produce (they send the data with every map). Made to test the
two segmentation values libavcodec 9.0.2's Vulkan hwaccel takes from CBS's stale copies (vtdirect_vp9.h): with the
data left out, CBS keeps the previous frame's tree / prediction probabilities and vulkan_vp9.c passes those.

Only the uncompressed header is rewritten (re-packed to its byte alignment); the compressed header and the tiles are
copied, the superframe indexes and the IVF frame sizes recomputed. Prints how many frames were changed."""
import struct
import sys


class Reader:
    def __init__(self, d):
        self.d, self.pos, self.out = d, 0, []

    def f(self, n, keep=True):
        v = 0
        for _ in range(n):
            b = (self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
            self.pos += 1
            v = v << 1 | b
            if keep:
                self.out.append(b)
        return v

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.out.append(v >> i & 1)


refs = [(0, 0)] * 8
feat = None   # (abs, [[enabled, value, sign] x4] x8) the decoder holds
changed = 0


def frame(d):
    """the rewritten frame"""
    global refs, feat, changed
    r = Reader(d)
    if r.f(2) != 2:
        return d
    lo, hi = r.f(1), r.f(1)
    profile = hi << 1 | lo
    if profile == 3:
        r.f(1)
    if r.f(1):   # show_existing_frame
        return d
    ftype, show, er = r.f(1), r.f(1), r.f(1)

    def color():
        if profile >= 2:
            r.f(1)
        cs = r.f(3)
        if cs != 7:
            r.f(1)
            if profile in (1, 3):
                r.f(3)
        elif profile in (1, 3):
            r.f(1)

    intra_only, refresh, w, h = 0, 0xff, 0, 0
    if ftype == 0:
        r.f(24); color()
        w, h = r.f(16) + 1, r.f(16) + 1
        if r.f(1):
            r.f(32)
    else:
        intra_only = 0 if show else r.f(1)
        if not er:
            r.f(2)
        if intra_only:
            r.f(24)
            if profile > 0:
                color()
            refresh = r.f(8)
            w, h = r.f(16) + 1, r.f(16) + 1
            if r.f(1):
                r.f(32)
        else:
            refresh = r.f(8)
            idx = []
            for _ in range(3):
                idx.append(r.f(3)); r.f(1)
            found = False
            for i in range(3):
                if r.f(1):
                    w, h = refs[idx[i]]; found = True
                    break
            if not found:
                w, h = r.f(16) + 1, r.f(16) + 1
            if r.f(1):
                r.f(32)
            r.f(1)
            if not r.f(1):
                r.f(2)
    if not er:
        r.f(2)
    r.f(2)
    if ftype == 0 or intra_only or er or feat is None:
        feat = (0, [[[0, 0, 0] for _ in range(4)] for _ in range(8)])
    # loop filter
    r.f(6); r.f(3)
    if r.f(1) and r.f(1):
        for _ in range(6):
            if r.f(1):
                r.f(7)
    r.f(8)
    for _ in range(3):
        if r.f(1):
            r.f(5)
    # segmentation: the place of the edit
    if r.f(1):
        upd_map = r.f(1)
        if upd_map:
            for _ in range(7):
                if r.f(1):
                    r.f(8)
            if r.f(1):
                for _ in range(3):
                    if r.f(1):
                        r.f(8)
        at = len(r.out)
        if r.f(1):   # segmentation_update_data
            bits, signed = (8, 6, 2, 0), (1, 1, 0, 0)
            ab = r.f(1)
            new = []
            for i in range(8):
                row = []
                for j in range(4):
                    en = r.f(1)
                    val = sign = 0
                    if en and bits[j]:
                        val = r.f(bits[j])
                        if signed[j]:
                            sign = r.f(1)
                    row.append([en, val, sign])
                new.append(row)
            if upd_map and (ab, new) == feat and ftype != 0 and not intra_only:
                del r.out[at:]
                r.put(0, 1)   # segmentation_update_data = 0: the decoder keeps the same data
                changed += 1
            feat = (ab, new)
    # tile info
    sb64 = (((w + 7) >> 3) + 7) >> 3
    mn = 0
    while (64 << mn) < sb64:
        mn += 1
    mx = 0
    while (sb64 >> (mx + 1)) >= 4:
        mx += 1
    c = mn
    while c < mx and r.f(1):
        c += 1
    if r.f(1):
        r.f(1)
    r.f(16)   # header_size_in_bytes
    end = (r.pos + 7) // 8
    for i in range(8):
        if refresh >> i & 1:
            refs[i] = (w, h)
    out = r.out + [0] * (-len(r.out) % 8)
    hdr = bytes(int(''.join(str(b) for b in out[i:i + 8]), 2) for i in range(0, len(out), 8))
    return hdr + d[end:]


def sample(s):
    m = s[-1]
    if m & 0xe0 == 0xc0:
        length, count = 1 + ((m >> 3) & 3), 1 + (m & 7)
        isz = 2 + count * length
        if len(s) >= isz and s[len(s) - isz] == m:
            frames, pos = [], 0
            for i in range(count):
                at = len(s) - isz + 1 + i * length
                v = int.from_bytes(s[at:at + length], 'little')
                frames.append(frame(s[pos:pos + v])); pos += v
            length = max(1, max((len(f).bit_length() + 7) // 8 for f in frames))
            mk = 0xc0 | (length - 1) << 3 | (count - 1)
            idx = bytes([mk]) + b''.join(len(f).to_bytes(length, 'little') for f in frames) + bytes([mk])
            return b''.join(frames) + idx
    return frame(s)


src = open(sys.argv[1], 'rb').read()
hl = struct.unpack('<H', src[6:8])[0]
out, pos = [src[:hl]], hl
while pos + 12 <= len(src):
    sz = struct.unpack('<I', src[pos:pos + 4])[0]
    s = sample(src[pos + 12:pos + 12 + sz])
    out.append(struct.pack('<I', len(s)) + src[pos + 4:pos + 12] + s)
    pos += 12 + sz
open(sys.argv[2], 'wb').write(b''.join(out))
print('segedit: %d frames code the map without the data' % changed)
