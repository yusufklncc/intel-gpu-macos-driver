The smooth-desktop release: WindowServer no longer reads every composited frame back into system memory (**scan-out
phase 4**: the frame is paged off only when something reads it), and a window started after the GPU was idle no longer
stutters while the GPU clock ramps up (**display boost**). Also: two fixes contributed by Theo R. (a 165 Hz panel
starts at 165 Hz, and the readback measurement that started phase 4), the installers handle the system-volume copy's
`NVMTLDriver.use-library` marker themselves, and Metal **mesh pipelines** work again, with and without an object stage.

> [!WARNING]
> Experimental. Tested on one machine with one GPU (Arc B580); the Arc B570, Pro B60 and Pro B50 are supported but
> untested. Not signed by Apple; needs relaxed SIP/AMFI and OpenCore. Keep a USB stick with a working EFI before
> installing.

> [!NOTE]
> Developed with AI assistance: the code was written by Anthropic's Claude Opus 5.5 (High effort level) under the
> direction of the maintainer, who tests every build on the real hardware.

### Highlights

- **Scan-out phase 4: WindowServer's composite is no longer read back every frame.** Until 0.1.5 the Metal driver
  copied every composited desktop frame from the card back into the IOSurface's system-memory pages, so that a CPU
  reader (a screenshot, a screen recording, a frame the kernel copies instead of flipping) would find it there. That
  copy goes over PCIe and costs WindowServer GPU time on every frame. Phase 4 writes the frame to the scan-out buffer
  only and marks the binding newer; a CPU lock of the composite, a copied frame or an unbind pages the newest frame off
  on demand (on the copy engine). It used to be gated on `debug.intelxe.surfvram >= 2`, which no default install
  sets, so it never engaged; it now engages whenever a composite is bound. On the B580 (HiDPI 4096x2304 at 180 Hz,
  `p4check smooth`):

  | | fps | late frames | WindowServer GPU time per command buffer |
  |---|---|---|---|
  | full readback (0.1.5) | 97.6 | 481 of 581 | 2.54 ms |
  | phase 4 (0.1.6) | 175.9 | 13 of 1041 | 1.37 ms |

  Screenshots and screen recordings show the current frame (12 of 12 held colours; 8 of 8 with flips off, where the
  kernel copies frames instead). Each binding
  that cannot take phase 4 (a pitch, plane offset or allocation that does not match the scan-out layout) is read back
  as before. `NVMTL_SCANOUT_READBACK=1` brings the full readback back, `=0` turns both off (measuring only).
- **Display boost: no stutter when a window starts after idle.** With the GPU idle, GuC SLPC keeps the clock low and
  raises it from its own busyness samples, which took about 370 ms to follow a desktop that had just started animating:
  the first frames of a window ran at 90 Hz on a 180 Hz display. Now the first frame NMXAccel presents after 250 ms of
  a quiet display asks `IntelXe.kext` for a 400 ms lease on SLPC's minimum clock (the highest the administrator
  allows), so the clock is up when the ramp would start. B580, 12 starts from idle each: late frames 303 -> 13 (the
  worst start 44 -> 2), 166.8 -> 179.3 fps. An idle desktop and 4K60 video take no lease and draw the same power.
  Knobs: `debug.intelxe.disp_boost` (0 off), `debug.intelxe.disp_boost_idle_ms` (the lease, 400),
  `debug.nmxaccel_idle_boost_ms` (the quiet time, 250), the boot-arg `-intelxe_nodispboost`; counters in
  `debug.intelxe.disp_boost_stats` ([design](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.6/docs/design/DESIGN-display-boost.md)).
