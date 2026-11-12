#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mkgolden.py <stream.264> [ffmpeg]: the reference output of h264parse_test for a recorded Annex B stream, made by
libavcodec's own H.264 decoder (FFmpeg 9.0.2, the version the plug-ins ship): the parameter sets from the trace_headers
bitstream filter (the syntax elements as read, scaling lists rebuilt from their delta_scale values), each picture's
type, frame_num and POCs from -debug pict, and the reference pictures it is decoded with from -debug mmco (the
short- and long-term lists libavcodec prints when it builds the picture's reference lists: the DPB before the
picture, which is what its Vulkan hwaccel passes to vkCmdDecodeVideoKHR). Writes to stdout."""
import re
import subprocess
import sys

stream = sys.argv[1]
ffmpeg = sys.argv[2] if len(sys.argv) > 2 else 'ffmpeg'


def run(args):
    return subprocess.run([ffmpeg, '-hide_banner', '-nostdin'] + args, capture_output=True, text=True).stderr.splitlines()


def scaling_from_deltas(deltas, size):
    """scaling_list(): the list values (bitstream order) and useDefaultScalingMatrixFlag from the delta_scale values"""
    out, last, nxt, use_default, it = [], 8, 8, False, iter(deltas)
    for j in range(size):
        if nxt:
            nxt = (last + next(it) + 256) % 256
            if j == 0 and nxt == 0:
                use_default = True
        out.append(nxt if nxt else last)
        last = out[-1]
    return out, use_default


def scaling_text(fields, lists, present_flag, nlists):
    if not fields.get(present_flag, 0):
        return ' scaling=-'
    present = default = 0
    parts = []
    for i in range(nlists):
        if i not in lists:
            continue
        present |= 1 << i
        vals, use_def = scaling_from_deltas(lists[i], 16 if i < 6 else 64)
        if use_def:
            default |= 1 << i
        else:
            parts.append(' L%d=%s' % (i, ','.join(map(str, vals))))
    return ' scaling=%x/%x%s' % (present, default, ''.join(parts))


# --- parameter sets and access units, in stream order (trace_headers)
line_re = re.compile(r'^\[trace_headers @ [^\]]+\] (\d+)\s+(\S+)\s+\S+ = (-?\d+)$')
title_re = re.compile(r'^\[trace_headers @ [^\]]+\] (.+)$')
units = []          # per packet: list of ('sps'|'pps', text) and ('slice', None)
cur, fields, lists, cur_list = None, {}, {}, None
sps_chroma = {}


def close_section():
    global cur, fields, lists
    if cur == 'sps':
        f = fields
        sid = f['seq_parameter_set_id']
        sps_chroma[sid] = f.get('chroma_format_idc', 1)
        poc_type = f['pic_order_cnt_type']
        txt = ('sps id=%d profile=%d level=%d chroma=%d lfn=%d poc_type=%d lpoc=%d refs=%d w=%d h=%d fmo=%d d8=%d crop=%d,%d,%d,%d'
               % (sid, f['profile_idc'], f['level_idc'], f.get('chroma_format_idc', 1), f['log2_max_frame_num_minus4'], poc_type,
                  f.get('log2_max_pic_order_cnt_lsb_minus4', 0) if poc_type == 0 else 0, f['max_num_ref_frames'],
                  f['pic_width_in_mbs_minus1'], f['pic_height_in_map_units_minus1'], f['frame_mbs_only_flag'],
                  f['direct_8x8_inference_flag'], f.get('frame_crop_left_offset', 0), f.get('frame_crop_right_offset', 0),
                  f.get('frame_crop_top_offset', 0), f.get('frame_crop_bottom_offset', 0)))
        txt += scaling_text(f, lists, 'seq_scaling_matrix_present_flag', 12)
        units[-1].append(('sps', txt))
    elif cur == 'pps':
        f = fields
        cqp = f['chroma_qp_index_offset']
        txt = ('pps id=%d sps=%d cabac=%d l0=%d l1=%d wp=%d wb=%d qp=%d qs=%d cqp=%d dbk=%d cip=%d rpc=%d t8=%d cqp2=%d'
               % (f['pic_parameter_set_id'], f['seq_parameter_set_id'], f['entropy_coding_mode_flag'],
                  f['num_ref_idx_l0_default_active_minus1'], f['num_ref_idx_l1_default_active_minus1'], f['weighted_pred_flag'],
                  f['weighted_bipred_idc'], f['pic_init_qp_minus26'], f['pic_init_qs_minus26'], cqp,
                  f['deblocking_filter_control_present_flag'], f['constrained_intra_pred_flag'], f['redundant_pic_cnt_present_flag'],
                  f.get('transform_8x8_mode_flag', 0), f.get('second_chroma_qp_index_offset', cqp)))
        txt += scaling_text(f, lists, 'pic_scaling_matrix_present_flag', 12)
        units[-1].append(('pps', txt))
    elif cur == 'slice':
        if fields.get('redundant_pic_cnt', 0) == 0:
            units[-1].append(('slice', None))
    cur, fields, lists = None, {}, {}


in_packets = False
for l in run(['-i', stream, '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-']):
    m = line_re.match(l)
    if m:
        if not in_packets or cur is None:
            continue
        name, val = m.group(2), int(m.group(3))
        lm = re.match(r'(seq|pic)_scaling_list_present_flag\[(\d+)\]', name)
        if lm:
            cur_list = int(lm.group(2)) if val else None
            if val:
                lists[cur_list] = []
        elif name.startswith('delta_scale'):
            lists[cur_list].append(val)
        else:
            fields.setdefault(name, val)
        continue
    t = title_re.match(l)
    if not t:
        continue
    title = t.group(1)
    if title.startswith('Packet:'):
        close_section()
        in_packets = True
        units.append([])
    elif title == 'Extradata':
        close_section()
        in_packets = False
    elif in_packets:
        close_section()
        if title == 'Sequence Parameter Set':
            cur = 'sps'
        elif title == 'Picture Parameter Set':
            cur = 'pps'
        elif 'Slice' in title:
            cur = 'slice'
        else:
            cur = 'other'
close_section()

# --- pictures (the decoder that ran last: ffmpeg's find_stream_info probes with a decoder of its own)
dbg = run(['-threads', '1', '-debug', 'pict+mmco', '-i', stream, '-f', 'null', '-'])
ctx_re = re.compile(r'^\[h264 @ (0x[0-9a-f]+)\] (.*)$')
last_ctx = None
for l in dbg:
    m = ctx_re.match(l)
    if m and m.group(2).startswith('slice:'):
        last_ctx = m.group(1)
pics = []
short, long_, mode = [], [], None
slice_re = re.compile(r'slice:(\d+) (\S) mb:\d+ (\S)( fix)?( IDR)? frame:(\d+) poc:(-?\d+)/(-?\d+)')
for l in dbg:
    m = ctx_re.match(l)
    if not m or m.group(1) != last_ctx:
        continue
    body = m.group(2)
    if body == 'short term list:':
        mode, short = 'S', []
        continue
    if body == 'long term list:':
        mode, long_ = 'L', []
        continue
    e = re.match(r'^(\d+) fn:(-?\d+) poc:(-?\d+)', body)
    if e and mode:
        (short if mode == 'S' else long_).append((int(e.group(1)), int(e.group(2)), int(e.group(3))))
        continue
    mode = None
    s = slice_re.search(body)
    if s:
        if s.group(1) == '1':
            refs = ' '.join('%d:%d' % (fn, poc) for _, fn, poc in short)
            lrefs = ' '.join('%d:%d' % (i, poc) for i, _, poc in long_)
            pics.append(['pic %s%s frame=%s poc=%s/%s' % (s.group(3), s.group(5) or '', s.group(6), s.group(7), s.group(8)), 1,
                         ' S' + (' ' + refs if refs else '') + ' L' + (' ' + lrefs if lrefs else '')])
        else:
            pics[-1][1] += 1

pi = 0
for u in units:
    has_slice = False
    for kind, txt in u:
        if kind == 'slice':
            has_slice = True
        else:
            print(txt)
    if has_slice:
        p = pics[pi]
        pi += 1
        print('%s slices=%d%s' % (p[0], p[1], p[2]))
if pi != len(pics):
    sys.exit('mkgolden: %d pictures from -debug pict, %d access units with slices' % (len(pics), pi))
