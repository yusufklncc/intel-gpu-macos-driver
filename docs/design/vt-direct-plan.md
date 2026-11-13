# VideoToolbox decoders: driving Vulkan Video directly

> Design note with its date (13 Nov 2026): it records the plan and the state of that day, not the current status. The
> current state is in `videotoolbox/README.md` ("Direct Vulkan Video decode") and the ROADMAP.

**[V]** = read in source. **[I]** = inference. Sources: FFmpeg 9.0.2 with `videotoolbox/ffmpeg/` applied
(`libavcodec/h264*.c`, `vulkan_decode.c`, `vulkan_h264.c`, `libavutil/hwcontext_vulkan.c`), Mesa (`src/intel/vulkan/
genX_cmd_video.c`, `anv_video.c`, `src/vulkan/runtime/vk_video.c`), ITU-T H.264 (08/2021).

## Why, and which codec first

The ROADMAP's 12 Nov item ("drive Vulkan Video directly where libavcodec adds nothing") came from an assessment of the
**encoders** (`videotoolbox/README.md`, "Driving Vulkan Video directly: assessment"): not bounded there, because
libavcodec does the GOP, rate control and headers. The decoders are different: on the decode side libavcodec's work per
picture is bookkeeping the hardware or our side already does [V]:

- the packet (our copy of the sample), then `ff_h2645_packet_split`: a pass over every NAL unit for emulation prevention
  bytes, copied when there is one;
- the full slice header, with reference list initialisation, modification and prediction weights;
- `ff_vk_decode_prepare_frame`: a views buffer and two `vkCreateImageView`, destroyed with the picture;
- `ff_vk_decode_add_slice`: the slice copied into a pooled host-visible buffer (this one stays);
- `ff_vk_decode_frame`: an exec context (a fence wait when all four are busy), an `av_buffer_ref` per frame, view set and
  buffer, the barriers, the submit.

What Vulkan Video needs from the application differs by codec [V Mesa]:

| Codec | Parsed by the application | Parsed on the GPU / by ANV |
|---|---|---|
| H.264 | SPS/PPS, slice header up to `dec_ref_pic_marking`, POC, DPB marking | MFX in short format: slice headers, reference lists, weights |
| HEVC | VPS/SPS/PPS, slice segment header to the RPS, POC, the RPS-driven DPB | the HuC slice-to-long kernel writes the HCP slice commands |
| VP9 | uncompressed header, segmentation / loop filter state, reference slots | compressed header, probabilities |
| AV1 | sequence header, the whole frame header, reference state | tiles |

H.264 is the smallest bounded piece, so it goes first; HEVC next (the RPS is the only large part); VP9 and AV1 later, if
the H.264 numbers say it pays.

## Parser: which one

- libavcodec's own parsers are not usable without its decoder: CBS (`ff_cbs_*`) and the H.264 parameter-set code are not
  exported from the shared `libavcodec.63.dylib` (only `av_*` / `avcodec_*`), and linking their sources statically would
  put LGPL code inside our MIT library, unlike today's separate FFmpeg libraries.
- Third-party: Khronos Vulkan-Video-Samples' NvVideoParser (Apache-2.0, C++, large), GStreamer's codecparsers (LGPL),
  Chromium's (BSD, needs its base library). None is small enough to be worth the dependency for H.264.
- So: our own, written from the specification, with libavcodec's conventions where the hardware must get the same values
  (`vtdirect_h264.c`, ~650 lines, MIT). No third-party code; nothing to add to the licence notes beyond that.

