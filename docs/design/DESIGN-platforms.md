# Platform descriptors and the road to other Intel GPU families

> Design note with its date (2026-11-10, branch `platform-layer`): it records the plan and the state of that day, not
> the current status. The current state is in the README and `docs/devices/b580.md`.

IntelXe.kext ports Linux `drivers/gpu/drm/xe` and drives one device family: Battlemage BMG-G21 (Arc B580 `8086:e20b`,
B570 `8086:e20c`). This note covers three things:

- the platform descriptor layer that now holds what is platform-specific;
- what still assumes BMG, Xe2 or a discrete card, and why it stays that way for now;
- what each other family still needs.

Linux references are to `dl/linux-xe` (torvalds/master as fetched for this repo). The DMC file names come from
`drivers/gpu/drm/i915/display/intel_dmc.c` on torvalds/master as read on 2026-11-10, because that file is not in `dl/`.

## 1. The descriptor layer

`kernel/IntelXe/xe_platform.h` and `xe_platform.cpp` model Linux `xe_pci.c`.

- There is one `XePlatformDesc` per device-id list in `xe_probe_ids.inc`, which mirrors pciids.h.
- Each entry is built like `xe_pci.c` builds its entries: a device desc per platform (`tgl_desc` … `ptl_desc`) folded
  with its graphics IP desc (`graphics_xelp`, `xehpg`, `xelpg`, `xe2`) and its media IP desc (`media_xem`, `xelpmp`).
- The table is `constexpr`, so the kext gets no static constructor.

| field | Linux source |
|---|---|
| `name`, `subplatform`, `platform`, `ids` | XeProbe family name; `xe_subplatform_desc`; `enum xe_platform`; pciids.h |
| `preGmdidGraphics/Media` | `pre_gmdid_graphics_ip` / `pre_gmdid_media_ip` (0 when the IP is read from GMD_ID) |
| `graphicsVerx100/mediaVerx100` | the pre-GMD_ID IP, or `tests/xe_pci.c` GMDID_CASE (BMG 20.01 / 13.01); 0 = unknown |
| `dmaMaskBits`, `vaBits`, `vmMaxLevel`, `maxGtPerTile` | `dma_mask_size`, `va_bits`, `vm_max_level`, `max_gt_per_tile` |
| `isDgfx`, `hasFlatCcs`, `hasLlc`, `hasDisplay`, `hasSriov`, `hasPxp`, … | `xe_device_desc` has_* flags, `needs_scratch`, `XE_VRAM_FLAGS_NEED64K` |
| `hasUsm`, `hasAsid`, `hasAccessCounter`, `hasRangeTlbInval`, … | `xe_graphics_desc` has_* flags of the graphics IP |
| `engineMask` | graphics `hw_engine_mask` together with media `hw_engine_mask` (`XE_HW_ENGINE_*` bits) |
| `wopcmBytes` | `xe_wopcm.c` `xe_wopcm_size` |
| `patEntries` | `xe_pat.c` `xe_pat_init_early` n_entries (BMG: 28, Wa_16023588340) |
| `mocsUcIndex` | `xe_mocs.c` `get_mocs_settings` uc_index |
| `gucFw`/`gucStem`/`gucMajor`/`gucMinor`, `hucFw`, `gscFw`, `dmcFw` | `XE_GUC/HUC/GSC_FIRMWARE_DEFS`; i915 `dmc_firmware_default` |
| `supported`, `missing` | this driver: whether it starts on the entry, and if not, what the family still lacks |

**Lookup.** `IntelXe::start` looks the descriptor up once, as `xe_pci_probe` does.

- The device id comes from config space. A card that does not answer yet (rb7u) is looked up by the PCI family's
  `device-id` property instead.
- Start stops with one log line in each of these cases: the id has no entry, the entry is not supported, or the
  entry names GuC/HuC files other than the built-in ones. `build.py` now emits `kGucFwName` and `kHucFwName` for the
  last check.
- On success `gXePlat` and `gXePlatId` are set, and start never clears them.
- `IOPCIMatch` is unchanged (`0xe20b8086 0xe20c8086`), so today only those two ids reach `start`. (Since 2026-11-13
  also `e211` and `e212`, and BMG-G31 through a personality of its own: section 3, "Pro B50/B60 enabled".)

**XeProbe.** XeProbe (`docs/PROBE-MODE.md`) reads the same table, through `xePlatformFind` and `xePlatformByName`. Its
private family table is gone.

### What reads the descriptor now

| value | where |
|---|---|
| `dmaMaskBits` | `DmaMap::map` (IODMACommand address bits), the `ggttMap` device-address limit, the `alloc` physical mask |
| `vaBits` | `QUERY_CONFIG_VA_BITS`, the `bindOpCheck` VM-size check, `xeScanoutPin` and `vmMadvise` uncanonicalize |
| `isDgfx` | `QUERY_CONFIG_FLAG_HAS_VRAM`; together with `hasUsm`, `gUsm` (the UM queues live in VRAM) |
| `graphics/mediaVerx100` | `QUERY_GT_LIST` `ip_ver_major/minor` of both GTs |
| `maxGtPerTile` | `gVideoWanted` (whether a standalone media GT exists) |
| `wopcmBytes` | `loadGuc` WOPCM layout |
| `patEntries` | `patCheck` (VM_BIND) and `vmMadvise` PAT limit |
| `mocsUcIndex` | GuC shim `GUC_MOCS_INDEX`, MEM_SET, MEM_COPY, XY_BLOCK_COPY MOCS fields |
| `engineMask` | the CCS fuse (`read_compute_fuses_from_reg`) and the VDBOX/VEBOX fuses (`read_media_fuses`) |
| `gucFw`, `hucFw` | the start check against the built-in images |
| `hasMbxPowerLimits` | the power limits in `debug.intelxe.sensors`, read through the pcode mailbox (`xe_pcode.h`; since 2026-11-10, branch `power-limits`) |

