# Staged probe mode (XeProbe)

**Status:** first implementation (v1), built into `IntelXe.kext` since rb5 (2026-10-30). Run so far: stages 1 and 2 on the Arc B580 (`xeprobe=0xe20b`), and stage 1 on a Raptor Lake-S iGPU (8086:A780), where stage 2 refused as designed because another driver was attached (2026-10-30, [docs/devices/b580.md](devices/b580.md), Tools). Stages 3–5 have not run on any machine yet. Per platform: [docs/PLATFORMS.md](PLATFORMS.md).

## Why

The driver runs on one GPU so far, the Arc B580 (the B570, the same chip, is matched since 0.1.4 but untested); its goal is Intel GPUs in general (see [docs/PLATFORMS.md](PLATFORMS.md)). Before we can support another Intel GPU, we need to know what it does at the steps our driver takes:
- how it identifies itself;
- whether it wakes;
- which engines it has;
- whether its GuC firmware loads;
- whether each engine runs a command.

Testers have the hardware and we do not. Probe mode lets a tester answer those questions one step at a time and send us the results, without risking the machine more than each step requires.

## What it is

`XeProbe` is a second personality inside `IntelXe.kext`. Its source is `kernel/IntelXe/xe_probe.cpp`. While it is waiting:
- **It is opt-in.** It attaches only to the device that the boot-arg `xeprobe=0xDEVICE` names, and only if that device is a display-class function. Without the boot-arg it attaches nowhere.
- **It is idle until asked.** Attaching touches no hardware. Nothing happens until root writes a stage number to a sysctl, the same way `debug.intelxe.step` works.
- **It stays out of the way.** It has its own IOKit match category (`XeProbe`), so it sits beside any other driver instead of replacing it. Stages that would disturb a running driver refuse to run (see Safety).

## Stages

| Stage | Name | Reads | Changes | Passes when | Time limit |
|---|---|---|---|---|---|
| 1 | identity | see below | nothing | the graphics IP version is known | 3 s |
| 2 | forcewake | see below | forcewake, released afterwards | forcewake is acknowledged | 3 s |
| 3 | guc | GuC status | see below | GuC reports READY (`0xf0`) and the CT channel is enabled | 15 s |
| 4 | store | — | see below | every engine on the primary GT has written its dword | 15 s |
| 5 | readback | see below | — | every dword reads back from the CPU (and the VRAM page, on discrete GPUs) | 5 s |
| 9 | release | — | see below | always | 5 s |

**Stage 1 (identity)** reads only. It records:
- the PCI IDs and BARs;
- `GMD_ID` for graphics, media and display;
- stolen memory: GGC/BDSM/DSMBASE on integrated GPUs, LMEM_INIT and the tile address ranges on discrete ones;
- the GGTT size;
- the planes the firmware lit, and the GGTT entry behind each surface;
- the GT and GuC state as found.

On an integrated GPU from graphics 12.70 (MTL, ARL, LNL, PTL) stage 1 also decodes the stolen memory as Linux `xe`
finds it (`detect_lmembar_integrated`): GGC and STOLEN_RESERVED over MMIO, and the DSM at BAR2 + 8 MB. From graphics
12.50 it takes the GGTT as 4 GiB, as `xe` does. On LNL and PTL every register read is preceded by four dummy writes to
0x130030 (Wa_15015404425, as `xe_mmio.c` does there), so stage 1 is not strictly read-only on those two.

**Stage 2 (forcewake)** reads:
- the GT fuses (EU, DSS, media, `FUSE4`);
- the ring registers of each engine.

The only thing it changes is forcewake, which it releases at the end.

**Stage 3 (guc)** changes:
- the GuC reset;
- the WOPCM registers;
- the GGTT entries in the probe window;
- the engine status pages;
- the GuC parameters and DMA.

**Stage 4 (store)** registers one context per engine with the GuC. Each context runs `MI_STORE_DATA_IMM`.

**Stage 5 (readback)** reads the dwords written in stage 4. On a discrete GPU it also makes one copy-engine write into a VRAM page and reads that page through BAR2.

**Stage 9 (release)** resets the GuC, restores the probe window's GGTT entries and frees the memory the probe used.