- **Contributed by Theo R. (`@td512`)**, each found and measured on a 3440x1440 165 Hz panel:
  - `IntelXeFB.kext` reports the firmware's mode as the current mode at start. It reported mode 1, which is the highest
    EDID rate when that sorts first (165 Hz before the firmware's 59.99 Hz), so WindowServer's switch to mode 1 did
    nothing and the panel stayed at 60 Hz until you switched to another mode and back (PR #5).
  - The scan-out readback measured and switched off (PR #4): at 3440x1440 it cost WindowServer 12.8 ms of GPU per frame
    (0.63 ms without; 56 -> 162 fps at 165 Hz). Turning it off left CPU readers with old frames; phase 4 (above) is
    the version that keeps them current.
- **The system-volume copy's `NVMTLDriver.use-library` marker is handled by the installers.** `install.sh --apply`
  creates it while the system-volume copy (Safari) is from another release, and from 0.1.6 records that it did;
  `system-volume/ssv-install.sh --apply` removes it once it has installed this package's copy. The manual
  `sudo rm` after an upgrade is gone (a marker you made yourself stays, and the scripts say so). `./install.sh
  --status` shows `use_library_by` (install, user, none) and `system_volume_state` (`pending` between
  `ssv-install.sh --apply` and the restart, then `same`); IntelXe Manager's Driver page shows the same states and warns
  when the marker hides a system-volume copy of this release.
- **Mesh shaders work again.** The translator's mesh lowering placed every mesh threadgroup's output at the start of
  its scratch (macOS 26's Metal compiler writes the handle with an attribute the lowering did not expect), over the
  header of the draw it then made: object + mesh pipelines lost the GPU, and mesh pipelines without an object stage
  drew wrongly. Fixed in `mesh_lower.rs` (`rebase_handle`); object + mesh pipelines are built again (0.1.5 refused
  them). `mtl_mesh_object` 4 of 4 on the B580 (both descriptor models). `NVMTL_MESH_OBJECT=0` refuses object stages as
  0.1.5 did.

### Checked on the B580 since 0.1.5 (no change needed)

- **SYCL and oneAPI over this driver's Level Zero** (built from source, not in the packages; see 0.1.5's *For
  developers*): `sycl-ls`, ahead-of-time and JIT kernels, oneDPL, `kernel_compiler` (SPIR-V, OpenCL and SYCL sources)
  and oneDNN (ReLU, matmul, convolution: 4 of 4) passed, with no GPU reset.
- **The `bar_revoke` fix** (0.1.5): its forced mode moved 128 of the test's own buffers (512 MB) out of the BAR window
  with every check passing, WindowServer unaffected, no GPU reset. The feature stays opt-in and off.

### Evaluated and not changed

- **Zink with descriptor sets by default** instead of descriptor buffers: it was 16% faster in GPU time per frame
  before 0.1.5's render-cache default (`mesa/patches/0108`); measured again on top of it, no difference (0.0% and
  +0.2%), so Zink keeps descriptor buffers.

### Packages

| File | For | Unpack on |
|---|---|---|
| `IntelXe-Manager-0.1.6.zip` | the IntelXe Manager app (macOS 15 or later) | macOS |
| `IntelXeManager-0.1.6-win-x64.zip` | IntelXe Manager for Windows: stages 2 and 4 from Windows, a check of the PC, the setup steps, reports (unchanged apart from its version) | Windows 10 or 11 (x64) |
| `intel-gpu-macos-driver-0.1.6-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.6-sequoia.zip` | macOS 15 Sequoia (same sources; not re-tested on Sequoia) | macOS |
| `intel-gpu-macos-driver-0.1.6-efi.zip` | the OpenCore pieces: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (optional), `IntelXe-EFI.ps1` and `efi-rules.json` | any system |
| `intel-gpu-macos-driver-0.1.6-zink-gl.zip` | optional: OpenGL through Mesa's Zink (0.1.5's binaries) | macOS |
| `intel-gpu-macos-driver-0.1.6-vulkan.zip` | optional, experimental: Vulkan for Wine (0.1.5's binaries) | macOS |
| `xe-probe-stick-0.1.0.img.zip` | optional, experimental: the Xe probe stick's image, for Intel GPUs the driver does not support yet (the same image as 0.1.5's) | write it to a USB stick |

Rebuilt for 0.1.6: `IntelXe.kext`, `IntelXeFB.kext`, `NMXAccel.kext`, the Metal driver and its shader translator (both
copies), IntelXe Manager. `HybridPM.kext` carries 0.1.5's code with the new version; `NMXAGDC.kext`, Mesa, the video
plug-ins and FFmpeg are 0.1.5's (`SOURCES.md` in each package lists every binary).

Checksums: `intel-gpu-macos-driver-0.1.6.sha256`, signed with the project's key in
`intel-gpu-macos-driver-0.1.6.sha256.sig`; it lists the Windows app's zip and the probe stick's image too. Install
guide: `README-INSTALL.md` in each macOS package and
[in the repository](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.6/release/README-INSTALL.md)
(Turkish: `README-INSTALL.tr.md`).

### Upgrading from 0.1.5

1. Install the package (the app: *Download v0.1.6*, then *Install this package*; or `sudo ./install.sh --apply`).
2. **Replace `IntelXe.kext` in the EFI** before you restart: the app's EFI page or `sudo ./efi.sh --apply` on macOS;
   on Windows IntelXe Manager for Windows (EFI page, *IntelXe.kext and SSDT-ARCOFF (stage 4)*) or `IntelXe-EFI.ps1`;
   or by hand. Its `config.plist` entry stays as it is. The same step replaces `HybridPM.kext` where it is used. The
   display boost is in `IntelXe.kext`: with 0.1.5's, NMXAccel's requests are only counted as failed
   (`debug.nmxaccel_vbl_boost_fail`).
3. If you installed the system-volume copy for Safari, **install this package's too** (the app's Driver page, *Safari
   and WebKit*; or `sudo ./system-volume/ssv-install.sh --apply`). Until then `install.sh` makes
   `NVMTLDriver.use-library`, so every process uses the new `/Library` copy (0.1.6 changes the Metal driver and not
   Mesa; `install.sh` now compares both), and `ssv-install.sh --apply` removes the file again once it has installed
   this package's copy. If 0.1.5's `install.sh` left `NVMTLDriver.use-library` behind, the 0.1.6
   installers remove it; one you made yourself stays.