### Equivalence on BMG (how it was checked)

- `tests/platform_test.cpp` (host, no GPU) checks four things:
  - every id has exactly one entry;
  - only BMG-G21 is supported, and e20b/e20c are in that entry;
  - the BMG-G21 values equal the literals the code used before (each check names the old file:line);
  - XeProbe's old family table, copied into the test, gives the same values per id and per name.

  Result: 1437 checks, 0 failed.
- An object-code check (`~/hackintosh/work/plat/tools/fold.py`, not in the repo) builds `IntelXe.cpp` and `xe_core.cpp`
  with `gXePlat` folded to a constant that holds the BMG-G21 values, then compares each function's disassembly with
  the build before the change. Data references are compared by the string they load.
  - `xe_core.o`: all 220 functions are identical.
  - `IntelXe.o`: the only differences are `start` and `waitDeviceReady` (both intended) and an out-of-line copy of
    the unchanged `intelxeBootArg`.
- The build adds no compiler warnings: the same 22 as before, by file and message.
- The output of `tools/parity/uapi.py` is the same: the same 182 cells with the same statuses (only line numbers
  differ).

Intended differences on BMG:

- One added log line at start:

  ```
  IntelXe: 8086:e20b: BMG-G21, graphics 20.01, media 13.01, discrete
  ```
- `waitDeviceReady` waits for the id that start found rather than for e20b-or-e20c.

## 2. What still assumes BMG / Xe2 / discrete (left on purpose)

All of this sits behind `supported`, which only BMG-G21 has. Each item would need per-platform data that Linux holds in
a table or a version check. Guessing at those values would be worse than leaving the code explicit.

- **GT layout.**
  - `MEDIA_GT_GSI_OFFSET` (0x380000) is a single Linux constant for every standalone media GT; only *whether* one
    exists is per platform (`maxGtPerTile`).
  - Media register bases, the media GuC objects (`mAdsG` …) and the step 13–16 paths assume media 13.01.
- **Engines.**
  - Fixed RCS0/BCS0 bases and LRC sizes in `firstBatchSetupOnly` (Xe2 `xe_gt_lrc_size`: render 16 KB, compute 12 KB,
    copy 8 KB); `goldenRcsOff` and the other offsets in `IntelXe.h` follow from those sizes.
  - The VCS/VECS bases and the at-most-4-media-engines check.
  - BCS8 (in the mask) is not driven, as before.
- **LRC images.** `lrc.h` has only `xe2_rcs/xcs/bcs_offsets`, and only Xe2 has no indirect ring state. XeProbe carries
  gen12/dg2/mtl tables in `xe_probe_lrc.inc`; IntelXe does not use them yet.
- **PAT / MOCS tables.**
  - `programPatMocs` writes `xe2_pat_table` (28 entries), `xe2_pat_ats` and `xe2_pat_pta` (dGFX), plus `xe2_mocs_table`.
  - `kPatComp`, `kPatReserved` and `kPatCoherent` are masks of `xe2_pat_table`.
  - GGTT PTEs use `xelpg_ggtt_pte_flags` (PAT bits 52/53, graphics ≥ 12.70); PPGTT PTEs use the Xe2 PAT encoding.
- **Memory.**
  - BAR2 is mapped unconditionally, and `vramUsableEnd` reads `SG_TILE_ADDR_RANGE0` and the Xe2 flat-CCS base
    (`XE2_FLAT_CCS_BASE_RANGE_*`).
  - The DSM/GSM bases are VRAM addresses.
  - The UM queues are in VRAM.
  - `deviceWmb` is VF_CAP_REG.
  - Every BO is a VRAM run or system memory.
  - **There is no integrated memory path:** no stolen memory, no LMEM-less placement, and no flat CCS in system
    memory (LNL/PTL).
- **GGTT.** GSM is at BAR0 + 8 MB, and a 4 GiB GGTT is implied (Linux: `gsm_size = 8M` for graphics ≥ 12.55, otherwise
  `GMCH_CTRL GGMS`). The layout constants in `IntelXe.h` and `xe_vram.h` sit inside 4 GiB.
- **Forcewake.** The GT ack is `FORCEWAKE_ACK_GT_MTL`, which Linux uses for graphics ≥ 12.70 (otherwise
  `FORCEWAKE_ACK_GT`). The render domain is taken on the primary GT, and the media VDBOX/VEBOX domains in `mediaWake`.
- **Interrupts.** `irqInit`/`irqHandle` follow `dg1_irq_handler` (Linux: graphics ≥ 12.10; `xelp_irq_handler` below).
  The display interrupt goes to IntelXeFB.
- **USM.** The GuC UM queues (`guc_um_init_params`) are set up on the primary GT only. Linux also sets them on the
  media GT.
