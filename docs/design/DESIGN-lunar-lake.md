# Lunar Lake (Xe2_LPG, integrated): what it needs, and the order of work

> Design note with its date (2026-11-13, branch `lnl`). It records the plan and the state of that day, not the current
> status. No Lunar Lake hardware was available: everything here comes from the Linux source and host tests.

Lunar Lake (Core Ultra 200V, Arc 130V/140V) has graphics 20.04 (Xe2_LPG) and media 20.00 (Xe2_LPM) on a standalone
media GT. It is the integrated sibling of Battlemage (graphics 20.01, media 13.01), the only family IntelXe.kext runs
on today. The two share the Xe2 LRC layout, the Xe2 PAT encoding, the MOCS table and most workarounds. What differs is
everything around them:

- there is no VRAM;
- the stolen memory sits behind a BAR that is not VRAM;
- the CPU caches are not shared;
- the GSC firmware is part of the boot;
- the display is a laptop display (xe2lpd: eDP panel, Type-C).

This note compares what Linux xe does on LNL with what this kext does on BMG, subsystem by subsystem, and gives an
ordered plan. Its milestones can each be checked by a tester with XeProbe ([PROBE-MODE.md](../PROBE-MODE.md)).

**Sources.**
- Linux: `dl/linux-xe` (torvalds/master, abf8c33, the v7.4 merge window), paths relative to `drivers/gpu/drm/xe/`.
- Linux v6.18 from kernel.org, read 2026-11-13: `include/drm/intel/pciids.h`, `xe_pci.c`, `xe_uc_fw.c`, `xe_pat.c`,
  `xe_ttm_stolen_mgr.c`, `tests/xe_pci.c`, `i915/display/intel_dmc.c`.
- Mesa: the ANV fork in `~/hackintosh/src/mesa` (62a38137177).
- This repository: `kernel/`, `nullmoth/plugin`, `user/xemac`.

Size estimates are lines of kext code, new or changed. They are orders of magnitude, not promises.

## 0. The biggest pieces

| # | piece | why | size |
|---|---|---|---|
| 1 | **Display (xe2lpd)** | IntelXeFB drives display 14.01 (BMG): C20 PHYs on DDI A + TC1-4, no eDP, no Type-C, no DMC, no PSR. LNL is display 20.00: C10 PHYs for the eDP panel, Type-C through PICA, the DMC (`i915/xe2lpd_dmc.bin`) for DC states, PSR, panel power and backlight, LPDDR5X bandwidth tables. | 3000-5000 |
| 2 | **Integrated memory path** | Every BO, page table, LRC, GuC object and display buffer is in system memory. The VRAM buddies, the BAR window, eviction, the migrate identity map and the VRAM save at suspend all go away on this path. Stolen memory (DSM) is reached through the LMEMBAR. | 2000-2500 |
| 3 | **Flat CCS in system memory** | LNL compresses system-memory BOs. Their CCS lives in extra pages after the BO (size/512). It has to be cleared on creation and carried by the migrate engine on swap-out and suspend. BMG keeps CCS in VRAM only. | 400-600 |
| 4 | **GSC firmware** | Two jobs depend on it. HuC's second authentication goes through the GSC (`xe_gsc.c gsc_upload_and_init`), and media C6 needs the GSC loaded (`xe_gsc.c:416-421`). It needs the GSCCS engine, a 4 MB stolen BO and HECI status registers. A missing `xe/lnl_gsc_1.bin` fails Linux's probe on LNL (`xe_gsc.c:422-428`). | 600-800 |
| 5 | **Workarounds, GuC KLVs, SLPC** | The Xe2_LPG/Xe2_LPM deltas are small (section 6), but each one is per-register work. Two of them change behaviour: 22019338487 caps the media GT at 800 MHz until the GSC is up and flushes GGTT writes every 63 entries; 15015404425 adds dummy writes before every MMIO read. | 300 |
| 6 | **Userland** | ANV is already right for an integrated device if the kernel reports one. The Metal plugin assumes a non-DEVICE_LOCAL memory type exists for every spill and eviction path, and reports `hasUnifiedMemory` NO. | 300-500 (plugin) |

Each piece can be built and host-tested in parts. None of them can be finished without the hardware.

## 1. Identity: the descriptor