Each stage needs the stages before it. Stages 3 to 5 also need a known GPU family. If the device ID is not in the table, set `debug.xeprobe.family`.

The sequences follow Linux `xe` (`drivers/gpu/drm/xe`); each function names the Linux code it follows:
- forcewake domains: `xe_force_wake.c`;
- fuses: `read_media_fuses` and `read_compute_fuses_from_reg`;
- WOPCM layout: `xe_wopcm_init`;
- GuC load: `guc_prepare_xfer`, `uc_fw_xfer` and `guc_wait_ucode`;
- ADS: `xe_guc_ads.c`;
- CT channel: `xe_guc_ct.c`;
- context image: `set_offsets` and `reg_offsets`;
- GGTT TLB invalidation: `send_tlb_inval_ggtt`.

The family table is the kext's platform descriptor table (`kernel/IntelXe/xe_platform.cpp`, since 10 Nov 2026), the
same one `IntelXe` starts from ([DESIGN-platforms.md](design/DESIGN-platforms.md)). For the probe it maps device IDs to:
- the graphics and media versions, for GPUs without `GMD_ID`;
- the WOPCM size;
- the GuC image (`<stem>_guc_70.bin`);
- the MOCS index.

The device IDs come from Linux `include/drm/intel/pciids.h`.

| Families | GuC image | Graphics | WOPCM |
|---|---|---|---|
| TGL, RKL, ADL-S, RPL-S, ADL-N | `tgl` | 12.00 | 2 MB |
| ADL-P, RPL-U, RPL-P | `adlp` | 12.00 | 2 MB |
| DG1 | `dg1` | 12.10 | 4 MB |
| DG2, ATS-M (Arc Alchemist) | `dg2` | 12.55 | 4 MB |
| MTL, ARL | `mtl` | `GMD_ID` (12.70–12.74) | 4 MB |
| LNL | `lnl` | `GMD_ID` (20.04) | 8 MB |
| BMG | `bmg` (built-in B580 image) | `GMD_ID` (20.01/20.02) | 8 MB |
| PTL, WCL | `ptl` | `GMD_ID` (30.xx) | 8 MB |

`build.py` builds in every `<stem>_guc_70.bin` it finds in `firmware/` (or `--firmware DIR`; `firmware/fetch.sh` downloads them). If an image is missing, stage 3 says so and nothing else is affected.

## Lunar Lake (Core Ultra 200V)

Lunar Lake's integrated GPU (Xe2_LPG, graphics 20.04, media 20.00) is in the family table. IntelXe refuses it at
start; XeProbe takes it like any listed device. Its design note: [DESIGN-lunar-lake.md](design/DESIGN-lunar-lake.md).

**Device ids** (Linux `include/drm/intel/pciids.h` `INTEL_LNL_IDS`): `0x6420`, `0x64a0`, `0x64b0`. Read yours first.
In macOS, run `system_profiler SPDisplaysDataType | grep -i 'device id'`. In Linux, run `lspci -nn | grep VGA` from
the live USB of step 1. The boot-arg takes the id as it is, for example `xeprobe=0x64a0`.

**Steps:**

1. Make the Linux reference capture (`collect-linux.sh`). On LNL it records the `xe` driver's view: GuC/HuC/GSC versions,
   engines, `stolen` and `GGTT` lines.
2. Use a probe build of `IntelXe.kext`. For stage 3 it needs `xe/lnl_guc_70.bin` built in. `firmware/fetch.sh` fetches
   only the Battlemage files, so copy `lnl_guc_70.bin` from linux-firmware (`xe/`) into the firmware directory next to
   them, and build with `build.py --firmware DIR`. Without it, stages 1-2 still run, and stage 3 says the image is
   missing.
3. Add `xeprobe=0x64a0` (your id) and, the first time, `-xeprobecrumb`. Keep a working EFI on a USB stick.
4. Run the stages one at a time:
   `sudo sysctl debug.xeprobe.stage=1`, read `sysctl debug.xeprobe.log`, then stage 2, and so on.
5. Release with `sudo sysctl debug.xeprobe.stage=9`, reboot, and collect with `collect-macos.sh --probe`.

**What each stage should print on LNL:**

