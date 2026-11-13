#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""dpbslots.py [--slots N] [--expect-old-over] <golden.txt | stream.bit> ...: the DPB slot numbers FFmpeg's Vulkan HEVC
hwaccel passes to the driver, replayed on this Mac (no GPU), before and after
ffmpeg-9.0.2-vulkan-decode-dpb-slots-below-maxDpbSlots.patch.

Before: the slotIndex of a picture was its index in libavcodec's DPB array (HEVCLayerContext.DPB, 32 entries). The
replay follows hevcdec.c's hevc_frame_start() and refs.c over a golden file of mkgolden.py (made here from a .bit with
FFmpeg 9.x if a .bit is given): a new sequence (IDR / BLA) outputs every waiting picture, an IDR drops the references,
alloc_frame() takes the lowest free entry for the new picture, ff_hevc_frame_rps() keeps the references the golden
file lists and frees the rest that do not wait for output, then ff_hevc_output_frames() bumps while more pictures wait
than sps_max_num_reorder_pics or the DPB holds more than sps_max_dec_pic_buffering; start_frame came after that, with
the entry taken before the release and the bumping. On a 16-picture DPB the index reaches 16 (the 13 Nov B580 crash:
RPS_B/C/E_qualcomm/ericsson, ANV's 16 slots).

After: each reference keeps the slot it was decoded into and the new picture takes the lowest slot none of the
references holds (vk_hevc_assign_slots). Checked for every picture: the slot is below N (--slots, default 16: ANV's
maxDpbSlots for H.265 decode), no two references share one, a reference's slot never changes.

Assumes pic_output_flag 1 (warns when a PPS has output_flag_present_flag) and no end of sequence NAL units (the golden
format has none). Exit 0 when every stream passes; 1 when one fails (or, with --expect-old-over, when the old
numbering of a stream never reached N: the replay is then not the one that explains the crash)."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def golden_lines(path):
    if not path.endswith(('.bit', '.bin', '.265')):
        return open(path).read().splitlines()
    with tempfile.TemporaryFile('w+') as g:
        subprocess.run([sys.executable, os.path.join(HERE, 'mkgolden.py'), path], stdout=g, stderr=subprocess.DEVNULL)
        g.seek(0)
        return g.read().splitlines()


def kv(line):
    return dict(f.split('=', 1) for f in line.split()[1:] if '=' in f)


def replay(lines, nslots):
    sps = {}            # id -> (max_dec_pic_buffering, num_reorder_pics)
    cur_sps = None
    warnings, errors = [], []
    dpb = [None] * 32   # entry: dict(poc, out, ref, slot)
    old_max, old_over, new_max, npic = -1, 0, -1, 0
    first = True
    for line in lines:
        word = line.split(' ', 1)[0]
        if word == 'sps':
            f = kv(line)
            d = [int(x) for x in f['dpb'].split('/')]
            sps[f['id']] = (d[0] + 1, d[1])
            if cur_sps is not None and sps[f['id']] != cur_sps:
                warnings.append('a second SPS with other DPB sizes: the replay uses the last one')
            cur_sps = sps[f['id']]
            continue
        if word == 'pps':
            if kv(line).get('ofp') == '1':
                warnings.append('output_flag_present_flag 1: pic_output_flag taken as 1')
            continue
        if word != 'pic':
            continue
        name = line.split()[1]
        f = kv(line)
        poc = int(f['poc'])
        held = set() if f['dpb'] == '-' else {int(x.split(':')[0]) for x in f['dpb'].split(',')}
        max_dec, reorder = cur_sps
        idr, bla = name.startswith('IDR'), name.startswith('BLA')

        def free_unused():
            for i, e in enumerate(dpb):
                if e and not e['out'] and not e['ref']:
                    dpb[i] = None

        if (idr or bla) and not first:      # new_sequence: output (or discard) every waiting picture
            for e in dpb:
                if e:
                    e['out'] = False
            free_unused()
        if idr:                             # ff_hevc_clear_refs()
            for e in dpb:
                if e:
                    e['ref'] = False
            free_unused()
        first = False
        idx = dpb.index(None)               # alloc_frame()
        cur = dpb[idx] = dict(poc=poc, out=True, ref=True, slot=None)
        for e in dpb:                       # ff_hevc_frame_rps()
            if e and e is not cur:
                e['ref'] = e['poc'] in held
        missing = held - {e['poc'] for e in dpb if e and e is not cur}
        if missing:
            warnings.append('POC %d: references %s not in the replayed DPB' % (poc, sorted(missing)))
        free_unused()
        while True:                         # ff_hevc_output_frames()
            waiting = [e for e in dpb if e and e['out']]
            used = sum(1 for e in dpb if e)
            if waiting and (len(waiting) > reorder or used > max_dec):
                e = min(waiting, key=lambda x: x['poc'])
                e['out'] = False
                if not e['ref']:
                    dpb[dpb.index(e)] = None
                continue
            break
        # start_frame: the old slot number (the DPB index) and the new one
        npic += 1
        old_max = max(old_max, idx)
        old_over += idx >= nslots
        refs = [e for e in dpb if e and e is not cur and e['ref']]
        taken = set()
        for e in refs:
            if e['slot'] is None:
                errors.append('POC %d: reference POC %d was never given a slot' % (poc, e['poc']))
                continue
            if e['slot'] in taken:
                errors.append('POC %d: slot %d held by two references' % (poc, e['slot']))
            taken.add(e['slot'])
        s = next((i for i in range(nslots) if i not in taken), None)
        if s is None:
            errors.append('POC %d: no free slot (%d references)' % (poc, len(refs)))
            s = nslots
        cur['slot'] = s
        new_max = max(new_max, s)
    return dict(pictures=npic, old_max=old_max, old_over=old_over, new_max=new_max, warnings=warnings,
                errors=errors)


def main():
    args = sys.argv[1:]
    nslots, expect_over = 16, False
    while args and args[0].startswith('--'):
        a = args.pop(0)
        if a == '--slots':
            nslots = int(args.pop(0))
        elif a == '--expect-old-over':
            expect_over = True
        else:
            sys.exit(__doc__)
    if not args:
        sys.exit(__doc__)
    bad = 0
    for path in args:
        name = os.path.basename(path).rsplit('.', 1)[0]
        r = replay(golden_lines(path), nslots)
        ok = not r['errors'] and r['new_max'] < nslots and r['pictures'] > 0
        if expect_over and r['old_over'] == 0:
            ok = False
        print('%s %s: %d pictures; DPB index as slot (before): max %d, %d at or above %d; '
              'compact slots (after): max %d%s' % (
                  'PASS' if ok else 'FAIL', name, r['pictures'], r['old_max'], r['old_over'], nslots, r['new_max'],
                  ''.join('; ' + e for e in r['errors'][:3])))
        for w in sorted(set(r['warnings']))[:3]:
            print('  warning: ' + w)
        bad += not ok
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
