A fix release for the three problems reported against 0.1.6: **no kernel panic at shutdown** any more (issue #8),
**app icons keep all their layers** (issue #9: Safari's red compass needle, WeChat's front bubble), and IntelXe Manager
no longer reports a kernel panic after a normal restart. After updating, clear the icon caches once (below), or icons
drawn wrongly by 0.1.6 stay broken. The install guide is shorter: the usual path first, the background folded away.

> [!WARNING]
> Experimental. Tested on one machine with one GPU (Arc B580); the Arc B570, Pro B60 and Pro B50 are supported but
> untested. Not signed by Apple; needs relaxed SIP/AMFI and OpenCore. Keep a USB stick with a working EFI before
> installing.

> [!NOTE]
> Developed with AI assistance: the code was written by Anthropic's Claude Opus 5.5 (High effort level) under the
> direction of the maintainer, who tests every build on the real hardware.

### Highlights

- **No kernel panic at shutdown (issue #8).** 0.1.6's scan-out phase 4 copies the newest frame of WindowServer's
  composite from its scan-out buffer into the surface's pages when something reads them (a "page-off", done by the GPU).
  When such a copy did not finish within 2 seconds, `IntelXe.kext` deliberately kept the pages mapped for the device,
  because the GPU might still write them, but it kept no wire of its own on them. NMXAccel's normal unwire right after
  the copy then left pages with a DMA mapping and no wire, which macOS treats as fatal: `complete() while dma active`
  in `NMXResource::pageoff`. At shutdown the GPU is being stopped, which is when such a copy can time out. Now
  `IntelXe.kext` keeps a wire together with the kept mapping, and NMXAccel wires a surface's pages only when there is a
  page-off to do (a bound composite marked newer than its pages); before, every CPU lock of every surface without VRAM
  wired and unwired its pages for nothing. On the B580: restarts with no panic, page-offs still work.
- **App icons keep all their layers (issue #9).** Some layered app icons were drawn with a layer missing, differently
  each time they were rendered: Safari's red compass needle half, WeChat's front bubble. The cause is in Mesa's Vulkan
  driver (ANV), with descriptor buffers: a set offset given together with a set layout that has no bindings flagged no
  shader stage, so ANV kept the offset it had pushed before; a later layout with stages at that same offset then looked
  like "no change", and its stages read the set at an older region's offset. The Metal driver gave such empty stage
  plans the offset where the next real region starts, which is exactly that case (Safari's icon in RenderBox: 3 of 168
  draws). Fixed twice:
  - the Metal driver gives empty stage plans offset 0, and the first 64 bytes of every descriptor page are never a
    region (`nullmoth/plugin/nvmtl_dbuf.c`). On the B580 this alone renders Safari's icon correctly;
  - Mesa patch `0109` fixes ANV itself, for every Vulkan program on this driver (the Zink OpenGL and Vulkan packages use
    the installed ANV, so they get it too). The reproduction `user/tests/vulkan/vk_dbuf_empty_set` passes on the B580
    with 0.1.7's ANV and fails with 0.1.6's.

  macOS keeps rendered icons in its icon caches, so icons drawn wrongly before the update stay wrong until the caches
  are cleared: see *Upgrading from 0.1.6*, step 5.
- **IntelXe Manager: no "macOS restarted after a kernel panic" after a normal restart.** The Overview page reported any
  panic report of the last 7 days, so after one panic every restart that week showed it. It now reports only a panic
  report written during this boot, which is when macOS writes the report of a panic in the boot before.
- **Blur and translucent windows cost the GPU much less.** Backdrop blur and the other translucent windows and panels
  read the pixels already on screen while they are drawn. To keep those reads in order, the Metal driver made the GPU
  wait between almost every two triangles of such a drawing; it now waits only where two triangles can really overlap.
  Web pages with many `backdrop-filter` panels gain the most: on the B580 with twelve blurred panels on screen, the
  GPU's waits went from about 300,000 to under 10,000 a second, and the desktop needs less GPU time for each frame. If
  a translucent window ever looks wrong, the line `NVMTL_FETCH_OVERLAP=0` in `/Library/GPUBundles/nvmtl/nvrm610.conf`
  brings back the old way after a restart; please report it.
- **A simpler install guide.** `README-INSTALL.md` (and the Turkish `README-INSTALL.tr.md`) gives the usual path first
  in short steps, with the background and the rarer cases folded below; the site's guide was simplified the same way.

### Packages

| File | For | Unpack on |
|---|---|---|
| `IntelXe-Manager-0.1.7.zip` | the IntelXe Manager app (macOS 15 or later) | macOS |
| `IntelXeManager-0.1.7-win-x64.zip` | IntelXe Manager for Windows: stages 2 and 4 from Windows, a check of the PC, the setup steps, reports (unchanged apart from its version) | Windows 10 or 11 (x64) |
| `intel-gpu-macos-driver-0.1.7-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.7-sequoia.zip` | macOS 15 Sequoia (same sources; not re-tested on Sequoia) | macOS |
| `intel-gpu-macos-driver-0.1.7-efi.zip` | the OpenCore pieces: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (optional), `IntelXe-EFI.ps1` and `efi-rules.json` | any system |
| `intel-gpu-macos-driver-0.1.7-zink-gl.zip` | optional: OpenGL through Mesa's Zink (0.1.5's binaries; it uses the installed ANV, so the issue #9 fix applies) | macOS |
| `intel-gpu-macos-driver-0.1.7-vulkan.zip` | optional, experimental: Vulkan for Wine (0.1.5's binaries; the same) | macOS |
| `xe-probe-stick-0.1.0.img.zip` | optional, experimental: the Xe probe stick's image, for Intel GPUs the driver does not support yet (the same image as 0.1.5's and 0.1.6's) | write it to a USB stick |

Rebuilt for 0.1.7: `IntelXe.kext`, `NMXAccel.kext`, the Metal driver (both copies), Mesa's Vulkan driver
`libvulkan_intel.dylib` (both copies: 0.1.6's source plus patch `0109`), IntelXe Manager. `HybridPM.kext` carries
0.1.5's code with the new version; `IntelXeFB.kext`, `NMXAGDC.kext`, the shader translator, the video plug-ins and
FFmpeg are the same files as in 0.1.6 (`SOURCES.md` in each package lists every binary).

Checksums: `intel-gpu-macos-driver-0.1.7.sha256`, signed with the project's key in
`intel-gpu-macos-driver-0.1.7.sha256.sig`; it lists the Windows app's zip and the probe stick's image too. Install
guide: `README-INSTALL.md` in each macOS package and
[in the repository](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.7/release/README-INSTALL.md)
(Turkish: `README-INSTALL.tr.md`).

### Upgrading from 0.1.6

1. Install the package (the app: *Download v0.1.7*, then *Install this package*; or `sudo ./install.sh --apply`).
2. **Replace `IntelXe.kext` in the EFI** before you restart: the app's EFI page or `sudo ./efi.sh --apply` on macOS;
   on Windows IntelXe Manager for Windows (EFI page, *IntelXe.kext and SSDT-ARCOFF (stage 4)*) or `IntelXe-EFI.ps1`;
   or by hand. Its `config.plist` entry stays as it is. The same step replaces `HybridPM.kext` where it is used. Half
   of the shutdown panic fix is in `IntelXe.kext`: with 0.1.6's left in the EFI, the panic can still happen.
3. If you installed the system-volume copy for Safari, **install this package's too** (the app's Driver page, *Safari
   and WebKit*; or `sudo ./system-volume/ssv-install.sh --apply`). 0.1.7 changes the Metal driver and Mesa in both
   copies; until you install it, `install.sh` makes `NVMTLDriver.use-library` so that every process uses the new
   `/Library` copy, and `ssv-install.sh --apply` removes that file again.
4. Restart.
5. **After the restart, clear the icon caches once**, in Terminal:

   ```bash
   rm -rf "$(getconf DARWIN_USER_CACHE_DIR)com.apple.iconservices" "$(getconf DARWIN_USER_CACHE_DIR)com.apple.dock.iconcache"
   sudo rm -rf /Library/Caches/com.apple.iconservices.store
   killall Dock
   ```

   The Dock and Finder then draw the icons again, with the new driver. Do this after the restart, not before it:
   until the restart the earlier driver would draw them again.

From 0.1.5, take these steps too, then read 0.1.6's notes for what 0.1.6 changed (its switches back to 0.1.5's
behaviour still apply). From 0.1.4 or earlier, follow the guide's upgrade section first.

### Fixed

- `IntelXe.kext`, `NMXAccel.kext`: a kernel panic at shutdown, `complete() while dma active` in
  `NMXResource::pageoff`, after a scan-out page-off copy timed out (issue #8, above).
- The Metal driver and Mesa's ANV: layered app icons drawn with a layer missing (issue #9, above).
- IntelXe Manager: "macOS restarted after a kernel panic" shown after every normal restart for a week after a panic
  (above).

### For developers

- `mesa/patches/0109` (ANV, descriptor buffers: per set, the stages not flagged at the last offset change are flagged
  when a later layout has them; platform independent, applies on upstream `f01e9b07a91` and `intelxe.diff`) and its
  row in `mesa/README.md`. 0.1.7's ANV is 0.1.6's source plus `0109` and nothing else: the same build without `0109` is
  0.1.6's file bit for bit.
- `user/tests/vulkan/vk_dbuf_empty_set.c`: the issue #9 case on Vulkan alone (it loads the ANV given on its command
  line, so two builds can be compared side by side); build it with `MESA=<a Mesa tree> user/tests/vulkan/build.sh`.
- The Metal driver's descriptor pages: `NVMTL_DB_EMPTY_OFF` and the reserved first 64 bytes in
  `nullmoth/plugin/nvmtl_dbuf.c`; `dbuf_model` (the host model of the descriptor-buffer binds) passes with them.
- NMXAccel's phase 4: `nmxScanNewer` decides whether a CPU lock or a copied frame has a page-off to do before any page
  is wired; `IntelXe.kext`'s `svCopyBo` keeps a wire with the IOVA mapping of a copy that timed out.

### Known issues

- **This release on the B580 as a package:** installed from 0.1.6 through its own package (EFI, `/Library` and
  the system-volume copy). Versions as expected, Metal loaded from the system volume; after the icon caches were cleared
  Safari's icon rendered correctly 3 times out of 3; `vk_dbuf_empty_set` passes with both ANV copies; no panic in three
  restarts; phase 4 and display boost counters as expected; the framebuffer-fetch order test `mtl_fetch_order` 18 of 18;
  regress 18 of 19 (the GPU faults below; WebGPU skipped because Safari was open); Geekbench 7 Metal 96,824 to 99,496,
  OpenCL 87,144 to 91,609; four minutes of sleep and a wake, then regress 13 of 13.
- **A scaled 2560x1440 mode on a 4K panel garbles the output** (issue #10, since 0.1.4): screenshots are clean, the
  panel shows garbage, so the scaled frame goes wrong on its way to the display. The test machine has a 2560x1440 panel
  and cannot reproduce it; we are waiting for the reporter's diagnostics. Until then use the panel's native mode or
  another scaled mode.
- **Display boost at the first minutes after login**, as in 0.1.6: when another screen update (a clock, a
  notification) comes just before a window starts after idle, that update takes the 400 ms lease and the window's own
  frames can start before the GuC's clock has risen.
- **Phase 4 on a 3440x1440 panel** (issue #7) is checked by the host test but has not yet run on such a panel. If a
  screenshot shows an old frame, `NVMTL_SCANOUT_READBACK=1` in `/Library/GPUBundles/nvmtl/nvrm610.conf` restores the
  full readback (WindowServer takes it at the next restart); please report it.
- **GPU faults while Safari is open (cause open)**, also in 0.1.6 and earlier: with Safari running, even in the
  background without a window, a heavy OpenCL program can make the GPU fault on an address that is not mapped
  (`MEMORY_CAT_ERROR`). The GuC resets that program's work, and the program may lose it or quit (Geekbench 7 OpenCL on
  the B580: faults in 5 of 6 runs with Safari open, none in 18 runs without it; a finished run scores normally). The
  faulting work is the OpenCL program's own; Safari's GPU process only has to be there, so the cause is being looked
  for in how the driver handles two such clients. Until it is fixed, quit Safari before long OpenCL work; if you see a
  `MEMORY_CAT_ERROR`, please send a report.
- The rest as in 0.1.6: OpenCL about 3.5% slower than with 0.1.4's kernel driver in Geekbench 6; `ocvalidate` warns
  about `ResizeAppleGpuBars = 10` (keep 10); a boot held by the boot guard should come up on the firmware framebuffer
  (not yet tested on hardware); IntelXe Manager for Windows has not yet changed a real EFI partition, and no PC has yet
  booted an EFI, a USB installer or a probe stick it made; the Windows app is not code-signed; the B570, Pro B60 and
  Pro B50 are untested; one monitor on DisplayPort; Safari offers no WebGPU `timestamp-query` on Intel GPUs.

### Credits

- `@mydogbro-beep` for issues #8, #9 and #10: the shutdown panic with its backtrace, the icons with a layer missing
  and the steps that showed them again, and the scaled mode on a 4K panel.
- As in every release: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` and i915, Mesa, FFmpeg,
  Emil J. Tywoniak's Xe2 VDEnc work and Lilu (acidanthera); see the README.