`kernel/IntelXe/xe_platform.cpp` `lnl()` was checked field by field against Linux. `tests/platform_test.cpp` section 7
holds the checks.

| field | LNL | Linux |
|---|---|---|
| ids | 6420, 64a0, 64b0 (Intel's media-driver also names 6480, which Linux does not list: no entry, as xe would not bind it) | `pciids.h` `INTEL_LNL_IDS` (v6.18 :846-849) |
| IP | graphics 20.04, media 20.00; GMD_ID platform | `tests/xe_pci.c:58-59` GMDID_CASE (A0 and B0 graphics, A0 media) |
| flags | dma 46, va 48, vm_max_level 4, 2 GTs per tile, has_display, has_flat_ccs, has_pxp, needs_scratch; **no** is_dgfx, has_llc, has_cached_pt, has_sriov, GSC NVM, HECI | `xe_pci.c:399-410` `lnl_desc` (v6.18 :319-326 with va_bits / vm_max_level / has_flat_ccs in `XE2_GFX_FEATURES`) |
| graphics features | access counter, ASID, atomic PTE bit, range TLB invalidation, USM, 64-bit timestamp | `xe_pci.c:105-121` `XE2_GFX_FEATURES` |
| engines | RCS0, BCS0, BCS8, CCS0-3; VCS0-7, VECS0-3, GSCCS0 (fused at runtime) | `graphics_xe2`, `media_xelpmp` (:152-157) |
| WOPCM | 8 MB | `xe_wopcm.c:182` (platform >= LUNARLAKE) |
| PAT | all 32 entries of `xe2_pat_table` (no Wa_16023588340, which is 20.01 only), `xe2_pat_ats`, **no PTA** | `xe_pat.c:619-641` |
| MOCS | `xe2_mocs_table`, uc_index 3, wb_index 4 | `xe_mocs.c:606-614` |
| GuC | `xe/lnl_guc_70.bin`, 70.53.0 (v6.18: 70.45.2) | `xe_uc_fw.c:121` |
| HuC | `xe/lnl_huc.bin`, any version | `xe_uc_fw.c:134` |
| GSC | `xe/lnl_gsc_1.bin`, compatibility version 104.1.0 | `xe_uc_fw.c:146`, file name from `fw_filename_gsc` (:162-163) |
| DMC | `i915/xe2lpd_dmc.bin` (display 20.00) | i915 `intel_dmc.c` `dmc_firmware_default` (v6.18 :195-197) |
| device workarounds | Wa_15015404425 | `xe_device_wa_oob.rules:2` |

**A bug found on the way.** The GSC file names were wrong for MTL, LNL and PTL: `mtl_gsc_102.bin`, `lnl_gsc_104.bin`,
`ptl_gsc_105.bin`. The reason is that xe names the GSC file by the *compatibility* major (the second number of the
`fw_def`), not by the project version. The correct names are `i915/mtl_gsc_1.bin`, `xe/lnl_gsc_1.bin` and
`xe/ptl_gsc_1.bin`, the names linux-firmware uses. The descriptor now also has `gscMajor`/`gscMinor` (104.1 on LNL).
`xe_gsc.c query_compatibility_version` checks these after the load.

**The capability set.** `xe_platform.h` derives from `isDgfx` what xe derives:

- integrated, has VRAM, has LLC;
- the region mask, and the near/far regions of the GT list;
- the stolen model, and whether stolen memory is reached through the GGTT;
- whether BAR2 is VRAM, and the GSM size;
- whether system memory is always coherent;
- CPU-WC page tables, and system-memory flat CCS;
- the PTA PAT entry, the VF_CAP barrier, and the LMEM_INIT wait.

Section 8 of the platform test checks every entry against Linux's rules. LNL stays refused at start.

## 2. Memory model

### What Linux does on LNL

- **No VRAM region.** `xe_device_vram_alloc`, `xe_tile_alloc_vram` and `xe_vram_probe` return early when `!IS_DGFX`
  (`xe_device.c:678-690`, `xe_tile.c:119`, `xe_vram.c:315`). `mem_region_mask` stays 1 (`xe_device.c:897`;
  the VRAM bit is added only in `xe_tile.c:193-199`).
- **Queries.**
  - `query_mem_regions` lists system memory only (`xe_query.c:247-317`).
  - `query_gt_list` gives near = 0x1 and far = 0 (:418-424).
  - `query_config` leaves HAS_VRAM off (:342-344).
- **Every BO is TT (system pages).** `xe_bo.c xe_ttm_tt_create` (:590-640) does the following:
  - The TT caching follows the BO's `cpu_caching`: WC is write-combined, WB is cached.
  - Page tables are write-combined, because `has_cached_pt` is off (Xe_LPG and later).
  - A user BO without `cpu_caching` is a warning.
  - On a discrete part, every TT page is WB and coherent.
- **Kernel objects.**
  - Page tables (`xe_pt.c:124`), LRCs (`xe_lrc.c:1713`), GuC UM queues (`xe_guc_ads.c:465`) and display buffers
    (`display/xe_fb_pin.c:168`, `xe_display_bo.c:153`, `xe_dsb_buffer.c:56`) are `XE_BO_FLAG_VRAM_IF_DGFX`, so they
    are system memory here.
  - GuC firmware, log and ADS stay in system memory (`xe_guc.c:722`, `xe_guc_realloc_post_hwconfig`).
  - The UM queues are one power-of-two block, because the GuC needs them DMA-contiguous (`xe_guc_ads.c:455-488`).
- **Stolen memory (DSM)** comes from `xe_ttm_stolen_mgr.c detect_lmembar_integrated` (:105-178), for graphics 12.70 and
  later.
  - BAR2 is the LMEMBAR: the first 8 MB are the GSM (the GGTT's entries) and the DSM follows. So `stolen_base` is 8 MB
    and `io_base` is BAR2 + 8 MB.
  - The size is GGC.GMS (MMIO 0x108040; GGMS must be 3) minus the WOPCM at the top (STOLEN_RESERVED bits 9:7).
  - Wa_14019821291 also keeps out of the GSC PSMI area if the BIOS placed it inside the DSM.
  - The CPU reaches stolen memory directly through BAR2, not through the GGTT (`xe_ttm_stolen_cpu_access_needs_ggtt`
    is false from 12.70).
- **Users of stolen memory:**
  - the firmware's framebuffer (`display/xe_initial_plane.c:86-111`: an `XE_BO_FLAG_STOLEN` BO at the PTE's address;
    the PTE carries the DM bit, because 12.70+ has an LMEMBAR);
  - FBC (`display/xe_stolen.c`);
  - the GSC's 4 MB private BO (`xe_gsc.c:473-475`).
  On LNL with fbdev emulation the firmware framebuffer is discarded (`xe_display_bo.c:134`, device WA
  `22019338487_display`).
- **GGTT.** 8 MB of GSM at BAR0 + 8 MB, so a 4 GiB GGTT (`xe_ggtt.c:401-423`), the same as BMG. `xelpg_pt_wa_ops`
  applies on LNL's media GT: a GMD_ID dummy write every 63 GGTT writes (Wa_22019338487, `xe_ggtt.c:186-213, 430-434`).
- **Flat CCS in system memory.**
  - `probe_has_flat_ccs` reads `XE2_FLAT_CCS_ENABLE`, because the BIOS may have turned flat CCS off
    (`xe_device.c:765-790`).
  - `xe_bo_needs_ccs_pages` (`xe_bo.c:4048-4080`) appends size/512 bytes of CCS pages to a compressible TT BO.
  - The CCS is cleared and copied by the migrate engine: `handle_system_ccs` (`xe_bo.c:1087`),
    `xe_migrate_needs_ccs_emit` (`xe_migrate.c:590`), `MAX_CCS_LIMITED_TRANSFER` (:871).
  - On suspend, TT BOs are evicted because "CCS state lives inside graphics stolen memory" (`xe_bo_evict.c:125-134`).
- **Migrate.** No VRAM identity map (`xe_migrate.c:542`). The batch is mapped through the migrate VM's PT slots
  (:465-475), and copies go through per-chunk PTEs.
- **Discrete-only steps that disappear:**
  - `xe_pcode_ready`, the DGFX_PCODE_STATUS wait (`xe_pcode.c:305`);
  - `assert_lmem_ready` (`xe_device.c:654-661`);
  - the VF_CAP_REG write in `xe_device_wmb` (:1155-1160);
  - `xe_device_td_flush` (:1267). On LNL the L3 is flushed at the end of each submission.
  - `hwmon` (`xe_hwmon.c:1568`);
  - the PCI barrier mmap (`xe_bo.c:3761`);
  - the 1 MB USM BB pool, which is 512K on LNL (`xe_gt.c:671`).

### What the kext does on BMG, call site by call site

| object | where (kernel/IntelXe) | BMG | LNL needs |
|---|---|---|---|
| user BOs | `xe_core.cpp` GEM_CREATE (`vramAllocBo`, spill to `allocSys`), moves and eviction (`boMove`, `evictWindow`, `vramBlocks*`) | VRAM buddies `gVis`/`gHi`, the BAR window, system spill | system memory only, mapped by `cpu_caching`; no window, no moves |
| CPU maps | `xeClientMemory` (:7520) | VRAM through BAR2 (WC); system BOs always WB | WC system BOs mapped WC (xemac maps WC only for bit 63 today) |
| page tables | `ptPageAlloc` (:2129), written through `vram64` | 64 KB VRAM slabs in the window | system pages, CPU-WC (or clflushed), DMA addresses |
| PTE bits | `nullLeaf` (:2198) sets bit 11 (DM) always; leaves set DM for `bo->vram` | VRAM null/scratch | no DM anywhere (`xe_pt.c:78` passes IS_DGFX) |
| LRC | `lrcAlloc` (:2732) | 64 KB VRAM block (`debug.intelxe.lrc_vram`) | system memory (the path exists); now chosen by `xeMemKernelRegion` |
| GuC UM queues | `IntelXe.cpp` ADS, `gUmDpa` | top of VRAM | system memory, one contiguous power-of-two block; `gUsm` is off on integrated today |
| GGTT | `ggttMapVram` (DM bit), `ggttMap` (DMA addresses) | both | `ggttMap` only; Wa_22019338487 flush every 63 writes |
| migrate / clear | `migrateInit` (:4832, identity map of the tile), `vramClear`, `vramClearInit` | VRAM identity at 1 GB pages, compressed identity at PAT 12 | system-page windows; CCS clear and copy for compressed system BOs |
| display buffers | `xeDispVramAlloc` (:8167), scan-out pin, `SURFACE_VRAM` (:9041) | contiguous VRAM in the window, DPA runs | system pages with DMA runs; the firmware framebuffer in stolen |
| suspend / resume | `pmAdd`/`pmCopyAll`, LRC save, ReBAR save/restore, DPA 0-32 MB clear | VRAM saved to system memory | none of it; GGTT replay only |
| bring-up | `bringUp` maps BAR2 as VRAM, `vramUsableEnd` reads the tile and flat-CCS registers, applies Wa_16023588340 | 20.01 | refused today (below); later: the stolen manager instead |
| queries | QUERY_MEM_REGIONS, QUERY_GT_LIST, QUERY_CONFIG | sys + VRAM, near VRAM | sys only, near sys (done, below) |
| sensors | `sensorsString` (`IntelXe.cpp:308`) reads `BMG_PACKAGE/VRAM_TEMPERATURE` unconditionally, `publishVram`, `waitDeviceReady` / `pcodeInitStatusLocked` | BMG registers | none (Linux has no hwmon on LNL); skip the pcode wait |

In `xe_core.cpp` about 300 code lines touch VRAM. By group: BO allocation, moves and maps 129; display and surfaces
84; queries 26; page tables 24; suspend 16; migrate 10; bring-up and UM 12; LRC 9. `IntelXe.cpp` has about 80 more:
sensors and pcode, ReBAR, debug steps, GGTT.

### The first memory-region interface (done)

`kernel/IntelXe/xe_memregion.h` is header only and host-tested:

- `XeMemRegion` (system, VRAM0, stolen): xe's `XE_PL_TT`/`VRAM0`/`STOLEN`.
- `xeMemUserRegion`: the region a user BO goes to first, from its placement bits.
- `xeMemQueryRegions`: the number of uAPI regions.
- `xeMemKernelRegion`: the region Linux gives each kernel object this kext keeps in VRAM.
- `xeStolenDecodeIgfx`: `detect_lmembar_integrated` on register values.

The kext asks these where it decides:

- GEM_CREATE's checks moved to `xeGemCreateCheck` (`xe_uapi_args.cpp`), with the device's region mask and graphics
  version. The VRAM branch is taken by `xeMemUserRegion`.
- QUERY_MEM_REGIONS lists VRAM only where it exists. This matters: ANV decides it is discrete from the existence of a
  VRAM region of any size (`anv_physical_device_has_vram`), not from HAS_VRAM.
- QUERY_GT_LIST near/far come from the model.
- The LRC goes to VRAM only where Linux puts it there.
- `bringUp` and the `vramTest` step refuse a platform whose BAR2 is not VRAM. On LNL, BAR2 starts with the GGTT itself.

On BMG nothing changes:

- `tests/uapi_test.cpp` runs the new check and the inline one it replaced side by side over 4480 argument
  combinations, and all answers are equal;
- the query answer keeps its size (static_assert);
- the build has the same 22 + 24 warnings.

Still VRAM-only, all behind the bring-up refusal: `ptPageAlloc`, `xeDispVramAlloc` and the scan-out pin, the UM
queues, `migrateInit`/`vramClear`, the PM save, and `ggttMapVram`. Each has an `XeKernelObject` that names its Linux
placement, so the next step is mechanical: a system-memory backend behind the same question.

## 3. CPU coherency and the PAT

- **No shared LLC.** `lnl_desc` has no `has_llc`, so the ROADMAP's "shared LLC" was wrong (now corrected). The CPU and
  GPU are coherent only through the PAT index's coherency mode. `xe2_pat_table` has `coh_mode` 0 (no snoop), 2
  (1-way) and 3 (2-way), and `xe_pat.c` documents the fields at :124-153. The kext's `kPatCoherent` mask
  (`xe_core.cpp:6572`) is already this table's.
- **On a dGPU** system memory is always snooped (`xe_bo.c:600-604`). On LNL, a WB BO bound with a non-coherent PAT
  index reads stale data. Linux refuses a userptr or CPU-mirror bind with `coh_mode == NONE` (`xe_vm.c:3835-3838`). ANV
  picks `cached_coherent` (2-way) for WB system memory (`anv_device.c:2560-2594`).
- **The kext.**
  - `patEntries` is already 32 from the descriptor.
  - The PTA entry is programmed on discrete parts only (`xePlatformPatPta`).
  - `xe2_pat_ats` is programmed on both.
  - WB system BOs need the CPU mapping cached and coherent PAT indices; WC BOs need a WC CPU mapping (`xeClientMemory`)
    and no clflush dependence.
  - Page tables written by the CPU must be WC or flushed (`xePlatformPtWriteCombined`).
- **Compute MOCS.** On a DGFX Xe2 part, compute engines read through MOCS `wb_index` (`xe_hw_engine.c:405`). On LNL
  they use `uc_index` for both read and write.
- **Flushes.**
  - Wa_16023588340 (the L2 flush and CLOS3, 20.01) does not apply. `bringUp` applies it unconditionally today
    (`xe_core.cpp:1436`), so it must become conditional.
  - The L3 is flushed at the end of each submission: the "L3 RW flush all cache" tunings (`xe_tuning.c:88-92`, LNL
    only).

Size: about 150 lines (mapping modes, PAT conditions, Wa_16023588340 gate).

## 4. MMIO and BAR layout

| | BMG | LNL |
|---|---|---|
| BAR0 | 16 MB: registers 0-4 MB, GSM 8-16 MB (`xe_mmio.c:93-125`) | the same |
| BAR2 | VRAM (resizable; 256 MB-16 GB) | LMEMBAR: GSM (8 MB), then the DSM; not resizable here; no ReBAR save |
| media GT | +0x380000, 0x40000 long | the same |
| reads | plain | Wa_15015404425: four dummy writes to 0x130030 before every MMIO read (`xe_mmio.c:149-160`) |
| probe-time waits | DGFX_PCODE_STATUS, GU_CNTL.LMEM_INIT | none |

Wa_15015404425 is a cost in the kext's hot `rd()`: about 2x MMIO traffic on reads. Linux pays it as well. Size: about
100 lines, mostly gates (`xePlatformWmbMmio`, `xePlatformLmemInitWait`, ReBAR and sensors off).

## 5. Firmware

- **GuC.**
  - `xe/lnl_guc_70.bin`, at least 70.53 on master (70.45.2 on v6.18). Only the major has to match
    (`xe_uc_fw.c:370-414`).
  - The same ABI as BMG's 70.x, so the kext's GuC code carries over.
  - The built-in image is BMG's: `build.py` would need a second image and `IntelXe::start`'s firmware check per
    platform.
  - Differences in the GuC objects:
    - ADS doorbell count from `DIST_DBS_POPULATED` (`xe_guc_ads.c:968-981`, integrated only);
    - UM queues in contiguous system memory;
    - WA KLVs: 13011645652 is LNL only; 18024947630 and 14025515070 apply to both GTs on LNL (BMG: primary only, media
      only); 14020001231 and 16023683509 only with PSMI (`xe_guc_ads.c guc_waklv_init`, :319-410);
    - Wa_14025883347's reset preamble also applies to the primary GT (`xe_guc.c:995`).
- **HuC.** It is loaded by DMA and authenticated through the GuC as on BMG (`xe_uc.c:193, 224`), on the media GT. On
  LNL a second authentication follows after the GSC firmware is up (`xe_gsc.c:297-299`): a PXP 4.3 `NEW_HUC_AUTH`
  packet over GSCCS, then `HECI_FWSTS5.HUC_AUTH_DONE` (`xe_huc.c:156-228`). It needs `has_gsc_headers` (a CPD-layout
  HuC). BMG's HuC is CPD-layout too, and its GSC packet allocation is the same code.
- **GSC** (`xe_gsc.c`), media GT only:
  - It needs the GSCCS engine (`XE_FW_GSC` forcewake 0xa618/0xdf8), a 4 MB stolen BO, the GSC_FW_LOAD command, a
    500 ms wait on `HECI_FWSTS1.INIT_COMPLETE`, and the MKHI compatibility query (`xe_gsc.c:127-260`).
  - Wa_14018094691 takes forcewake on the primary GT around the load.
  - The GSC proxy (`xe_gsc_proxy.c`) relays GSC↔CSME messages through the Linux MEI driver. macOS has none. Linux's
    order (load, HuC GSC auth, then proxy) means HuC authentication does not need the proxy; PXP and HDCP 2.x do.
  - The GSC survives GT reset and D3hot. Unloading needs a driver FLR, or the GSC keeps using its stolen BO
    (`xe_gsc.c:222-237`).
  - Media C6 needs it (`xe_gsc.c:416-421`).
- **DMC.** `i915/xe2lpd_dmc.bin` for display 20.00. IntelXeFB loads no DMC today (BMG runs without DC states). On a
  laptop, DC states and PSR depend on it.

Size: GSC + HuC GSC authentication about 600-800; a second GuC image, per-platform KLVs and ADS about 150; the DMC
loader belongs to the display (section 7).

## 6. Forcewake, power, workarounds

- **Forcewake.** The domains are the same as BMG's (`FORCEWAKE_GT` with `FORCEWAKE_ACK_GT_MTL`, render, VDBOX/VEBOX),
  plus GSC on LNL (`xe_force_wake.c:45-94`).
- **Power gating.**
  - `xe_gt_idle_enable_pg` has nothing LNL-specific. Wa_14020316580 (VD0/VD2 power gating off) is BMG-media only.
  - C6 goes through GuC RC on both.
  - Runtime PM: D3cold needs a root port with `_PR3`, which an integrated function lacks (my inference from the PCI
    core, which is not in `dl/`).
- **SLPC** (`xe_guc_pc.c`).
  - `BMG_MIN_FREQ` 1200 MHz never applies.
  - Under Wa_22019338487 the media GT is capped at `LNL_MERT_FREQ_CAP` (800 MHz) until `xe_gt_sanitize_freq` restores
    it after the GSC load (:57, :854-866, :1071-1100). The kext's media SLPC code must take that.
- **pcode.** The mailbox works as on BMG. `xe_pcode_init_min_freq_table` needs `has_llc`, so it is skipped on both.
- **Workarounds.** Most of the Xe2 rules are written `GRAPHICS_VERSION_RANGE(2001, 2004)`, so the kext's 20.01 set
  carries over. LNL-only work:

  | kind | items |
  |---|---|
  | `xe_wa.c` | 14017421178 (media 20.00, VDBOX_CGCTL3F10), 16018610683 (20.04, TDL_TSL_CHICKEN SLM_WMTP_RESTORE); 14020756599 and 14021490052 have 20.04 entries identical to BMG's |
  | `xe_tuning.c` | the media L3/CCS tunings at `XE2LPM_*` (0xb658, 0x82a8, 0xb608, 0xb604), L3 RW flush all (0xb154, 0xb654) |
  | `xe_wa_oob.rules` | 13011645652 (GuC KLV), 16023683509 (PSMI only); 14019821291 (stolen, integrated); 14018094691 (GSC load); 22019338487 now on the media GT |
  | device | 15015404425 (MMIO flush), 22019338487_display |
  | BMG-only, to drop | 16023588340 (PAT 28, L2/CLOS3), 14020316580, 14022085890, 16025250150, 14019811474, 14021821874/14022954250, 22021007897, the "VS hit max" tuning |

Size: about 300 lines, mostly tables.

## 7. Display: xe2lpd (LNL) against xe2hpd (BMG)

IntelXeFB (`kernel/IntelXeFB`) is written for display 14.01. The differences, in outline:

- **Ports and PHYs.** BMG has DDI A + TC1-4, all C20 (IntelXeLink.cpp:30-32, :1049), and no eDP transcoder. LNL has
  C10 on A/B for the eDP panel, Type-C ports through the PICA/FIA, and xe2lpd AUX and HPD differences.
- **The boot framebuffer** lives in stolen memory. Its GGTT PTE has the DM bit and holds the offset from GSMBASE.
  `adoptFirmwareMode` (IntelXeFB.cpp:3508-3545) assumes VRAM inside BAR2 (`fSurfPhys = barBase + fDpa`). On LNL the
  CPU address is BAR2 + 8 MB + offset into the DSM, and the window checks change. The fixed cursor at DPA 0x1F00000
  and `intelxeDispVramAlloc` must become system pages with DMA runs.
- **Clocks and bandwidth.** CDCLK uses the xe2hpd table, fixed at 652800 (IntelXeDisplayPlan.h:208). Bandwidth comes
  from GDDR QGV (`xe2_hpd_get_bw_info`). LNL needs the xe2lpd CDCLK table and the LPDDR5X QGV points.
- **Panel features.** There is no DMC, no DC states, no PSR, no panel power sequencing and no backlight. A laptop panel
  needs all of them.
- **Flushes.** The BAR2 L2 flush (`intelxeL2Flush`, Wa_16023588340) is BMG only. On LNL, CPU-written surfaces need WC
  or clflush.

Size: 3000-5000 lines. The display is the largest piece and can be developed last. Until then, the firmware's panel
mode can be kept, as IntelXeFB did on BMG at first: adopt the GOP framebuffer from stolen memory and do not modeset.

## 8. Userland

- **ANV** (Mesa, upstream behaviour kept by the fork) is right for LNL when the kernel reports correctly:
  - `XE2_CONFIG(LNL)` has `has_local_mem=false` and `has_llc=false` (`intel_device_info.c:1216-1219`, :1047);
  - LNL ids are in `iris_pci_ids.h:260-262`;
  - heaps: one DEVICE_LOCAL system heap (`anv_physical_device.c:2642-2648`);
  - memory types (`xe/anv_device.c:156-175`): compressed (ver 20), WC, and WB coherent;
  - `deviceType` INTEGRATED;
  - GEM_CREATE placement from `region->instance`, `cpu_caching` from the PAT.

  It decides "discrete" from the existence of a VRAM region, which is why QUERY_MEM_REGIONS changed (section 2). The
  fork's macOS changes (budget, video memory, query pools) are harmless on an integrated part. `SURFACE_VRAM` is gated
  by the kernel's cap.
- **xemac** (`user/xemac`): `mmap` maps WC only for offset bit 63 (the BAR2 marker). LNL's WC system BOs need a marker
  too. The fake device is BMG only (testing aid).
- **Metal plugin** (`nullmoth/plugin`; another branch owns it, so it is only listed here):
  - `nvmtl_vk_sysmem_type` looks for a type without DEVICE_LOCAL, and on an iGPU there is none. Every spill and
    eviction path then fails (`nvmtl_vk.c` :1698-1735, :2490-2502, :8964-9105, :11216-11225).
  - The VRAM wall, budget and headroom treat RAM as the card.
  - `memtype()` picks WC for Shared memory where WB coherent is right.
  - Managed is a VRAM copy plus a system shadow; it should be one copy.
  - `hasUnifiedMemory` is hard-coded NO (`NVMTLDevice.m:2745`), and `dedicatedMemorySize` = the largest DEVICE_LOCAL
    heap.

  What it needs: an "integrated" switch from `VkPhysicalDeviceProperties.deviceType` that bypasses the wall, spill and
  residency eviction, picks HOST_CACHED for Shared and Managed, keeps one copy for Managed, reports unified memory, and
  skips surface VRAM. About 300-500 lines.

## 9. Order of work, with milestones a tester can check

Each milestone ends with something a tester with an LNL laptop can run and send back. XeProbe stages 1-5 come first:
they need no IntelXe start. The boot-arg and the stage outputs are in [PROBE-MODE.md](../PROBE-MODE.md), section
"Lunar Lake".

| # | milestone | built here | the tester checks |
|---|---|---|---|
| M0 | **done (this branch)**: descriptor = `lnl_desc`, GSC names, capability set, `xe_memregion.h`, queries without VRAM, bring-up refusal; XeProbe stage 1 decodes the LMEMBAR stolen memory, uses the 4 GiB GGTT, applies Wa_15015404425 and Wa_22019338487 | host tests | XeProbe stages 1-2: GMD_ID 20.04 / 20.00 / display 20.00, stolen size, fuses, engines |
| M1 | XeProbe stages 3-5 on LNL: build with `xe/lnl_guc_70.bin` from linux-firmware in the `--firmware` directory. **Code done (13 Nov, branch `xeprobe`)**: `firmware/fetch.sh --probe lnl` fetches it, pinned by sha256 to linux-firmware 20261019 (GuC 70.72.1) in `firmware/probe-guc.sha256`, with the other families' images; `debug.xeprobe.info` says whether it is built in | build only | GuC READY, a store per primary-GT engine, readback from system memory |
| M2 | system-memory backends for the kernel objects: page tables (CPU-WC), LRC (exists), GuC UM queues (contiguous), no DM bits; GGTT via `ggttMap` only; a stolen range manager over `xeStolenDecodeIgfx` | host tests for the range manager and PTE encoding; BMG unchanged | `-intelxe_allow_lnl` (a tester boot-arg, as for G31): IntelXe starts, GuC up, step tests (`debug.intelxe.step`) without display |
| M3 | user BOs in system memory: GEM_CREATE without VRAM, maps by `cpu_caching`, no window, eviction or moves; the migrate engine on system windows; LNL PAT/WA/tuning tables; Wa_16023588340 off; sensors/pcode/ReBAR gated | uapi and PAT host tests | `vulkaninfo`, a compute test (`tools/clbench`), then vkcube offscreen |
| M4 | flat CCS in system memory: CCS pages, clear on create, copy on suspend; `probe_has_flat_ccs` | | compressed render targets, suspend/resume |
| M5 | media GT: GSC firmware load over GSCCS, HuC GSC authentication, media SLPC cap until then, media C6 | | VideoToolbox decode, media C6 residency |
| M6 | display xe2lpd: adopt the stolen boot framebuffer (no modeset), then the DMC, the eDP panel (C10, panel power, backlight, PSR), Type-C | | the internal panel through IntelXeFB, then external monitors |
| M7 | plugin integrated switch (nullmoth branch) | | Metal apps, `hasUnifiedMemory` |

M2 needs a tester boot-arg like BMG-G31's (`testerBootArg` in the descriptor; build.py would add a tester personality
for the LNL ids). It is not added on this branch, because without M2's backends there is nothing safe to start.

## 10. What was checked, and how

- `kernel/IntelXe/tests/platform_test.cpp`, 1874 checks:
  - LNL = `lnl_desc` field by field, the GSC names for every entry;
  - the memory model of all 18 entries;
  - regions and kernel objects;
  - `detect_lmembar_integrated` on register vectors (GGMS, GMS ranges, WOPCM sizes, the PSMI carve-out, BAR2 fit).
- `kernel/IntelXe/tests/uapi_test.cpp`, 3916 checks: `xeGemCreateCheck` against the replaced inline code, and the
  integrated answers.
- Every kernel host test passes.
- The kext builds with the same 22 (IntelXe) + 24 (IntelXeFB) warnings.
- `tools/parity/devices.py` reports 0 problems.