libavcodec's conventions it follows [V `h264dec.c` idr(), `h264_parse.c` ff_h264_init_poc, `h264_refs.c`]: POCs offset by
65536 from an IDR (its previous MSB starts at 1<<16, the previous LSB at the IDR's own); memory_management_control_operation
5 sets frame_num to 0 and keeps the POC; the sliding window at short + long >= max_num_ref_frames; a duplicate frame_num
dropped when the current picture is added; references short-term newest first, then long-term by index.

## Design

- `vtdirect_h264.c`: NAL units from the length-prefixed sample, SPS/PPS (VUI only to the full-range flag), slice headers
  read with an emulation-prevention-aware bit reader (no unescaped copy), POC, DPB slots (17: 16 references and the
  current picture), marking. No Vulkan calls: unit-tested here.
- `vtdirect.c`: the session (the device's largest size, DPB slots and references, inline parameter sets), its memory, a
  libavutil frames context made like `ff_vk_frame_params` (so the copy into the IOSurface, `gpu_copy`, takes the frames
  unchanged), eight command buffers and bitstream buffers in a ring on a timeline semaphore, one view per pooled image.
  Per picture: one copy of the slices (with start codes) into the bitstream buffer, barriers to the DPB layout under each
  frame's lock, a wait on and a signal of each frame's timeline (libavutil's protocol, which the copy engine follows),
  begin / decode / end coding, one submit.
- `intelxeav1.c`: `INTELXEDEC_DIRECT` read at StartSession; libavcodec opened only when the direct path does not take
  the stream, or when it hands it over mid-stream (`VTD_FALLBACK`: from that sample on, which is an IDR when it comes
  with a new SPS). Reads the sample in place when the block buffer is contiguous. `IntelXeDec_LastStats` and the close
  log line give the path, fps, wall and thread-CPU ms per DecodeFrame, latency until emitted.
- Known difference, broken streams only: libavcodec's reference lookup (`find_ref_idx`) also finds pictures that are no
longer references but wait for output, so after a picture that misses a reference (not decoded) a later picture may
find a picture the direct path has dropped; the direct path then answers that picture with an error as well. Not
modelled: the duplicate-POC check, recovery point SEI (missing references generated instead), slice header errors.

Not covered (libavcodec decodes): fields and MBAFF, other profiles / bit depths / chroma formats, FMO, data
  partitioning, SPS that allow frame_num gaps, streams that do not start with an IDR, a size change. A P/B picture whose
  references are missing (after a seek to a non-IDR picture) is not decoded but answered with an error.

## Tests

- Off the GPU (`videotoolbox/tests/h264/run.sh --conformance DIR`): each picture's type, frame_num, POCs, slice count and
  reference pictures (frame_num and POC, short and long-term), and each SPS/PPS field with the scaling lists, compared
  with libavcodec 9.0.2 (`ffmpeg -debug pict+mmco`, `-bsf:v trace_headers`). 13 Nov: 6 recorded streams and 89 of the 185
  JVT conformance streams identical, 96 not covered (fields/MBAFF 67, other profiles 22, not 8-bit 4:2:0 2, gaps allowed 5), 0 differ.
- On the B580 (`videotoolbox/tests/vtdirect-ab.sh`, nightly): per-picture MD5 on both paths for the 92 conformance
  streams of the kit (89 covered, 3 that must fall back) and the mediakit H.264 clips (also against their software
  reference md5s); time and CPU, three alternated runs per path on the mediakit clips.

## Expected gain, and what decides the default

The B580's VDBOX bounds throughput (H.264 1080p30: 922 fps on the libavcodec path, ~1.1 ms per picture end to end), so
fps should move little; the gain is CPU on the thread that calls DecodeFrame (the client's, or VTDecoderXPCService's):
no packet copy, no unescape pass, no slice/ref-list work, no per-picture view creation, fewer allocations. [I] Expected:
the decoding thread's CPU ms per picture down by a third or more, fps within ±5%, latency the same or lower. The default
changes only if every MD5 matches and the CPU gain shows; HEVC then follows the same pattern.

## HEVC (13 Nov, second part)

The H.264 path's A/B on the B580 (13 Nov night): every picture's MD5 identical on both paths for the 92 conformance
streams of the kit (89 covered, 3 that fall back). HEVC follows the same pattern: `vtdirect_hevc.c` (MIT, from ITU-T
H.265 V10 7.3 / 7.4 / 8.3.1 / 8.3.2 / E.2), `INTELXEDEC_DIRECT=hevc` (or `h264,hevc`, `1`, `all`).

What ANV reads for H.265 [V Mesa `genX_cmd_video.c` anv_h265_decode_video, `genX_huc.h` S2L DMEM]: the SPS/PPS fields
of HCP_PIC_STATE / HCP_TILE_STATE / HCP_QM_STATE and of the HuC slice-to-long kernel's picture parameters; the current
POC, every reference slot's POC; RefPicSetStCurrBefore / After / LtCurr (slot indices); NumBitsForSTRefPicSetInSlice
(the kernel skips the slice's st_ref_pic_set() with it); the slice segment offsets (the kernel parses the slice headers,
builds the lists and writes the HCP slice commands). Not read: the VPS, the VUI, pShortTermRefPicSet,
pLongTermRefPicsSps, NumDeltaPocsOfRefRpsIdx, the level. maxDpbSlots 16, maxActiveReferencePictures 8 (HCP).

libavcodec 9.0.2's behaviour the direct path copies [V `hevcdec.c`, `refs.c`, `ps.c`, `vulkan_hevc.c`], because the
hardware must get the same values or decode the same pictures:

- POC: `ff_hevc_compute_poc2` (the previous TemporalId-0 picture's POC split with C's `%`, MSB 0 for BLA only, not for a
  CRA that starts a sequence: such POCs are offset from the specification's by a constant, which the hardware does not
  see); pocTid0 updated for every picture that starts (failed ones included) except RASL/RADL/sub-layer non-reference;
- end of sequence: `eos` starts at 1 (and a flush sets it); an EOS/EOB NAL unit first in a sample applies to it, later
  ones to the next sample; NoRaslOutputFlag = IDR, BLA, or CRA after an end of sequence; RASL pictures then are not
  decoded (VideoToolbox gets a dropped frame, as from the libavcodec path, which allocates nothing for them);
- DPB: an IDR, or a picture with another SPS (another id, or the same id re-sent with other content: an identical
  repeat changes nothing), drops every reference; otherwise only the RPS marks (libavcodec does not empty the DPB at a
  CRA/BLA with NoRaslOutputFlag either); every decoded picture becomes a short-term reference; short-term entries by
  full POC, long-term by POC or its LSBs (not the current POC); a missing StFoll/LtFoll entry, or any of an IRAP
  picture, is ignored (libavcodec generates a frame for it and passes it as an unused slot; the direct path passes
  nothing: the hardware only reads the current lists); a missing entry of the current lists of a non-IRAP picture:
  the picture is not decoded and is not added, the entries after it are dropped (libavcodec stops there);
- the references passed: every picture the RPS kept (StFoll/LtFoll included), slot order (libavcodec: its DPB order;
  the hardware maps them through RefPicSetStCurr*/LtCurr, so the order and the slot numbers do not change the decode);
- Std values the driver reads that are not the syntax: `sps_scaling_list_data_present_flag` = scaling_list_enabled_flag
  with the default lists when the SPS codes none; the PCM fields of an SPS without PCM wrap as libavcodec's unsigned
  subtraction (bit depth 255, log2 min 253: masked into HCP_PIC_STATE); tile column widths / row heights derived (6.5.1)
  also for uniform spacing; `pps_act_*_plus5/3` = 5/5/3. Fields the driver does not read follow the specification
  (conf_win in chroma units, delta_idx_minus1 0 for SPS sets, use_delta_flag as a bit mask, the level enum from
  general_level_idc / 3) where libavcodec has quirks (luma units, 0xFFFFFFFF, the last flag, always 6.2).
- parameter sets as `ps.c` keeps them: an identical repeat changes nothing; a VPS with other content drops the SPSs
  that use it and their PPSs, an SPS with other content drops its PPSs (a byte-identical SPS sent again after that is
  a new SPS: the references go);
- a parameter set that does not parse (a PPS before its SPS: RPS_C, VPSSPSPPS_A) is skipped, as libavcodec skips it;
  SPS/PPS extension data (`sps_extension_4bits`, PS_B) is ignored; the range and SCC extensions are not covered.

Not covered (libavcodec decodes): other profiles (RExt, SCC, multilayer), 4:0:0/4:2:2/4:4:4, bit depths other than 8 /
10 or luma != chroma, a stream that does not start with an IRAP picture, a size or bit depth change, an hvcC without an
SPS (hev1 with every parameter set in band). Like H.264: one decode per sample, the slices copied once with start codes.

Tests: `tests/hevcparse_test.c` prints, per picture, the type, POC, slice segments, the RPS (sps index or in the header,
NumBitsForSTRefPicSetInSlice, NumDeltaPocsOfRefRpsIdx), the slice header's length to byte_alignment() (the whole header
parsed in test mode: entry points, weights, list modification, header extension), the current lists and the
references held; every VPS/SPS/PPS field Vulkan gets, the scaling lists after prediction and the SPS's RPS sets.
`tests/hevc/mkgolden.py` makes the same text from FFmpeg 9.0.2 without our C code: the syntax from trace_headers (CBS),
the derivations (inter RPS prediction, scaling-list prediction and defaults, tile sizes, POC, the RPS-driven DPB as
above) written again in Python, and the decoded POC sequence checked against libavcodec's own decoder ("Decoded frame
with POC"). There is no libavcodec debug output of HEVC reference lists (unlike H.264's `-debug mmco`): the lists are
checked by two implementations of the same rules, the POCs by libavcodec itself, the pixels on the B580 by the A/B.

13 Nov results off the GPU: also 42 more streams made while writing mkgolden.py (x265 options, Apple's encoder modes,
edited streams: EOS/EOB before a CRA, CRA as BLA_W_LP / W_RADL / N_LP, a stream cut at a CRA, a VPS change, splices):
35 identical, 7 not covered (other formats, size changes, a stream starting with a TRAIL picture). The 6 recorded
streams and 144 of the 147 JCT-VC HEVC_v1 conformance streams identical
(2 change size mid-stream: identical up to the change, then libavcodec; TSUNEQBD_A_MAIN10 has luma and chroma bit
depths that differ, which libavcodec does not decode either). Not yet run on the B580: the night A/B
(`tests/night-vtdirect.sh` `phase_vtdirect_hevc`) compares every picture's MD5 on both paths for the conformance streams
and the mediakit HEVC 8/10-bit clips, then the time and the decoding thread's CPU.