- **SLPC.**
  - The minimum frequency is 1200 MHz: `BMG_MIN_FREQ`, Wa_14022085890, which Linux applies to BMG-G21 only
    (`xe_device_wa_oob.rules`). The value sits in `gSlpcMin`, `slpcParams` and `mediaPcStart`. (Since 2026-11-13 the
    media GT's minimum follows the descriptor's `wa14022085890`, as Linux does: section 3.)
  - The NVRAM limits of IntelXe Manager are checked against 3000 MHz.
- **Workarounds.**
  - `firstBatchSetupOnly` applies the `gt_was`/`engine_was`/tunings that match graphics 20.01.
  - `loadGuc` sends the GuC WA KLVs for 20.01 and for media 13.01.
  - Also: Wa_16023588340 (L2 / CLOS3 / `l2Flush` XE2_GLOBAL_INVAL), Wa_16023105232 (PSMI), Wa_14021490052 (FF_MODE),
    Wa_14025883347 (media GuC reset), and `adjust_idledly`.
  - Every other family needs its own rule set from `xe_wa.c` / `xe_wa_oob.rules` / `xe_tuning.c`.
- **CCS mode.** `RCU_MODE` CCS enable, `CCS_MODE` in the engine-reset regset when there is more than one CCS, and all
  cslices on CCS0 (Xe_HPG and later, discrete).
- **Firmware.**
  - The GuC (70.72.1) and HuC (8.2.10, GSC CPD layout, authenticated through the GuC) are built in for BMG.
  - `QUERY_UC_FW_VERSION` reports the GuC submission version as 1.38.1.
  - No DMC is loaded: IntelXeFB keeps the pipe the GOP lit.
- **pcode, sensors, ReBAR.** `DGFX_PCODE_STATUS` waits at start, the BMG temperature/fan registers
  (`sensorsString`, step 1), Resizable BAR save/restore and `publishVram` (`VRAM,totalsize`) are all dGFX/BMG
  registers.
- **Names and matching.**
  - The `model` property is "B570" for e20c and "B580" otherwise. This needs a name per id before a second card.
    (Since 2026-11-13 a table per id in `IntelXe.cpp`: B580, B570, Pro B60, Pro B50, Pro B70.)
  - The ids are in `build.py` IOPCIMatch.
- **IP version checks.** At runtime there are none: GMD_ID is only printed (step 1, step 13, `mediaGucLoad`). A GMD_ID
  check against `graphicsVerx100` at bring-up would be the natural next step. It was not added, because a read of GMD_ID
  at that moment has not been proved stable on the B580 (rb7u reads 0xffffffff early).
- **Display.** IntelXeFB (not part of this change) programs display 14.01 (BMG) registers.

## 3. What each family still needs

The table entries are complete for these families: IP versions, flags, engine mask, firmware names. What follows is
the code that does not exist yet. "Integrated memory path" means the following:

- BOs in system memory only (no VRAM, no BAR2);
- stolen memory (DSM) from `BDSM`, and GGTT size from `GMCH_CTRL`;
- coherent PAT indices for CPU-cached memory (`has_llc` on Gen12);
- on LNL/PTL, flat CCS backed by system memory;
- no `DGFX_PCODE_STATUS` wait, no ReBAR;
- `QUERY_MEM_REGIONS` without a VRAM region.

| family (ids) | IP (gfx / media) | firmware | display (DMC) | memory | other work |
|---|---|---|---|---|---|
| TGL, RKL, ADL-S/RPL-S, ADL-P/RPL-P/RPL-U, ADL-N | 12.00 / 12.00, no media GT | `i915/tgl_guc_70.bin` (ADL-P/RPL: `adlp_guc_70.bin`) 70.44; HuC `i915/tgl_huc.bin` (none on ADL-N), legacy (non-GSC) layout | 12 / 13: `tgl_dmc_ver2_12`, `rkl_dmc_ver2_03`, `adls_dmc_ver2_01`, `adlp_dmc` | integrated path; dma 39 bits; `has_llc` | Gen12 LRC images and register lists; xelp PAT (4) / gen12 MOCS (uc 3); `FORCEWAKE_ACK_GT`; `xelp_irq_handler`; xelp GGTT PTE; 3-level PPGTT (`vm_max_level` 3); no CCS; Gen12 workarounds |
| DG1 | 12.10 / 12.00 | `i915/dg1_guc_70.bin` 70.44, `i915/dg1_huc.bin` | 12: `dg1_dmc_ver2_02` | the VRAM path, without flat CCS (`vramUsableEnd` reads Xe2 flat-CCS registers); dma 39 | Gen12 LRC; xelp PAT / dg1 MOCS (uc 1); `dg1_irq_handler` fits; 3-level PPGTT; no CCS |
| DG2 (A770/A750/A580/A380/A310…), ATS-M | 12.55 / 12.55, no media GT | `i915/dg2_guc_70.bin` 70.53; no HuC in xe | 13: `dg2_dmc_ver2_08` (ATS-M: no display) | the VRAM path with 64K VRAM pages (`NEED64K`, GGTT 64K flag); flat CCS through the pre-Xe2 base registers | XeHP LRC images (`xp_dg2_*` in `xe_probe_lrc.inc`); xelp PAT (4) with xehp ops / dg2 MOCS (uc 1); `FORCEWAKE_ACK_GT`; xelp GGTT PTE; 3-level PPGTT; DG2 workarounds per G10/G11/G12 and stepping (needs the subplatform split of `kIdsDG2`); media on the primary GT |
| MTL, ARL | 12.70–12.74 / 13.00 (media GT) | `i915/mtl_guc_70.bin` 70.53; HuC `i915/mtl_huc_gsc.bin` through the GSC (`i915/mtl_gsc_102.bin`) | 14.00: `mtl_dmc` | integrated path; dma 46 | xelpg PAT (5) / MTL MOCS (uc 9); MTL LRC; GSC proxy; one CCS; 3-level PPGTT; MTL workarounds |
| LNL | 20.04 / 20.00 (media GT) | `xe/lnl_guc_70.bin` 70.53, `xe/lnl_huc.bin`, `xe/lnl_gsc_104.bin` | 20.00: `xe2lpd_dmc` | integrated path with flat CCS in system memory; `needs_scratch` | closest to BMG: Xe2 LRC, Xe2 PAT but 32 entries (no Wa_16023588340), the same MOCS; Xe2_LPG / Xe2_LPM workarounds; media 20.00 KLVs; GSC |
| BMG-G31 (e220–e223) | 20.01 assumed (GMD_ID; possibly 20.02) | the same BMG firmware | 14.01: `bmg_dmc` | the same dGFX path | not tested; Wa_14022085890 is conditional since 2026-11-13; started only with `-intelxe_allow_g31` (section 3, "Pro B50/B60 enabled") |
| PTL, WCL | 30.00 (WCL: from GMD_ID) / 30.00 (media GT) | `xe/ptl_guc_70.bin` 70.54, `xe/ptl_huc.bin`, `xe/ptl_gsc_105.bin` | 30.00: `xe3lpd_dmc`; WCL 30.02: `xe3lpd_3002_dmc` | integrated path with flat CCS in system memory | Xe3 PAT (32, compressed WB 16) and LRC; Xe3 workarounds; GSC |

