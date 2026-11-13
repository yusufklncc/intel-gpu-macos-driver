#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mkgolden.py <stream.ivf> [ffmpeg]: the reference output of vp9parse_test for a recorded VP9 stream, made from FFmpeg
9.0.2 (the version the plug-ins ship) without our C code: every syntax element of every frame's uncompressed header as
FFmpeg's trace_headers bitstream filter prints it (CBS, with its bit position), the samples as the IVF file holds them,
the superframe index read here from the samples' bytes (as vp9_superframe_split reads it), and, derived here from the
VP9 specification the way libavcodec's VP9 decoder and Vulkan hwaccel derive them (vp9.c, vulkan_vp9.c,
cbs_vp9_syntax_template.c), everything Vulkan Video gets for each frame: the loop filter deltas and segmentation
features carried from frame to frame, the reference slots' sizes and pictures, the DPB slot of each picture (the
lowest one no reference slot holds), the references and reference names, UsePrevFrameMvs, the Std enums, and where the
compressed header and the tiles start. Where vulkan_vp9.c passes CBS's stale segmentation values (abs_or_delta_update
of a frame that does not update the data; the tree / prediction probabilities of a frame that codes the map but not
the data) the golden has the specification's and a "lavc" line with libavcodec's. The number of shown frames is
checked against libvpx (libvpx-vp9 through ffmpeg, a decoder that is not libavcodec's; it outputs one picture per
sample, so the samples that show a frame are counted); on a mismatch the golden is
still written, the difference goes to stderr and the exit status is 1. Writes to stdout.