4. Restart.

To go back to 0.1.5's behaviour for one of the changes only: `NVMTL_SCANOUT_READBACK=1` in
`/Library/GPUBundles/nvmtl/nvrm610.conf` (the full readback; WindowServer takes it at the next restart);
`-intelxe_nodispboost` in the boot-args, or `sudo sysctl debug.intelxe.disp_boost=0` until the next restart (the
display boost); `NVMTL_MESH_OBJECT=0` in the same file (object + mesh pipelines refused).

From 0.1.4 or earlier, follow the guide's upgrade section first.

### Fixed

- The Metal driver: object + mesh pipelines that lost the GPU, and mesh pipelines that drew wrongly (above).
- `IntelXeFB.kext`: a panel whose highest EDID rate sorts before the firmware's mode stayed at 60 Hz after start (above).
- Scan-out phase 4 never engaged without `intelxe_surfvram=2`, and with the readback off (PR #4) CPU readers of the
  composite could see an old frame (above).
- The installers: after an upgrade with the system-volume copy, Safari and WindowServer kept using the `/Library` copy
  until `NVMTLDriver.use-library` was removed by hand (above).

### For developers

- `docs/design/DESIGN-display-boost.md`: what Linux does (i915 boosts the clock when a flip misses its vblank; `xe`
  has no boost at all), why this stack has a display boost and no wait boost, the design, and the B580 measurements.
  The miss trigger (a lease while frames are still running at the vblank they were due at, as i915 does) is in the
  release but off (`debug.intelxe.disp_boost_misses` 0): in 4K60 video it held the clock up for half the time or more
  (+8-10 W) without fewer misses. `IntelXeFB.kext`'s NMXScanout interface is version 4 (op 4: the vblank phase), which
  NMXAccel 0.1.6 uses to know when a frame was due.
- `docs/design/DESIGN-iosurface-vram.md` section 12 (why phase 4 did not engage) and `docs/design/DESIGN-mesh-object.md`
  (the mesh lowering's cause and fix, and what native task and mesh shaders on ANV would take).
- Tests: host tests `kernel/IntelXe/tests/dispboost_test.cpp` (84 checks), `nmx_vbldue_test` (21),
  `user/tests/plugin/scanout_p4_unit.sh` (phase 4's per-frame decision over every input, real IOSurfaces at 2560x1440
  and 3440x1440), three translator tests on the current Metal compiler's mesh output (mesh lowering 28 of 28);
  `mtl_mesh_object`; B580 phases `user/tests/wsi/night-phase4.sh` and `night-dispboost.sh` with `p4check` (`smooth
  -v` lists every late frame with what the app saw just before it), `user/tests/metal/night-meshobj.sh`.
- GUI test agents run with `ProcessType Interactive`: a launchd agent without it is throttled (`p4check smooth` 44.9
  fps as an agent, 171.6 with it).

### Known issues

- **This release as a whole ran on the B580** (14 November, installed through its own package: EFI by `efi.sh`,
  `install.sh --apply`, `system-volume/ssv-install.sh --apply`, with no manual step: the use-library file was made and
  removed by the scripts). Every kext 0.1.6 (NMXAGDC 0.1.5, its source did not change), Metal's plugin loaded from the
  system volume, regress 21 of 21 (and 4 of 4 after a 4-minute sleep and wake), Geekbench 7 Metal 95,327 and OpenCL
  83,978 / 90,325 / 89,047 (three runs; 0.1.5: 94,828 and 90,289), the mesh object test, H.264 and HEVC 10-bit decode on
  the direct path, smoothness 179.6 fps with 3 of 1789 frames late over 10 s, phase 4 and display boost counters
  growing, no GPU reset.
- **Display boost at the first minutes after login:** 14 of 16 window starts from idle had 0-2 late frames; two had
  about 20 (as without the boost). When another screen update (a clock, a notification) comes just before the window,
  that update takes the 400 ms lease and the window's own frames start without a new one; the lease can then end
  before the GuC's clock has risen.
- **Phase 4 on a 3440x1440 panel** (issue #7) is checked by the host test (the frame's page rounding engages) but has
  not yet run on such a panel; the B580 runs were at 2560x1440 and HiDPI 4096x2304. If a screenshot shows an old
  frame, `NVMTL_SCANOUT_READBACK=1` (above) restores the full readback; please report it.
- **GPU faults after a long idle (cause open)**, as in 0.1.5: the GuC low-latency hint stays off by default; if you
  see a `MEMORY_CAT_ERROR`, please send a report.
- The rest as in 0.1.5: OpenCL about 3.5% slower than with 0.1.4's kernel driver in Geekbench 6; `ocvalidate` warns
  about `ResizeAppleGpuBars = 10` (keep 10); a boot held by the boot guard should come up on the firmware framebuffer
  (not yet tested on hardware); IntelXe Manager for Windows has not yet changed a real EFI partition, and no PC has yet
  booted an EFI, a USB installer or a probe stick it made; the Windows app is not code-signed; the B570, Pro B60 and
  Pro B50 are untested; one monitor on DisplayPort; Safari offers no WebGPU `timestamp-query` on Intel GPUs.

### Credits

- Theo R. (`@td512`) for the start mode of 165 Hz panels and the scan-out readback measurement (PRs #4 and #5, issue
  #7), each measured on their machine.
- As in every release: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` and i915, Mesa, FFmpeg,
  Emil J. Tywoniak's Xe2 VDEnc work and Lilu (acidanthera); see the README.