Other G21 cards (B50/B60 Pro: e211, e212, …) share the supported G21 entry. They need only an IOPCIMatch id and a
`model` name, and then a test on the card. (The Pro B60 and Pro B50 have both since 2026-11-13.)

### Order of work for a new family

1. Run XeProbe on it (`xeprobe=0xDEVICE`, `docs/PROBE-MODE.md`): identity, forcewake, GuC load, a store per engine.
2. Fill in what the entry is missing and mark it `supported`, then add the id to IOPCIMatch.
3. Replace each item of section 2 that the family hits with a descriptor field or a version check modelled on Linux.
   Examples: PAT and MOCS tables keyed by `platform`, LRC offsets by `graphicsVerx100`, the forcewake ack by
   `graphicsVerx100 >= 1270`, the IRQ handler by `>= 1210`, and an integrated memory path selected by `isDgfx`.
4. Bring its workaround rules over from `xe_wa_oob.rules` / `xe_wa.c`.
5. Extend `tests/platform_test.cpp` with that family's expected values.

### How to enable another BMG card

> Added 2026-11-10 (branch `power-limits`). No id was added to the supported list by this change.

**The ids today.** Linux `include/drm/intel/pciids.h:852-868` (tree abf8c33) lists `INTEL_BMG_G21_IDS` (e202, e209,
e20b, e20c, e20d, e210, e211, e212, e216) and, for the rest of `INTEL_BMG_IDS`, the G31 ids e220-e223.
`xe_pci.c:412-439` gives all of them `bmg_desc`; the G21 ones also get the G21 subplatform.
`kIdsBMGG21` / `kIdsBMGG31` (`xe_platform.cpp`) and `kIdsBMG` (`xe_probe_ids.inc`) hold exactly these.
`platform_test` section 5 pins them to the Linux lists.

Mesa's `include/pci_ids/iris_pci_ids.h:276-289` (the Mesa tree of 28 Oct) names e20b "Arc B580", e20c "Arc B570",
e211 "Arc Pro B60" and e212 "Arc Pro B50"; it names every other id "Intel(R) Graphics". It also has e215, which Linux
does not list, so the descriptor has no entry for it. It files e216 under "BMG" where Linux files it under G21.

**Where each card stands.**

| ids | descriptor | reaches `start`? |
|---|---|---|
| e20b (B580), e20c (B570) | G21, `supported` | yes: `build.py` IOPCIMatch, `data/platforms.json` "supported" |
| e211 (Pro B60), e212 (Pro B50), e202, e209, e20d, e210, e216 | G21, `supported` | no: not in IOPCIMatch, so IntelXe never matches them (XeProbe can) |
| e220-e223 (G31) | G31, not `supported` | no; if matched, `start` refuses with the entry's `missing` line |

**A G21 card (Pro B50/B60 and the others).** The code paths are the B580's. What can differ is the card, not the
chip. Check, in order:

1. **Identity.** Boot with `xeprobe=0x<id>` (`docs/PROBE-MODE.md`), stages 1 and 2. GMD_ID must read graphics 20.01
   and media 13.01 (`tests/xe_pci.c` GMDID_CASE(BATTLEMAGE, 2001, ..., 1301, ...)). Then run stages 3-5.