Lines (the same as vp9parse_test prints):
  frame tu= k= type= show= intra= er= prof= size=WxH bd= refresh=<hex> dpb= hb=<header bits before the trailing bits>
      hdr=<uncompressed header bytes>/<compressed header bytes> at=<offset>:<size of the frame in the sample> ctx= reset=
      rctx= par= hp= prev=<UsePrevFrameMvs> sb=<ref_frame_sign_bias_mask> interp=<Std enum> q=<base>/<y dc>/<uv dc>/<uv ac>
      tiles=<cols log2>/<rows log2> color=<range>/<BitDepth>/<color_space> missing=
   refs idx=<ref_frame_idx> names=<referenceNameSlotIndices> list=<dpb>@<slot>:<w>x<h>,... (or -)
   lf level= sharp= en= upd= uref=<mask> umode=<mask> ref=<4 deltas> mode=<2 deltas>
   seg map= temp= data= abs= tree=<7> pred=<3> feat=<FeatureEnabled hex>:<FeatureData x4>,... x8   (segmented frames)
   lavc abs= tree= pred=          libavcodec's values, where they differ and matter
  show tu= k= slot= dpb= size= missing=    a show_existing_frame
  tu <n> frames= shown= extra= sf=<superframe index>"""
import re
import struct
import subprocess
import sys

stream = sys.argv[1]
ffmpeg = sys.argv[2] if len(sys.argv) > 2 else 'ffmpeg'
problems = []


def run(args):
    p = subprocess.run([ffmpeg, '-hide_banner', '-nostdin'] + args, capture_output=True)
    return p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace').splitlines()


# --- the samples: IVF frames
data = open(stream, 'rb').read()
if data[:4] != b'DKIF':
    sys.exit('%s: not an IVF file' % stream)
samples = []
pos = struct.unpack('<H', data[6:8])[0]
while pos + 12 <= len(data):
    sz = struct.unpack('<I', data[pos:pos + 4])[0]
    pos += 12
    if sz > len(data) - pos:
        break
    samples.append(data[pos:pos + sz])
    pos += sz


def split(sample):
    """[(offset, size)] of the frames of a sample (Annex B; vp9_superframe_split's checks), superframe flag"""
    m = sample[-1]
    if m & 0xe0 == 0xc0:
        length, count = 1 + ((m >> 3) & 3), 1 + (m & 7)
        isz = 2 + count * length
        if len(sample) >= isz and sample[len(sample) - isz] == m:
            out, total = [], 0
            for i in range(count):
                at = len(sample) - isz + 1 + i * length
                v = int.from_bytes(sample[at:at + length], 'little')
                total += v
                if v == 0 or total > len(sample) - isz:
                    raise ValueError('invalid superframe index')
                out.append((total - v, v))
            return out, 1
    return [(0, len(sample))], 0


# --- trace_headers: per packet, per frame, the elements
_, err = run(['-i', stream, '-c', 'copy', '-copyinkf', '-bsf:v', 'trace_headers', '-f', 'null', '-'])
LINE = re.compile(r'^\[trace_headers @ [0-9a-fx]+\] (.*)$')
ELEM = re.compile(r'^(\d+)\s+(\S+)\s+(?:([01]+)\s+)?=\s+(-?\d+)$')
packets = []   # [[frame: {'v': {name: value}, 'end': bits}]]
cur = None
for l in err:
    m = LINE.match(l)
    if not m:
        continue
    c = m.group(1).strip()
    if c.startswith('Packet:'):
        cur = []; packets.append(cur); continue
    if cur is None:
        continue
    if c == 'Frame':
        cur.append({'v': {}, 'hb': 0, 'end': 0}); continue
    if c == 'Superframe Index':
        cur.append(None); continue   # the index's own elements are not a frame's
    e = ELEM.match(c)
    if e and cur and cur[-1] is not None:
        f = cur[-1]
        p, name, bits, val = int(e.group(1)), e.group(2), e.group(3), int(e.group(4))
        f['v'][name] = val
        n = len(bits) if bits else 0
        f['end'] = max(f['end'], p + n)
        if name != 'zero_bit':
            f['hb'] = max(f['hb'], p + n)
for p in packets:
    p[:] = [f for f in p if f is not None]
if len(packets) != len(samples):
    problems.append('trace_headers saw %d packets, the file has %d samples' % (len(packets), len(samples)))

# --- the shown frames, from libvpx (not libavcodec's decoder)
out, _ = run(['-c:v', 'libvpx-vp9', '-i', stream, '-f', 'framecrc', '-'])
libvpx_frames = len([l for l in out.splitlines() if l and not l.startswith('#')])

# --- the state the uncompressed header carries (CBS's context, vp9.c's) and the reference slots
REMAP = [1, 0, 2, 3]   # raw interpolation filter -> StdVideoVP9InterpolationFilter (EIGHTTAP_SMOOTH, EIGHTTAP, SHARP, BILINEAR)
st = {
    'lf_ref': [1, 0, -1, -1], 'lf_mode': [0, 0], 'fen': [[0] * 4 for _ in range(8)], 'fval': [[0] * 4 for _ in range(8)], 'abs': 0,
    'tree': [0] * 7, 'pred': [0] * 3, 'cbs_tree': [0] * 7, 'cbs_pred': [0] * 3,
    'cs': 0, 'range': 0, 'bd': 0, 'last_invisible': 0, 'have_last': 0, 'last_w': 0, 'last_h': 0,
}
refs = [{'valid': 0, 'w': 0, 'h': 0, 'bd': 0, 'pic': 0, 'dpb': -1, 'pw': 0, 'ph': 0} for _ in range(8)]
outl = []
shown_total = 0


def past_independence():
    st['lf_ref'] = [1, 0, -1, -1]; st['lf_mode'] = [0, 0]
    st['fen'] = [[0] * 4 for _ in range(8)]; st['fval'] = [[0] * 4 for _ in range(8)]; st['abs'] = 0


def csv(v):
    return ','.join(str(x) for x in v)


for tu, sample in enumerate(samples):
    frames, sf = split(sample)
    tr = packets[tu] if tu < len(packets) else []
    if len(tr) != len(frames):
        problems.append('tu %d: trace_headers has %d frames, the superframe index %d' % (tu, len(tr), len(frames)))
    shown, extra = -1, 0
    for k, (off, size) in enumerate(frames):
        v = tr[k]['v'] if k < len(tr) else {}
        g = lambda name, d=0: v.get(name, d)
        if g('show_existing_frame'):
            slot = g('frame_to_show_map_idx')
            r = refs[slot]
            outl.append('show tu=%d k=%d slot=%d dpb=%d size=%dx%d missing=%d' % (
                tu, k, slot, r['dpb'] if r['pic'] else -1, r['pw'], r['ph'], 0 if r['pic'] else 1))
            if shown < 0:
                shown = k
            else:
                extra += 1
            continue
        profile = g('profile_high_bit') << 1 | g('profile_low_bit')
        ftype, show, er = g('frame_type'), g('show_frame'), g('error_resilient_mode')
        intra_only = g('intra_only')
        intra = ftype == 0 or intra_only
        refresh = 0xff if ftype == 0 else g('refresh_frame_flags')
        idx = [0, 0, 0] if intra else [g('ref_frame_idx[%d]' % i) for i in range(3)]
        new_color = False
        if ftype == 0 or (intra_only and profile > 0):
            bd = 8 if profile < 2 else (12 if g('ten_or_twelve_bit') else 10)
            cs, rng = g('color_space'), g('color_range')
            new_color = True
        elif intra_only:
            bd, cs, rng = 8, 1, 0
            new_color = True
        else:
            bd = st['bd'] or 8
        if intra:
            w, h = g('frame_width_minus_1') + 1, g('frame_height_minus_1') + 1
        else:
            found = [i for i in range(3) if g('found_ref[%d]' % i)]
            if found:
                r = refs[idx[found[0]]]
                w, h = r['w'], r['h']
                if r['valid']:
                    bd = r['bd']
            else:
                w, h = g('frame_width_minus_1') + 1, g('frame_height_minus_1') + 1
        # the header: its bits, the compressed header after it
        hb = tr[k]['hb'] if k < len(tr) else 0
        uncomp = (tr[k]['end'] + 7) // 8 if k < len(tr) else 0
        comp = g('header_size_in_bytes')
        if comp == 0 or comp > size - uncomp or sample[off + uncomp] & 0x80:
            problems.append('tu %d frame %d: a compressed header vp9.c refuses' % (tu, k))
        # the carried state
        last_invisible = st['last_invisible']
        st['last_invisible'] = 0 if show else 1
        if intra or er:
            past_independence()
        if g('loop_filter_delta_update'):
            for i in range(4):
                if g('update_ref_delta[%d]' % i):
                    st['lf_ref'][i] = g('loop_filter_ref_deltas[%d]' % i)
            for i in range(2):
                if g('update_mode_delta[%d]' % i):
                    st['lf_mode'][i] = g('loop_filter_mode_deltas[%d]' % i)
        seg_en, upd_map, upd_data = g('segmentation_enabled'), g('segmentation_update_map'), g('segmentation_update_data')
        temporal = g('segmentation_temporal_update') if upd_map else 0
        tree = [g('segmentation_tree_probs[%d].prob' % i, 255) if g('segmentation_tree_probs[%d].prob_coded' % i) else 255 for i in range(7)]
        pred = [g('segmentation_pred_prob[%d].prob' % i, 255) if temporal and g('segmentation_pred_prob[%d].prob_coded' % i) else 255 for i in range(3)]
        if seg_en and upd_data:
            st['abs'] = g('segmentation_abs_or_delta_update')
            for i in range(8):
                for j in range(4):
                    st['fen'][i][j] = g('feature_enabled[%d][%d]' % (i, j))
                    val = g('feature_value[%d][%d]' % (i, j)) if st['fen'][i][j] else 0
                    st['fval'][i][j] = -val if g('feature_sign[%d][%d]' % (i, j)) and st['fen'][i][j] else val
            if upd_map:
                st['cbs_tree'], st['cbs_pred'] = list(tree), list(pred)
        if seg_en and upd_map:
            st['tree'], st['pred'] = list(tree), list(pred)
        if new_color:
            st['cs'], st['range'], st['bd'] = cs, rng, bd
        missing = 0 if intra else int(any(not refs[i]['pic'] for i in idx))
        use_prev = int(not er and not last_invisible and (intra or (st['have_last'] and st['last_w'] == w and st['last_h'] == h)))
        for i in range(8):
            if refresh >> i & 1:
                refs[i].update(valid=1, w=w, h=h, bd=bd)
        sb = g('ref_frame_sign_bias[1]') << 1 | g('ref_frame_sign_bias[2]') << 2 | g('ref_frame_sign_bias[3]') << 3
        interp = 4 if g('is_filter_switchable') else REMAP[g('raw_interpolation_filter_type')]
        dpb, names, lst = -1, [-1, -1, -1], []
        if not missing:
            busy = {r['dpb'] for r in refs if r['pic']}
            dpb = min(i for i in range(32) if i not in busy)
            for i in range(3):
                r = refs[idx[i]]
                if not r['pic']:
                    continue
                names[i] = r['dpb']
                if r['dpb'] not in [x[0] for x in lst]:
                    lst.append((r['dpb'], idx[i], r['pw'], r['ph']))
            for i in range(8):
                if refresh >> i & 1:
                    refs[i].update(pic=1, dpb=dpb, pw=w, ph=h)
            st['have_last'], st['last_w'], st['last_h'] = 1, w, h
        dq = [g('%s.delta_q' % n) if g('%s.delta_coded' % n) else 0 for n in ('delta_q_y_dc', 'delta_q_uv_dc', 'delta_q_uv_ac')]
        outl.append('frame tu=%d k=%d type=%d show=%d intra=%d er=%d prof=%d size=%dx%d bd=%d refresh=%02x dpb=%d hb=%d hdr=%d/%d at=%d:%d '
                    'ctx=%d reset=%d rctx=%d par=%d hp=%d prev=%d sb=%x interp=%d q=%d/%d/%d/%d tiles=%d/%d color=%d/%d/%d missing=%d' % (
                        tu, k, ftype, show, intra_only, er, profile, w, h, bd, refresh, dpb, hb, uncomp, comp, off, size,
                        g('frame_context_idx'), g('reset_frame_context'), g('refresh_frame_context'),
                        g('frame_parallel_decoding_mode', 1 if er else 0), g('allow_high_precision_mv'), use_prev, sb, interp,
                        g('base_q_idx'), dq[0], dq[1], dq[2], g('tile_cols_log2'), g('tile_rows_log2'), st['range'], st['bd'], st['cs'], missing))
        outl.append(' refs idx=%s names=%s list=%s' % (csv(idx), csv(names),
                                                       ','.join('%d@%d:%dx%d' % x for x in lst) if lst else '-'))
        uref = sum(g('update_ref_delta[%d]' % i) << i for i in range(4)) if g('loop_filter_delta_update') else 0
        umode = sum(g('update_mode_delta[%d]' % i) << i for i in range(2)) if g('loop_filter_delta_update') else 0
        outl.append(' lf level=%d sharp=%d en=%d upd=%d uref=%x umode=%x ref=%s mode=%s' % (
            g('loop_filter_level'), g('loop_filter_sharpness'), g('loop_filter_delta_enabled'), g('loop_filter_delta_update'),
            uref, umode, csv(st['lf_ref']), csv(st['lf_mode'])))
        if seg_en:
            fe = [sum(st['fen'][i][j] << j for j in range(4)) for i in range(8)]
            outl.append(' seg map=%d temp=%d data=%d abs=%d tree=%s pred=%s feat=%s' % (
                upd_map, temporal, upd_data, st['abs'], csv(st['tree']), csv(st['pred']),
                ','.join('%x:%s' % (fe[i], '/'.join(str(x) for x in st['fval'][i])) for i in range(8))))
            lavc_abs = g('segmentation_abs_or_delta_update') if upd_data else 0
            ql = any(e & 3 for e in fe)
            if (upd_map and (st['cbs_tree'] != st['tree'] or st['cbs_pred'] != st['pred'])) or (ql and lavc_abs != st['abs']):
                outl.append(' lavc abs=%d tree=%s pred=%s' % (lavc_abs, csv(st['cbs_tree']), csv(st['cbs_pred'])))
        if show:
            if shown < 0:
                shown = k
            else:
                extra += 1
    if shown >= 0:   # libvpx hands out one picture per sample (the last shown frame of a superframe: SVC layers)
        shown_total += 1
    outl.append('tu %d frames=%d shown=%d extra=%d sf=%d' % (tu, len(frames), shown, extra, sf))

if shown_total != libvpx_frames:
    problems.append('%d shown frames here, libvpx decodes %d' % (shown_total, libvpx_frames))
sys.stdout.write('\n'.join(outl) + '\n')
for p in problems:
    sys.stderr.write('mkgolden: %s\n' % p)
sys.exit(1 if problems else 0)