| stage | expected |
|---|---|
| 1 identity | `PCI 8086:64a0 ... class 030000`. BAR0 16 MB and BAR2 (the LMEMBAR: 8 MB of GSM plus the DSM). GMD_ID graphics `0x0501000x` (20.04), media `0x0500000x` (20.00, "own GT"), display `0x0500000x` (20.00); `x` is the stepping. "integrated", GGTT 4096 MB. Two `stolen (LMEMBAR)` lines: GGC with GGMS 3 and the GMS the BIOS set, WOPCM (8 MB is typical), and the usable DSM at BAR2 + 8 MB. The lit plane (the panel) with a GGTT PTE whose bit 1 (DM) is set: on 12.70+ that means stolen memory, as the offset from GSMBASE. |
| 2 forcewake | Forcewake acks on the GT (0xdfc) and on the media GT (0x380dfc). Fuses: geometry and compute DSS (one bit per Xe core), EU, VDBOX/VEBOX on the media GT, and CCS from FUSE4. Engines rcs0, bcs0, ccs0..., vcs/vecs on the media GT. GSCCS and BCS8 are not listed: the probe does not drive them. |
| 3 guc | WOPCM (possibly "locked" by the BIOS on an integrated part: kept as found), the GuC DMA, GuC status `0xf0`, CT enabled. Only if `lnl_guc_70.bin` was built in. |
| 4 store | One context per primary-GT engine, each dword "written". |
| 5 readback | Every dword read back by the CPU. There is no VRAM page on an integrated part. |

**Risks on LNL:**

- Stages 1-2 are reads plus forcewake, as on the other families. The firmware's panel keeps running; the probe never
  writes display registers.
- Stages 3-5 load the GuC once per boot, and the WOPCM layout then stays until the next boot. A hang there is caught by
  the watchdog (a panic naming the step) or the breadcrumb.
- Two Lunar Lake workarounds are applied as in Linux:
  - Wa_15015404425: the dummy writes before reads, described above;
  - Wa_22019338487: a GMD_ID write on the media GT after every 63 GGTT entry writes, in stages 3 and 9.

  Others are not:
  - the Xe2_LPG/LPM register workarounds and tunings (as on every family);
  - the GSC firmware (the probe never loads HuC or the GSC);
  - the 800 MHz media-GT frequency cap.

  The media GT is only listed; the probe runs nothing on it.
- With `-xeprobecrumb`, each step writes NVRAM. On a laptop's firmware NVRAM that wears flash. Use it only to find
  where a hang happens.

## Safety

**It never touches the display pipe the firmware lit:**
- **No display register writes.** Every MMIO write goes through one function, which refuses the display engine's ranges: 0x40000–0x9ffff (pipes, planes, transcoders, DDI, DMC) and 0x160000–0x16ffff (PHYs).
- **No change to the firmware's GGTT entries.** The probe changes only the entries in its own 64 MB window, placed 128 MB below the top of the GuC-addressable GGTT. Before it uses the window, it checks three things:
  - the window does not overlap any lit plane's surface;
  - every entry in the window is zero or one repeated value (a scratch page);
  - on a discrete GPU, its VRAM test page lies above everything a lit surface uses (and at 64 MB or more).

  It saves the entries and puts them back at release.

**It does not take a GPU from another driver.** Stages 2–5 refuse while a driver that is not on a short harmless list is attached to the device. That list is `IONDRVFramebuffer` and `AppleGraphicsDevicePolicy`. IntelXe, AppleIntel\* and anything unknown all block.

**It does not turn on memory decoding.** If the device's memory decoding is off, stage 1 stops. Stage 3 turns on bus mastering if it is off, because the GuC DMA needs it, and records that in the log.

**What stays changed until the next boot:**
- The WOPCM registers are write-once until reset. After stage 3, the GPU keeps the probe's WOPCM layout.
- Release stops the GuC and restores the GGTT window, but it does not reset the GT.

Use the probe on a GPU that nothing else is using, and reboot afterwards.

## When something hangs

**The watchdog.** Every stage arms a watchdog with the stage's time limit. Each stage marks its progress with named steps, for example "GuC DMA" or "store bcs0". If a stage has not returned when its time limit runs out, the watchdog panics:

