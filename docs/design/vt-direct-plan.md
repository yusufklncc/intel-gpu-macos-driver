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
depths that differ, which libavcodec does not decode either). The night A/B (`tests/night-vtdirect.sh`
`phase_vtdirect_hevc`) compares every picture's MD5 on both paths for the conformance streams and the mediakit HEVC
8/10-bit clips, then the time and the decoding thread's CPU.

### First B580 A/B (13 Nov 17:42) and who was right

Both paths: 141 of 150 identical, 8 DIFFER, 1 decoded by neither; the mediakit clips identical and equal to the software
reference; the direct path's decoding thread 20-35% less CPU. Identical is not right, so every picture was then
checked against a software reference: libavcodec's HEVC decoder on the CPU over the published `.bit` (its whole yuv
output equals the zip's published MD5 for 142 of the 144 streams of the kit), one MD5 per picture in vtdec's format
(NV12 / P010 planes), compared as multisets (vtdec lists decode order). And, as a third decoder, Apple's own (vtdec
without `--lib` on an Apple-silicon Mac, 8-bit streams: its P010 variant `p420` is not hashed alike) over the kit's
streams. Findings:

- **The kit, not a decoder, broke 13 streams.** FFmpeg's MP4 muxer drops every in-band VPS/SPS/PPS from an `hvc1`
  track (`movenc.c`: `filter_ps` for the `hvc1` tag), and INITQP_A/B, SLIST_A-D, TILES_A/B, DELTAQP_A/B, DBLK_A/B and
  TSKIP_A send other parameter sets mid-stream (INITQP: a PPS per picture with another init_qp_minus26, 61 in the
  `.bit`, 1 in the MP4). Both paths decoded those pictures with the first PPS: 12 wrong on both (DBLK_A happens to come
  out right), 5 of them reported DIFFER only because the paths fail differently on the wrong input (libavcodec rejects a
  slice QP below 0 for 8 bits and drops the picture; the direct path, like the hardware, decodes it). The direct
  parser's Std values for these streams (init_qp_minus26 down to -26 / Main 10 -38, the scaling lists after prediction
  incl. DC and the 32x32 step of 3, uniform and explicit tile sizes) are identical to FFmpeg's in `tests/hevc/run.sh
  --conformance`. Fix (kit): mux as `hev1` (keeps them), then rename the sample entry `hvc1`
  (`tests/hevc/inband-hvc1.py`; VideoToolbox has no `hev1` decoder of its own). With those files Apple's decoder gets
  every picture of all 13 right (the 8-bit ones; INITQP_B is 10-bit).
- **MAXBINS_C_TI_5 (decoded by neither):** its MP4 had one timestamp for all 3 packets, a zero-length track from which
  AVFoundation reads no sample (`samples 0`): no decoder ran. Fix (kit): muxed again with the packets numbered (setts).
- **RPS_B_qualcomm_5, RPS_C_ericsson_5, RPS_E_qualcomm_5: the direct path is right** (every picture equals the
  reference), **the libavcodec path aborts** (SIGABRT before the first picture). These are the only conformance streams
  with sps_max_dec_pic_buffering 16. libavcodec's Vulkan hwaccel passes its DPB array index (0..31) as the
  `slotIndex`; `alloc_frame` takes the new picture's entry before `ff_hevc_frame_rps` frees the unused references, so
  with a 16-picture DPB the index reaches 16 (a replay of `refs.c`'s allocation and bumping over the golden files: index
  16 on 4 / 1 / 14 pictures of RPS_B / C / E, at most 5 / 10 / 5 on RPS_A / D / F). ANV (maxDpbSlots 16) writes
  `dpb_idx[slot]` into a 16-entry stack array without a check (asserts off), the stack protector aborts. FFmpeg's bug
  (slotIndex must be below the session's maxDpbSlots), ANV's missing guard; not ours: the direct path keeps its own
  16 slots (`hevcparse_test` now checks every slot is below 16 and used once; the 144 streams pass). To confirm on the
  B580: the vtdec crash report of that night (`__stack_chk_fail` under `anv_h265_decode_video`). With the direct path
  off, a real stream with a 16-picture DPB crashes the decoding process the same way; the fix belongs in FFmpeg
  (compact slot numbers, as the direct path does) or in ANV (refuse a slot >= maxDpbSlots). Confirmed on the B580
  (crash report: `__stack_chk_fail` <- `anv_h265_decode_video` <- `gfx20_CmdDecodeVideoKHR`); both fixed, H.264 too:
  [DPB slots: both sides fixed](#dpb-slots-both-sides-fixed-13-nov-evening).
- Both paths "right" but not equal to libavcodec's software output, three conventions checked against Apple's decoder:
  pictures with pic_output_flag 0 / dropped by no_output_of_prior_pics are answered like any other (BUMPING_A,
  NoOutPrior_A/B, OPFLAG_B/C: every reference picture there, plus 17 / 3 / 3 / 2 / 4; Apple's decoder the same);
  CONFWIN_A's left/top conformance window offsets are not applied to the pixels (the picture is the visible size at the
  top left; Apple's decoder the same); SAODBLK_A/B_MainConcept_4: libavcodec's software output differs from the
  published MD5, while both B580 paths and Apple's decoder give the same 500 pictures. The kit's reference follows
  the first two conventions and is left out where libavcodec does not reproduce the published MD5.

Against these references the 13 Nov results were: direct right on 129 of 142, libavcodec on 126; the 13 the direct path
missed are the 12 broken inputs and MAXBINS_C; libavcodec also loses RPS_B/C/E. `vtdirect-ab.sh` now prints the
reference check per stream ("vs the software reference: libavcodec right, direct right" / "right (+K not output by the
reference)" / "WRONG (...)") and a `SUMMARY reference` line; the kit for the next night is `vtdirect3` (14 Nov).

## AV1 (13 Nov, third part)

`vtdirect_av1.c` (MIT, from the AV1 specification 5.3-5.12 / 7.20 / 7.21 and VK_KHR_video_decode_av1),
`INTELXEDEC_DIRECT=av1` (or `h264,hevc,av1`, `1`, `all`).

What ANV supports on Xe2/BMG [V Mesa `anv_video.c`, `genX_cmd_video.c` anv_av1_decode_video]: VK_KHR_video_decode_av1
(ver >= 12), Main profile only, 8/10-bit, film grain only on Gen20 (`filmGrainSupport`, AVP's film grain injection into
`dstPictureResource` while the setup slot keeps the grain-free reference), maxDpbSlots 9, maxActiveReferencePictures 8,
up to 16384x16384, DPB_AND_OUTPUT_COINCIDE only. No large-scale tile (tile list OBUs: libavcodec refuses them too).
ANV reads the sequence header and the whole StdVideoDecodeAV1PictureInfo (tile layout, quantizer, segmentation, loop
filter, CDEF, LR, global motion, SkipModeFrame, OrderHints, film grain), every reference's StdVideoDecodeAV1ReferenceInfo
(frame type, OrderHint, SavedOrderHints, RefFrameSignBias, disable_frame_end_update_cdf), referenceNameSlotIndices, the
tile offsets and sizes; it keeps per DPB slot state of its own (coded widths, CDF table sets), so the DPB slot numbers
must be libavcodec's for the same decode.

libavcodec 9.0.2's behaviour the direct path copies [V `av1dec.c`, `cbs_av1_syntax_template.c`, `vulkan_av1.c`,
`vulkan_decode.c`]:

- DPB slot of a picture: the lowest one no reference frame slot holds (`frame_id`); the references passed are those
  the seven ref_frame_idx[] name, once each, in that order; the current picture is the setup slot and is bound inactive;
- KEY / INTRA_ONLY frames: libavcodec's frame header has ref_frame_idx[] all 0, so reference slot 0's picture is
  passed (when there is one) and OrderHints[1..7] / RefFrameSignBias come from slot 0's order hint; the hardware does
  not read references for intra frames;
- OrderHints[0] and SavedOrderHints[0] are 0; a reference's SavedOrderHints are its own OrderHints[]; the setup slot's
  reference information has none (zeros); force_integer_mv is set for intra frames;
- loop filter deltas with loop_filter_delta_enabled = 0 are the defaults (CBS), not the values loaded from
  primary_ref_frame (the specification); segmentation features and loop filter deltas otherwise loaded from the
  primary reference frame; CDEF secondary strengths and segmentation feature values as coded (no 3 -> 4, no clip);
  LoopRestorationSize as log2(size) - 5; FrameRestorationType remapped from lr_type;
- global motion decoded against the primary reference frame's parameters (defaults without one);
- film grain: update_grain = 0 loads the referenced frame's parameters (apply_grain and film_grain_params_ref_idx
  included) with the new seed; the Std flags carry no update_grain; with apply_grain the reference picture comes from
  a second frames context with DPB usage only (libavcodec's DPB pool) and the output picture is in VIDEO_DECODE_DST;
  a show_existing_frame of such a frame shows that output picture;
- SkipModeFrame[] derived for every inter frame with reference_select and order hints, when every reference has a
  picture (libavcodec's `skip_mode_params`), whatever skip_mode_present says;
- show_existing_frame of a key frame refreshes every slot with it (7.21); a shown key frame invalidates the slots for
  parsing (CBS) but the references' order hints stay (libavcodec's frames keep them, also across a flush);
- OBUs outside operating point 0's layers are dropped; frame headers repeated by redundant frame header OBUs ignored.

Per sample (a temporal unit): the whole sample is parsed first (every frame header and tile group, the reference slots
updated as each frame completes); only then is anything submitted, so a sample the path cannot take (a sequence header
it does not cover, a frame whose size is not the sequence's maximum, another bit depth, more than 256 tiles) goes to
libavcodec whole, with the parser state put back. Each decoded frame gets its own bitstream buffer (its tiles copied one
after the other, as `ff_vk_decode_add_slice` lays them out) and its own submission; the shown frame (or the picture of a
show_existing_frame) is handed back; a sample that shows nothing (FFmpeg's VideoToolbox hwaccel sends hidden frames
alone) is answered with its last decoded frame, as the libavcodec path answers it. A frame whose references have no
picture (after a flush, or after a frame that could not be decoded) is not decoded, and the slots it refreshes hold no
picture until the next key frame (libavcodec would decode it from whatever it has).

Hand-over mid-stream: AV1 streams change frame size inside a GOP (reference scaling, switch frames), where libavcodec
cannot start. The plug-in keeps the samples since the last shown key frame (up to 32 MB; one memcpy per sample, of the
compressed data) and replays them into libavcodec, their pictures dropped, before the sample it hands over; past 32 MB it
answers samples with an error until the next key frame. The pictures are the sequence's maximum size, as libavcodec's
frames context is (it is made before the first frame's own size is known).

Not covered (libavcodec, or refused on both paths): profiles High / Professional, monochrome, 4:2:2 / 4:4:4, 12-bit,
several spatial layers, large-scale tile, frames smaller than the sequence's maximum, an av1C without a sequence header.

Tests: `tests/av1parse_test.c` prints each sequence header as it goes into the Std structures, and per frame every
StdVideoDecodeAV1PictureInfo field (flags, tile layout, quantizer, segmentation, loop filter, CDEF, LR, global motion,
film grain, OrderHints, SkipModeFrame), the DPB slot, the references with their reference information, the frame
header's length in bits and where its tiles are in the sample; `tests/av1/mkgolden.py` makes the same text without our C
code, from FFmpeg 9.0.2's trace_headers (CBS) with the inferences and derivations written again in Python (frame size
from references, superres, set_frame_refs, tile layout, loaded segmentation / loop filter deltas / film grain, global
motion decoding, skip mode frames, DPB slots), the tile sizes read from the stream's bytes, and the number of shown
frames checked against dav1d. `--fuzz` damages samples at random (bit flips, cuts) under ASan/UBSan.

13 Nov results off the GPU: the 10 recorded SVT-AV1 streams and 243 of the 247 conformance streams (libaom's test
vectors from AOM's bucket, FFmpeg's FATE AV1 samples: frame_refs_short_signaling, non-uniform tiling, switch frames,
decoder model, operating point parameters; SHA-256 pinned in `tests/av1/conformance.list`) identical; the other 4 are
monochrome (2) or have two spatial layers (2). Fuzzing: no crash, no sanitizer report, no invariant broken (one
undefined left shift of a negative value found and fixed). Not yet run on the B580: `phase_vtdirect_av1`.

### DPB slots: both sides fixed (13 Nov, evening)

**What happens.** FFmpeg 9.0.2's H.264 and HEVC Vulkan hwaccels pass a picture's index in libavcodec's own DPB array
as its `slotIndex` (and as its `baseArrayLayer` with a layered DPB). That array (HEVC 32 entries, H.264 36) also holds
pictures that only wait for output, and HEVC's `alloc_frame` takes the new picture's entry before `ff_hevc_frame_rps`
releases the unneeded references and before `ff_hevc_output_frames` bumps. Measured with libavcodec itself
(`tests/dpbindex.sh`: a software-only build with one `fprintf` in `alloc_frame` / `find_unused_picture`, run on this
Mac over every conformance stream):

| Streams | Index reached | ANV's slots | Pictures past them |
|---|---|---|---|
| HEVC RPS_B_qualcomm_5 / RPS_C_ericsson_5 / RPS_E_qualcomm_5 | 16 | 16 (`dpb_idx[16]`) | 4 / 1 / 14 |
| HEVC, the other 144 HEVC_v1 streams | at most 10 (RPS_D) | 16 | none |
| H.264 HCBP2_HHI_A, HCMP1_HHI_A (hierarchical B, 352x288) | 19 | 17 (`dpb_slots[17]`) | 14 each |
| H.264 MR3_TANDBERG_B | 16 | 17 | none |
| H.264, the other 182 JVT streams | at most 15 | 17 | none |

So H.264 has the same bug: HCBP2/HCMP1 are not in the vtdirect kit (the direct path's parser does not take them), so
they never ran on the B580; with the installed libraries they would write `dpb_slots[17..19]` the same way.
`tests/hevc/dpbslots.py` replays `refs.c` (alloc, RPS, bumping) over the HEVC golden files and gives the same numbers
as libavcodec for all 147 streams (4 / 1 / 14 and the maxima above), without building anything.

**FFmpeg** (`videotoolbox/ffmpeg/ffmpeg-9.0.2-vulkan-decode-dpb-slots-below-maxDpbSlots.patch`, ninth in
`apply-patches.sh`): compact slot numbers, as `vulkan_av1.c` / `vulkan_vp9.c` already do with their `frame_id`. Each
picture keeps the slot it was decoded into in its `hwaccel_picture_private` while it is a reference; the new picture
takes the lowest slot none of the current references holds (`ff_vk_decode_free_dpb_slot` in `vulkan_decode.h`); a
reference that was never decoded (generated for a missing reference, an H.264 frame_num gap) gets a free slot when
first used; the second field of an H.264 pair keeps the first one's. A conforming stream needs at most
references + 1 slots (HEVC 16, H.264 17: what ANV offers); with none free the picture fails with `AVERROR_INVALIDDATA`
before the driver sees it. The H.265 `RefPicSet*` lists name the same slots. Side fix: H.264 long-term references were
looked up among the first 16 DPB entries only (slot 0 and entry 0's field flags for one further on). `dpbslots.py`:
with the patch's allocation the slots stay at most 15 on all 147 HEVC streams (every reference keeps its slot, none
shared); H.264 is bounded the same way by max_num_ref_frames <= 16. Built into
`~/hackintosh/work/ffbuild/build-slot/install` (configure-shared.sh, relative paths as the release; source
`ffmpeg-9.0.2-slot` = the eight released patches + this one); the existing `build/install` is unchanged.

**ANV** (`mesa/patches/0106`, second version; the first one is described under the B580 results below): slot indices
are keys, not array positions, for H.264 and H.265: pictures are addressed through each reference's picture resource,
and the index only ties `pReferenceSlots[i]` to the `RefPicSet*` entries (H.265) and to `PictureID` (H.264, a 16-bit
field). So `dpb_idx` (H.265) and `dpb_slots` (H.264) are now tables of `ANV_VIDEO_DECODE_SLOT_KEYS` (256) entries:
any `uint8_t` `RefPicSet*` entry is a valid read, any slot index below 256 a valid write, and a stream with FFmpeg
9.0.2's slots 16-19 decodes as meant. `genX(CmdDecodeVideoKHR)` first makes a sanitized copy of the decode info
(`anv_video_decode_sanitize`): a reference whose index is beyond the codec's table (256 for H.264/H.265, AV1's 9
`prev_refs`) or past the number of references the codec path holds (16 H.264, 15 H.265: the HuC S2L parameters'
`PicOrderCntValList` / `RefIdxMapping` have 15 entries, which a 16-picture DPB never exceeds) is left out and decodes
as a missing reference; a VP9 inter frame whose reference names are not all bound (`anv_vp9_get_ref_idx` returned
0xff, then `pReferenceSlots[255]` was read) is not recorded. H.265 also keeps at most 8 active references
(`ReferencePictureAddress[8]`, `NumPicTotalCurr` <= 8 in a conforming stream; a ninth was written past it). Each case
logs one warning per process (`mesa_logw_once`). Nothing sets an error on the command buffer any more: it always ends
with `VK_SUCCESS`, so the client's submission happens and signals its semaphores and fence. Built in
`~/hackintosh/work/mesa-hevcslot` (`intelxe.diff` + `0102`, the installed 0.1.3/0.1.4 Mesa, file for file, + `0106`)
with `mesa/configure.sh`'s released options; applies on upstream `f01e9b07a91` and after `0103`-`0105`.

**FFmpeg, second patch** (`ffmpeg-9.0.2-vulkan-exec-fence-reset-at-submit.patch`, tenth in `apply-patches.sh`):
`ff_vk_exec_start()` waited for the execution context's fence and reset it at once; the submission that signals it
comes later in `ff_vk_exec_submit()`. When `vkEndCommandBuffer` failed (the first 0106) the context kept an
unsignalled fence nothing would signal, and the next `ff_vk_exec_start()` on it (the pool hands contexts out round
robin), `ff_vk_exec_wait()` or `ff_vk_exec_pool_free()` waited with `UINT64_MAX`: that is the 13 Nov hang. The
timeline semaphores were not the problem (their values only advance after a successful submit). Now the fence is
reset right before `vkQueueSubmit2` and replaced with a signalled one if that fails.

**The B580 kit** `~/hackintosh/work/hevcslot-kit` (`vtdirect3` plus): `libintelxeav1dec.dylib` and `vtdec` rebuilt
against the new FFmpeg (the plug-in's source is unchanged); `libs-ffmpeg/` (`libavcodec.63` / `libavutil.61` with the
slot and fence patches), `libs-anv/` (`libvulkan_intel.dylib` with the second 0106, ad-hoc signed), `libs-both/`,
`libs-fence/` (the first 0106 with an FFmpeg carrying only the fence patch); `conf-h264slot/` (HCBP2_HHI_A,
HCMP1_HHI_A, MR3_TANDBERG_B remuxed, with software references made like `conf-hevc/ref`); `hevcslot-ab.sh`. Nothing
is installed: a set is staged with `DYLD_LIBRARY_PATH` exported inside bash (dyld takes a library of that leaf name
from the folder first, for the plug-in's linked libavcodec/libavutil and for its `dlopen` of
`/Library/GPUBundles/nvmtl/libvulkan_intel.dylib`; the plug-in has no Vulkan loader, so `VK_ICD_FILENAMES` does not
apply). `vtdec` is ours and not hardened, so dyld honours it; never put `perl`/`env`/`sh` in front (they drop
`DYLD_*`); each run lists the copies it loaded (`DYLD_PRINT_LIBRARIES`).

**First B580 run (13 Nov 18:54-19:04, 0 GPU resets), first 0106:**

| Set | RPS_B / C / E | HCBP2, HCMP1 | RPS_A | MR3_TANDBERG_B |
|---|---|---|---|---|
| `installed` | SIGABRT | right (slots 17-19 past `dpb_slots[17]` hit no canary) | right | WRONG: 90 of 300 |
| `ffmpeg` | right | right | right | right |
| `anv` (first 0106: refuse) | hang, exit 137 (60 s box), 0 pictures | hang, exit 137 | right | WRONG: 90 of 300 |
| `both` | right | right | right | right |

The hang is FFmpeg's reset fence above (a refused decode never reaches `vkQueueSubmit2`). MR3_TANDBERG_B is the
other FFmpeg bug the slot patch fixes on the way: `vk_h264_start_frame` looked long-term references up among the
first 16 DPB entries only; MR3_TANDBERG_B (field pictures, long-term references) has one long-term reference use at
DPB entry 16 (`tests/dpbindex.sh`'s build with a print at the hwaccel's start_frame: 112 long-term uses, one at entry
16; the only JVT stream with any), which then went out as slot 0 with entry 0's field flags, colliding with the
picture that really holds slot 0. The pictures predicted from it (90) are wrong with the old FFmpeg whatever the
driver does.

**Expected, second round** (`hevcslot-ab.sh`, libavcodec path only, against the software references):

| Set | RPS_B / C / E, HCBP2, HCMP1 | RPS_A | MR3_TANDBERG_B |
|---|---|---|---|
| `installed` | as above | right | WRONG (90) |
| `ffmpeg` (slot + fence patches, installed ANV) | right | right | right |
| `anv` (installed FFmpeg, second 0106) | right: slots 16-19 used as keys; no abort, no hang | right | WRONG (90), FFmpeg's |
| `both` | right | right | right |
| `fence` (first 0106 + FFmpeg with the fence patch only) | no hang: the refused pictures (4 / 1 / 14 / 14 / 14) lost, WRONG | right | WRONG |

For `vtdirect-ab.sh`: with `VTDIRECT_LIBS=<kit>/libs-ffmpeg` (or `libs-both`) RPS_B/C/E are expected "libavcodec
right" too, which makes the reference summary libavcodec 129 of 142 (the direct path's 129 unchanged).

**Upstream report texts** (not posted).

Mesa (ANV): *anv: vkCmdDecodeVideoKHR writes past stack arrays for a slotIndex >= maxDpbSlots.*
`anv_h265_decode_video` stores `dpb_idx[pReferenceSlots[i].slotIndex] = i` into `uint8_t dpb_idx[16]` and reads it
back through `RefPicSetStCurrBefore/After/LtCurr`, guarded only by `assert`; `anv_h264_decode_video` does the same
with `dpb_slots[17]`; `PicOrderCntValList` / `RefIdxMapping` of the HuC S2L parameters hold 15 references and
`ReferencePictureAddress` 8 active ones, with no check either. With asserts off, a slotIndex of 16 (H.265) or 17+
(H.264) is a stack buffer overflow. That is invalid usage, but FFmpeg's Vulkan hwaccel up to at least 9.0.2 produces
it (it numbers slots by its DPB array index): the JCT-VC conformance streams RPS_B_qualcomm_5, RPS_C_ericsson_5,
RPS_E_qualcomm_5 (sps_max_dec_pic_buffering 16) abort the decoding process (`__stack_chk_fail` under
`anv_h265_decode_video`, Xe2/BMG); the JVT streams HCBP2_HHI_A, HCMP1_HHI_A write slots 17-19 past `dpb_slots`.
Failing the command buffer is not a good answer (FFmpeg 9.0.2 then hangs on its execution fence). Suggested fix: the
slot index is only a key for H.264/H.265, so key tables of 256 entries make such streams decode correctly; drop
references the codec path cannot hold, skip a VP9 frame whose reference names are not bound (`anv_vp9_get_ref_idx`
returns 0xff and `pReferenceSlots[255]` is read), never fail the command buffer (patch available).

FFmpeg: *lavc/vulkan_hevc, vulkan_h264: slotIndex can exceed maxDpbSlots.* `vk_hevc_start_frame` and
`vk_h264_start_frame` use the picture's index in `HEVCLayerContext.DPB` / `H264Context.DPB` as the Vulkan
`slotIndex` and layered-DPB layer. That index counts pictures waiting for output and, for HEVC, the new picture's
entry is taken before the RPS releases unused references, so it reaches 16 on HEVC streams with a 16-picture DPB
(RPS_B_qualcomm_5: 4 pictures, RPS_E_qualcomm_5: 14) and 19 on H.264 HCBP2_HHI_A / HCMP1_HHI_A, beyond
`maxDpbSlots` (16 / 17 on Intel ANV, which then corrupts its stack). Also, long-term H.264 references are only
searched among the first 16 DPB entries. Suggested fix: allocate compact slots per picture as `vulkan_av1.c` does
with `frame_id` (patch available). FFmpeg master was not checked for these lines.

FFmpeg: *lavu/vulkan: a started but never submitted execution leaves its fence reset; the next start waits
forever.* `ff_vk_exec_start()` resets the fence right after waiting on it; if `vkEndCommandBuffer` or anything
before `vkQueueSubmit2` fails, `ff_vk_exec_submit()` returns an error and the fence stays unsignalled, so the next
`ff_vk_exec_start()` on that pool context (or `ff_vk_exec_wait()`, or the pool's free after an earlier submission)
blocks with `UINT64_MAX`. Seen as a decoder hang after a driver failed `vkEndCommandBuffer` for an invalid decode.
Suggested fix: reset the fence in `ff_vk_exec_submit()` right before `vkQueueSubmit2`, and recreate it signalled if
the submission fails (patch available).
