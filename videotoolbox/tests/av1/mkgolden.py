#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mkgolden.py <stream.ivf|.obu> [ffmpeg]: the reference output of av1parse_test for a recorded AV1 stream, made from
FFmpeg 9.0.2 (the version the plug-ins ship) without our C code: every syntax element of every sequence header, frame
header and tile group as FFmpeg's trace_headers bitstream filter prints it (with its bit position), the samples as
FFmpeg's demuxer cuts them, and, derived here from the AV1 specification the way libavcodec's AV1 decoder and Vulkan
hwaccel derive them (av1dec.c, vulkan_av1.c, cbs_av1_syntax_template.c), everything Vulkan Video gets for each frame:
the inferred syntax (frame sizes from references, superres, set_frame_refs, tile layout, segmentation and loop filter
deltas loaded from the primary reference frame, global motion decoded against the previous parameters, film grain
loaded from a reference), the order hints, sign bias and skip mode frames, the DPB slot of each picture (the lowest one
no reference slot holds), the references with their reference information, and the tiles' offsets and sizes in the
sample (the tile size fields read here from the stream's bytes). The number of shown frames is checked against dav1d
(libdav1d through ffmpeg, a decoder that is not ours either); on a mismatch the golden is still written, the difference
goes to stderr and the exit status is 1. Writes to stdout.

Lines (the same as av1parse_test prints):
  seq profile= ops= op0= level= tier= w= h= bits= fid= flags=<19 sequence header flags, Std order> ohbits= sct= imv= bd=
      mono= ss= range= cdp= cp= tc= mc= csp= sep= [timing=] dm=
  frame tu= type= show= oh= prf= refresh= fid= size=<UpscaledWidth>x<FrameHeight> fw=<FrameWidth> denom= dpb= hb=<header
      bits> flags=<29 StdVideoDecodeAV1PictureInfoFlags, Std order>
   refs idx=<ref_frame_idx> names=<referenceNameSlotIndices> ohs=<OrderHints> skip=<SkipModeFrame> list=<dpb>@<slot>=<
      frame_type:OrderHint:RefFrameSignBias:disable_frame_end_update_cdf:segmentation_enabled:SavedOrderHints, _ between them> missing=
   q / seg / lf / cdef / lr / tile / gm / grain    the Std structures
   setup <the current picture's reference information> tiles n= tg= size= at=<offset in the sample>:<size>,...
  show tu= slot= dpb= refresh= size= grain= missing=    a show_existing_frame
  tu <n> frames= shown= extra="""
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


# --- the samples: IVF frames, or temporal units of a low-overhead OBU stream (ffmpeg's demuxers cut them the same way)
data = open(stream, 'rb').read()
samples = []
if data[:4] == b'DKIF':
    pos = struct.unpack('<H', data[6:8])[0]
    while pos + 12 <= len(data):
        sz = struct.unpack('<I', data[pos:pos + 4])[0]
        pos += 12
        if sz > len(data) - pos:
            break
        samples.append(data[pos:pos + sz])
        pos += sz
else:
    pos = start = 0
    while pos < len(data):
        at = pos
        b0 = data[pos]
        h = 1 + ((b0 >> 2) & 1)
        if not (b0 >> 1) & 1:
            break
        sz, shift = 0, 0
        while True:
            c = data[pos + h]; h += 1
            sz |= (c & 0x7f) << shift; shift += 7
            if not c & 0x80:
                break
        pos += h + sz
        if (b0 >> 3) & 15 == 2 and at > start:
            samples.append(data[start:at]); start = at
    if pos > start:
        samples.append(data[start:pos])


def obus_of(sample):
    """[(type, temporal_id, spatial_id, has_ext, obu start, payload start, payload size)] of a sample"""
    out, pos = [], 0
    while pos < len(sample):
        b0 = sample[pos]
        typ, ext, has_size = (b0 >> 3) & 15, (b0 >> 2) & 1, (b0 >> 1) & 1
        h, tid, sid = 1, 0, 0
        if ext:
            tid, sid = sample[pos + 1] >> 5, (sample[pos + 1] >> 3) & 3
            h = 2
        if has_size:
            sz, shift = 0, 0
            while True:
                c = sample[pos + h]; h += 1
                sz |= (c & 0x7f) << shift; shift += 7
                if not c & 0x80:
                    break
        else:
            sz = len(sample) - pos - h
        out.append((typ, tid, sid, ext, pos, pos + h, sz))
        pos += h + sz
    return out


# --- trace_headers: per packet, per OBU, the sections and their elements
_, err = run(['-i', stream, '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'])
LINE = re.compile(r'^\[trace_headers @ [0-9a-fx]+\] (.*)$')
ELEM = re.compile(r'^(\d+)\s+(\S+)\s+(?:([01]+)\s+)?=\s+(-?\d+)$')
packets = []   # [[obu: {'sections': [name, ...], 'v': {name: value}, 'order': [(section, name, value)], 'end': bits}]]
cur = None
for l in err:
    m = LINE.match(l)
    if not m:
        continue
    c = m.group(1).strip()
    if c.startswith('Packet:'):
        cur = []; packets.append(cur); continue
    if c == 'Extradata':
        cur = None; continue
    if cur is None:
        continue
    e = ELEM.match(c)
    if e:
        o = cur[-1]
        pos, name, bits, val = int(e.group(1)), e.group(2), e.group(3), int(e.group(4))
        o['v'][name] = val
        o['order'].append((o['sections'][-1], name, val))
        if bits is not None:
            o['end'] = max(o['end'], pos + len(bits))
            o['ends'].setdefault(o['sections'][-1], 0)
            if not name.startswith(('trailing_', 'zero_bit')):
                o['ends'][o['sections'][-1]] = max(o['ends'][o['sections'][-1]], pos + len(bits))
        continue
    if c == 'OBU header':
        cur.append({'sections': ['OBU header'], 'v': {}, 'order': [], 'end': 0, 'ends': {}})
    elif cur:
        cur[-1]['sections'].append(c)

if len(packets) != len(samples):
    problems.append('trace_headers saw %d packets, the file has %d samples' % (len(packets), len(samples)))

# --- the shown frames, from dav1d (not our decoder, not libavcodec's hwaccel-only one)
out, _ = run(['-c:v', 'libdav1d', '-i', stream, '-f', 'framecrc', '-'])
dav1d_frames = len([l for l in out.splitlines() if l and not l.startswith('#')])


# --- the rules (av1dec.c / vulkan_av1.c / cbs_av1_syntax_template.c conventions, the AV1 specification otherwise)
KEY, INTER, INTRA_ONLY, SWITCH = 0, 1, 2, 3
NONE = 7
DEFAULT_LF_REF = [1, 0, 0, 0, -1, 0, -1, -1]


def lst(xs, sep=','):
    return sep.join(str(x) for x in xs)


def tile_log2(blk, target):
    k = 0
    while (blk << k) < target:
        k += 1
    return k


def rel(seq, a, b):
    if not seq['enable_order_hint']:
        return 0
    m = 1 << seq['order_hint_bits_minus_1']
    d = a - b
    return (d & (m - 1)) - (d & m)


def gm_default():
    return [[0] * 6] + [[0, 0, 1 << 16, 0, 0, 1 << 16] for _ in range(7)]


def empty_ref():
    return {'valid': 0, 'pic': 0, 'dpb': -1, 'gen': 0, 'frame_id': 0, 'upscaled_width': 0, 'frame_width': 0,
            'frame_height': 0, 'render_width': 0, 'render_height': 0, 'frame_type': 0, 'order_hint': 0,
            'order_hints': [0] * 8, 'sign_bias': 0, 'dfeuc': 0, 'seg_enabled': 0, 'lf_ref': [0] * 8,
            'lf_mode': [0, 0], 'fe': [[0] * 8 for _ in range(8)], 'fv': [[0] * 8 for _ in range(8)],
            'gm': [[0] * 6 for _ in range(8)], 'grain_apply': 0, 'grain': None}


refs = [empty_ref() for _ in range(8)]
seq = None
seq_payload = None
gen = 0
lines = []


def emit(s):
    lines.append(s)


def parse_seq(v, payload):
    q = {}
    g = lambda n, d=0: v.get(n, d)
    q['profile'] = g('seq_profile')
    q['still'] = g('still_picture')
    q['rsph'] = g('reduced_still_picture_header')
    q['timing'] = g('timing_info_present_flag')
    q['dm'] = g('decoder_model_info_present_flag')
    q['idd'] = g('initial_display_delay_present_flag')
    q['ops'] = g('operating_points_cnt_minus_1') + 1
    q['op_idc'] = [g('operating_point_idc[%d]' % i) for i in range(q['ops'])]
    q['level'] = g('seq_level_idx[0]')
    q['tier'] = g('seq_tier[0]')
    q['dm_op'] = [g('decoder_model_present_for_this_op[%d]' % i) for i in range(q['ops'])]
    q['brtl'] = g('buffer_removal_time_length_minus_1') + 1
    q['fptl'] = g('frame_presentation_time_length_minus_1') + 1
    q['epi'] = g('equal_picture_interval')
    q['tick'] = g('num_units_in_display_tick'); q['scale'] = g('time_scale'); q['nticks'] = g('num_ticks_per_picture_minus_1')
    for n in ('frame_width_bits_minus_1', 'frame_height_bits_minus_1', 'max_frame_width_minus_1', 'max_frame_height_minus_1',
              'frame_id_numbers_present_flag', 'delta_frame_id_length_minus_2', 'additional_frame_id_length_minus_1',
              'use_128x128_superblock', 'enable_filter_intra', 'enable_intra_edge_filter', 'enable_interintra_compound',
              'enable_masked_compound', 'enable_warped_motion', 'enable_dual_filter', 'enable_order_hint', 'enable_jnt_comp',
              'enable_ref_frame_mvs', 'order_hint_bits_minus_1', 'enable_superres', 'enable_cdef', 'enable_restoration',
              'film_grain_params_present', 'mono_chrome', 'color_description_present_flag', 'color_range',
              'separate_uv_delta_q'):
        q[n] = g(n)
    if q['rsph']:
        q['sct'] = 2; q['imv'] = 2
    else:
        q['sct'] = 2 if g('seq_choose_screen_content_tools') else g('seq_force_screen_content_tools')
        if q['sct'] > 0:
            q['imv'] = 2 if g('seq_choose_integer_mv') else g('seq_force_integer_mv')
        else:
            q['imv'] = 2
    hb = g('high_bitdepth')
    q['bd'] = 12 if g('twelve_bit') else 10 if hb else 8
    if q['color_description_present_flag']:
        q['cp'], q['tc'], q['mc'] = g('color_primaries'), g('transfer_characteristics'), g('matrix_coefficients')
    else:
        q['cp'] = q['tc'] = q['mc'] = 2
    q['csp'] = 0
    if q['mono_chrome']:
        q['ssx'] = q['ssy'] = 1
    elif q['cp'] == 1 and q['tc'] == 13 and q['mc'] == 0:
        q['ssx'] = q['ssy'] = 0; q['color_range'] = 1
    else:
        if q['profile'] == 0:
            q['ssx'] = q['ssy'] = 1
        elif q['profile'] == 1:
            q['ssx'] = q['ssy'] = 0
        elif q['bd'] == 12:
            q['ssx'] = g('subsampling_x'); q['ssy'] = g('subsampling_y') if q['ssx'] else 0
        else:
            q['ssx'], q['ssy'] = 1, 0
        if q['ssx'] and q['ssy']:
            q['csp'] = g('chroma_sample_position')
    q['payload'] = payload
    flags = [q['still'], q['rsph'], q['use_128x128_superblock'], q['enable_filter_intra'], q['enable_intra_edge_filter'],
             q['enable_interintra_compound'], q['enable_masked_compound'], q['enable_warped_motion'], q['enable_dual_filter'],
             q['enable_order_hint'], q['enable_jnt_comp'], q['enable_ref_frame_mvs'], q['frame_id_numbers_present_flag'],
             q['enable_superres'], q['enable_cdef'], q['enable_restoration'], q['film_grain_params_present'], q['timing'], q['idd']]
    s = ('seq profile=%d ops=%d op0=%03x level=%d tier=%d w=%d h=%d bits=%d/%d fid=%d/%d/%d flags=%s ohbits=%d sct=%d imv=%d '
         'bd=%d mono=%d ss=%d/%d range=%d cdp=%d cp=%d tc=%d mc=%d csp=%d sep=%d') % (
        q['profile'], q['ops'], q['op_idc'][0], q['level'], q['tier'], q['max_frame_width_minus_1'] + 1,
        q['max_frame_height_minus_1'] + 1, q['frame_width_bits_minus_1'] + 1, q['frame_height_bits_minus_1'] + 1,
        q['frame_id_numbers_present_flag'], q['delta_frame_id_length_minus_2'], q['additional_frame_id_length_minus_1'],
        ''.join(str(x) for x in flags), q['order_hint_bits_minus_1'] + 1, q['sct'], q['imv'], q['bd'], q['mono_chrome'], q['ssx'],
        q['ssy'], q['color_range'], q['color_description_present_flag'], q['cp'], q['tc'], q['mc'], q['csp'], q['separate_uv_delta_q'])
    if q['timing']:
        s += ' timing=%d/%d/%d/%d' % (q['tick'], q['scale'], q['epi'], q['nticks'])
    s += ' dm=%d' % q['dm']
    q['line'] = s
    q['unsupported'] = (q['profile'] != 0 or q['mono_chrome'] or not q['ssx'] or not q['ssy'] or q['bd'] not in (8, 10) or
                        (q['op_idc'][0] >> 8) & ~1)
    return q


def set_frame_refs(seq, h, last, gold):
    idx = [-1] * 7
    idx[0] = last; idx[3] = gold
    used = [0] * 8; used[last] = used[gold] = 1
    cur = 1 << seq['order_hint_bits_minus_1']
    sh = [cur + rel(seq, refs[i]['order_hint'], h['oh']) for i in range(8)]
    latest, earliest = sh[last], sh[gold]
    r = -1
    for i in range(8):
        if not used[i] and sh[i] >= cur and (r < 0 or sh[i] >= latest):
            r, latest = i, sh[i]
    if r >= 0:
        idx[6] = r; used[r] = 1
    for slot in (4, 5):   # BWDREF, then ALTREF2: the closest backward ones
        r = -1
        for i in range(8):
            if not used[i] and sh[i] >= cur and (r < 0 or sh[i] < earliest):
                r, earliest = i, sh[i]
        if r >= 0:
            idx[slot] = r; used[r] = 1
    for rf in (1, 2, 4, 5, 6):   # LAST2, LAST3, BWDREF, ALTREF2, ALTREF: forward ones, latest first
        if idx[rf] >= 0:
            continue
        r = -1
        for i in range(8):
            if not used[i] and sh[i] < cur and (r < 0 or sh[i] >= latest):
                r, latest = i, sh[i]
        if r >= 0:
            idx[rf] = r; used[r] = 1
    r = -1
    for i in range(8):
        if r < 0 or sh[i] < earliest:
            r, earliest = i, sh[i]
    return [x if x >= 0 else r for x in idx]


def ns_decode_unsigned_with_ref(v, mx, r):
    def inv(r, v):
        return v if v > 2 * r else (r - ((v + 1) >> 1) if v & 1 else r + (v >> 1))
    return inv(r, v) if (r << 1) <= mx else mx - 1 - inv(mx - 1 - r, v)


def frame(tu, o, sample, sobu, kind, tid, sid):
    """a frame header OBU: returns the frame's dict, or None for show_existing_frame"""
    global gen
    v = o['v']
    g = lambda n, d=0: v.get(n, d)
    q = seq
    h = {}
    all_frames = 0xff
    if q['rsph']:
        h['show_existing'] = 0; h['type'] = KEY; h['show'] = 1; h['showable'] = 0; h['er'] = 0
    else:
        h['show_existing'] = g('show_existing_frame')
        if h['show_existing']:
            idx = g('frame_to_show_map_idx')
            r = refs[idx]
            if not r['valid']:
                problems.append('tu %d: show_existing_frame of an empty slot' % tu)
            shown = dict(r)
            refresh = all_frames if r['frame_type'] == KEY else 0
            emit('show tu=%d slot=%d dpb=%d refresh=%02x size=%dx%d grain=%d missing=%d' % (
                tu, idx, r['dpb'] if r['pic'] else -1, refresh, r['upscaled_width'], r['frame_height'], r['grain_apply'],
                0 if r['pic'] else 1))
            if r['frame_type'] == KEY:
                for i in range(8):
                    refs[i] = dict(shown)
            return None
        h['type'] = g('frame_type'); h['show'] = g('show_frame')
        h['showable'] = (h['type'] != KEY) if h['show'] else g('showable_frame')
        h['er'] = 1 if h['type'] == SWITCH or (h['type'] == KEY and h['show']) else g('error_resilient_mode')
    intra = h['type'] in (KEY, INTRA_ONLY)
    if h['type'] == KEY and h['show']:
        for r in refs:
            r['valid'] = 0
    h['dcu'] = g('disable_cdf_update')
    h['sct'] = g('allow_screen_content_tools') if q['sct'] == 2 else q['sct']
    h['fimv'] = (g('force_integer_mv') if q['imv'] == 2 else q['imv']) if h['sct'] else 0
    h['fid'] = g('current_frame_id') if q['frame_id_numbers_present_flag'] else 0
    if q['frame_id_numbers_present_flag']:
        id_len = q['additional_frame_id_length_minus_1'] + q['delta_frame_id_length_minus_2'] + 3
        diff_len = q['delta_frame_id_length_minus_2'] + 2
        for r in refs:
            f = r['frame_id']
            if h['fid'] > (1 << diff_len):
                if f > h['fid'] or f < h['fid'] - (1 << diff_len):
                    r['valid'] = 0
            elif h['fid'] < f < (1 << id_len) + h['fid'] - (1 << diff_len):
                r['valid'] = 0
    h['fso'] = 1 if h['type'] == SWITCH else 0 if q['rsph'] else g('frame_size_override_flag')
    h['oh'] = g('order_hint') if q['enable_order_hint'] else 0
    h['prf'] = NONE if intra or h['er'] else g('primary_ref_frame')
    h['brtp'] = g('buffer_removal_time_present_flag') if q['dm'] else 0
    h['refresh'] = all_frames if h['type'] == SWITCH or (h['type'] == KEY and h['show']) else g('refresh_frame_flags')
    if (not intra or h['refresh'] != all_frames) and h['er'] and q['enable_order_hint']:
        for i in range(8):
            if g('ref_order_hint[%d]' % i) != refs[i]['order_hint']:
                refs[i]['valid'] = 0
    idx = [0] * 7
    h['frss'] = 0
    h['hp'] = h['ifs'] = h['interp'] = h['mms'] = h['urfm'] = 0
    h['intrabc'] = 0

    def frame_size():
        if h['fso']:
            h['fw'] = g('frame_width_minus_1') + 1; h['fh'] = g('frame_height_minus_1') + 1
        else:
            h['fw'] = q['max_frame_width_minus_1'] + 1; h['fh'] = q['max_frame_height_minus_1'] + 1
        superres()

    def superres():
        h['sr'] = g('use_superres') if q['enable_superres'] else 0
        h['denom'] = g('coded_denom') if h['sr'] else 0
        d = h['denom'] + 9 if h['sr'] else 8
        h['uw'] = h['fw']
        h['fw'] = (h['uw'] * 8 + d // 2) // d

    def render_size():
        h['rsd'] = g('render_and_frame_size_different')
        if h['rsd']:
            h['rw'] = g('render_width_minus_1') + 1; h['rh'] = g('render_height_minus_1') + 1
        else:
            h['rw'] = h['uw']; h['rh'] = h['fh']

    h['rsd'] = 0
    if intra:
        frame_size(); render_size()
        if h['sct'] and h['uw'] == h['fw']:
            h['intrabc'] = g('allow_intrabc')
    else:
        if q['enable_order_hint']:
            h['frss'] = g('frame_refs_short_signaling')
        if h['frss']:
            idx = set_frame_refs(q, h, g('last_frame_idx'), g('golden_frame_idx'))
        else:
            idx = [g('ref_frame_idx[%d]' % i) for i in range(7)]
        if h['fso'] and not h['er']:
            found = -1
            for i in range(7):
                if g('found_ref[%d]' % i):
                    found = i
                    break
            if found >= 0:
                r = refs[idx[found]]
                h['uw'] = r['upscaled_width']; h['fw'] = h['uw']; h['fh'] = r['frame_height']
                h['rw'] = r['render_width']; h['rh'] = r['render_height']
                superres()
            else:
                frame_size(); render_size()
        else:
            frame_size(); render_size()
        h['hp'] = 0 if h['fimv'] else g('allow_high_precision_mv')
        h['ifs'] = g('is_filter_switchable')
        h['interp'] = 4 if h['ifs'] else g('interpolation_filter')
        h['mms'] = g('is_motion_mode_switchable')
        h['urfm'] = 0 if h['er'] or not q['enable_ref_frame_mvs'] else g('use_ref_frame_mvs')
    h['idx'] = idx
    # libavcodec's order_hint_info (intra frames: ref_frame_idx[] is 0)
    ohs = [0] + [refs[idx[i]]['order_hint'] for i in range(7)]
    sb = 0
    for i in range(7):
        if q['enable_order_hint'] and rel(q, ohs[1 + i], h['oh']) > 0:
            sb |= 1 << (1 + i)
    h['dfeuc'] = 1 if q['rsph'] or h['dcu'] else g('disable_frame_end_update_cdf')
    mi_cols = 2 * ((h['fw'] + 7) >> 3); mi_rows = 2 * ((h['fh'] + 7) >> 3)
    # tile_info
    sb128 = q['use_128x128_superblock']
    sbc = (mi_cols + 31) >> 5 if sb128 else (mi_cols + 15) >> 4
    sbr = (mi_rows + 31) >> 5 if sb128 else (mi_rows + 15) >> 4
    shift = 5 if sb128 else 4
    max_w = 4096 >> (shift + 2)
    max_area = (4096 * 2304) >> (2 * (shift + 2))
    min_log2_tiles = max(tile_log2(max_w, sbc), tile_log2(max_area, sbr * sbc))
    uni = g('uniform_tile_spacing_flag')
    cols_start, rows_start, wsb, hsb = [], [], [], []
    if uni:
        cl2 = g('tile_cols_log2')
        tw = (sbc + (1 << cl2) - 1) >> cl2
        cols_start = list(range(0, sbc, tw))
        rl2 = g('tile_rows_log2')
        th = (sbr + (1 << rl2) - 1) >> rl2
        rows_start = list(range(0, sbr, th))
        wsb = [tw - 1] * (len(cols_start) - 1) + [sbc - (len(cols_start) - 1) * tw - 1]
        hsb = [th - 1] * (len(rows_start) - 1) + [sbr - (len(rows_start) - 1) * th - 1]
    else:
        st, i = 0, 0
        while st < sbc and i < 64:
            cols_start.append(st); wsb.append(g('width_in_sbs_minus_1[%d]' % i)); st += wsb[-1] + 1; i += 1
        st, i = 0, 0
        while st < sbr and i < 64:
            rows_start.append(st); hsb.append(g('height_in_sbs_minus_1[%d]' % i)); st += hsb[-1] + 1; i += 1
        cl2, rl2 = tile_log2(1, len(cols_start)), tile_log2(1, len(rows_start))
    ncols, nrows = len(cols_start), len(rows_start)
    if cl2 or rl2:
        ctx, tsb = g('context_update_tile_id'), g('tile_size_bytes_minus1') + 1
    else:
        ctx, tsb = 0, 1
    # quantization
    base = g('base_q_idx')
    dq = lambda n: g(n + '.delta_q') if g(n + '.delta_coded') else 0
    dyd = dq('delta_q_y_dc')
    diffuv = g('diff_uv_delta') if q['separate_uv_delta_q'] else 0
    dud, dua = dq('delta_q_u_dc'), dq('delta_q_u_ac')
    dvd, dva = (dq('delta_q_v_dc'), dq('delta_q_v_ac')) if diffuv else (dud, dua)
    uqm = g('using_qmatrix')
    qm = (g('qm_y'), g('qm_u'), g('qm_v') if q['separate_uv_delta_q'] else g('qm_u')) if uqm else (0, 0, 0)
    # segmentation
    seg = g('segmentation_enabled')
    fe = [[0] * 8 for _ in range(8)]; fv = [[0] * 8 for _ in range(8)]
    sum_ = stu = sud = 0
    if seg:
        if h['prf'] == NONE:
            sum_, stu, sud = 1, 0, 1
        else:
            sum_ = g('segmentation_update_map'); stu = g('segmentation_temporal_update') if sum_ else 0
            sud = g('segmentation_update_data')
        if sud:
            for i in range(8):
                for j in range(8):
                    fe[i][j] = g('feature_enabled[%d][%d]' % (i, j))
                    fv[i][j] = g('feature_value[%d][%d]' % (i, j)) if fe[i][j] else 0
        else:
            pr = refs[idx[h['prf']]]
            fe = [list(x) for x in pr['fe']]; fv = [list(x) for x in pr['fv']]
    dqp = g('delta_q_present') if base > 0 else 0
    dqr = g('delta_q_res') if dqp else 0
    dlp = dlr = dlm = 0
    if dqp and not h['intrabc']:
        dlp = g('delta_lf_present')
        if dlp:
            dlr, dlm = g('delta_lf_res'), g('delta_lf_multi')
    lossless = 1
    for i in range(8):
        qi = base
        if seg and fe[i][0]:
            qi = min(255, max(0, base + fv[i][0]))
        if qi or dyd or dua or dud or dva or dvd:
            lossless = 0
    all_lossless = lossless and h['fw'] == h['uw']
    # loop filter
    lvl = [0, 0, 0, 0]; sharp = de = du = 0
    lfref, lfmode = list(DEFAULT_LF_REF), [0, 0]
    if not (lossless or h['intrabc']):
        lvl[0], lvl[1] = g('loop_filter_level[0]'), g('loop_filter_level[1]')
        if lvl[0] or lvl[1]:
            lvl[2], lvl[3] = g('loop_filter_level[2]'), g('loop_filter_level[3]')
        sharp = g('loop_filter_sharpness'); de = g('loop_filter_delta_enabled')
        if de:
            if h['prf'] != NONE:
                pr = refs[idx[h['prf']]]
                lfref, lfmode = list(pr['lf_ref']), list(pr['lf_mode'])
            du = g('loop_filter_delta_update')
            if du:
                for i in range(8):
                    if g('update_ref_delta[%d]' % i):
                        lfref[i] = g('loop_filter_ref_deltas[%d]' % i)
                for i in range(2):
                    if g('update_mode_delta[%d]' % i):
                        lfmode[i] = g('loop_filter_mode_deltas[%d]' % i)
    # cdef
    cdamp = cbits = 0
    ypri, ysec, uvpri, uvsec = [0] * 8, [0] * 8, [0] * 8, [0] * 8
    if not (lossless or h['intrabc'] or not q['enable_cdef']):
        cdamp, cbits = g('cdef_damping_minus_3'), g('cdef_bits')
        for i in range(1 << cbits):
            ypri[i], ysec[i] = g('cdef_y_pri_strength[%d]' % i), g('cdef_y_sec_strength[%d]' % i)
            uvpri[i], uvsec[i] = g('cdef_uv_pri_strength[%d]' % i), g('cdef_uv_sec_strength[%d]' % i)
    # loop restoration
    lrt = [0, 0, 0]; lus = luv = 0
    if not (all_lossless or h['intrabc'] or not q['enable_restoration']):
        lrt = [g('lr_type[%d]' % i) for i in range(3)]
        if any(lrt):
            lus = g('lr_unit_shift')
            luv = g('lr_uv_shift') if any(lrt[1:]) else 0
    remap = [0, 3, 1, 2]
    tx = 0 if lossless else g('tx_mode')
    rsel = 0 if intra else g('reference_select')
    smp = g('skip_mode_present')
    awm = 0 if intra or h['er'] or not q['enable_warped_motion'] else g('allow_warped_motion')
    rts = g('reduced_tx_set')
    # global motion
    gm = gm_default(); gmt = [0] * 8
    if not intra:
        prev = gm_default() if h['prf'] == NONE else [list(x) for x in refs[idx[h['prf']]]['gm']]
        for ref in range(1, 8):
            t = 0
            if g('is_global[%d]' % ref):
                t = 2 if g('is_rot_zoom[%d]' % ref) else 1 if g('is_translation[%d]' % ref) else 3
            gmt[ref] = t

            def param(i):
                ab, pb = 12, 15
                if i < 2:
                    if t == 1:
                        ab, pb = 9 - (not h['hp']), 3 - (not h['hp'])
                    else:
                        ab, pb = 12, 6
                pd = 16 - pb
                rnd = (1 << 16) if i % 3 == 2 else 0
                sub = (1 << pb) if i % 3 == 2 else 0
                mx = 1 << ab
                r = (prev[ref][i] >> pd) - sub
                val = ns_decode_unsigned_with_ref(g('gm_params[%d][%d]' % (ref, i)), 2 * mx + 1, r + mx) - mx
                gm[ref][i] = (val << pd) + rnd
            if t >= 2:
                param(2); param(3)
                if t == 3:
                    param(4); param(5)
                else:
                    gm[ref][4] = -gm[ref][3]; gm[ref][5] = gm[ref][2]
            if t >= 1:
                param(0); param(1)
    # film grain
    gapply, grain = 0, None
    if q['film_grain_params_present'] and (h['show'] or h['showable']) and g('apply_grain'):
        seed = g('grain_seed')
        upd = g('update_grain') if h['type'] == INTER else 1
        if not upd:
            src = refs[g('film_grain_params_ref_idx')]
            gapply = src['grain_apply']
            grain = dict(src['grain']) if src['grain'] else None
            if grain is None:
                grain = {'seed': 0, 'ref': 0, 'y': [], 'cfl': 0, 'cb': [], 'cr': [], 'scal': 0, 'lag': 0, 'ary': [0] * 24,
                         'arcb': [0] * 25, 'arcr': [0] * 25, 'shift': 0, 'gss': 0, 'cbm': (0, 0, 0), 'crm': (0, 0, 0), 'ov': 0, 'clip': 0}
            grain['seed'] = seed
        else:
            gapply = 1
            ny = g('num_y_points')
            y = [(g('point_y_value[%d]' % i), g('point_y_scaling[%d]' % i)) for i in range(ny)]
            cfl = g('chroma_scaling_from_luma')
            cb = [(g('point_cb_value[%d]' % i), g('point_cb_scaling[%d]' % i)) for i in range(g('num_cb_points'))]
            cr = [(g('point_cr_value[%d]' % i), g('point_cr_scaling[%d]' % i)) for i in range(g('num_cr_points'))]
            lag = g('ar_coeff_lag')
            npl = 2 * lag * (lag + 1)
            npc = npl + 1 if ny else npl
            s8 = lambda x: x - 256 if x > 127 else x
            ary = [s8(g('ar_coeffs_y_plus_128[%d]' % i)) if ny and i < npl else 0 for i in range(24)]
            arcb = [s8(g('ar_coeffs_cb_plus_128[%d]' % i)) if (cfl or cb) and i < npc else 0 for i in range(25)]
            arcr = [s8(g('ar_coeffs_cr_plus_128[%d]' % i)) if (cfl or cr) and i < npc else 0 for i in range(25)]
            grain = {'seed': seed, 'ref': 0, 'y': y, 'cfl': cfl, 'cb': cb, 'cr': cr, 'scal': g('grain_scaling_minus_8'),
                     'lag': lag, 'ary': ary, 'arcb': arcb, 'arcr': arcr, 'shift': g('ar_coeff_shift_minus_6'),
                     'gss': g('grain_scale_shift'),
                     'cbm': (g('cb_mult'), g('cb_luma_mult'), g('cb_offset')) if cb else (0, 0, 0),
                     'crm': (g('cr_mult'), g('cr_luma_mult'), g('cr_offset')) if cr else (0, 0, 0),
                     'ov': g('overlap_flag'), 'clip': g('clip_to_restricted_range')}
    # libavcodec's skip_mode_params: from the references' pictures
    skip = [0, 0]
    if not intra and rsel and q['enable_order_hint'] and all(refs[idx[i]]['pic'] for i in range(7)):
        fwd = bwd = -1; fh = bh = 0
        for i in range(7):
            rh = refs[idx[i]]['order_hint']
            d = rel(q, rh, h['oh'])
            if d < 0:
                if fwd < 0 or rel(q, rh, fh) > 0:
                    fwd, fh = i, rh
            elif d > 0:
                if bwd < 0 or rel(q, rh, bh) < 0:
                    bwd, bh = i, rh
        if fwd >= 0 and bwd >= 0:
            skip = [1 + min(fwd, bwd), 1 + max(fwd, bwd)]
        elif fwd >= 0:
            f2, f2h = -1, 0
            for i in range(7):
                rh = refs[idx[i]]['order_hint']
                if rel(q, rh, fh) < 0 and (f2 < 0 or rel(q, rh, f2h) > 0):
                    f2, f2h = i, rh
            if f2 >= 0:
                skip = [1 + min(fwd, f2), 1 + max(fwd, f2)]
    # the DPB slot, the references
    busy = {r['dpb'] for r in refs if r['pic'] and r['dpb'] >= 0}
    dpb = 0
    while dpb in busy:
        dpb += 1
    gen += 1
    names, lst_ = [], []
    missing = 0
    for i in range(7):
        r = refs[idx[i]]
        if not r['pic']:
            names.append(-1); missing = 1; continue
        names.append(r['dpb'])
        if any(x[0] == r['dpb'] for x in lst_):
            continue
        lst_.append((r['dpb'], idx[i], '%d:%d:%02x:%d:%d:%s' % (r['frame_type'], r['order_hint'], r['sign_bias'], r['dfeuc'],
                                                                 r['seg_enabled'], lst(r['order_hints'], '_'))))
    missing = missing and not intra
    flags = [h['er'], h['dcu'], h.get('sr', 0), h['rsd'], h['sct'], h['ifs'], 1 if (h['fimv'] or intra) else 0, h['fso'], h['brtp'],
             h['intrabc'], h['frss'], h['hp'], h['mms'], h['urfm'], h['dfeuc'], awm, rts, rsel, smp, dqp, dlp, dlm, seg, sum_,
             stu, sud, 1 if any(lrt) else 0, 0, gapply]
    hb = o['ends'].get(kind, 0) - (sobu[5] - sobu[4]) * 8
    emit('frame tu=%d type=%d show=%d oh=%d prf=%d refresh=%02x fid=%d size=%dx%d fw=%d denom=%d dpb=%d hb=%d flags=%s' % (
        tu, h['type'], h['show'], h['oh'], h['prf'], h['refresh'], h['fid'], h['uw'], h['fh'], h['fw'], h['denom'], dpb, hb,
        ''.join(str(x) for x in flags)))
    emit(' refs idx=%s names=%s ohs=%s skip=%d,%d list=%s missing=%d' % (
        lst(idx), lst(names), lst(ohs), skip[0], skip[1], ' '.join('%d@%d=%s' % x for x in lst_) if lst_ else '-', missing))
    emit(' q base=%d dq=%d,%d,%d,%d,%d diffuv=%d qm=%d:%d,%d,%d dqres=%d dlfres=%d interp=%d tx=%d' % (
        base, dyd, dud, dua, dvd, dva, diffuv, uqm, qm[0], qm[1], qm[2], dqr, dlr, h['interp'], tx))
    emit(' seg ' + ' '.join('%02x:%s' % (sum(fe[i][j] << j for j in range(8)), lst(fv[i])) for i in range(8)))
    emit(' lf lvl=%s sharp=%d de=%d du=%d ref=%s mode=%s' % (lst(lvl), sharp, de, du, lst(lfref), lst(lfmode)))
    emit(' cdef damp=%d bits=%d y=%s uv=%s' % (cdamp, cbits, lst('%d/%d' % (ypri[i], ysec[i]) for i in range(8)),
                                                 lst('%d/%d' % (uvpri[i], uvsec[i]) for i in range(8))))
    emit(' lr type=%s size=%d,%d,%d' % (lst(remap[t] for t in lrt), 1 + lus, 1 + lus - luv, 1 + lus - luv))
    emit(' tile cols=%d rows=%d uni=%d ctx=%d tsb=%d mc=%s mr=%s w=%s h=%s' % (
        ncols, nrows, uni, ctx, tsb - 1, lst(x << shift for x in cols_start), lst(x << shift for x in rows_start), lst(wsb), lst(hsb)))
    gms = ['%d:%d:%s' % (r, gmt[r], lst(gm[r])) for r in range(1, 8) if gmt[r]]
    emit(' gm ' + (' '.join(gms) if gms else '-'))
    if gapply:
        G = grain
        emit(' grain seed=%d ref=%d y=%s cfl=%d cb=%s cr=%s scal=%d lag=%d ary=%s arcb=%s arcr=%s shift=%d gss=%d cb=%d/%d/%d '
             'cr=%d/%d/%d ov=%d clip=%d' % (
                 G['seed'], G['ref'], lst('%d:%d' % p for p in G['y']), G['cfl'], lst('%d:%d' % p for p in G['cb']),
                 lst('%d:%d' % p for p in G['cr']), G['scal'], G['lag'], lst(G['ary']), lst(G['arcb']), lst(G['arcr']), G['shift'],
                 G['gss'], G['cbm'][0], G['cbm'][1], G['cbm'][2], G['crm'][0], G['crm'][1], G['crm'][2], G['ov'], G['clip']))
    fr = {'h': h, 'tu': tu, 'dpb': dpb, 'gen': gen, 'missing': missing, 'ncols': ncols, 'nrows': nrows, 'cl2': cl2, 'rl2': rl2,
          'tsb': tsb, 'tiles': [], 'ntg': 0, 'next': 0, 'intra': intra,
          'ref': {'valid': 1, 'pic': 0 if missing else 1, 'dpb': -1 if missing else dpb, 'gen': gen, 'frame_id': h['fid'],
                  'upscaled_width': h['uw'], 'frame_width': h['fw'], 'frame_height': h['fh'], 'render_width': h['rw'],
                  'render_height': h['rh'], 'frame_type': h['type'], 'order_hint': h['oh'], 'order_hints': ohs, 'sign_bias': sb,
                  'dfeuc': h['dfeuc'], 'seg_enabled': seg, 'lf_ref': lfref, 'lf_mode': lfmode, 'fe': fe, 'fv': fv, 'gm': gm,
                  'grain_apply': gapply, 'grain': grain}}
    fr['setup'] = '%d:%d:%02x:%d:%d' % (h['type'], h['oh'], sb, h['dfeuc'], seg)
    return fr


def tile_group(fr, o, sample, sobu, kind):
    """the tiles of a tile group: their offsets in the sample, read from the stream's bytes"""
    v = o['v']
    num = fr['ncols'] * fr['nrows']
    tg_start, tg_end = 0, num - 1
    if num > 1 and v.get('tile_start_and_end_present_flag', 0):
        tg_start, tg_end = v.get('tg_start', 0), v.get('tg_end', 0)
    start = sobu[4] + (o['end'] + 7) // 8   # the tile data follows the last element read (byte aligned)
    end = sobu[5] + sobu[6]
    pos = start
    for t in range(tg_start, tg_end + 1):
        if t == tg_end:
            size = end - pos
        else:
            size = int.from_bytes(sample[pos:pos + fr['tsb']], 'little') + 1
            pos += fr['tsb']
        fr['tiles'].append((pos, size))
        pos += size
    fr['ntg'] += 1
    fr['next'] = tg_end + 1
    return fr['next'] == num


def finish(fr):
    h = fr['h']
    for i in range(8):
        if h['refresh'] & (1 << i):
            refs[i] = dict(fr['ref'])
    emit(' setup %s tiles n=%d tg=%d size=%d at=%s' % (fr['setup'], len(fr['tiles']), fr['ntg'], sum(s for _, s in fr['tiles']),
                                                       lst('%d:%d' % t for t in fr['tiles'])))


shown_total = 0
stopped = False
printed_seq = None
for tu, sample in enumerate(samples):
    if stopped:
        break
    trace = packets[tu] if tu < len(packets) else []
    sobus = obus_of(sample)
    if len(trace) != len(sobus):
        problems.append('tu %d: trace_headers has %d OBUs, the sample %d' % (tu, len(trace), len(sobus)))
    nframes, shown, extra = 0, -1, 0
    fr, seen = None, False
    first_line = len(lines)
    for o, sobu in zip(trace, sobus):
        typ = sobu[0]
        op = seq['op_idc'][0] if seq else 0
        if sobu[3] and op and typ not in (1, 2) and (not (op >> sobu[1]) & 1 or not (op >> (sobu[2] + 8)) & 1):
            continue
        secs = o['sections']
        if typ == 1:
            payload = sample[sobu[5]:sobu[5] + sobu[6]]
            if seq is None or payload != seq['payload']:
                seq = parse_seq(o['v'], payload)
                if seq['unsupported']:
                    print('tu %d sample: -1 sequence header not covered' % tu, file=sys.stderr)
                    stopped = True
                    break
        elif typ == 2:
            seen = False
        elif typ in (3, 6, 7):
            if typ == 7 and seen:
                continue
            kind = 'Frame Header' if 'Frame Header' in secs else next((s for s in secs if 'Frame Header' in s), 'Frame Header')
            f = frame(tu, o, sample, sobu, kind, sobu[1], sobu[2])
            nframes += 1
            if f is None:
                if shown < 0:
                    shown = nframes - 1
                else:
                    extra += 1
                continue
            fr, seen = f, True
            if typ == 6:
                if tile_group(fr, o, sample, sobu, kind):
                    finish(fr)
                    if fr['h']['show']:
                        if shown < 0:
                            shown = nframes - 1
                        else:
                            extra += 1
                    fr, seen = None, False
        elif typ == 4:
            if tile_group(fr, o, sample, sobu, 'Tile Group'):
                finish(fr)
                if fr['h']['show']:
                    if shown < 0:
                        shown = nframes - 1
                    else:
                        extra += 1
                fr, seen = None, False
    if stopped:
        break
    # the sequence header as it stands after the sample, before the sample's frames (as av1parse_test prints it)
    if seq is not None and seq['payload'] != printed_seq:
        lines.insert(first_line, seq['line'])
        printed_seq = seq['payload']
    if nframes:
        emit('tu %d frames=%d shown=%d extra=%d' % (tu, nframes, shown, extra))
        shown_total += 1 if shown >= 0 else 0
    else:
        emit('tu %d frames=0 shown=-1 extra=0' % tu)

if not stopped and shown_total != dav1d_frames:
    problems.append('%d temporal units show a frame here, dav1d outputs %d frames' % (shown_total, dav1d_frames))
print('\n'.join(lines))
for p in problems:
    print('mkgolden: ' + p, file=sys.stderr)
sys.exit(1 if problems else 0)
