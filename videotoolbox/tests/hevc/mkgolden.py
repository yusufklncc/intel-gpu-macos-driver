#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mkgolden.py <stream.265|.bit> [ffmpeg]: the reference output of hevcparse_test for a recorded Annex B stream, in the
HEVC golden format, made from FFmpeg 9.0.2 (the version the plug-ins ship): the syntax elements of every VPS, SPS, PPS
and slice segment header from the trace_headers bitstream filter, with their RBSP bit positions; the access units as
libavcodec's HEVC parser cuts them (the packets of a stream copy); and, derived here from ITU-T H.265 the way
libavcodec's decoder does it (hevcdec.c, refs.c, ps.c), each picture's POC, reference picture set and the reference
pictures it is decoded with. The POCs are checked against libavcodec's own decoder (the "Decoded frame with POC" lines
of -loglevel debug); on a mismatch the golden is still written, the difference goes to stderr and the exit status is 1.
Writes to stdout.

trace_headers runs on the stream rewrapped as Matroska with one NAL unit per block: CBS gives up on the rest of a
packet at the first NAL unit it cannot read (a PPS before its SPS, a slice of a PPS it does not have), and one unit
per packet keeps that to the one unit."""
import re
import struct
import subprocess
import sys

stream = sys.argv[1]
ffmpeg = sys.argv[2] if len(sys.argv) > 2 else 'ffmpeg'
problems = []


def warn(msg):
    problems.append(msg)


def run(args, data=None):
    p = subprocess.run([ffmpeg, '-hide_banner', '-nostdin'] + args, input=data, capture_output=True)
    return p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace').splitlines()


def lst(items):
    return ','.join(str(x) for x in items) if items else '-'


def c_mod(a, b):
    """C's % (the sign of the dividend)"""
    r = abs(a) % b
    return -r if a < 0 else r


NAL_NAMES = {0: 'TRAIL_N', 1: 'TRAIL_R', 2: 'TSA_N', 3: 'TSA_R', 4: 'STSA_N', 5: 'STSA_R', 6: 'RADL_N', 7: 'RADL_R',
             8: 'RASL_N', 9: 'RASL_R', 16: 'BLA_W_LP', 17: 'BLA_W_RADL', 18: 'BLA_N_LP', 19: 'IDR_W_RADL', 20: 'IDR_N_LP',
             21: 'CRA_NUT'}
VPS_NUT, SPS_NUT, PPS_NUT, EOS_NUT, EOB_NUT = 32, 33, 34, 36, 37

# Table 7-6, in coded (up-right diagonal) order
DEFAULT_INTRA = [16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21, 19, 20, 21, 20,
                 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31, 29, 36, 41, 44,
                 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115]
DEFAULT_INTER = [16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20, 20, 20, 20, 20,
                 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28, 28, 33, 33, 33,
                 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91]

# the syntax elements of st_ref_pic_set()
RPS_NAMES = re.compile(r'^(inter_ref_pic_set_prediction_flag|delta_idx_minus1|delta_rps_sign|abs_delta_rps_minus1|'
                       r'used_by_curr_pic_flag\[\d+\]|use_delta_flag\[\d+\]|num_negative_pics|num_positive_pics|'
                       r'delta_poc_s[01]_minus1\[\d+\]|used_by_curr_pic_s[01]_flag\[\d+\])$')

# --- NAL units of the stream (Annex B), in order
data = open(stream, 'rb').read()
nals = []           # [payload offset, bytes (escaped, trailing zeros removed), type, layer, temporal id]
pos = data.find(b'\x00\x00\x01')
while pos >= 0:
    start = pos + 3
    pos = data.find(b'\x00\x00\x01', start)
    raw = data[start:len(data) if pos < 0 else pos].rstrip(b'\x00')
    if len(raw) >= 2:
        nals.append([start, raw, (raw[0] >> 1) & 63, ((raw[0] & 1) << 5) | (raw[1] >> 3), (raw[1] & 7) - 1])