2. **What the code reads at run time but has only seen on the B580/B570:**
   - the VRAM size from `SG_TILE_ADDR_RANGE0` (the Pro B60 has 24 GB, the Pro B50 16 GB);
   - the Resizable BAR sizes the slot grants and the 1 GB CPU-visible window that follows from them;
   - the Xe-core and engine fuses (`XEHP_GT_COMPUTE_DSS_ENABLE`, the CCS and VDBOX/VEBOX fuses);
   - the outputs IntelXeFB finds (DisplayPort over the C20 PHYs; an HDMI connector has never been driven);
   - `debug.intelxe.sensors`: the card PL1 should be the card's rated board power (the Pro B50 draws only from the
     slot, about 70 W), and an idle machine should show 0 rejected power windows.
3. **SR-IOV.** The Pro cards are sold for SR-IOV (`bmg_desc` has `has_sriov`). The driver only ever runs as the PF
   with no VFs; nothing enables them.
4. **Add the id.** Add it to `data/platforms.json` "supported" (id, name, `model`, `tested: false`, and `gpu_test`
   figures from the specification with their basis). Then add it to every copy in section 4: IOPCIMatch,
   NMXAccel's plist, `kMatch`, `install.sh`, `efi-rules.json`, `Model.swift`, `clbench`, the READMEs,
   and `SupportedDevices.swift` (`devices.py --write-swift`). `IntelXe.cpp` picks the `model` property with a
   B570-or-B580 test; a third card turns that into a per-id table.
5. **Run:**
   - `platform_test`, `pmt_test`, `pcode_test`, `uapi_test` and `aspm_test` (commands in `kernel/README.md`);
   - `tools/parity/devices.py` (0 problems);
   - the kext build (the same 22 warnings);
   - on the card: `tools/regress` (full), the app's GPU test, Metal and Vulkan, and a sleep/wake cycle.
   Only then set `tested: true`.

**A G31 card (e220-e223)** also needs everything above, plus:

- **Wa_14022085890** (the SLPC minimum of 1200 MHz, `BMG_MIN_FREQ`). Linux applies it to G21 only
  (`xe_device_wa_oob.rules:5` SUBPLATFORM(BATTLEMAGE, G21)), but here it is applied unconditionally. Make it
  conditional on the subplatform.
- **GMD_ID.** Read it on the card. `tests/xe_pci.c` lists BMG as 20.01 / 13.01 only, so the entry's
  `graphicsVerx100` and `mediaVerx100` are an assumption until then.
- **Then** mark the entry `supported` and change `platform_test` section 2, which expects only G21 to be supported.

### Pro B50/B60 enabled, Pro B70 (G31) prepared

> Added 2026-11-13 (branch `bmgpro`). No hardware was used: nothing here has run on a Pro card.

**The ids, from the source.** Linux `include/drm/intel/pciids.h`, fetched from git.kernel.org on 2026-11-13:

| tree | BMG lists |
|---|---|
| v6.17 (:846-860), v6.18 (:851-865) | one list, `INTEL_BMG_IDS`: e202 e209 e20b e20c e20d e210 e211 e212 e216 e220 e221 e222 e223 |
| v6.19, v7.0, master (v7.4-rc2) (:851-868), and `dl/linux-xe` (abf8c33) | `INTEL_BMG_G21_IDS`: e202 e209 e20b e20c e20d e210 e211 e212 e216; `INTEL_BMG_IDS` = those plus e220 e221 e222 e223 |

