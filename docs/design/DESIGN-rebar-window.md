# The BAR window: `ResizeAppleGpuBars = 10` and ocvalidate's sleep-wake warning

> Design note with its date (2026-11-13, branch `rebar`): it records the research and the state of that day, not the
> current status. Nothing here was run on the B580; section 6 is the test plan for the night runner. Sections 8 and 9
> (branch `barwin`, the same day) add revocable CPU mappings and the >1 GB research path.

Stage 2 of the install guide sets `Booter > Quirks > ResizeAppleGpuBars = 10` (`release/templates/efi-rules.json`,
applied by efi.sh, IntelXe-EFI.ps1, IntelXe Manager and the EFI builder in `windows/efibuild/`; the test machine's
`efi/sample-z790/config.plist` has it too). OpenCore 1.0.8's `ocvalidate` reports every such config:

    Booter->Quirks->ResizeAppleGpuBars is set to 10, which is unstable with macOS sleep-wake!

The EFI builder accepts exactly this line (`validate.py`, `accepted_findings`). This note answers what the value does,
why OpenCore warns, whether the warning applies to IntelXe, and whether to keep it.

**Answer: keep 10.** The warning is about the card's Resizable BAR size, which sleep resets and macOS's PCI driver
does not restore. Apple's own GPU drivers do not set it again, and OpenCore's warning was written for them.
IntelXe.kext restores it on wake, as Linux's `pci_restore_rebar_state` does, and sleep works on 0.1.4. The other
values give no gain:

- 9 gets the same warning, with half the window.
- 8 gets a different warning ("not useful").
- Only 0 and -1 pass silently. 0 gives the B580 a 256 MB window, which is below the desktop's measured use.
- -1 leaves BAR2 unusable on macOS.

The docs now say this. Suggested wording for the site is in section 7.

## 1. What the value does (OpenCore 1.0.8)

