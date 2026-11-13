# VideoToolbox codecs on the B580

VideoToolbox decoder and encoder plug-ins that run on the B580's media engines through Vulkan Video (Mesa ANV from
`/Library/GPUBundles/nvmtl/`), with libavcodec doing the bitstream and reference bookkeeping (H.264 and HEVC decode can
do without it: [`INTELXEDEC_DIRECT`](#direct-vulkan-video-decode-intelxedec_direct), off by default). FFmpeg 9.0.2 with the
patches in `ffmpeg/` (Vulkan decode and encode fixes) is used as shared libraries next to the plug-ins.

- **Decoders** (`intelxeav1.c`, `libintelxeav1dec.dylib`; the file name is historical): AV1 (`av01`) and VP9, and H.264
  (High and below, 8-bit) and HEVC (Main, Main10) as the hardware decoder of last resort when the system has none for
  them; NV12/P010 IOSurface pixel buffers, full or video range as the stream is ('420f'/'xf20' or '420v'/'x420').
- **Encoders** (`encoder/intelxevenc.c`, `libintelxevenc.dylib`): HEVC (Main, Main10), H.264 (High) and AV1 (Main) on VDEnc,
  with libavcodec's `hevc_vulkan`, `h264_vulkan` and `av1_vulkan` (H.264 and AV1 encode on Xe2 need `mesa/patches/0010`,
  `0012`, `0015`).

**How they are registered (2026-10-29, the same on macOS 26):** macOS 15 and later drop third-party codec bundles in `/Library/Video/Plug-Ins` unless
they are Apple-signed, so the Metal plugin registers both libraries in-process (`VTRegisterVideoDecoderWithInfo` /
`VTRegisterVideoEncoderWithInfo`, plugin fix28) in every process that loads it, VTDecoderXPCService included. Installed
files: `/Library/GPUBundles/nvmtl/libintelxeav1dec.dylib`, `libintelxevenc.dylib`, `libavcodec.63.dylib`, `libavutil.61.dylib`.

## Files

| File | What |
|---|---|
| `intelxeav1.c`, `vtplugin.h` | the decoders (AV1, VP9, H.264, HEVC); the VideoToolbox decoder plug-in ABI (not in the public SDK: the CoreMedia derived-object layouts on macOS x86_64) |
| `vtdirect.c`, `vtdirect.h`, `vtdirect_h264.c`, `vtdirect_h264.h`, `vtdirect_hevc.c`, `vtdirect_hevc.h` | the direct Vulkan Video decode path (H.264 and HEVC, `INTELXEDEC_DIRECT`, [below](#direct-vulkan-video-decode-intelxedec_direct)): the session, the submission, the H.264 parameter sets, slice headers, POCs and reference marking, the HEVC parameter sets, slice segment headers, POCs and RPS-driven DPB |
| `build.sh` | builds `libintelxeav1dec.dylib` against the shared FFmpeg and copies `libavcodec.63`/`libavutil.61` next to it, and `vtdec`, all ad-hoc signed |
| `encoder/intelxevenc.c`, `encoder/vtencplugin.h` | the encoders; the encoder plug-in ABI (checked against Apple's x86_64 encoders) |
| `encoder/build.sh`, `encoder/vtenctest.m` | builds `libintelxevenc.dylib`, `vtenctest` and `vtencq`; `vtenctest` drives a `VTCompressionSession` the way OBS/HandBrake/ffmpeg do (a new pixel buffer per frame, so no import cache hits) |
| `ffmpeg/apply-patches.sh`, `ffmpeg/*.patch` | our FFmpeg 9.0.2 patches: the eight the released libraries (0.1.4) carry, then the later ones (`vulkan-decode-dpb-slots-below-maxDpbSlots`, 13 Nov), in order (two files there are not applied: see the script) |
| `ffmpeg/configure-shared.sh` | the FFmpeg 9.0.2 configuration: shared, LGPL only, AV1/H.264/HEVC/VP9 decoders + Vulkan hwaccels, Vulkan encoders |
| `tests/vtdec.m`, `tests/vtq.m`, `tests/sbprobe.c` | decode through `VTDecompressionSession` (`--lib`: a test build in-process and its statistics line, `--md5`, `--async`, wall and CPU per picture); what VideoToolbox offers; what a sandboxed video service can reach |
| `tests/h264parse_test.c`, `tests/h264/` | the direct path's H.264 parser against libavcodec, without a GPU: recorded streams and their golden files (`mkstreams.sh`, `mkgolden.py`), `run.sh [--conformance DIR]` (also the 185 JVT conformance streams of FFmpeg's FATE suite) |
| `tests/hevcparse_test.c`, `tests/hevc/` | the same for HEVC: recorded x265 / Apple streams (8 and 10 bits) and their golden files (`mkstreams.sh`, `mkgolden.py`: FFmpeg's trace_headers and libavcodec's decoded POCs), `run.sh [--conformance DIR]` (also the 147 JCT-VC HEVC_v1 conformance streams, official zips pinned by SHA-256 in `conformance.list`) |
| `tests/hevc/dpbslots.py`, `tests/dpbindex.sh`, `tests/hevcslot-ab.sh` | the DPB slot numbers of libavcodec's Vulkan hwaccels: a replay of `refs.c` over the HEVC golden files (slot numbers before and after the DPB slot patch, no GPU); libavcodec's own DPB index per picture (an instrumented software-only build, H.264 and HEVC); the B580 A/B of the FFmpeg and ANV fixes over the streams that reach the slot count (`libs-ffmpeg/`, `libs-anv/`, `libs-both/` staged with `DYLD_LIBRARY_PATH`) |
| `tests/vtdirect-ab.sh`, `tests/vtdirect-kit.sh`, `tests/night-vtdirect.sh` | the direct path against the libavcodec path on the B580 machine (per-picture MD5s, time and CPU per picture; `VTDIRECT_CODECS=h264` / `hevc`); the folder it runs from; the night phases (`phase_vtdirect`, `phase_vtdirect_hevc`) |
| `tests/vtencq.m` | encode quality: deterministic test sources made in memory, encoded, decoded straight from the encoder's sample buffers, PSNR Y/U/V and bit rate per pattern ([below](#encode-quality-vtencq)) |
| `tests/venc-ab.sh` | the encoder upload A/B on the B580 machine (`vtencq --upload` for HEVC, H.264, AV1, HEVC Main10 at 1080p and 2160p) with a test build, nothing installed |
| `tests/vcbench.m` | decode throughput the way Video Codec Test measures it (parallel sessions, its pixel formats), and hardware vs software checksums; `VCB_DUMP=<dir>` writes each picture's visible planes for `cmp -l` |

## Building

```sh
videotoolbox/ffmpeg/apply-patches.sh <ffmpeg-9.0.2>          # the released set and the later patches, in order
mkdir ffbuild && cd ffbuild && cp -R <mesa>/include vk-include  # relative paths: avcodec_configuration() names none
FFMPEG_SRC=<ffmpeg-9.0.2> MESA_INCLUDE=vk-include PREFIX=install ../videotoolbox/ffmpeg/configure-shared.sh && make -j8 && make install
FFMPEG_INSTALL=$PWD/install MESA_INCLUDE=<mesa>/include ../videotoolbox/build.sh
FFMPEG_INSTALL=$PWD/install MESA_INCLUDE=<mesa>/include ../videotoolbox/encoder/build.sh
```

## Encode quality (vtencq)

`tests/vtencq.m` checks an encoder objectively on any Mac, with frameworks only (no FFmpeg, no source files): four
sources made with integer arithmetic (the same pictures on every machine and architecture; the `source` column is
their checksum) - `gradient` (smooth moving ramps), `noise` (band-limited texture moving 3,1 px a frame plus temporal
grain), `text` (glyph-like dots typed and scrolled, screen content), `scroll` (fractal terrain, 0.75,2.5 px a frame with
sub-pixel motion) - are encoded, the encoder's CMSampleBuffers are decoded by VideoToolbox with the format description
the encoder made, and every picture is compared with its source. PSNR is computed as FFmpeg's psnr filter averages it
(from the mean squared error; `avg` weights Y:U:V 4:1:1; checked against `ffmpeg -lavfi psnr` on a `--dump` stream:
same to 0.01 dB). Decoders: `sw` (the default for H.264/HEVC, Apple's, independent of the encoder) or `hw`/`any`. macOS
has no AV1 software decoder in VideoToolbox (an `av01` session fails with -12906 on macOS 27.0.1 arm64), so AV1 goes
through the IntelXe AV1 decoder (`--metal` or `--declib`, `--decoder any`).

```sh
clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -fobjc-arc videotoolbox/tests/vtencq.m -framework Foundation \
  -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework Metal -o vtencq && codesign -f -s - vtencq
./vtencq --metal --codec hevc                                   # IntelXe encoders and decoders through the installed plug-in
./vtencq --lib libintelxevenc.dylib --codec hevc --decoder sw   # a test encoder build, registered in-process
./vtencq --metal --codec av1 --bitrate 6000000                  # AV1: INTELXEVENC_CODECS gets av1 before registration
./vtencq --encoder system --codec h264                          # whatever VideoToolbox picks (Apple's encoders on a Mac)
```

Options: `--codec h264|hevc|av1`, `--bitrate N` (bit/s; 8 Mbit/s, AV1 6), `--cbr`, `--10bit`, `--frames N` (120 per
pattern), `--size WxH`, `--fps F` (30), `--decoder hw|sw|any`, `--patterns`, `--keyint N`, `--noreorder`, `--realtime`,
`--dump DIR` (elementary streams; `--dump-source` adds the raw sources), `--list`, `--upload copy,import` (see
[below](#encoder-upload-copy-or-import)), `--ring K` (K pool buffers filled before the clock starts and handed over in
turn: the encoder's throughput without the client's per-frame fill), `--nodecode` (no PSNR decode, speed only). A mode written `MODE:N` also sets `INTELXEVENC_ASYNC=N` (`--upload copy,import,import:4`).

**Reference: Apple's hardware encoders** (M1 Pro, macOS 27.0.1, `--encoder system`, 1920x1080, 120 frames per
pattern at 30 fps, frame reordering on, Apple's software decoder; the x86_64 build under Rosetta gives the same
numbers and checksums). PSNR-Y / bit rate against the target:

| encoder, mode | gradient | noise | text | scroll | all: PSNR-Y, avg | all: rate |
|---|---|---|---|---|---|---|
| H.264 VBR 8 Mbit/s | 53.48 dB, -79.5% | 35.94, +67.7% | 45.42, -52.9% | 48.48, -51.5% | 41.22, 42.28 | -29.0% |
| H.264 CBR 8 Mbit/s | 53.10, -12.5% | 37.12, +44.0% | 45.67, -8.2% | 48.01, -12.3% | 42.18, 43.08 | +2.8% |
| HEVC VBR 8 Mbit/s | 53.67, -81.1% | 35.55, +26.4% | 49.64, -48.4% | 50.00, -65.4% | 41.19, 42.58 | -42.1% |
| HEVC CBR 8 Mbit/s | 53.17, -12.5% | 37.30, +30.7% | 45.59, -6.9% | 46.90, -12.5% | 42.24, 43.46 | -0.3% |
| HEVC VBR 6 Mbit/s | 53.45, -76.4% | 34.82, +41.9% | 49.09, -34.8% | 49.70, -57.6% | 40.49, 41.89 | -31.7% |
| HEVC Main10 VBR 8 Mbit/s | 61.69, -82.7% | 35.35, +17.7% | 49.64, -48.5% | 51.33, -73.0% | 41.10, 42.58 | -46.6% |

Apple's VBR stays far under the target where the content is easy (gradient, scroll, text: 48-54 dB already) and goes
over it on the noise pattern (temporal grain everywhere): on synthetic sources an under-shoot with a high PSNR is the
expected behaviour of a VBR encoder, not a rate control fault. Encode speed here was 165-185 fps (Apple's encoders,
frames copied from memory).

## Encoder upload: copy or import

How a pixel buffer reaches the encoder's input image (`INTELXEVENC_UPLOAD`, read when a session's codec opens):

- `copy` (default): the CPU copies the planes into libavutil's staging buffer, libavutil's transfer queue copies that
  into the image (`av_hwframe_transfer_data`). ~0.3 ms of CPU per 1080p frame, ~1.2 ms at 4K, on the encoding thread.
- `import`: the copy engine (the transfer-only queue family, the B580's BCS) reads the IOSurface itself
  (`upload_import` in `encoder/intelxevenc.c`). Each IOSurface's pages are imported once (`VK_EXT_external_memory_host`,
  keyed by IOSurfaceID and the retained surface; pools recycle a few surfaces, so after the first frames every upload
  is a cache hit; surfaces the client's pool has let go are dropped, at most 24 are kept). The copy is synchronised with
  libavutil the way its own uploads are: under `lock_frame`, wait on each image's timeline semaphore at `sem_value`,
  signal `sem_value + 1`, layout `TRANSFER_DST_OPTIMAL`, then the encode submission waits on it. The pixel buffer stays
  locked read-only and retained until its copy has completed. Anything the copy engine cannot take (a plane offset not
  a multiple of 4, a submit or import failure) goes the `copy` way for that frame; 8 failures in a row, or a client that
  hands over a new IOSurface for most frames (more than 24 surfaces in the first 48: an import per frame cost more than
  the CPU copy, 245 vs 352 fps on 11 Nov), turn it off for the session. The device always has the copy queue and the
  host-memory extension enabled; libavutil is not told about the extension, so `copy` does exactly what it did before.
- `INTELXEVENC_ASYNC=N`: libavcodec's `async_depth` (default 2: with two encodes in flight, the next
  `avcodec_receive_packet` blocks in `vkWaitForFences` on the older one before the next frame is uploaded). Only sizes the
  encode FIFO and the timestamp ring; the stream should stay the same.

`vtencq --lib libintelxevenc.dylib --upload copy,import` encodes every pattern once per mode in one process and prints,
under each row, the stream checksum (sample timing, payload, format description atoms) and the encoder's own line
(`IntelXeVEnc_LastStats`: fps, CPU ms per frame in the upload and in the encode call, copy engine uploads, fallbacks,
imports, evictions, ring waits); a stream equal to the first mode's reuses its decode, the last line says
`bit-identical streams in every mode` or `streams DIFFER` and the speed ratio. List a mode twice
(`copy,import,copy,import`) to see the order effect (the first session of a process pays the device's start-up).
Not measured on the B580 yet (12 Nov): the default stays `copy` until the A/B decides.

### Driving Vulkan Video directly: assessment (12 Nov, from code reading)

Per frame, libavcodec 9.0.2's Vulkan encoders (`hw_base_encode.c`, `vulkan_encode.c`) do: a picture allocation and the
input frame's reference; a reconstructed (DPB) frame from its pool; two `vkCreateImageView` (input and DPB view) and
two `vkDestroyImageView` when the picture is freed; GOP and reference selection; a pooled bitstream buffer (3 x w x h,
host-visible); the codec's Std picture parameters; an exec context (its fence waited if busy); dependency tracking (an
`av_buffer_ref` and `lock_frame` per input, reconstructed and reference frame, their semaphores); the barrier, video
coding begin/control/encode/end with the feedback query; the submit. Then `avcodec_receive_packet`, with `async_depth`
2, waits on the older encode's fence on the calling thread, reads the query, invalidates the bitstream memory and hands
the packet over without a copy (AV1 hidden frames: one memcpy per temporal unit). Our side then rewrites the packet
(Annex B to length prefixes: one copy) into a CMSampleBuffer.

What a direct Vulkan Video encoder could skip: the per-frame views (cacheable per pooled image), the dependency
bookkeeping (a few dozen small allocations and lock/unlock pairs), and the host fence wait on VideoToolbox's thread
(a completion thread could emit while the next frame is submitted). What it would have to redo: everything libavcodec
is used for - the GOP and the dyadic pyramid with our patches, references and their lists, rate-control setup,
VPS/SPS/PPS and AV1 sequence/frame headers, AV1 hidden frames - several thousand lines with their own bugs. Not
bounded, so not started. The bounded parts are measured first:

1. `INTELXEVENC_ASYNC=4` / `8` against 2 with `vtencq --ring 8 --nodecode` (the fence wait); bit-identical expected.
2. The stats line splits the thread's time: `upload_ms + encode_ms` close to `1000 / fps` means the encoding thread is
   busy (or blocked) all the time. A CPU profile tells which: `sample vtencq 10 -file venc.sample.txt` on the B580
   machine during a long `--ring 8 --nodecode --frames 3000` run, symbolicated here with the dSYM. Time in
   `vkWaitForFences` / `ioctl` is GPU-bound (VDBOX): direct Vulkan would not help. Time in `vkCreateImageView`,
   `ff_vk_exec_*`, `av_buffer_*` is libavcodec's: then a view cache (an FFmpeg patch in `vulkan_encode_init`) and a
   completion thread are the next steps, before any rewrite.

## Direct Vulkan Video decode (INTELXEDEC_DIRECT)

H.264 and HEVC can be decoded without libavcodec: `vtdirect_h264.c` / `vtdirect_hevc.c` parse the parameter sets and
the slice headers as far as Vulkan Video needs them, compute the picture order counts and keep the decoded picture
buffer (H.264: reference marking; HEVC: the reference picture sets), and `vtdirect.c` records the decode itself
(`vkCmdBeginVideoCodingKHR` / `vkCmdDecodeVideoKHR` / end, parameter sets inline through `VK_KHR_video_maintenance2`).
The pictures are frames of a libavutil Vulkan frames context made like libavcodec's (NV12, or P010 for 10-bit HEVC), so
the copy engine's copy into the IOSurface, its deferral and the emit queue are the same code on both paths. Off by
default until the B580 A/B decides (H.264: every picture identical on both paths for the 92 conformance streams of the
13 Nov night, timing still to come; HEVC: written and checked off the GPU only, 13 Nov).

- `INTELXEDEC_DIRECT=h264`, `hevc` (or `h265`), `h264,hevc`, or `1` / `all` (both) at session start; unset or `0`:
  libavcodec. A release Metal plug-in keeps inherited `INTELXE*` variables only with `NVMTL_PROCESS_ENV=1`;
  `/Library/GPUBundles/nvmtl/nvrm610.conf` sets it for every process (VTDecoderXPCService included).
- Covered, H.264: progressive (frame_mbs_only) 8-bit 4:2:0 Baseline (without slice groups), Main and High, streams that
  start with an IDR picture and do not allow frame_num gaps. HEVC: Main, Main 10 and Main Still Picture, 4:2:0, 8 or
  10 bits, without the range / SCC extensions, streams that start with an IRAP picture and whose hvcC carries an SPS
  (tiles, WPP, dependent slice segments, long-term references, scaling lists, PCM, CRA/BLA/RASL/RADL, EOS: covered).
  Everything else goes to libavcodec: at StartSession from the avcC / hvcC, or mid-stream when a sample brings what the
  path does not cover (an SPS with fields, another size or bit depth, ...), from that sample on (the session's
  statistics say `path=direct>libavcodec` and why). AV1 and VP9 stay on libavcodec.
- The hardware gets what libavcodec's Vulkan hwaccel gives it: the same images (format, tiling, usage, profile list),
  the same picture and reference information (POCs numbered as libavcodec numbers them; H.264: references short-term
  newest first then long-term; HEVC: every picture the RPS keeps, with RefPicSetStCurrBefore/After/LtCurr naming their
  slots, and NumBitsForSTRefPicSetInSlice), the slices with start codes. ANV programs the B580's MFX in short format, and
  for HEVC its HuC slice-to-long kernel parses the slice headers on the GPU: the hardware builds the reference lists
  itself, so neither path passes lists. The HEVC choices that follow libavcodec rather than the specification (POC of a
  CRA starting a sequence, the DPB at CRA/BLA, missing references, the Std fields the driver programs) are listed in
  `docs/design/vt-direct-plan.md` ("HEVC").
- What the direct path does not do per picture: the packet copy (the sample is read where AVAssetReader put it), the
  emulation-prevention pass over every slice, libavcodec's slice header and reference list work, two image views
  created and destroyed (here one per pooled image, made once), the execution-context bookkeeping (buffer references,
  dependency arrays).
- A P/B picture whose references are not there (after a seek to a non-IDR picture) is not decoded: it is answered with
  an error (H.264: it keeps its place in the DPB; HEVC: it does not, as in libavcodec). libavcodec decodes such H.264
  pictures from whatever it substitutes. HEVC RASL pictures of a CRA that starts a sequence are dropped, as libavcodec
  drops them (`skipped=` in the statistics).

Checks: off the GPU, `tests/h264/run.sh --conformance DIR` compares every picture's frame_num, POCs and reference
pictures, and every SPS/PPS field (scaling lists included), with libavcodec 9.0.2's (`-debug pict+mmco`,
`trace_headers`): the 6 recorded streams here (x264: pyramid B frames with MMCO, Baseline with 3 slices and POC type 2,
custom 8x8 scaling lists with temporal direct and implicit weights, frame_num / POC LSB wrap-around, CAVLC with an IDR
every 4 pictures; Apple's encoder) and 89 of the 185 JVT conformance streams (MMCO 1-6 including 5, long-term
references, POC types 0/1/2, scaling lists, multiple slices, redundant slices) are identical; the other 96 are fields,
MBAFF, 4:2:2/4:4:4/10-bit or allow frame_num gaps (libavcodec). HEVC, `tests/hevc/run.sh --conformance DIR`: every
picture's type, POC, slice segments, RPS (from the SPS or the header, NumBitsForSTRefPicSetInSlice,
NumDeltaPocsOfRefRpsIdx), slice header length, current lists and the references held, and every VPS/SPS/PPS field
(scaling lists after prediction, the SPS's RPS sets, tile sizes), against `tests/hevc/mkgolden.py` (the same rules
written again in Python over FFmpeg's trace_headers, its POCs checked against libavcodec's own decoder):
the 6 recorded streams here (x265: open GOP with CRA/RASL, closed GOP with RADL and 3 slices, Main 10, 300 pictures
with POC wrap-around and TSA sub-layer pictures; Apple's encoder, 8 and 10 bits) and 144 of the 147 JCT-VC HEVC_v1
conformance streams (tiles, WPP, dependent slices, long-term references, inter RPS prediction, scaling lists, PCM,
weighted prediction, BLA/CRA/RASL/RADL, EOS, NoOutPrior, parameter-set changes) are identical; the other 3 change size
mid-stream (identical up to the change, then libavcodec) or have luma and chroma bit depths that differ (not decoded by
libavcodec either). On the B580, `tests/vtdirect-ab.sh` (with the folder `tests/vtdirect-kit.sh` makes;
`VTDIRECT_CODECS=h264` / `hevc`) decodes each conformance stream and the mediakit H.264 / HEVC 8 and 10-bit clips on
both paths and compares every picture's MD5, then times both paths three times alternated (`vtdec --async`: wall ms per
picture, process CPU %, and the decoder's own line: fps, CPU ms of the thread calling DecodeFrame, latency until
emitted); `tests/night-vtdirect.sh` holds the night phases.

### Decode: what libavcodec adds (13 Nov, from code reading)

The 12 Nov assessment above is about the encoders. For the decoders, per picture, libavcodec 9.0.2 does: the packet
(our copy of the sample); `ff_h2645_packet_split` (a pass over every NAL unit for emulation prevention bytes, copied
when it finds one); the full slice header with reference list construction, modification and weights (the B580 parses
these again itself); `ff_vk_decode_prepare_frame` (a views buffer and two `vkCreateImageView`, destroyed with the
picture); `ff_vk_decode_add_slice` (the slice copied into a pooled host-visible buffer); `ff_vk_decode_frame` (an exec
context, possibly a fence wait, an `av_buffer_ref` per frame, view set and buffer, the barriers, the submit). The direct
path keeps the copy into the bitstream buffer, the header parsing (only what Vulkan needs) and the submit. By codec:

| Codec | What the application parses for Vulkan Video | Direct path |
|---|---|---|
| H.264 | SPS/PPS, slice header to dec_ref_pic_marking, POC, DPB marking (the hardware builds the lists) | **done** (behind the knob) |
| HEVC | VPS/SPS/PPS, slice segment header to the RPS (short/long-term sets), POC, the RPS-driven DPB | **done** (behind the knob; ANV's HuC slice-to-long kernel turns the slice headers into HCP commands on the GPU) |
| VP9 | the uncompressed header, segmentation and loop filter state, reference slots, superframes; frame size changes | after HEVC |
| AV1 | sequence and full frame header (tiles, quantizer, segmentation, loop filter, CDEF, LR, global motion, film grain), reference state | largest; libavcodec stays (its av1dec is already hwaccel-only) |

## Licensing: FFmpeg

- FFmpeg is LGPL-2.1-or-later. It is modified (`ffmpeg/apply-patches.sh` lists the patches) and linked dynamically:
  `libavcodec.63.dylib` and `libavutil.61.dylib` are separate files that can be replaced by the user's own build of the
  same version and configuration. Anyone who receives those binaries must also be offered the corresponding FFmpeg
  9.0.2 source, the patches and the configure line (`ffmpeg/configure-shared.sh`).
- The plug-ins' own code (`intelxeav1.c`, `vtdirect*.c/h`, `vtplugin.h`, `encoder/`, scripts, tests) is MIT. The direct
  path's H.264 and HEVC parsers are our own (written from ITU-T H.264 and H.265; FFmpeg and Mesa read for their
  behaviour, no code taken); no third-party code was added. The recorded test streams in `tests/h264/` and
  `tests/hevc/` are made by their `mkstreams.sh` (x264, x265 and Apple's encoder from FFmpeg's test pattern); the JVT
  and JCT-VC conformance streams are downloaded by the tests (the JCT-VC zips from the ITU-T site, checked against the
  SHA-256 in `tests/hevc/conformance.list`), not kept here.
- Earlier prototypes linked a patched FFmpeg statically (in the development history, not in this repository); that is no longer the case.