# --- access units: the packets of a stream copy (libavcodec's HEVC parser), the ones the decoder gets
out, _ = run(['-nofind_stream_info', '-f', 'hevc', '-i', stream, '-map', '0:v', '-c', 'copy', '-copyinkf',
                '-f', 'framecrc', '-'])
packets, end = [], 0
for l in out.splitlines():
    if l and not l.startswith('#'):
        end += int(l.split(',')[4])
        packets.append([end, []])
if end != len(data):
    warn('packets cover %d bytes of %d' % (end, len(data)))
pi = 0
for n in nals:
    while pi < len(packets) and n[0] >= packets[pi][0]:
        pi += 1
    if pi < len(packets):
        packets[pi][1].append(n)

# --- syntax elements: trace_headers on one NAL unit per packet (Matroska, no CodecPrivate: Annex B blocks)


def ebml(eid, body):
    return eid + b'\x01' + len(body).to_bytes(7, 'big') + body


def ebml_uint(eid, v):
    return ebml(eid, v.to_bytes(max(1, (v.bit_length() + 7) // 8), 'big'))


mkv = [ebml(b'\x1a\x45\xdf\xa3', ebml(b'\x42\x82', b'matroska') + ebml_uint(b'\x42\x87', 4) + ebml_uint(b'\x42\x85', 2)),
       b'\x18\x53\x80\x67\x01\xff\xff\xff\xff\xff\xff\xff',        # Segment, size unknown
       ebml(b'\x15\x49\xa9\x66', ebml_uint(b'\x2a\xd7\xb1', 1000000)),
       ebml(b'\x16\x54\xae\x6b', ebml(b'\xae', ebml_uint(b'\xd7', 1) + ebml_uint(b'\x73\xc5', 1) + ebml_uint(b'\x83', 1) +
                                    ebml(b'\x86', b'V_MPEGH/ISO/HEVC') +
                                    ebml(b'\xe0', ebml_uint(b'\xb0', 64) + ebml_uint(b'\xba', 64))))]
for c in range(0, len(nals), 16384):    # SimpleBlock timecodes are 16-bit offsets from the Cluster's
    mkv.append(ebml(b'\x1f\x43\xb6\x75', ebml_uint(b'\xe7', c) + b''.join(
        ebml(b'\xa3', b'\x81' + struct.pack('>hB', i, 0x80) + b'\x00\x00\x01' + n[1])
        for i, n in enumerate(nals[c:c + 16384]))))
_, trace = run(['-loglevel', 'repeat+info', '-nofind_stream_info', '-f', 'matroska', '-i', 'pipe:0', '-map', '0:v',
                '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'framecrc', '-'], b''.join(mkv))
field_re = re.compile(r'^\[trace_headers @ [^\]]+\] (\d+)\s+(\S+)\s+([01]*) = (-?\d+)$')
units = []          # per NAL unit: {'fields': [(pos, name, bits, value)], 'failed': bool}
for l in trace:
    if re.search(r'Packet: \d+ bytes', l):
        units.append({'fields': [], 'failed': False})
        continue
    if not units:
        continue
    m = field_re.match(l)
    if m:
        units[-1]['fields'].append((int(m.group(1)), m.group(2), len(m.group(3)), int(m.group(4))))
    elif 'Failed to read unit' in l:
        units[-1]['failed'] = True
if len(units) != len(nals):
    warn('trace_headers: %d packets for %d NAL units' % (len(units), len(nals)))
for n, u in zip(nals, units):
    n.append(u)
for n in nals[len(units):]:
    n.append({'fields': [], 'failed': True})


def fdict(fields):
    d = {}
    for _, name, _, val in fields:
        d.setdefault(name, val)
    return d


def profile(f):
    p = f['general_profile_idc']
    if p == 0:
        p = next((i for i in range(1, 32) if f.get('general_profile_compatibility_flag[%d]' % i)), 0)
    return p


class RPS:
    """a short-term reference picture set after 7.4.8: s0 / s1 lists of (DeltaPoc, used), closest first"""

    def __init__(self, fields, idx, sets, in_slice):
        f = fdict((0, n, 0, v) for n, v in fields)
        self.inter = f.get('inter_ref_pic_set_prediction_flag', 0)
        self.ref_num = 0
        self.s0, self.s1 = [], []
        if self.inter:
            ref_idx = idx - (f.get('delta_idx_minus1', 0) + 1 if in_slice else 1)
            if ref_idx < 0:
                raise ValueError('RefRpsIdx %d' % ref_idx)
            ref = sets[ref_idx]
            nneg, nd = len(ref.s0), len(ref.s0) + len(ref.s1)
            self.ref_num = nd
            drps = (1 - 2 * f['delta_rps_sign']) * (f['abs_delta_rps_minus1'] + 1)
            used = [f.get('used_by_curr_pic_flag[%d]' % j, 0) for j in range(nd + 1)]
            use = [1 if used[j] else f.get('use_delta_flag[%d]' % j, 1) for j in range(nd + 1)]
            for j in range(len(ref.s1) - 1, -1, -1):                     # (7-61)
                d = ref.s1[j][0] + drps
                if d < 0 and use[nneg + j]:
                    self.s0.append((d, used[nneg + j]))
            if drps < 0 and use[nd]:
                self.s0.append((drps, used[nd]))
            for j in range(nneg):
                d = ref.s0[j][0] + drps
                if d < 0 and use[j]:
                    self.s0.append((d, used[j]))
            for j in range(nneg - 1, -1, -1):                            # (7-62)
                d = ref.s0[j][0] + drps
                if d > 0 and use[j]:
                    self.s1.append((d, used[j]))
            if drps > 0 and use[nd]:
                self.s1.append((drps, used[nd]))
            for j in range(len(ref.s1)):
                d = ref.s1[j][0] + drps
                if d > 0 and use[nneg + j]:
                    self.s1.append((d, used[nneg + j]))
        else:
            d = 0
            for i in range(f.get('num_negative_pics', 0)):
                d -= f['delta_poc_s0_minus1[%d]' % i] + 1
                self.s0.append((d, f['used_by_curr_pic_s0_flag[%d]' % i]))
            d = 0
            for i in range(f.get('num_positive_pics', 0)):
                d += f['delta_poc_s1_minus1[%d]' % i] + 1
                self.s1.append((d, f['used_by_curr_pic_s1_flag[%d]' % i]))

    def text(self):
        return lst(['%d%s' % (d, '*' if u else '') for d, u in self.s0 + self.s1])


def scaling_text(f):
    """scaling_list_data(): the 20 lists after prediction, coded order, DC first for 16x16 / 32x32"""
    lists, dcs, parts = {}, {}, []
    for s in range(4):
        for m in range(0, 6, 3 if s == 3 else 1):
            dc = 16
            if not f['scaling_list_pred_mode_flag[%d][%d]' % (s, m)]:
                delta = f['scaling_list_pred_matrix_id_delta[%d][%d]' % (s, m)]
                if delta == 0:
                    vals = [16] * 16 if s == 0 else list(DEFAULT_INTRA if m < 3 else DEFAULT_INTER)
                else:
                    ref = m - delta * (3 if s == 3 else 1)
                    vals, dc = list(lists[s, ref]), dcs[s, ref]
            else:
                nxt, vals = 8, []
                if s > 1:
                    nxt = dc = f['scaling_list_dc_coef_minus8[%d][%d]' % (s - 2, m)] + 8
                for i in range(min(64, 1 << (4 + 2 * s))):
                    nxt = (nxt + f['scaling_list_delta_coeff[%d][%d][%d]' % (s, m, i)] + 256) % 256
                    vals.append(nxt)
            lists[s, m], dcs[s, m] = vals, dc
            parts.append('%d.%d=%s%s' % (s, m, '%d:' % dc if s > 1 else '', ','.join(map(str, vals))))
    return ' '.join(parts)


class ParamSet:
    """a parsed VPS / SPS / PPS; its identity is what libavcodec's pointer comparisons see"""

    def __init__(self, raw):
        self.raw = raw


def parse_vps(n):
    f = fdict(n[5]['fields'])
    o = ParamSet(n[1])
    o.id = f['vps_video_parameter_set_id']
    ms = f['vps_max_sub_layers_minus1']
    o.text = ('vps id=%d sub_layers=%d nesting=%d profile=%d tier=%d level=%d dpb=%d/%d/%d'
              % (o.id, ms + 1, f['vps_temporal_id_nesting_flag'], profile(f), f['general_tier_flag'],
                 f['general_level_idc'], f['vps_max_dec_pic_buffering_minus1[%d]' % ms],
                 f['vps_max_num_reorder_pics[%d]' % ms], f['vps_max_latency_increase_plus1[%d]' % ms]))
    return o


def parse_sps(n):
    fields = n[5]['fields']
    names = [x[1] for x in fields]
    i0, i1 = names.index('num_short_term_ref_pic_sets'), names.index('long_term_ref_pics_present_flag')
    f = fdict(fields[:i0 + 1] + fields[i1:])
    o = ParamSet(n[1])
    o.id, o.vps = f['sps_seq_parameter_set_id'], f['sps_video_parameter_set_id']
    o.log2_max_poc_lsb = f['log2_max_pic_order_cnt_lsb_minus4'] + 4
    ctb = 1 << (f['log2_min_luma_coding_block_size_minus3'] + 3 + f['log2_diff_max_min_luma_coding_block_size'])
    o.ctb_w = (f['pic_width_in_luma_samples'] + ctb - 1) // ctb
    o.ctb_h = (f['pic_height_in_luma_samples'] + ctb - 1) // ctb
    # st_ref_pic_set(i): set 0 has no inter_ref_pic_set_prediction_flag, every later one starts with it
    groups = [[]]
    for _, name, _, val in fields[i0 + 1:i1]:
        if name == 'inter_ref_pic_set_prediction_flag':
            groups.append([])
        groups[-1].append((name, val))
    nst = f['num_short_term_ref_pic_sets']
    if nst == 0:
        groups = []
    if len(groups) != nst:
        raise ValueError('SPS %d: %d short-term sets in the trace, %d coded' % (o.id, len(groups), nst))
    o.st = []
    for i, g in enumerate(groups):
        o.st.append(RPS(g, i, o.st, False))
    nlt = f.get('num_long_term_ref_pics_sps', 0)
    o.lt = [(f['lt_ref_pic_poc_lsb_sps[%d]' % i], f['used_by_curr_pic_lt_sps_flag[%d]' % i]) for i in range(nlt)]
    ms = f['sps_max_sub_layers_minus1']
    pcm = '0'
    if f['pcm_enabled_flag']:
        pcm = '1:%d/%d/%d/%d/%d' % (f['pcm_sample_bit_depth_luma_minus1'] + 1, f['pcm_sample_bit_depth_chroma_minus1'] + 1,
                                    f['log2_min_pcm_luma_coding_block_size_minus3'] + 3,
                                    f['log2_diff_max_min_pcm_luma_coding_block_size'], f['pcm_loop_filter_disabled_flag'])
    rng = f['video_full_range_flag'] if f['vui_parameters_present_flag'] and f.get('video_signal_type_present_flag') else -1
    lines = ['sps id=%d vps=%d sub_layers=%d profile=%d tier=%d level=%d chroma=%d w=%d h=%d conf=%d,%d,%d,%d bd=%d/%d '
             'lpoc=%d dpb=%d/%d/%d cb=%d/%d tb=%d/%d thd=%d/%d scaling=%d/%d amp=%d sao=%d pcm=%s strps=%d lt=%d/%d '
             'tmvp=%d sis=%d vui=%d range=%d rext=%d'
             % (o.id, o.vps, ms + 1, profile(f), f['general_tier_flag'], f['general_level_idc'], f['chroma_format_idc'],
                f['pic_width_in_luma_samples'], f['pic_height_in_luma_samples'], f.get('conf_win_left_offset', 0),
                f.get('conf_win_right_offset', 0), f.get('conf_win_top_offset', 0), f.get('conf_win_bottom_offset', 0),
                f['bit_depth_luma_minus8'] + 8, f['bit_depth_chroma_minus8'] + 8, o.log2_max_poc_lsb,
                f['sps_max_dec_pic_buffering_minus1[%d]' % ms], f['sps_max_num_reorder_pics[%d]' % ms],
                f['sps_max_latency_increase_plus1[%d]' % ms], f['log2_min_luma_coding_block_size_minus3'] + 3,
                f['log2_diff_max_min_luma_coding_block_size'], f['log2_min_luma_transform_block_size_minus2'] + 2,
                f['log2_diff_max_min_luma_transform_block_size'], f['max_transform_hierarchy_depth_inter'],
                f['max_transform_hierarchy_depth_intra'], f['scaling_list_enabled_flag'],
                f.get('sps_scaling_list_data_present_flag', 0), f['amp_enabled_flag'],
                f['sample_adaptive_offset_enabled_flag'], pcm, nst, f['long_term_ref_pics_present_flag'], nlt,
                f['sps_temporal_mvp_enabled_flag'], f['strong_intra_smoothing_enabled_flag'],
                f['vui_parameters_present_flag'], rng, f.get('sps_range_extension_flag', 0))]
    for i, r in enumerate(o.st):
        lines.append('sps-st id=%d i=%d inter=%d d=%s' % (o.id, i, r.inter, r.text()))
    if nlt:
        lines.append('sps-lt id=%d %s' % (o.id, lst(['%d%s' % (p, '*' if u else '') for p, u in o.lt])))
    if f.get('sps_scaling_list_data_present_flag', 0):
        lines.append('sps-scaling id=%d %s' % (o.id, scaling_text(f)))
    o.text = '\n'.join(lines)
    return o


def tile_sizes(n, total, uniform, coded):
    if uniform:
        return [((i + 1) * total) // n - (i * total) // n for i in range(n)]
    sizes = [c + 1 for c in coded]
    return sizes + [total - sum(sizes)]


def parse_pps(n, sps):
    f = fdict(n[5]['fields'])
    o = ParamSet(n[1])
    o.id, o.sps_id, o.sps = f['pps_pic_parameter_set_id'], f['pps_seq_parameter_set_id'], sps
    if f['tiles_enabled_flag']:
        nc, nr, uni = f['num_tile_columns_minus1'] + 1, f['num_tile_rows_minus1'] + 1, f['uniform_spacing_flag']
        cols = tile_sizes(nc, sps.ctb_w, uni, [f['column_width_minus1[%d]' % i] for i in range(nc - 1)] if not uni else [])
        rows = tile_sizes(nr, sps.ctb_h, uni, [f['row_height_minus1[%d]' % i] for i in range(nr - 1)] if not uni else [])
    else:
        cols, rows = [sps.ctb_w], [sps.ctb_h]
    o.text = ('pps id=%d sps=%d dep=%d ofp=%d xbits=%d sdh=%d cabac_init=%d l0=%d l1=%d qp=%d cip=%d tskip=%d cuqpd=%d/%d '
              'cb=%d cr=%d sqp=%d wp=%d wb=%d tqb=%d tiles=%d wpp=%d cols=%s rows=%s lfat=%d lfas=%d dbk=%d/%d/%d/%d/%d '
              'lmod=%d pml=%d she=%d scaling=%d rext=%d'
              % (o.id, o.sps_id, f['dependent_slice_segments_enabled_flag'], f['output_flag_present_flag'],
                 f['num_extra_slice_header_bits'], f['sign_data_hiding_enabled_flag'], f['cabac_init_present_flag'],
                 f['num_ref_idx_l0_default_active_minus1'], f['num_ref_idx_l1_default_active_minus1'],
                 f['init_qp_minus26'], f['constrained_intra_pred_flag'], f['transform_skip_enabled_flag'],
                 f['cu_qp_delta_enabled_flag'], f.get('diff_cu_qp_delta_depth', 0), f['pps_cb_qp_offset'],
                 f['pps_cr_qp_offset'], f['pps_slice_chroma_qp_offsets_present_flag'], f['weighted_pred_flag'],
                 f['weighted_bipred_flag'], f['transquant_bypass_enabled_flag'], f['tiles_enabled_flag'],
                 f['entropy_coding_sync_enabled_flag'], lst(cols), lst(rows),
                 f.get('loop_filter_across_tiles_enabled_flag', 1), f['pps_loop_filter_across_slices_enabled_flag'],
                 f['deblocking_filter_control_present_flag'], f.get('deblocking_filter_override_enabled_flag', 0),
                 f.get('pps_deblocking_filter_disabled_flag', 0), f.get('pps_beta_offset_div2', 0),
                 f.get('pps_tc_offset_div2', 0), f['lists_modification_present_flag'],
                 f['log2_parallel_merge_level_minus2'], f['slice_segment_header_extension_present_flag'],
                 f['pps_scaling_list_data_present_flag'], f.get('pps_range_extension_flag', 0)))
    if f['pps_scaling_list_data_present_flag']:
        o.text += '\n' + 'pps-scaling id=%d %s' % (o.id, scaling_text(f))
    return o


# --- the decoder state, as libavcodec keeps it
vps_tab, sps_tab, pps_tab = {}, {}, {}
active_sps = None   # the SPS object of the last picture started (HEVCLayerContext.sps)
dpb = {}            # reference pictures: POC -> 'S' | 'L'
poc_tid0 = 0
eos = 1
no_rasl = 0
pic_pocs = []       # POCs of the decoded pictures (pic lines without " missing")


def remove_sps(i):
    if i in sps_tab:
        for p in [p for p, o in pps_tab.items() if o.sps_id == i]:
            del pps_tab[p]
        del sps_tab[i]


def start_picture(n, last_eos):
    """the first slice segment of a picture: hls_slice_header() + hevc_frame_start(); returns its line"""
    global active_sps, dpb, poc_tid0, no_rasl
    fields = n[5]['fields']
    if n[5]['failed'] or not fields:
        warn('picture at byte %d: trace_headers could not read its first slice segment header' % n[0])
        return None
    f = fdict(fields)
    pps = pps_tab.get(f['slice_pic_parameter_set_id'])
    if pps is None:
        warn('picture at byte %d: PPS %d not available' % (n[0], f['slice_pic_parameter_set_id']))
        return None
    sps = pps.sps
    t, tid = n[2], n[4]
    idr, irap, bla = t in (19, 20), 16 <= t <= 23, 16 <= t <= 18
    max_lsb = 1 << sps.log2_max_poc_lsb
    lsb = f.get('slice_pic_order_cnt_lsb', 0)
    if idr:
        poc = 0
    else:
        prev_lsb = c_mod(poc_tid0, max_lsb)
        prev_msb = poc_tid0 - prev_lsb
        if lsb < prev_lsb and prev_lsb - lsb >= max_lsb // 2:
            msb = prev_msb + max_lsb
        elif lsb > prev_lsb and lsb - prev_lsb > max_lsb // 2:
            msb = prev_msb - max_lsb
        else:
            msb = prev_msb
        poc = (0 if bla else msb) + lsb
    if t in (8, 9) and no_rasl:
        return 'skip %s poc=%d' % (NAL_NAMES[t], poc)

    # the short- and long-term sets of the slice header
    names = [x[1] for x in fields]
    st, rps, rpsbits, refdpoc = '-', None, 0, 0
    if not idr:
        k = names.index('short_term_ref_pic_set_sps_flag')
        if f['short_term_ref_pic_set_sps_flag']:
            idx = f.get('short_term_ref_pic_set_idx', 0)
            st, rps = 'sps:%d' % idx, sps.st[idx]
        else:
            e = k + 1
            while e < len(fields) and RPS_NAMES.match(fields[e][1]):
                e += 1
            seg = fields[k + 1:e]
            rps = RPS([(x[1], x[3]) for x in seg], len(sps.st), sps.st, True)
            st, rpsbits, refdpoc = 'sh', seg[-1][0] + seg[-1][2] - seg[0][0], rps.ref_num
    lts = []            # (value, used, msb present)
    nsps = f.get('num_long_term_sps', 0)
    prev_delta = 0
    for i in range(nsps + f.get('num_long_term_pics', 0)):
        if i < nsps:
            val, used = sps.lt[f.get('lt_idx_sps[%d]' % i, 0)]
        else:
            val, used = f['poc_lsb_lt[%d]' % i], f['used_by_curr_pic_lt_flag[%d]' % i]
        msb_present = f.get('delta_poc_msb_present_flag[%d]' % i, 0)
        if msb_present:
            delta = f['delta_poc_msb_cycle_lt[%d]' % i]
            if i and i != nsps:
                delta += prev_delta
            val = poc - delta * max_lsb - (lsb - val)
            prev_delta = delta
        lts.append((val, used, msb_present))

    # hevc_frame_start()
    if active_sps is not sps:
        dpb, active_sps = {}, sps
    if idr:
        dpb = {}
    if irap:
        no_rasl = idr or bla or (t == 21 and last_eos)
    if tid == 0 and t not in (0, 2, 4, 6, 7, 8, 9):
        poc_tid0 = poc

    # ff_hevc_frame_rps(): S0, S1, then the long-term entries; lookups among the references held before this one.
    # (find_ref_idx() in refs.c also finds a picture that is no longer a reference but still waits for output; the
    # format does not model output, so after a " missing" picture libavcodec may decode pictures marked missing here.)
    entries = []        # (list, found POC or None, shown value, long-term)
    if rps:
        for d, u in rps.s0:
            entries.append(('bef' if u else 'foll', poc + d if poc + d in dpb else None, poc + d, False))
        for d, u in rps.s1:
            entries.append(('aft' if u else 'foll', poc + d if poc + d in dpb else None, poc + d, False))
    for val, used, msb_present in lts:
        if msb_present:
            found = val if val in dpb else None
        else:
            found = next((p for p in dpb if p & (max_lsb - 1) == val and p != poc), None)
        entries.append(('lt' if used else 'foll', found, val, True))
    marked, missing = {}, False
    for kind, found, val, is_lt in entries:
        if found is None:
            if not irap and kind != 'foll':
                missing = True
                break
            continue
        marked[found] = 'L' if is_lt else 'S'
    dpb = marked
    lists = {k: [str(fd) if fd is not None else '?%d' % v for kk, fd, v, _ in entries if kk == k]
             for k in ('bef', 'aft', 'lt')}
    held = lst(['%d:%s' % (p, dpb[p]) for p in sorted(dpb)])
    if not missing:
        dpb[poc] = 'S'
        pic_pocs.append(poc)
    return ('pic %s poc=%d tid=%d slices=? st=%s rpsbits=%d refdpoc=%d hdr=%d bef=%s aft=%s lt=%s dpb=%s%s'
            % (NAL_NAMES[t], poc, tid, st, rpsbits, refdpoc, fields[-1][0] + fields[-1][2], lst(lists['bef']),
               lst(lists['aft']), lst(lists['lt']), held, ' missing' if missing else ''))


def parameter_set(n):
    """a VPS / SPS / PPS NAL unit: stored as ps.c stores it (an identical copy changes nothing), printed"""
    t = n[2]
    if t == VPS_NUT:
        o = parse_vps(n)
        old = vps_tab.get(o.id)
        if not (old and old.raw == o.raw):
            for s in [s for s, so in sps_tab.items() if so.vps == o.id]:
                remove_sps(s)
            vps_tab[o.id] = o
    elif t == SPS_NUT:
        o = parse_sps(n)
        if o.vps not in vps_tab:
            return warn('SPS %d at byte %d: VPS %d not available' % (o.id, n[0], o.vps))
        old = sps_tab.get(o.id)
        if not (old and old.raw == o.raw):
            remove_sps(o.id)
            sps_tab[o.id] = o
    else:
        sid = fdict(n[5]['fields'])['pps_seq_parameter_set_id']
        if sid not in sps_tab:
            return warn('PPS at byte %d: SPS %d not available' % (n[0], sid))
        o = parse_pps(n, sps_tab[sid])
        old = pps_tab.get(o.id)
        if not (old and old.raw == o.raw):
            pps_tab[o.id] = o
    print(o.text)


def finish(pic, slices):
    if pic:
        print(pic.replace('slices=?', 'slices=%d' % slices))


for pkt_end, pk in packets:
    # decode_nal_units(): an EOS / EOB before any other NAL unit of the packet applies to this one, a later one to
    # the next
    last_eos, eos = eos, 0
    at_start = True
    for n in pk:
        if n[2] in (EOS_NUT, EOB_NUT):
            if at_start:
                last_eos = 1
            else:
                eos = 1
        else:
            at_start = False
    pic, slices = None, 0
    for n in pk:
        t = n[2]
        if n[3] != 0 or not (t in NAL_NAMES or t in (VPS_NUT, SPS_NUT, PPS_NUT)):
            continue
        if t in NAL_NAMES and not n[1][2] >> 7:     # first_slice_segment_in_pic_flag = 0
            slices += 1
            continue
        if t in NAL_NAMES:
            finish(pic, slices)
            if pic:
                warn('two pictures in the packet ending at byte %d' % pkt_end)
            pic, slices = None, 1
        elif n[5]['failed'] or not n[5]['fields']:
            warn('NAL unit type %d at byte %d: trace_headers could not read it' % (t, n[0]))
            continue
        try:
            if t in NAL_NAMES:
                pic = start_picture(n, last_eos)
            else:
                parameter_set(n)
        except (KeyError, ValueError, IndexError) as e:
            warn('NAL unit type %d at byte %d: %s %s' % (t, n[0], type(e).__name__, e))
    finish(pic, slices)

# --- libavcodec's decoder: the POC of every picture it decodes (the context that printed last: ffmpeg probes with
# a decoder of its own first)
_, dbg = run(['-threads', '1', '-loglevel', 'repeat+debug', '-f', 'hevc', '-i', stream, '-f', 'null', '-'])
dec_re = re.compile(r'^\[hevc @ (0x[0-9a-f]+)\] Decoded frame with POC 0/(-?\d+)\.$')
decoded = [m.groups() for m in map(dec_re.match, dbg) if m]
dec_pocs = [int(p) for c, p in decoded if decoded and c == decoded[-1][0]]
if dec_pocs != pic_pocs:
    i = next((i for i, (a, b) in enumerate(zip(dec_pocs, pic_pocs)) if a != b), min(len(dec_pocs), len(pic_pocs)))
    warn('POC mismatch at picture %d: libavcodec decoded %d pictures (%s), the golden has %d (%s)'
         % (i, len(dec_pocs), lst(dec_pocs[i:i + 8]), len(pic_pocs), lst(pic_pocs[i:i + 8])))
for p in problems:
    print('mkgolden: %s: %s' % (stream, p), file=sys.stderr)
sys.exit(1 if any(p.startswith('POC mismatch') for p in problems) else 0)