Sources are pinned to OpenCorePkg tag `1.0.8` (commit
[`6fe4d15`](https://github.com/acidanthera/OpenCorePkg/tree/6fe4d15cb66596d99d8cfedce69d8fc6dad71886)); paths below
are relative to it.

- **Encoding.** The value is a PCI Resizable BAR size code: 2^n MB. 8 is 256 MB (`PciBar256MB`), 9 is 512 MB, 10 is
  1 GB (`PciBar1GB`, also `PciBarMacMax`); `Include/Acidanthera/Library/OcDeviceMiscLib.h` lines 111-125. -1 (the
  failsafe, `OcConfigurationLib.h` line 155) turns the quirk off.
- **When.** It runs only when an Apple booter (`boot.efi`) is started: `Library/OcAfterBootCompatLib/ServiceOverrides.c`
  lines 1255-1259 call `ResizeGpuBars (ResizeAppleGpuBars, FALSE, ResizeUsePciRbIo)` in the `StartImage` hook of the
  Apple booter. Windows and Linux started from the same picker keep the firmware's BAR. That code is not conditional on
  a hibernation wake, so a resume from hibernation gets the same size as a cold boot.
- **What.** `Library/OcDeviceMiscLib/SetResizableBar.c`:
  - `ResizeGpuBarsPciIo` (line 611) walks every PCI device and keeps only display-class ones (line 665). That means
    every GPU with a Resizable BAR capability, not just the B580.
  - `SetResizableBarOnDevicePciIo` (line 155) does the following for each resizable BAR:
    - It limits the supported sizes to the requested one (line 278).
    - With `Increase = FALSE`, which is the Apple case, it also limits them to the current size (lines 284-286). The
      quirk only ever shrinks, so with Resizable BAR off in the firmware (a 256 MB BAR), 10 changes nothing.
    - It picks the largest size left, or the smallest supported one if the request is below all of them
      (lines 290-296).
    - It writes the BAR Size field of the control register (lines 322-329), then writes the old BAR address
      registers back (lines 362-378: "after writing the BAR Size field, the contents of the corresponding BAR are
      undefined").
  - On the test machine the firmware gives BAR2 16 GB at a 16 GB-aligned address. 10 makes it 1 GB at that same
    address. The address is not moved, and the rest of the bridge window the firmware sized for 16 GB stays unused.
- **The documentation** (`Docs/Configuration.tex` lines 1843-1873): "While Apple macOS supports a theoretical 1 GB
  maximum, in practice all non-default values may not work correctly. For this reason the only supported value for
  this quirk is the minimal supported BAR size, i.e. `0`. ... For development purposes one may take risks and try
  other values."

### Why macOS stops at 1 GB

Apple's IOPCIFamily (latest published source,
[`IOPCIFamily-726.0.5`](https://github.com/apple-oss-distributions/IOPCIFamily/tree/4822b27a36e2de70e231ecf2bf3021384fab6ec2)):

- `IOPCIConfigurator.cpp` line 67 defines `#define MAX_BAR_SIZE (1ULL*GB)`.
- `probeBaseAddressRegister`, lines 2773-2778, does this for any BAR it sizes above 1 GB: `size &= 0xFFFFFFFF;
  clean64 = false; nextBarNum--`. The size is cut to its low 32 bits (0 for 16 GB), and the BAR's upper half is probed
  again as a BAR of its own.
- That is why BAR2 stayed unassigned with -1 on the test machine (2026-10-28) and why ocvalidate calls values above 10
  "unsupported by macOS".

### Why ocvalidate calls 9 and 10 unstable

`Utilities/ocvalidate/ValidateBooter.c` lines 217-226:

| value | ocvalidate | counted as an issue |
|---|---|---|
| -1, 0 | nothing | no |
| 1-8 | "is not useful for macOS!" | yes |
| 9, 10 | "is unstable with macOS sleep-wake!" | yes |
| > 10 | "is unsupported by macOS!" | yes |

Every line counts as an issue: `ocvalidate.c` lines 133-153 print "Found N issues" and exit with `EXIT_FAILURE`.
`Utilities/ocvalidate/README.md` line 43 has the rule: "`ResizeAppleGpuBars` must be set to `0` or `-1`".

History:

- [`e70048c`](https://github.com/acidanthera/OpenCorePkg/commit/e70048cc76bbcb40cd361a72b2a817f16ba57b96)
  (2021-10-11, 0.7.5): the quirk is added. Its documentation then read: "Apple macOS supports 1 GB maximum, which is
  10".
- [`ecceb47`](https://github.com/acidanthera/OpenCorePkg/commit/ecceb47e79c55779d95b75c5a244f915c5a03e6e)
  (2021-10-12, "Docs: Improve RBAR documentation") adds the rationale: "*Note 2*: Certain GPU drivers do not support
  non-standard BAR sizes, causing sleep wake issues, for this reason for macOS it is recommended to use minimal
  supported BAR sizes, i.e. specify `0` (1 MB)."
- [`841eab2`](https://github.com/acidanthera/OpenCorePkg/commit/841eab2934e45a157d07e873b8b9f68c88bdc59b)
  (2021-11-04, 0.7.6, Changelog line 390 "Explicitly restricted `ResizeAppleGpuBars` to 0 and -1") adds the ocvalidate
  check above. It also replaces Note 2 with today's "all non-default values may not work correctly" text.
- The 0.7.5 release announcement ([acidanthera/bugtracker#1818](https://github.com/acidanthera/bugtracker/issues/1818))
  said: "While macOS does support this feature, it limits the supported BAR sizes by 1 GB and seems to be unstable
  with the BARs above 256 MB. ... setting `ResizeAppleGpuBars` to `0` will update GPU registers to their defaults when
  booting macOS and fix the hangs on newer boards."

No bug report with a mechanism was found: the bugtracker has no issue about `ResizeAppleGpuBars` and sleep. Dortania's
guide only says to set 0 when Resizable BAR is on (`config.plist/comet-lake.md` line 88). The boundary at 8 is
256 MB, the BAR size most GPUs have at power-on: the `ResizeGpuBars` text, `Configuration.tex` line 9320, says
"devices of the last decade use BARs up to 256 MB by default".

### The mechanism, from IOPCIFamily's source

1. In S3 the slot loses main power. The card comes back with its Resizable BAR control register at its power-on value,
   unless the firmware's S3 boot script writes it. OpenCore's write was made through PciIo just before `boot.efi`, so
   it is not in that script.
2. IOPCIFamily saves and restores, around sleep:
   - the first 16 dwords of config space (`IOPCIBridge.cpp` lines 1987-1990 and 2328-2333, with
     `kIOPCIConfigEPShadowRegs = 16`), which include the BAR address registers;
   - the L1 PM substates, LTR, ACS, AER, PCIe, FPB and PTM capabilities (lines 2336-2405).

   It never saves or restores the Resizable BAR capability (`kIOPCIExpressCapabilityIDResizableBAR` appears only in the
   capability-name table, line 3657).
3. After a wake, macOS therefore writes the 1 GB BAR's address back into a BAR whose size may be different:
   - 256 MB from power-on: offsets above 256 MB no longer decode;
   - 16 GB from a firmware script: the low address bits are read-only and the BAR covers more than its bridge window.

   A driver that uses the BAR as it was at boot, which is what Apple's GPU drivers do, then reads all-ones, loses
   writes or hangs.
4. With 0, or any size at most the power-on size, step 3 changes nothing, so the warning does not apply there.

Linux does the step macOS leaves out:

- `pci_restore_state` calls `pci_restore_rebar_state` (`drivers/pci/pci.c` lines 1877-1917 in v6.17). It writes the
  BAR size the kernel assigned back into each control register, before the BAR addresses.
- `xe` itself sizes the BAR at probe (`drivers/gpu/drm/xe/xe_vram.c` `resize_vram_bar`). It runs with a 256 MB BAR as
  a "Small BAR device" (same file, lines 320-350).

## 2. What IntelXe does with the BAR

**At start** (`kernel/IntelXe/IntelXe.cpp`, the block after `registerService()`, about line 805): the driver saves the
Resizable BAR state, as `pci_save_rebar_state` does. That is the capability offset, the number of resizable BARs, each
control dword (with the 1 GB size OpenCore set) and BAR2's address registers (0x18/0x1c).

**At bring-up** (`kernel/IntelXe/xe_core.cpp` `IntelXe::bringUp`, about line 1333):

- BAR2 is mapped write-combined, and `vramVisible` is its length rounded down to 64 KB (at most the usable VRAM).
- Nothing assumes 1 GB: 0 (256 MB) works too (`efi/README.md`; the kext ran that way on 2026-10-28).
- Two buddy allocators split VRAM:
  - `gVis`: the window, from `vramUserStart` (32 MB; below it the firmware framebuffer and IntelXeFB's cursor) to
    `vramVisible`;
  - `gHi`: the rest, up to the end of usable VRAM (`vramUsableEnd`: below flat CCS, GSM and DSM).
- The window holds everything the CPU writes in VRAM:
  - page tables (`ptPageAlloc`, with a reserve of `kWinReserve` = 32 MB that user BOs may not take);
  - LRCs (64 KB blocks, unless `lrc_vram` 0);
  - scan-out and cursor buffers (`xeDispVramAlloc`, contiguous, down to `kDispFloor` = 8 MB);
  - every user BO the CPU maps.
- A mapped BO never leaves the window (unless `debug.intelxe.bar_revoke` is 1: section 8 redirects its mapping to
  system memory). When the window is full:
  - BOs that allow it spill to system memory;
  - a CPU map of a BO that cannot enter moves it to system memory;
  - Vulkan sees the window as the CPU-visible heap (`cpu_visible_size = vramVisible`).
- `user/xemac/xemac.c` (about line 1327) puts background processes' CPU-visible BOs in system memory from the start.
  The reason: on 5 Nov the window filled at login and Minecraft's mapped buffers spilled, and the game ran at half
  speed.
- VRAM-shared IOSurfaces (`intelxe_surfvram=2`, DESIGN-iosurface-vram.md) are pinned outside the window (`gHi`). They
  do not depend on its size; their CPU access goes through a copy-engine page-off.

**On sleep and wake** (`IntelXe::setPowerState`, `xeSuspend`, `xeResume`, `IntelXe::gpuResume`):

1. Suspend copies every VRAM BO (window and beyond) and the VRAM LRCs to system memory, then soft-resets the GuC.
2. On wake, the PCI device is in D0 again with IOPCIFamily's config restore done. `gpuResume` first compares each
   Resizable BAR control's size field with the saved one. If any differs, it:
   1. turns memory decode off;
   2. writes the saved sizes;
   3. writes BAR2's saved address again;
   4. restores the command register.

   It records "resume: Resizable BAR size restored" in `debug.intelxe.state`. This is `pci_restore_rebar_state` with
   the order the PCIe specification asks for (size, then address).
3. Only after that does anything use BAR2. It is used to:
   - restore the LRCs (`lrcResumeRestore`, CPU through BAR2);
   - rewrite the page tables (`ptRewrite`);
   - copy the BOs back (copy engine);
   - tell IntelXeFB (event 2), which then writes its cursor and planes.

So the kext does what OpenCore's warning says macOS drivers do not do, before its own first access. The ROADMAP's sleep
entry records that it works: on 12 Nov, on 0.1.4, three 4-minute sleeps woke the display each time, did a full modeset
and passed the Metal checks.

**Not known yet** (section 6 measures it):

- Which size the B580's control register holds after an S3 wake on this board: the power-on 256 MB, the firmware's
  16 GB, or the 1 GB kept. That is, whether the restore has ever fired.
- The line goes to the `say` buffer, which drops text once 4 KB are full. After bring-up the buffer may well be full
  already, and no counter records the restore.

**Remaining exposure** (the same for 9 and 10; none for 0 or 8):

- **Before the restore.** Between IOPCIFamily's config restore and `IntelXe::setPowerState(1)`, a user process that
  stores into a CPU-mapped BO above the size the card came back with is not decoded. The same stores into any BO are
  already overwritten by the restore copy, and VRAM did not survive D3cold anyway, so nothing new is lost.
- **A 16 GB size between those two points.** The BAR covers more than the bridge window that IOPCIFamily restored. The
  bridge forwards only its window, so the excess is unreachable, not a conflict.
- **Other GPUs.** The quirk applies to every display-class device. A second GPU with a Resizable BAR capability and
  Apple's driver, such as an AMD card added to the same PC, gets a 1 GB BAR too. It is then exactly OpenCore's case,
  with nothing to restore its size. The guide's configuration has the B580 as the only GPU in use. The UHD 770 is off
  for macOS, and SSDT-ARCOFF keeps Apple's display drivers off the B580.
- **Hibernation.** OpenCore resizes again before `boot.efi` on a hibernation wake, and IOPCIFamily's dehibernate restore
  writes the BAR addresses. This was not tested: desktops default to `hibernatemode 0`.

## 3. Does the driver need 1 GB?

Measured use of the window:

- 12 Nov: after about 40 Geekbench runs and the desktop, 194 of 1024 MB used, 0 refusals, 0 spills (ROADMAP).
- Per process: a Metal process about 12 MB; a drawing process's plugin descriptor pools about 30 MB (ROADMAP: 100 to
  30 MB); ANV's mapped internal pools per Vulkan process.
- 5 Nov, before the background-process placement: 24 processes and Minecraft spilling 137 MB.

What each size leaves for user BOs (the window minus the 32 MB below `vramUserStart` and the 32 MB page-table reserve):

| value | BAR2 | for user BOs | against the 194 MB measured |
|---|---|---|---|
| 10 | 1 GB | ~960 MB | 5x headroom |
| 9 | 512 MB | ~448 MB | 2.3x |
| 8 or 0 | 256 MB | ~192 MB | already full: spills on the plain desktop |

What a smaller window costs:

- **Games and Vulkan applications** that ask for device-local, host-visible memory (DXVK/vkd3d under Wine, Zink's
  streaming buffers, engines written for Resizable BAR). They spill to system memory sooner, and spilled mapped buffers
  are what halved Minecraft's frame rate on 5 Nov.
- **Uploads** through Metal Shared storage do not change: it is already in system memory.
- **CPU write speed** into a window BO (`vbw`) does not change: write-combined BAR writes run the same in any size of
  window.
- **VRAM-backed IOSurfaces** do not change (outside the window).
- **Scan-out buffers** compete for the same room: a 5120x2880 HiDPI frame is 59 MB.
- **Compute.** NEO (Level Zero, OpenCL) treats every size below the full VRAM as a small BAR. 1 GB is already one, so
  nothing changes there.

So the driver does not *need* 1 GB to work. It needs it to keep the measured desktop and a game out of the spill path:
9 would probably still do for today's measured load, 8 or 0 would not.

## 4. The options

### A. Keep 10 (recommended)

- **For:**
  - A 1 GB window: the largest macOS can address (IOPCIFamily's `MAX_BAR_SIZE`).
  - The kext restores the size on wake, which is the step whose absence the warning is about.
  - Sleep works on 0.1.4.
  - No code change.
- **Against:**
  - ocvalidate reports one issue, and every validator built on it (OCAT and similar) shows it. Users who follow
    Dortania (0 with Resizable BAR on) need the reason written down; the builder already whitelists exactly this line.
  - It applies to every GPU in the PC (section 2), so a second GPU with Apple's driver would be at risk.
  - The restore path has not been seen firing (section 6 checks it).
- **To do:**
  - The docs (done in this branch, section 7 for the site).
  - The test plan below.
  - Two small kernel diagnostics for the maintainer:
    - log the restore with `LOG` (the system log), not only `say`, with the control and BAR2 values found and written;
    - count restores in `debug.intelxe.stats`' "system sleep" line.

### B. 9 (512 MB), no kernel change

- **For:** a smaller step away from the power-on size. That is irrelevant with the restore in place: any size other
  than the power-on one needs it.
- **Against:**
  - ocvalidate prints the same "unstable with macOS sleep-wake" line, so the warning stays.
  - Half the window.
  - Every EFI tool changes for nothing.
- **Verdict:** no gain.

### C. 0 or 8 (256 MB) with a kernel change

- **For:**
  - 0 is ocvalidate-clean and is Dortania's and OpenCore's recommended value.
  - 256 MB is most likely the card's power-on size, so a wake changes nothing even without the restore.
  - The kext already runs with it.
- **Against:**
  - About 192 MB for user BOs, which is below the plain desktop's 194 MB. Without a kernel change, games spill.
  - The kernel change that would make a small window cheap is the revocable CPU mapping scheme: mapped BOs that can
    leave the window, like Linux's faulting mmaps. Section 8 implements it behind a knob (off by default), with
    `IOMemoryMap::redirect` and a pre-populated device-pager object as the new backing; not yet run on the B580.
  - 8 is not even ocvalidate-clean ("not useful").
- **Verdict:** only if A shows sleep failures that the restore cannot fix.
- **Specification if it is ever chosen:**
  - `efi-rules.json` `value: 0`;
  - the vectors in `release/efi-rules-vectors.json` changed (10 to 0 in the expected values, the "already does" vector);
  - `validate.py` with no accepted finding;
  - guides, apps and tests (`windows/app/IntelXe.Core.Tests` asserts "10") updated;
  - the existing `-efi` packages' plans still accept 10 as "already set" during a transition;
  - in the kernel, the window reserve (`kWinReserve`) and the background-process placement in `xemac.c` tuned for
    192 MB.

### D. -1 (the firmware's 16 GB) and our own resize

- **For:** ocvalidate-clean, and a later path to more than 1 GB.
- **Against:**
  - IOPCIFamily misparses a 16 GB BAR: it cuts the size to 0 and probes the upper dword as another BAR (section 1).
    BAR2 is then unassigned, and its address range is not part of IOPCIFamily's resource tree.
  - The kext would have to resize the BAR and claim address space behind IOPCIFamily's back, and keep the bridge
    windows right across IOPCIFamily's own wake restore.
  - More than 1 GB would also need IOPCIFamily patched (`MAX_BAR_SIZE`), which means Lilu. IntelXe.kext is plain IOKit
    by design.
  - On wake the same restore is needed.
- **Verdict:** a research item, not a release setting.

## 5. Recommendation

Keep `ResizeAppleGpuBars = 10` and keep the builder's exact-line whitelist. Make the docs say:

- why ocvalidate warns;
- that IntelXe restores the BAR size on wake;
- that 9 would not silence the warning;
- that 0 works with a smaller window.

Run section 6 to see the restore fire and to put numbers on 10 against 9. Re-open only if wake failures appear that
trace to the BAR.

## 6. Hardware test plan (night runner, B580)

Run the phases in this order:

1. **Phase A, the current 10.** One boot:
   1. `rebar-window.sh info`;
   2. `rebar-window.sh bench`;
   3. `rebar-window.sh loop 10 3` (ten 3-minute sleeps), polled through `$O/loop.txt`.
2. **Phase B, 9.**
   1. Back up the SN550's `config.plist`. Resolve the EFI partition by the disk's model name: disk numbers move. The
      USB stick keeps its proven EFI.
   2. `plutil -replace Booter.Quirks.ResizeAppleGpuBars -integer 9` on it.
   3. Before the reboot, keep `TALLogoutSavesState` and `LoginwindowLaunchesRelaunchApps` false.
   4. Reboot, and wait for SSH / `kern.boottime`. Someone logs in at the login window.
   5. Run the same three commands. `info` must show "window 512 MB".
3. **Phase C, optional: 0** (256 MB), the same way. This is the cost of the ocvalidate-clean value.
4. **Back to 10** and reboot. `info` shows 1024 MB again.

Each phase writes to `$INTELXE_HOME/rebar/w<window MB>/`. Every test binary runs under `tmo` (a bash timer, not
perl/env, so `DYLD_*` survives). Binaries come from `$REBAR_KIT`, by default `$INTELXE_HOME`:

- `vk_smoke` and `mtl_tests`, as sleeptest.sh uses them;
- from `user/tests/kernel`: `vbw`, `vfill`, `winfill` and `surfvram`;
- `cl_xfer` from `user/tests/metal`.

A missing binary is reported and skipped.

Pass per cycle, and what to compare between phases:

- **Each cycle passes when:**
  - "device resume 0" and "restore copy 0" in the stats;
  - the WindowServer pid is unchanged;
  - no new GT or GuC resets and no CAT errors;
  - `vk_smoke` and `mtl_tests` pass;
  - `vfill` reports 0 failures; it writes and reads every page of every BO, so the whole window, its top included, is
    decoded after the wake;
  - BAR2's size and address register are the same as before the sleep.
- **Recorded for each cycle:** `RESTORED` (whether the kext had to put the size back), plus the window line.
- **Compared between phases:**
  - `bench`: `vbw` MB/s, `cl_xfer` p50s and GB/s, `winfill fill` and `vfill` (how much fits before spilling, and
    whether everything reads back), `surfvram` PASS;
  - the window lines before and after;
  - a desktop-plus-Geekbench window snapshot (`info`) at the same point in each phase.
- **Expected:**
  - The same pass results at 10 and 9.
  - `vbw` and `cl_xfer` the same.
  - At 9, `winfill` and `vfill` spill about 512 MB earlier.
  - `RESTORED` = yes or no the same in both phases. Yes means the board resets the size in S3, so the kext's restore
    is what keeps sleep working.

```bash
#!/bin/bash
# SPDX-License-Identifier: MIT
# rebar-window.sh info | bench | loop N M  (DESIGN-rebar-window.md section 6): the B580's BAR window at the current
# ResizeAppleGpuBars, its upload/window tests, and N sleep/wake cycles of M minutes with checks after each wake.
# Needs passwordless sudo for pmset and debug.intelxe.step (sleeptest.sh's set-up). Each test is time-boxed by tmo.
INTELXE_HOME=${INTELXE_HOME:-$HOME/b580}
K=${REBAR_KIT:-$INTELXE_HOME}
set -u
tmo() {   # tmo <seconds> <command...>: run it, kill it after <seconds>; its exit code (137 = killed)
	local s=$1; shift
	"$@" & local p=$!
	( trap 'kill $sp 2>/dev/null; exit 0' TERM; sleep "$s" & sp=$!; wait $sp; kill -9 $p 2>/dev/null ) > /dev/null 2>&1 &
	local w=$!
	wait $p; local r=$?
	kill $w 2>/dev/null; wait $w 2>/dev/null
	return $r
}
run() {   # run <seconds> <binary> [args]: from the kit, skipped when missing
	local s=$1 b=$2; shift 2
	[ -x "$K/$b" ] || { echo "$b: missing in $K, skipped"; return 0; }
	tmo "$s" "$K/$b" "$@"; local r=$?; [ $r -ne 0 ] && echo "$b: exit $r"; return 0
}
win() { sysctl -n debug.intelxe.vram 2>/dev/null | sed -nE 's/^window ([0-9]+) MB.*/\1/p'; }
O=$INTELXE_HOME/rebar/w$(win); mkdir -p "$O"
pcidump() {   # BAR registers and the Resizable BAR capability (step 7; it also empties the say buffer)
	sudo -n sysctl -w debug.intelxe.step=7 > /dev/null && sysctl -n debug.intelxe.state | grep -E 'BAR2|current'
}
bar2size() { sed -nE 's/.*BAR2: current ([0-9]+) MB.*/\1/p' "$1" | head -1; }
bar2reg() { sed -nE 's/^BAR2 \(0x18\) (0x[0-9a-f]+).*/\1/p' "$1"; }
stat() { sysctl -n debug.intelxe.stats 2>/dev/null; }
resets() { stat | sed -nE 's/.*GuC context resets ([0-9]+); GT resets ([0-9]+).*/\1 \2/p'; }
busy() { [ -x "$INTELXE_HOME/wsitest/gpu_busy.sh" ] && "$INTELXE_HOME/wsitest/gpu_busy.sh"; }
case ${1:-info} in
info)
	echo "$(date)  ResizeAppleGpuBars window $(win) MB"
	pcidump
	sysctl -n debug.intelxe.vram | head -2
	stat | grep -E 'system sleep|CAT errors|resets' ;;
bench)
	if b=$(busy); then echo "skipped: GPU busy ($b)"; exit 0; fi
	{ echo "== $(date) window $(win) MB"; sysctl -n debug.intelxe.vram | head -2
	  echo "== vbw";      run 60 vbw
	  echo "== cl_xfer";  run 60 cl_xfer
	  echo "== winfill";  run 60 winfill fill | tail -4
	  echo "== vfill";    run 60 vfill 1152 | tail -3
	  echo "== surfvram"; [ "$(sysctl -n debug.intelxe.surfvram)" -ge 1 ] && run 60 surfvram | tail -1
	  echo "== mtl_tests"; run 60 mtl_tests | tail -1
	  sysctl -n debug.intelxe.vram | head -2; } 2>&1 | tee "$O/bench-$(date +%H%M).txt" ;;
loop)
	N=${2:-10} M=${3:-3}
	if [ -z "${REBAR_CHILD:-}" ]; then   # detach: the loop outlives the caller's ssh session and its time box
		REBAR_CHILD=1 nohup bash "$0" loop "$N" "$M" >> "$O/loop.txt" 2>&1 &
		echo "loop of $N x $M min started (pid $!), log $O/loop.txt"; exit 0
	fi
	ws0=$(pgrep -x WindowServer); r0=$(resets)
	for i in $(seq 1 "$N"); do
		pcidump > "$O/pci-pre-$i.txt"
		w0=$(sysctl -n kern.waketime)
		sudo -n pmset schedule wake "$(date -v+${M}M '+%m/%d/%y %H:%M:%S')" || { echo "CYCLE $i: schedule failed"; break; }
		sleep 3; sudo -n pmset sleepnow > /dev/null
		t=0; lim=$(( (M + 5) * 60 ))
		while [ "$(sysctl -n kern.waketime)" = "$w0" ] && [ $t -lt $lim ]; do sleep 5; t=$((t + 5)); done
		[ "$(sysctl -n kern.waketime)" = "$w0" ] && { echo "CYCLE $i: NO WAKE within $lim s"; break; }
		sleep 20   # the display's modeset and WindowServer settle
		sysctl -n debug.intelxe.state > "$O/state-$i.txt"   # before any step: the suspend/resume lines
		pcidump > "$O/pci-post-$i.txt"
		st=$(stat | grep -m1 'system sleep')
		dev=$(sed -nE 's/.*device resume (-?[0-9]+).*/\1/p' <<< "$st")
		cp=$(sed -nE 's/.*restore copy (-?[0-9]+).*/\1/p' <<< "$st")
		rest=no; grep -q 'Resizable BAR size restored' "$O/state-$i.txt" && rest=yes
		s0=$(bar2size "$O/pci-pre-$i.txt"); s1=$(bar2size "$O/pci-post-$i.txt")
		g0=$(bar2reg "$O/pci-pre-$i.txt"); g1=$(bar2reg "$O/pci-post-$i.txt")
		vk=$(run 60 vk_smoke /Library/GPUBundles/nvmtl/libvulkan_intel.dylib 1 2>&1 | tail -1)
		mt=$(run 60 mtl_tests 2>&1 | tail -1)
		vf=$(run 60 vfill 1152 2>&1 | grep -E 'failures|missing' | tail -1)
		ws=$(pgrep -x WindowServer)
		ok=PASS
		[ "$dev" = 0 ] && [ "$cp" = 0 ] && [ "$ws" = "$ws0" ] && [ "$(resets)" = "$r0" ] && [ "$s0" = "$s1" ] &&
			[ "$g0" = "$g1" ] && grep -qE ' 0 failures|missing' <<< "$vf" || ok=FAIL
		grep -qiE 'fail' <<< "$vk $mt" && ok=FAIL
		echo "CYCLE $i $ok: window $(win) MB, BAR2 ${s0}->${s1} MB reg ${g0}->${g1}, RESTORED $rest, device resume $dev," \
			"restore copy $cp, WindowServer $ws0->$ws, resets $r0->$(resets) | vk_smoke: $vk | mtl_tests: $mt | vfill: $vf"
		stat | grep -m1 'CAT errors'
		sysctl -n debug.intelxe.vram | head -1
		sleep 30
	done
	echo "LOOP DONE $(date)" ;;
*) echo "usage: $0 info | bench | loop [N cycles] [M minutes]"; exit 1 ;;
esac
```

Notes for the runner:

- `loop` detaches itself. Poll `grep -E '^CYCLE|^LOOP' $O/loop.txt` in calls of at most 60 s.
- A cycle that never wakes stops the loop. That is a mains-cycle case: report it, do not retry.
- The pre-sleep `pcidump` runs step 7 on purpose: it empties the 4 KB `say` buffer that bring-up may have filled, so the
  suspend and resume lines fit.
- Run `bench` and `loop` with no game or benchmark running (`gpu_busy.sh`); `winfill fill` and `vfill` take the whole
  window for a few seconds.

## 7. Documentation

Changed in this branch, in English and Turkish where both exist:

- `release/templates/README-INSTALL.md` and `.tr.md`, stage 2 item 1: what ocvalidate says and why it does not apply;
- `efi/README.md`;
- `docs/design/DESIGN-efi-build.md` (the old text said ocvalidate "flags any value above 8"; it flags every value but
  0 and -1);
- the reason text of the builder's accepted finding (`validate.py`);
- `docs/ROADMAP.md`: the 1 GB limit's source, and the test plan as an open item.

Not changed:

- `release/README-INSTALL*.md` (the published 0.1.4 guide) and `site/`: the site has its own release-time branch.

Suggested wording for that branch:

`site/guide/efi.html`, stage 2, item 1 (English):

> `Booter > Quirks > ResizeAppleGpuBars = 10`. macOS cannot map the card's 16 GB BAR; this gives it a 1 GB window.
> OpenCore's `ocvalidate` reports this value as "unstable with macOS sleep-wake". The card forgets its BAR size during
> sleep and macOS does not set it again, which Apple's own GPU drivers do not handle. IntelXe.kext sets it again on
> wake, as Linux does, and sleep works on the test machine. The warning is expected; keep 10. (9 gets the same warning
> with half the window; 0, the value ocvalidate accepts, works too, but its 256 MB window is too small for the
> desktop and a game.)

The same, Turkish:

> `Booter > Quirks > ResizeAppleGpuBars = 10`. macOS, kartın 16 GB'lık BAR'ını eşleyemiyor; bu ayar 1 GB'lık bir
> pencere açıyor. OpenCore'un `ocvalidate` aracı bu değer için "unstable with macOS sleep-wake" uyarısı veriyor: kart
> uykuda BAR boyutunu unutuyor ve macOS onu yeniden ayarlamıyor; Apple'ın kendi GPU sürücüleri bununla başa çıkamıyor.
> IntelXe.kext uyanışta boyutu Linux'taki gibi yeniden ayarlıyor ve test makinesinde uyku çalışıyor. Bu uyarı
> beklenen bir şey; 10'da bırakın. (9 aynı uyarıyı yarım pencereyle alır; ocvalidate'in kabul ettiği 0 da çalışır,
> ama 256 MB'lık penceresi masaüstü ve bir oyun için küçük kalır.)

`site/guide/hardware.html` `#bar`: add at the end of each paragraph.

English:

> OpenCore's `ocvalidate` warns about this value; it is expected (see EFI, stage 2).

Turkish:

> OpenCore'un `ocvalidate` aracı bu değer için uyarı veriyor; bu beklenen bir şey (bkz. EFI, 2. aşama).

## 8. Revocable CPU mappings: `debug.intelxe.bar_revoke` (branch `barwin`, 2026-11-13)

> Implemented behind a knob that is off by default (`debug.intelxe.bar_revoke=0`). Built and host-tested only: nothing
> here has run on the B580 yet. Section 8.6 is the test.

Until now a BO the CPU had mapped never left the window (section 2): placement was first come, first served, and once
the window was full of mapped BOs, every new CPU-visible BO went to system memory, the game's included (5 Nov: mapped
buffers that spilled halved Minecraft's frame rate). Linux has no such limit: TTM evicts an mmapped VRAM BO to system
memory (`ttm_bo_unmap_virtual` zaps the user PTEs) and the next CPU access faults it in again from wherever the BO is
(`ttm_bo_vm_fault`). An IOKit user mapping of a physical range has no fault handler a driver can hook. What it has is
`IOMemoryMap::redirect`.

### 8.1 What xnu offers (xnu-12377.1.9, `iokit/Kernel/IOMemoryDescriptor.cpp`, `osfmk/vm/*`)

A user mapping that `IOConnectMapMemory64` makes from IntelXe's descriptor (`boVramDesc`, one physical range of BAR2)
is backed by a device pager (`memoryReferenceCreate`: `device_pager_setup`, one named entry for the whole descriptor).
For one physical run the VM object is `phys_contiguous`: a fault maps a block from `vo_shadow_offset`
(`vm_fault.c`, `pmap_map_block_addr`); the pager is never asked.

The options, and what each does to such a mapping:

| API | what it does | usable? |
|---|---|---|
| `IOMemoryDescriptor::redirect(task, true/false)` (IOFramebuffer/IONDRVFramebuffer around sleep) | removes the user PTEs (`IOUnmapPages`); faults that reach the pager (`handleFault`) sleep while redirected. A `phys_contiguous` object never asks its pager, and a populated private page is mapped again at once. Afterwards only kernel mappings are populated again (`populateDevicePager`). | No: blocks nothing here, and comes back to the same physical pages |
| re-initialising the descriptor (`initWithOptions` on an initialised one: "being retargeted to point to somewhere else") | with `kIOMemoryRedirected` set, keeps the memory reference and mappings; nothing repopulates the user mapping's object with the new ranges (`populateDevicePager` is protected, and run for kernel maps only) | No |
| `IOSubMemoryDescriptor` | forwards `makeMapping` to its parent | No help |
| `IOMemoryMap::redirect(NULL)` | takes a `UPL_BLOCK_ACCESS` UPL on the mapping's object (`blocked_access`: `vm_fault` waits), then removes the user PTEs | Yes: the block |
| `IOMemoryMap::redirect(newBacking)` | `newBacking->makeMapping(... kIOMapUnique \| kIOMapReference)`: `doMap`'s `upl_transpose` swaps the user's VM object with the new backing's (`vm_object_transpose`: pages, pager, `vo_shadow_offset`, `phys_contiguous`, `wimg_bits`), swaps the two descriptors' memory references, re-parents the mapping to `newBacking`, then commits the blocking UPL | Yes, with the right kind of backing (below) |

`kIOMapUnique` is not needed on the original mapping: `redirect` only needs the mapping's descriptor to have one named
entry, which a physical descriptor always has.

Which new backing:

- **A physical descriptor of the new pages.** `makeMapping` then builds a fresh physical descriptor of its own, and its
  device-pager object is populated only after the transpose and after the user's object is unblocked (the
  `redirUPL2` commit comes before `populateDevicePager` in `doMap`). A CPU fault in that window maps physical address 0
  plus the offset: `vo_shadow_offset` is still 0. Not usable.
- **A pageable buffer** (`IOBufferMemoryDescriptor`, `kIOMemoryPageable`): xnu's own case ("with Virtual VRAM, the
  object might not be backed by that device memory anymore", `vm_fault.c`). No pager to populate, no race. But the
  buffer's own kernel address keeps mapping its old object, which after the transpose is the device memory: the BO's
  `sys` (used through `getBytesNoCopy` for user fences, purges, DMA) would need a second kind of backing everywhere.
- **Chosen: a "bridge".** The new pages are a physically contiguous wired buffer (`allocSys(size, true)`), exactly what
  a spilled BO has. A physical descriptor of them is mapped into the kernel (`kmap`): its device-pager object is
  populated at once (kernel mappings are). `bridge` is a kernel-task virtual descriptor over `kmap`'s range: its memory
  reference is a named entry for that already populated object. `redirect(bridge)` transposes the user's object with
  it, so the user's object holds the new pages' `vo_shadow_offset` the moment it is unblocked, and `bridge` has no
  pager of its own, so nothing runs after the unblock. The BO keeps its ordinary `IOBufferMemoryDescriptor`.

### 8.2 The sequence (`boRevoke`, xe_core.cpp; state machine in `xe_barrevoke.h`)

Under the BO's client lock, with the BO marked migrating and its VMs held (`moveSetBegin`: EXEC and VM_BIND wait):

1. **Find the mapping**: `md->createMappingInTask(client task, kIOMapReference)`, the lookup `IOConnectUnmapMemory64`
   does. The client now holds a task reference (`xeClientCreate`), so the lookup never meets a freed task.
2. **Set-up**, nothing visible yet: the new pages, their DMA mapping, `kmap`, `bridge`.
3. **Block** (`redirect(NULL)`): the user PTEs go with their TLB shootdown (whose IPI also drains the CPUs'
   write-combining buffers into the BAR); a CPU access waits in `vm_fault`.
4. **Copy** VRAM to the new pages on the copy engine (`vramCopyToSys`, as `boMoveSys`).
5. **GPU**: the BO's fields point at the new pages, every binding's PTEs are rewritten (`boRemap`).
6. **Redirect** (`redirect(bridge)`): transposed, unblocked.
7. **Verify**: a kernel mapping of what the map now names (`kIOMapUnique`, so a real mapping of its memory reference,
   not the static shortcut), faulted at its first and last page, must resolve (`pmap_find_phys`) to the new pages.
   xnu logs a failed `upl_transpose` and carries on, so success from `redirect` is not proof.
8. **Commit**: the map's descriptor becomes `bridge` (the mapping's parent now: unmaps find it there, and the existing
   "mapping gone = descriptor back to one reference" rule keeps working), the old descriptor and the set-up go, the old
   VRAM is freed after the remap's TLB invalidation (deferred, as every move).

Failures (each path is enumerated by `tests/barrevoke_test.cpp`):

| at | CPU | GPU | new pages | VRAM | feature |
|---|---|---|---|---|---|
| set-up, lookup, busy VM | never blocked | VRAM | freed | kept | on |
| copy failed | unblocked (`redirect(own descriptor)`) | VRAM | freed | kept | on |
| copy late (may still run) | unblocked | VRAM | kept for good | held (`vramCopyToSys`) | on |
| PTE rewrite (no memory) | unblocked | back to VRAM | freed | kept | on |
| redirect refused (map still on the old descriptor) | unblocked by `redirect` | back to VRAM | kept for good | kept | latched off |
| verify: still VRAM | on VRAM | back to VRAM | kept for good | kept | latched off |
| verify impossible (no memory for the check, 3 tries) | probably new pages | stays on new pages | the BO's | kept for good | latched off |
| verified | new pages | new pages | the BO's | freed | on |

"Latched off" stops every revocation until `debug.intelxe.bar_revoke` is written again; it is logged in the G2H log and
shown in `debug.intelxe.vram`.

### 8.3 Safety argument

- **No CPU or GPU view ever reaches freed memory.** The old VRAM is freed only after the verification proves the
  user's object resolves to the new pages, and after the GPU's PTEs were rewritten and invalidated. The new pages are
  freed only on paths where no redirect touched the mapping (the descriptors that could name them are dropped first);
  wherever a descriptor may still name them, they are kept for good and counted.
- **No other mapping of the BO's VRAM exists.** Only BOs with exactly one live CPU map are taken, owned by the
  revoking client, not by handle (old user space), no map of a closed handle, and the map's descriptor held by exactly
  the map entry and the mapping (anything more is IOKit mapping or unmapping it right now; new requests wait for the
  client lock). Not shared (another client's handle, a GEM name, a PRIME export), not pinned for scan-out, not a
  SCANOUT BO. Every reference is accounted for (`refs == handles + binding records + the map`), which excludes pending
  user-fence stores through the BAR (`ufAsyncAddBo` holds the BO), deferred frees and lingering maps. The kernel's own
  BAR2 mapping is not used for such BOs outside those paths.
- **No lost write.** The CPU is blocked before the copy reads the VRAM and unblocked only once the user's object
  points at the copy; the GPU's VMs are idle (in-flight jobs pin a BO: nothing is waited for) and held until the PTEs
  point at the copy. Compressed BOs (CCS has no system-memory equivalent) are not taken.
- **No new deadlock.** The CPU block is bounded by the copy (2-5 s timeouts, then the rollback). A thread blocked in
  `vm_fault` holds no driver lock it could need: the driver's `copyin` / `copyout` run without its global locks held
  (a scan of xe_core.cpp found none between a global lock and its unlock), and the revoking thread already holds the
  BO's client lock. The eviction pass only try-locks other clients; the forced sysctl locks them one at a time, as
  `debug.intelxe.evict` does.
- **Cache attributes.** The user's object takes the new pages' object's `wimg` (write-back, the default for RAM).
  A dGPU's system-memory accesses are coherent with the CPU's caches (`xe_ttm_tt_create`: "DGFX GPU system memory
  accesses are always coherent with the CPU"), so a BO created write-combined and bound with a non-coherent PAT still
  reads right. A second map of the same cookie is no longer asked for write-combining (it would change the cache mode
  under the existing mapping).
- **Off by default**: with `bar_revoke` 0 the only changes are a task reference per client, a touch timestamp per BO
  and, in `ufAsyncAddBo`, a lookup that also accepts cookie maps of system BOs (which only revocation creates).

### 8.4 Policy

- **When**: `evictWindow` (GEM_CREATE of a CPU-visible BO, or a map of a BO beyond the window) found nothing the
  ordinary eviction could move. `debug.intelxe.bar_revoke_now=<pid>` forces it for a process (tests; any knob value,
  no hot window).
- **What**: CPU-mapped window BOs that pass `xeRevokeCheck`: contiguous, at most `bar_revoke_max_mb` (16 MB: the system
  pages must be physically contiguous), cleared, idle, and not touched in the last `bar_revoke_hot_ms` (1 s).
- **Order**: least recently touched first. A touch is a GEM_MMAP_OFFSET or a map of the BO, or an EXEC on a VM that
  binds it (`XeVm::execNs`, only recorded while the knob is on): the kernel cannot see CPU loads and stores, so an
  active process's BOs all count as recent, and a background process's as old, which is the 5 Nov case. Over several
  clients without holding two client locks at once: one pass collects candidates and computes the cut-off of the
  oldest that cover the need (`xeRevokeCutoff`), a second revokes those at or before it, the caller's own first.
- **Where they go**: system memory, for good, as a spilled BO (xe would fault it back into VRAM on the next CPU
  access; here a later move back would need the reverse redirect, which is possible but not done).

### 8.5 Counters

`debug.intelxe.vram`, line `bar_revoke`: knob, latched, passes (forced), candidates, tried, moved (MB), rolled back,
broken, undecided, time the CPU was blocked (total, max), set-up / lookup failures, pages and VRAM kept, last error,
each state-machine event, and the scan's reasons for not taking BOs. Per process: `system N MB (M, revoked K)`.

### 8.6 Test on the B580

- Host: `c++ -std=c++17 -Wall -Wextra -I kernel/IntelXe -o barrevoke_test kernel/IntelXe/tests/barrevoke_test.cpp`.
- `user/tests/kernel/barrevoke.c`: fills the window with 4 MB CPU-mapped BOs (CPU and GPU patterns), revokes them
  (`now`: `bar_revoke_now`, root; `natural`: knob 1 and more mapped BOs than fit, from a second VM so that their EXECs
  do not make the old ones recent), then checks through the old pointers: the old patterns, CPU write -> GPU copy ->
  CPU read, GPU store -> CPU read, and that every `IOConnectUnmapMemory64` succeeds and the maps are counted gone.
- `kernel/IntelXe/tests/night-barrevoke.sh` (`phase_barrevoke`): `now 4` at knob 0, then knob 1: `natural 4`,
  `now 16`; WindowServer, resets and CAT errors unchanged; knob back to 0.

What to look at first: `verify ok` equal to `redirect ok` and `broken 0`. A `redirect refused` or `verify fail` means the
transpose does not happen on this macOS; the feature latches off and nothing is lost.

## 9. More than 1 GB: research path (not implemented)

The window is 1 GB because IOPCIFamily caps a BAR at `MAX_BAR_SIZE` (section 1). A larger window would make most of
section 8 unnecessary; it would need, all of it outside plain IOKit:

1. **IOPCIFamily**: `probeBaseAddressRegister` (`IOPCIConfigurator.cpp` lines 2773-2778) must keep a 64-bit size above
   1 GB instead of truncating it and probing the upper dword as a BAR of its own. That is a binary patch of the shipped
   IOPCIFamily (a Lilu plug-in routing that function, or patching the compare against `0x40000000`), per macOS build.
   IntelXe.kext is plain IOKit by design, so this would be a separate, optional kext.
2. **Address space**: the range allocator then has to place a 16 GB BAR (or 4/8 GB) in the root bridge's 64-bit
   aperture. The firmware already sized it with Above 4G Decoding and Resizable BAR on (`ResizeAppleGpuBars = -1`
   keeps that size); whether IOPCIFamily keeps the firmware's assignment or re-allocates (`kIOPCIConfiguratorNoReallocation`
   style boot flags) decides whether the bridge windows fit.
3. **IntelXe**: `bringUp` maps BAR2 whole into the kernel (`mapDeviceMemoryWithRegister`); 12 GB of write-combined
   kernel mapping would cost its page tables, so the kext would map BAR2 in pieces (the GTT/LRC part at bring-up, user
   BOs through their own descriptors, as now). `vramVisible` would equal the usable VRAM and `gVis`/`gHi` become one.
4. **Sleep**: `gpuResume` already restores the Resizable BAR size; a 16 GB BAR must also get its 64-bit address back
   before IOPCIFamily's config restore is trusted.
5. **Risks**: every macOS update can move the patched code; a second GPU with Apple's driver gets the same cap removed;
   hibernation was never tested; OpenCore's `ocvalidate` calls values above 10 unsupported, so the EFI side would use
   -1 (clean) plus the patch.

Order of work if it is ever taken up: read IOPCIFamily for every other place that keeps BAR sizes in 32 bits; a
debug-only Lilu plug-in on the test machine; `ioreg` must show BAR2 assigned at the firmware's address with its full
size; then IntelXe's piecewise mapping.