```
XeProbe watchdog: stage 3 (guc) on 8086:7d55 stalled in step "GuC DMA" (15002 ms in the step, limit 15000 ms for the stage)
```

That turns a silent freeze into a panic report that names the stage and the step. `collect-macos.sh` already collects the panic text. With `debug.xeprobe.panic_on_stall=0`, the stage is only marked STALLED.

**The NVRAM breadcrumb.** A hang can stop every CPU, and then the watchdog cannot run. For that case the probe can leave a record in NVRAM. It is off by default, because each write wears the NVRAM flash. To turn it on, set `debug.xeprobe.nvram=1` or add the boot-arg `-xeprobecrumb`.

When it is on, every step writes the variable `xeprobe-crumb`, for example:

```
XeProbe v1 8086:7d55 stage 3 guc step GuC DMA seq 7
```

It then forces an NVRAM sync before the step runs. After a hard reset, the next boot still shows the last step that started.

Stage boundaries are also recorded (`begin`, `done` or `failed`), so a stage has only a few writes.

## Results

```
sysctl debug.xeprobe.results     # one line per stage: status, ms, detail
sysctl debug.xeprobe.log         # everything the stages printed (also in the kernel log, "XeProbe:")
ioreg -r -c XeProbe -l           # XeProbeResults = {"1 identity" = {result, ms, detail}, ...}
nvram xeprobe-crumb              # the last step of the previous boot (when the breadcrumb was on)
```

`collect-macos.sh --probe` collects all four into `probe/` and adds a section to `summary.txt`. It also lists any panic report from the watchdog.

## Tester workflow

1. **Make a Linux reference capture first.** Run `tools/report/collect-linux.sh` from a live USB. It shows what Linux finds on the same GPU: firmware versions, engines, the VBT.
2. **Inject the probe build of `IntelXe.kext`.** Put it into OpenCore `Kernel > Add` on a USB-stick EFI, and keep a working EFI available. Add the boot-args `xeprobe=0xDEVICE`, and `-xeprobecrumb` if you want the breadcrumb.
3. **Run the stages one at a time:**

   ```
   sudo sysctl debug.xeprobe.stage=1
   sudo sysctl debug.xeprobe.stage=2
   sudo sysctl debug.xeprobe.stage=3
   sudo sysctl debug.xeprobe.stage=4
   sudo sysctl debug.xeprobe.stage=5
   ```

   Or run them all with `sudo sysctl debug.xeprobe.run=5`, which stops at the first failure. Then release:

   ```
   sudo sysctl debug.xeprobe.stage=9
   ```
4. **Collect.** Run `sh tools/report/collect-macos.sh --probe`. If the machine hung or panicked, reboot normally first, then run the collector. The breadcrumb and the panic report survive the reboot.

## v1 limits

**What it does not do:**
- **The media GT (MTL and later).** Its engines are discovered, but the store test does not run on them: they need the media GT's own GuC.
- **The GT setup Linux does around the GuC load:**
  - PAT/MOCS tables (the hardware defaults are used);
  - workarounds and tunings;
  - SLPC;
  - HuC.

  The store test needs none of these, but their absence can make a later stage behave differently from the real driver.

**What it relies on or assumes:**
- **The context image.** It relies on the ring registers being loaded with `ENGINE_CTX_RESTORE_INHIBIT`, as xe's empty LRC does. Graphics 35 and later (indirect ring state) is refused in stage 4.
- **The plane surface size estimate** (used to keep the window clear) is the stride × 512 × height, and at least 16 MB. It is generous on purpose.
- **NVRAM sync.** It uses xnu's force-sync property (`IONVRAM-FORCESYNCNOW-PROPERTY`). Whether that commits immediately on every board's NVRAM, especially emulated NVRAM, is to be verified.

## Where the code is

| What | Where |
|---|---|
| Probe | `kernel/IntelXe/xe_probe.cpp`, `xe_probe_lrc.inc` (context layouts from `xe_lrc.c`), `xe_probe_ids.inc` (device IDs from `pciids.h`), `xe_platform.cpp` (the family table); built by `kernel/IntelXe/build.py` |
| Collector side | `collect-macos.sh --probe` |