The split came with 78de8f876683 ("drm/xe: Handle Wa_22010954014 and Wa_14022085890 as device workarounds", Matt
Roper, 2025-10-13), which needed a G21 subplatform for Wa_14022085890. e220-e223 came with 9b779ff0e1d1 ("drm/xe/xe2_hpg:
Add PCI IDs for xe2_hpg", 2025-06-06, Bspec 68090), e211 with cca9734ebe55, e209 with ccfb15b8158c. `kIdsBMGG21`,
`kIdsBMGG31` and `kIdsBMG` already held exactly these; `platform_test` section 5 pins them.

**The names.** Intel's product specification pages (ark.intel.com, read 2026-11-13) give each card's "Device ID":

| card | id | chip | Intel page (`intel.com/.../products/sku/<n>/.../specifications.html`) | memory, bus, bandwidth | Xe-cores, graphics clock | board power |
|---|---|---|---|---|---|---|
| Arc B580 | 0xE20B | G21 | 241598 | 12 GB, 192 bit, 456 GB/s | 20, 2670 MHz | |
| Arc B570 | 0xE20C | G21 | 241676 | 10 GB, 160 bit | 18, 2500 MHz | |
| Arc Pro B60 | 0xE211 | G21 | 243916 | 24 GB, 192 bit, 456 GB/s | 20, 2400 MHz | 200 W, PCIe 5.0 x8 |
| Arc Pro B50 | 0xE212 | G21 | 242615 | 16 GB, 128 bit, 224 GB/s | 16, 1700 MHz (max 2600) | 70 W, PCIe 5.0 x8 |
| Arc Pro B70 | 0xE223 | G31 | 245797 (launched Q1'26) | 32 GB, 256 bit, 608 GB/s | 32, 2280 MHz (max 2800) | 230 W, PCIe 5.0 x16 |

Mesa's `include/pci_ids/iris_pci_ids.h` (main, fetched 2026-11-13) agrees for the G21 cards: 270035fdce11 ("intel/dev:
update BMG device names", 2025-06-17) names e20b/e20c/e211/e212 after the first four pages above. It names every G31
id "Intel(R) Graphics" (17fbd0df5180); the B70 id comes from Intel's page only. Which G31 id the other G31 products
(e220-e222) are, no public source says; they get the model "Intel Arc Graphics".

**Pro B50 / B60: supported, untested, as the B570 was in 0.1.4.** They are the same BMG-G21 die (`INTEL_BMG_G21_IDS`,
Mesa "BMG G21") as the B580 and B570, so the reasoning of the ROADMAP holds: the same descriptor, the same code paths,
the same firmware. They go into `"supported"` with `tested: false` (install.sh and the apps say "supported; not yet
tested"), not behind a switch. What differs is read from the card:

- VRAM: `vramUsableEnd` takes the tile size from `SG_TILE_ADDR_RANGE0` and the flat-CCS, DSM and GSM bases from their
  registers; `publishVram` the same tile size (it refuses more than 64 GB). The buddy allocator (`xe_vram.cpp`) has 64
  roots of at most 1 GB, enough for 32 GB in any alignment. The only B580-sized value was the 11 GB fallback for an
  implausible register read; it is now 11/12 of the tile where the tile is smaller than 12 GB (it was past the end of a
  B570's 10 GB).
- BAR: `vramVisible` is BAR2's length as the firmware sized it (`ResizeAppleGpuBars`), capped at the VRAM end; no 16 GB
  constant exists in the kexts. The `0x300000000` of `user/xemac` is the fake device of the host tests only.
- The Xe-core, engine and L3 fuses, RP0/RPn (SLPC), the power limits (`debug.intelxe.sensors`): read from the card.
- The app's GPU test has figures per card (`gpu_test`), from Intel's specification and marked as not measured.

Still B580 figures, not correctness: the app's Quiet (1800 MHz) and Performance (2400 MHz) settings, and the "No Arc
B580 or B570 found" message.

**Pro B70 / BMG-G31: prepared, refused by default.**

- **Wa_14022085890 is conditional now, as in Linux.** `xe_device_wa_oob.rules:5` (abf8c33 and master) applies it to
  SUBPLATFORM(BATTLEMAGE, G21). `xe_guc_pc.c` `pc_needs_min_freq_change` (:893-908) then raises the SLPC minimum to
  `BMG_MIN_FREQ` (1200 MHz) on both GTs; without the workaround it still does so on the primary GT of every
  `XE_BATTLEMAGE` (and CRI), but not on the media GT. (In v6.17/v6.18 the rule was `GRAPHICS_VERSION(2001)` in
  `xe_wa_oob.rules`, applied by `XE_GT_WA` on each GT.) Here: the descriptor's `wa14022085890` (G21 only),
  `xePlatformSlpcMinPrimary`/`xePlatformSlpcMinMedia`; `mediaPcStart` sends SLPC MIN only where the workaround applies
  and leaves the GuC's minimum otherwise; the primary GT keeps 1200 MHz on every BMG. `platform_test` section 6.
- **The boot-arg.** The G31 entry carries `testerBootArg = "intelxe_allow_g31"`. Both kexts match e220-e223 in a
  personality of their own (`IntelXe-G31`, `IntelXeFB-G31`; `build.py` `G31_TESTER_MATCH`), so that the supported list
  stays the tested one. Without `-intelxe_allow_g31` `start` refuses the card with the entry's `missing` line, as
  before. With it, `start` logs two lines that begin `!!!!!!!!` (not supported, starting anyway; what is known) and
  the `IntelXePlatform` registry line ends in `UNSUPPORTED (tester boot-arg -intelxe_allow_g31)`. NMXAccel (in
  `nullmoth/`) does not match G31, so there is no Metal on it; efi.sh and the Windows tool do not count it as a card
  (no SSDT-ARCOFF): the EFI is set by hand. `devices.py` checks that `G31_TESTER_MATCH` equals `kIdsBMGG31` and holds
  no supported id.
- **Firmware: no G31-specific files.** `xe_uc_fw.c` (abf8c33 = master) has one `fw_def(BATTLEMAGE, GT_TYPE_ANY, ...)`
  for the GuC (`xe/bmg_guc_70.bin`, 70.54 minimum) and one for the HuC (`xe/bmg_huc.bin`), keyed by platform, not
  subplatform; BMG has no GSC entry. i915 `intel_dmc.c` picks `i915/bmg_dmc.bin` by display 14.01. linux-firmware's
  `xe/` holds `bmg_guc_70.bin` and `bmg_huc.bin` only for BMG, `i915/` `bmg_dmc.bin` only (tree listing of 2026-11-13).
  So the built-in GuC 70.72.1 and HuC 8.2.10 are the files Linux would load on a G31.
- **Workarounds and tunings: the same for G31 if its GMD_ID reads 20.01.** Every Xe2_HPG rule in `xe_wa.c`,
  `xe_tuning.c`, `xe_wa_oob.rules`, `xe_hw_engine.c` (`engine_sr`) and `xe_reg_whitelist.c` is keyed by graphics or
  media IP version (and engine class, or a FUNC that reads the hardware), none by subplatform or stepping; the only
  subplatform rule is Wa_14022085890 above. Outside the tables, `xe_hwmon.c:845` reads the PCIe temperature at another
  bit offset on G21 than elsewhere; this driver does not read that sensor.
- **But G31 may be graphics 20.02.** `tests/xe_pci.c` lists BMG as 20.01 only. The series that added e220-e223
  (lore 20250605190804.1287289) also added `{ 2002, "Xe2_HPG" }` to `xe_pci.c` (26ff87d2e776) and widened the Xe2_HPG
  workarounds to `GRAPHICS_VERSION_RANGE(2001, 2002)` (a5d221924e13); no public source names the 20.02 part. A table
  diff of abf8c33 for graphics 20.02 against 20.01 (media 13.01 for both; a short script over the rule tables, not kept in the repository) finds no rule
  that 20.02 adds, and these that 20.01 has and 20.02 does not. This driver applies all of them unconditionally:

  | rule (abf8c33) | what | applied here |
  |---|---|---|
  | `xe_wa.c:255` gt_was Wa_16025250150 | `LSN_VC_REG2` weights, `LSC_CHICKEN_BIT_0_UDW` | `firstBatchSetupOnly` |
  | `xe_wa.c:554` engine_was Wa_14019811474 | `LSC_CHICKEN_BIT_0` `WR_REQ_CHAINING_DIS` | `firstBatchSetupOnly` |
  | `xe_wa.c:758` lrc_was Wa_14020756599 | `WM_CHICKEN3` `HIZ_PLANE_COMPRESSION_DIS` | `xe_core.cpp` render LRC |
  | `xe_wa.c:762` lrc_was Wa_14021490052 | `FF_MODE` mesh autostrip, `VFLSKPD` | `gFfMode`, render LRC |
  | `xe_tuning.c:176` lrc_tunings "vs hit max value" | `FF_MODE` `VS_HIT_MAX_VALUE` 0x3f | `gFfMode` |
  | `xe_wa_oob.rules:23,26` Wa_18024947630, Wa_16022287689 | GuC WA KLVs (`xe_guc_ads.c:331,334`) | `loadGuc` KLVs |
  | `xe_wa_oob.rules:32` Wa_22019338487 | GGTT/MERT frequency caps during probe and L2 flush | not applied (as on G21) |
  | `xe_wa_oob.rules:34` Wa_16023588340 | 28 PAT entries (`xe_pat.c:638` `== 2001`), L2 for host VRAM, CLOS3, L2 flush, display flush | `patEntries` 28, `bringUp`, `l2Flush`, IntelXeFB |

  On a 20.02 part Linux would expose 32 PAT entries and skip the L2/CLOS3 setup. If the tester's GMD_ID (step 1 of the
  bring-up log, or XeProbe stage 1) reads 20.02, the next step is a G31 descriptor with `graphicsVerx100 = 2002` and
  `patEntries = 32`, and these eight items behind a version check; until then a 20.02 card runs with the G21 set.

**What a tester with a Pro B50 or B60 does:** install as usual; send `tools/report/` output (it carries
`IntelXePlatform`, the VRAM and BAR lines of the bring-up, `debug.intelxe.sensors`); then `tools/regress` and the app's
GPU test if they can. With a Pro B70: add `-intelxe_allow_g31` (and keep a USB stick with a known-good EFI), first with
`xeprobe=0xe223` for stages 1-2 (GMD_ID), then the full boot; report the `!!!!!!!!` lines, the GMD_ID and the bring-up
log. After a good report, a card's `tested` becomes `true`; for G31, `supported` (and `platform_test` section 2).

## 4. The userland side

> Added 2026-11-10 (branch `userland-platform`). This section was checked against the files outside `kernel/`.
> Updated 2026-11-10 (branch `userland-platform2`): the items marked **Done** below.

Almost nothing outside the kernel decides by device.

- **Metal plugin (`nullmoth/plugin`).**
  - It decides by GPU vendor, which it reads from the PCI device above NMXAccel (`nvmtl_vk_vendor`, 0x8086 = ANV).
  - It names the device, VideoToolbox's codecs included, after IntelXe's `model` property (`nvmtl_registry_model`).
  - It sizes memory from ANV's Vulkan heaps and budget.
  - A compute pipeline's execution width is the SIMD width ANV compiled it for.
  - Its "B580" mentions are comments and measurement notes.
- **Translator.** The Rust translator holds no hardware facts. Its three mentions are comments.
- **Mesa.** Mesa picks its code paths from the device id, through its own device table (`intel_device_info`), so the
  repository ships no device configuration for it. The patches name the B580 only in comments. The `0xb580xxxx`
  capability numbers are a frozen ABI.

What is device-specific is the list of cards the driver attaches to. Each part keeps its own copy of that list, in its own
language. `data/platforms.json` `"supported"` is now the one source of that list:

- each device's id, the name that messages use, the kernel `model` string and whether the project has tested it;
- what the app's GPU test expects of the card (`gpu_test`): compute rate and memory bandwidth, the good and fair limits,
  and whether the figures were measured or come from the specification;
- the kernel descriptor that covers the devices (`kernel_ids`).

`tools/parity/devices.py` reads every copy and fails when one differs. `release/test-efi-flow.sh` and
`release/release.sh` run it.

| copy | form | checked |
|---|---|---|
| `kernel/IntelXe/build.py` | IntelXe IOPCIMatch | exact list |
| `nullmoth/kexts/NVRM/accel/Info-NMX-IntelXe.plist` | NMXAccel IOPCIPrimaryMatch | exact list |
| `kernel/IntelXe/tests/platform_test.cpp` | `kMatch` | exact list |
| `kernel/IntelXe/xe_platform.cpp` | the `kernel_ids` array of a `kXePlatforms` entry | holds every id |
| `release/templates/install.sh`, `release/install.sh` | `CARDS=("e20b B580" "e20c B570 untested")` | ids, names, untested = not `tested` |
| `release/templates/efi-rules.json`: efi.sh and `IntelXe-EFI.ps1` read it (the app carries efi.sh and the file; since 2026-11-12, `DESIGN-efi-rules.md`) | `"cards": {"device_ids": ["e20b", "e20c"]}` | exact list |
| `app/Sources/Model.swift` | `supportedCards` | ids and names |
| `tools/clbench/clbench.c` | `kSupported`, the OpenCL devices it prefers | exact list |
| `app/Sources/SupportedDevices.swift` | generated from `gpu_test` (`devices.py --write-swift`) | the file is what the JSON gives |
| the READMEs, `README-INSTALL*.md`, `efi/README.md`, `tools/parity/platforms.py`; `IntelXe.cpp` | text | names every id and card (`IntelXe.cpp`: every `model`) |

Each copy stays a plain literal that can be read in its own file. One file is generated:
`app/Sources/SupportedDevices.swift`, the GPU test's figures, which the app needs at run time. On the B580 and the
B570 the install check, efi.sh, the app and the Windows tool behave as before. Their messages are built from the list
and come out the same.

Deliberately left alone:

- the Arc B580 test fixtures: xemac's `XEMAC_FAKE` device (`FAKE_DEVICE 0xe20b`, the BMG hwconfig, 20.01/13.01,
  12 GB) and the app's `SelfTest` facts;
- the frozen names: the `'B580'` IOServiceOpen type, `0xb580xxxx`, and `B580.kext` as the earlier name;
- the app's "No Arc B580 or B570 found" message and the install.sh header, which test expectations depend on (a
  separate change);
- prose and measurement notes.

### What a new family needs outside the kernel

The ids come first. Add the device to `"supported"` and run `python3 tools/parity/devices.py`; it names every copy to
change. The kernel side is in section 3. After that, the work is in these places:

- **Device detection by name. Done.**
  - The app's GPU test takes its verdict and its expected figures by PCI device id: the IOPCIDevice above the Metal
    device's registry entry, or else the supported card on the bus when the Metal device carries its name. The
    figures are the card's `gpu_test` (`SupportedDevices.swift`). The B580's are the earlier constants (13 TFLOP/s,
    400 GB/s, the same limits). The B570's come from Intel's specification and are marked as not measured
    (11.5 TFLOP/s, 380 GB/s; the derivation is in the JSON and the generated file). Any other GPU gets no verdict and
    no figures.
  - `tools/clbench` prefers a GPU with vendor id 0x8086 whose PCI id (`CL_DEVICE_ID_INTEL`, Intel's runtime) is
    supported, then any 0x8086 GPU, then a GPU whose `CL_DEVICE_VENDOR` names Intel, then the first GPU. Its argument
    (a name substring) still overrides. `user/tests/metal/cl_lat.c` does the same without the PCI id, which Apple's
    OpenCL does not report; `CL_LAT_DEVICE=<n>` picks a GPU.
  - Not known yet: what vendor id and vendor string Apple's OpenCL reports for this driver's device. It reports ids of
    its own (Apple M1 Pro: 0x1027f00). Both tools print them, and on the test machine (one GPU) they fall back to it.
- **B580 figures in the app. Done.**
  - The Settings label is built from `debug.intelxe.sensors`: `Default (<floor>–<RP0> MHz)`. The floor is the
    kernel's 1200 MHz SLPC minimum, kept within RPn and RP0 as `slpcParams` keeps it. On the B580 it reads
    `Default (1200–2850 MHz)` as before. Without the sysctl it says `Default`.
  - When the sensors give no RP0 (the GPU asleep), the clock chart's top is the last RP0 read, or else the highest
    reading. It no longer falls back to 2850.
  - Still B580 figures: the Quiet (1800 MHz) and Performance (2400 MHz) settings.
- **Discrete-only steps.**
  - efi.sh, the Windows tool and SSDT-ARCOFF look for a PCIe card two bridges below the root port, and they set
    `ResizeAppleGpuBars`.
  - An integrated GPU has no such port and no VRAM BAR, so these steps would be skipped or replaced for one.
  - The same holds for the install guide's 1 GB BAR window.
- **Video.** The VideoToolbox codecs register by what ANV offers. **Done:** `videotoolbox/encoder/intelxevenc.c`
  `IntelXeVEnc_Register` (the test tools' path) now names its encoders after the `model` property, as the plugin's own
  registration does. "Intel Arc B580 …" is left only as the fallback when the registry has no model.
- **zink-gl. Done.** `zink-gl/src/mesa-zink.diff` is the source record of the shipped zink binaries. No build script
  applies it. It still held the pre-rename ANV changes (`b580_drm.h`, `DRM_IOCTL_B580_*`) of the tree it was cut from.
  It now holds only what a zink-only build compiles (everything outside `src/intel/`). Those sections are byte for
  byte as before, and the diff still applies to `f01e9b07a91`. ANV's current changes are `mesa/intelxe.diff`.
- **Mesa.** Mesa's own support for the family. For a pre-Xe2 family, the Xe2 video patches (`mesa/patches` 0010–0095)
  would need a check against that family's video engine.
