# Kernel extensions

## IntelXe/ — GPU core (`io.github.yusufklncc.IntelXe`, IntelXe.kext)

A plain IOKit kext, without Lilu, for the Arc B580 (Battlemage BMG-G21, `8086:e20b`) and, since 0.1.4, the Arc B570
(the same chip with fewer Xe-cores, `8086:e20c`; untested on hardware). It follows Linux `drivers/gpu/drm/xe` closely.

- **Platforms:** `xe_platform.h`/`xe_platform.cpp` hold one descriptor per device-ID list, as Linux `xe_pci.c` does (IP
  versions, flags, engine mask, WOPCM, PAT entries, MOCS index, firmware names). Start looks the device up once and
  logs it (`IntelXe: 8086:e20b: BMG-G21, graphics 20.01, media 13.01, discrete`); only BMG-G21 is supported, every
  other entry stops start with one log line. The Battlemage constants of the code read the descriptor. Design and what
  the other families need: [DESIGN-platforms.md](../docs/design/DESIGN-platforms.md).

- **Bring-up:**
  - pcode init status, forcewake;
  - PAT/MOCS tables (xe2), GT and engine workarounds and tunings;
  - GuC 70.72.1 load with ADS, CT, SLPC and GuC RC;
  - golden contexts per engine class;
  - MSI interrupts;
  - media GT (VCS0/VCS2, VECS) with its own GuC, HuC 8.2.10 authentication and media SLPC.
- **Memory:**
  - VRAM buddy allocator (drm_buddy style) and GGTT range allocator;
  - 5-level PPGTT with 64 KB and 2 MB/1 GB leaves;
  - a CPU-visible BAR2 window, with buffers spilling to system memory when it is full;
  - userptr, including read-only imports.
- **uAPI:** the xe uAPI that Mesa's ANV uses (`uapi/xe_drm.h`), over an IOUserClient (`IOAccelIntelXeClient`):
  - queries, GEM create/mmap (the `GEM_CREATE` extension chain checked as `xe_bo.c` does, in `xe_uapi_args.cpp`),
    VM bind (async), exec queues on GuC submission;
  - DRM syncobjs (binary and timeline);
  - user fences: `DRM_XE_SYNC_TYPE_USER_FENCE` on exec (written by the ring after the batch) and VM_BIND (written once
    the bind's fence signals), and `DRM_IOCTL_XE_WAIT_USER_FENCE`, as Intel's compute runtime uses them.
  - `uapi/intelxe_drm.h` adds IOSurface implicit fences (the dma-resv model) and scan-out pinning for the display side.
- **Robustness:** job watchdog / TDR, GT reset, shutdown quiesce, sleep/wake (`intelxePowerEvent`); an engine and
  fault-register dump in the G2H log when the GuC resets a context or a job never starts, with the BO moves, TLB
  invalidations and the faulting VM's last unbind/remap/bind.
- **Direct submission** (`debug.intelxe.direct`; 0.1.4: opt-in, default 0; in this tree since 12 Nov: on once the device is
  up, `-intelxe_nodirect` keeps it off): an application's render or compute queue keeps an
  armed slot on its ring, so a job starts without a GuC round trip ([design](../docs/design/DESIGN-direct-submission.md)).
- **Settings and readings:** the GPU power profile and clock range from NVRAM (`intelxe-gpu-profile`,
  `intelxe-gpu-min-mhz`, `intelxe-gpu-max-mhz`, written by the app); `debug.intelxe.sensors` (GT clock, temperatures,
  and on BMG the card and package energy and average power from PMT telemetry as Linux xe_hwmon reads them, with
  implausible power windows refused and counted, then the card and package PL1/PL2 and the card's I1 read through the
  pcode mailbox as xe_hwmon reads them, read only; readable by anyone); `debug.intelxe.jobtime` (each job's stages in GPU timestamps, off by default).
- **PCIe ASPM (experimental, untested, off unless written):** `xe_aspm.cpp`, the checks in `xe_aspm.h`.
  - `sysctl debug.intelxe.aspm` (anyone): every link between the GPU function and its root port (on the B580: the
    card's switch downstream port <-> GPU, and root port <-> the card's switch upstream port), each end's Link
    Capabilities (ASPM support, L0s/L1 exit latency), Link Control (ASPM, Common Clock, CLKREQ#), Link Status (speed,
    width, slot clock), Device Capabilities (acceptable latencies, RBER) and L1 PM Substates, whether L1 passes the
    checks, the endpoints below the path, and the last change made.
  - `sudo sysctl debug.intelxe.aspm_l1=1`: ASPM L1 on every link of the path, only if every link passes Linux
    `drivers/pci/pcie/aspm.c`'s rules (L1 in both ends' Link Capabilities, no pre-1.1 function, Common Clock configured
    where the slot clock is shared, each endpoint's acceptable L1 latency against the link's exit latency plus 1 us per
    link); otherwise nothing changes and the reason is in the report and the log. `=0`: ASPM off on the path. `=-1`
    (default): leave the links alone (what was changed stays until a reboot). Changes go through
    `IOPCIDevice::setASPMState`, upstream end first to enable and downstream end first to disable, the root port's
    link first to enable and the GPU's link first to disable; L1 substates are not enabled (IOPCIFamily clears their
    control registers on a link it changes). After a system wake the last 0 or 1 is applied again where the links lost
    it.

