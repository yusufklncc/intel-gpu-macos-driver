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
- Not covered (libavcodec decodes): fields and MBAFF, other profiles / bit depths / chroma formats, FMO, data
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