**Version:** since 0.1.2 the kext carries the number of the release that last rebuilt it (0.1.4 here). 0.1.0 to 0.1.3
shipped `rb7u` (renamed to IntelXe; tested on macOS 26.7.1); 0.1.4 adds the B570's ID, the NVRAM settings, the sensors and job-timing sysctls, the engine dump and direct submission (the 9 Nov test build), and since 10 Nov in this tree the platform descriptors, user fences, the wider CAT-fault dump, the card and package power in `debug.intelxe.sensors` and the experimental, untested ASPM sysctls. `rb7u` is
`rb7t` (VRAM-shared IOSurfaces fixed at boot, purgeable BOs, shared BOs, external fds behind `debug.intelxe.fds`)
plus `rb7u` (macOS 26 starts drivers while the card is still leaving its reset: wait for config space, read the VRAM
size again). The earlier steps, each a separate patch in the development tree:

| Step | What |
|---|---|
| `ph` | `pi` + IODMACommand-based DMA (VT-d ready) + `intelxePowerEvent` for IntelXeFB's sleep/wake save/restore |
| `pv` | **BAR window under pressure.** 32 MB of the 1 GB CPU-visible window is kept for page tables (a full window had made every VM_BIND fail with ENOMEM); one memory descriptor per CPU map (GEM_MMAP_OFFSET returns a cookie), so `IOConnectUnmapMemory64` finds and removes VRAM mappings (it never did); cheap eviction scans; `debug.intelxe.vram` (window, VRAM and system BOs per process) and window counters in `debug.intelxe.stats` |
| `pv2` | EXEC waits behind the VM's kernel fences (BO clears), as `xe_exec` waits on the VM resv KERNEL slot |
| `pv3` | a CPU map of a BO beyond a full window moves it to system memory (`boMoveSys`, xe's fault migrate to TT) instead of failing with ENOSPC |
| `sv1`-`sv7` | VRAM-shared IOSurfaces (`uapi/intelxe_surfvram_kabi.h`, the registry NMXAccel and the plugin use; on at boot with `intelxe_surfvram=2`, as the install guide sets it, otherwise off until `debug.intelxe.surfvram` is set) |

What is not done yet for the window: a BO the CPU has mapped never leaves it (an IOKit user mapping of a physical range
cannot be pointed elsewhere), so the window is first come, first served, with spills to system memory.

## IntelXeFB/ — display (`io.github.yusufklncc.IntelXeFB`, IntelXeFB.kext, class `IntelXeFramebuffer`)

An IOFramebuffer for the card's DisplayPort outputs (display IP 14.01, C20 PHYs).

- **Mode setup:**
  - takes over the mode the GOP lit;
  - reads EDID over DP AUX;
  - scaled modes through the pipe scaler;
  - the modes and refresh rates the monitor's EDID offers (all blocks, Linux's drm_edid parsing; 120 modes on the test
    monitor, the same rates as Windows);
  - Adaptive-Sync (VRR) with an LFC pacer.
- **Link:** retraining and a full modeset with C20 PLL programming (`IntelXeLink.cpp`).
- **Cursor:** hardware cursor.
- **Pipe gamma:** for IOPresentment.
- **Flips:** real flips from scan-out buffers pinned in VRAM.
- **Power:** sleep save/restore.
- **Boot-time arming:** at boot it matches from the AuxKC and arms acceleration in the kernel together with NMXAccel ("boot1"), with an NVRAM panic-loop guard (`intelxe-guard`).

**Version:** the number of the release that last rebuilt it (0.1.4 here). 0.1.0 to 0.1.2 shipped v10m: boot1 + v9 + v10b (`intelxefbRunGated`: NMXAccel's pipe registration inside IOGraphics' gate order; detailed timings answered with their mode size) + v10c (boot-guard fix) + the later steps up to v10m (modes from the EDID). 0.1.3 added refresh rates above the firmware's mode, Adaptive-Sync modes on by default and the opt-in HiDPI modes (`intelxefb-hidpi`); 0.1.4 reads the user settings from NVRAM (`intelxefb-hwcursor`, `intelxefb-dpms`, `intelxefb-vrr-lfc`, `intelxefb-vrr-floor`).

Boot guard (`intelxe-guard` in NVRAM): after two accelerated boots in a row that end inside their first minute the next boots arm with
NMXAccel's safest settings (`IntelXeFBAccelSafe`); four in a row turn acceleration off for the next boots (on macOS 26 that leaves the Apple
logo on screen: WindowServer uses no display without the accelerator's pipe) (`intelxe-accel-off`, until `sudo nvram intelxe-accel-off=ok`; `nvram -d` leaves it in place on macOS 26). One such boot only makes
the next one try again: it may have been a power cut. On x86 macOS the kernel's NVRAM writes after boot stay in memory until a clean
shutdown (AppleEFINVRAM ignores the SYNCNOW keys, and calling `IODTNVRAM::sync()` from IntelXeFB's start hung the boot), so `install.sh`
adds a LaunchDaemon that writes the cleared guard with `nvram` once per boot; tested on the B580 with a power cut (5 Nov). Since v10c "healthy" needs WindowServer itself: its
IOFramebuffer server connection (opened by the process named WindowServer, the last time 45 s ago or more), steady VBL
clients, and for the accelerated stage at least 120 NMXAccel frame calls. Before, VBL clients and one frame call were
enough, and NMXAccel's display pipe provides both on its own: at a boot where WindowServer deadlocked before it started
the guard still cleared.

Scripts (`scripts/`):
- `intelxe-accel.sh`: arms WindowServer acceleration by hand, used by builds before boot1.
- `intelxefb-load.sh` + `io.github.yusufklncc.intelxefb.plist`: the LaunchDaemon loader for builds before boot1; a no-op with boot1.
- `install-boot1.sh` / `rollback-boot1.sh`: switch the installed kexts to boot1 and back. Paths inside them are those of the development machine.

Licence: `IntelXeFB.cpp` is derived from nullmoth's NMIntelFB/NVRMFB and is therefore PolyForm Noncommercial 1.0.0. `IntelXeLink.*` is MIT. See `IntelXeFB/LICENSE`.

## HybridPM/ — CPU clocks for hybrid Intel CPUs (`io.github.yusufklncc.HybridPM`, HybridPM.kext)

A Lilu plugin, separate from the GPU driver, for Alder Lake / Raptor Lake CPUs (12th to 14th generation Core with P- and
E-cores). macOS's XCPM writes the P-cores' HWP request in 100 MHz units where they count 78.741 MHz, and pins busy cores
to low clocks; HybridPM converts the request and lets the hardware pick each core's clock. On the test machine's
i7-14700K one thread went from ~55 to 128 M iterations/s and Geekbench's GPU scores rose about a third (the driver's
CPU work per command buffer runs faster). `efi.sh` and the app add it below Lilu at stage 4 when the CPU needs it.
Details, controls and `hpmstat`: [HybridPM/README.md](HybridPM/README.md).
Built by `kernel/HybridPM/build.py` (MacKernelSDK and Lilu's headers).

## Building

```sh
firmware/fetch.sh                                   # GuC/HuC blobs (not in git)
git clone https://github.com/acidanthera/MacKernelSDK external/MacKernelSDK   # or MACKERNELSDK=/path
python3 kernel/IntelXe/build.py                        # -> build/IntelXe.kext, build/IntelXeFB.kext
python3 kernel/IntelXe/build.py --debug                # -O0, INTELXE_DEBUG=1 (destructive bring-up steps, batch dumps)
python3 kernel/IntelXe/build.py --only IntelXe --output /tmp/k
python3 kernel/HybridPM/build.py --lilu /path/to/Lilu.kext   # -> build/HybridPM/HybridPM.kext, hpmstat
```

Built on an Apple Silicon Mac with the Command Line Tools (x86_64 kext target).

Host tests (no GPU, any Mac), from `kernel/IntelXe/tests/`:

```sh
c++ -std=c++17 -Wall -Wextra -I.. -o platform_test platform_test.cpp ../xe_platform.cpp && ./platform_test
c++ -std=c++17 -Wall -Wextra -I.. -o uapi_test uapi_test.cpp ../xe_uapi_args.cpp && ./uapi_test
c++ -std=c++17 -Wall -Wextra -I.. -o pmt_test pmt_test.cpp ../xe_platform.cpp && ./pmt_test
c++ -std=c++17 -Wall -Wextra -I.. -o aspm_test aspm_test.cpp && ./aspm_test
c++ -std=c++17 -Wall -Wextra -I.. -o pcode_test pcode_test.cpp ../xe_platform.cpp && ./pcode_test
```

`platform_test` checks the descriptor table against the constants the code used, XeProbe's former family table and
Linux's BMG id lists; `uapi_test` checks the sync, user-fence wait and `GEM_CREATE` extension argument handling against
Linux xe; `pmt_test` checks the BMG energy path (PMT offsets, GUID decode, field extraction, unit scaling, wrap
accumulation, average power, the power-window sanity guard); `pcode_test` checks the pcode mailbox protocol against a
fake pcode (write order, busy mailbox, timeout, error codes as Linux `xe_pcode.c`) and the power-limit units and report
text; `aspm_test` checks the ASPM path logic (latency decoding against Linux aspm.c, which links may take L1, the call
order for enabling and disabling, the report text). The GPU test for user fences is `user/tests/kernel/ufence.c`.
