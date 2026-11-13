# A desktop for a held boot: the firmware framebuffer

> Design note with its date (2026-11-13, branch `bootguard`): the plan and the state of that day. Untested on
> hardware; section 6 is the test.

## 1. The problem

The boot guard (0.1.2, IntelXeFB `guardBegin`) counts accelerated boots that end inside their first minute in NVRAM
`intelxe-guard` (`accel<n>`). From two in a row NMXAccel arms with its safest settings ("safe": no async swaps, no
flips; an accelerated login window, tested 5 Nov). From four it writes `intelxe-accel-off` and acceleration is held
on every boot until that is cleared ("held"; also `-intelxe_noaccel` and safe mode). A held boot ran IntelXeFB
alone, and on macOS 26 that stays at the Apple logo: macOS runs (SSH works) but WindowServer never draws. The only
way out without SSH was OpenCore's Reset NVRAM, and the user could not reach the IntelXe Manager to see why or to
repair.

## 2. What WindowServer needs on macOS 26 (what was measured)

From the 4-5 Nov 2026 bring-up and the held-boot tests on the B580 (project notes; nothing new was measured for this
note):

| Configuration | Result |
|---|---|
| macOS installer and first setup, no SSDT-ARCOFF, none of our kexts: Apple's IONDRVFramebuffer on the GOP's surface | desktop, unaccelerated |
| 4 Nov: IntelXeFB (class `B580IntelFramebuffer`) + NMXAGDC idle, NMXAccel not in the AuxKC, no Metal plugin installed | login window and desktop, unaccelerated |
| 5 Nov held boot (`intelxe-accel-off` set): IntelXeFB, NMXAccel registered but never armed | logo; WindowServer opened the display pipe and set the display offline |
| the same, NMXAccel not registered (test5), NMXAccel not started at all and no `IOAccelTypes` on IntelXeFB (test6) | logo |
| the same with NMXAGDC's device control brought up by hand and a WindowServer restart | logo |
| the same, then everything armed by hand (agdc, nmxaccelfb 1/3/4, iop) and WindowServer restarted | Metal compositor and `kIOWindowServerActiveAttribute` set, still the logo, 0 frame calls: a late arm is not the boot-time arm |

The held boots logged "Software compositor activated", CoreDisplay "Unable to find display pipe for IOFB service",
no "Display woke on GPU", and WindowServer never set `kIOWindowServerActiveAttribute` on IntelXeFB. So WindowServer
on 26 can run unaccelerated on a plain IOFramebuffer (the installer, and the 4 Nov boot show it), but on IntelXeFB
as it is published since the accelerated path exists (an "Intel" class name, which makes CoreDisplay take the
IOPresentment display-pipe path; the display class written back on the PCI device; `NVRMTiming`, VRR and EDID modes,
the stored display configuration of accelerated boots) it waits for a display pipe that only the boot-time arm
provides. Which of these differences decides it was not isolated: each experiment costs a held boot on the test
machine. The approach below does not depend on knowing it.

## 3. Options

1. **A minimal "dumb" IntelXeFB for held boots** (a class name without a vendor, one mode, no VRR, no cursor, no
   timing for NMXAGDC, the class code left as SSDT-ARCOFF set it). Plausible, but a guess at the unknown difference
   above, and every guess needs a held boot to check.
2. **A dumb accelerator path WindowServer accepts** (NMXAccel registered, armed with a CPU-copy pipe and no GPU
   submission). The late manual arm on 5 Nov showed WindowServer composite and still not present; the boot-time arm is
   what works, and arming at boot is exactly what the guard held because it crashed. Rejected.
3. **macOS's own fallback: the firmware framebuffer.** The path macOS takes on a GPU it has no driver for (the
   "VESA" mode of older hackintosh guides): `IONDRVFramebuffer` with its built-in `IOBootNDRV`, which publishes the
   surface the firmware lit (the boot console's framebuffer, found inside one of the device's BARs, `IOBootNDRV::
   fromRegistryEntry` in IOGraphics' IONDRVFramebuffer.cpp). It is what the macOS 26 installer ran on with this exact
   card. SSDT-ARCOFF keeps it off the card today ("#display" name, `IOName` "#display", class-code `ffffffff`), because
   it would take the IOFramebuffer match category from IntelXeFB and its controller held system sleep for 90 s.
   **Chosen.** The only new mechanism is undoing SSDT-ARCOFF's identity for one boot, which is a registry change.

There is no other unaccelerated framebuffer on macOS 26 to fall back to: OpenCore offers nothing at run time (its
GOP is gone once boot.efi exits; `-igfxvesa` is WhateverGreen's switch for Intel iGPUs, and it, too, ends in
IONDRVFramebuffer).

## 4. The design

IntelXe.kext (in the EFI, loaded before the AuxKC) decides the boot's guard state before anything is offered to
IntelXeFB:

1. **Rules in one place**: `kernel/IntelXe/uapi/intelxe_bootguard.h` (`decide`, `heldDisplay`,
   `guardAfterFirmwareHold`), plain code with no kernel calls. IntelXeFB's `guardBegin` now uses it (same behaviour;
   one deliberate change: an empty `intelxe-accel-off` counts as absent, as NMXAccel always read it). NMXAccel's
   `nmxGuardHolds` keeps its own copy (it is built separately); the host test checks the two agree.
2. **`IntelXe::bootGuardDecide`**, on the bring-up thread after IODTNVRAM is up (waited for up to 10 s, as the
   shutdown breadcrumb already was): reads `intelxe-guard`, `intelxe-accel-off`, `intelxe-held-display`, the
   boot-args `-intelxe_noaccel`, `-x` and `intelxe_helddisplay`. State: normal, retry, safe, held. A held boot shows
   the **firmware framebuffer** unless `intelxe-held-display=fb` (or `intelxe_helddisplay=1`) asks for IntelXeFB.
   Because IntelXeFB will not start, IntelXe writes what its `guardBegin` would have: the hold by count to
   `intelxe-accel-off`, and `intelxe-guard=ok` (the count is in `intelxe-accel-off` from then on, so clearing that
   alone still ends the hold, as before). Kernel writes reach flash at a clean shutdown only; after a power cut the
   old `accel4` holds the next boot the same way (tested in the host test).
3. **`IntelXe::bootGuardHandOff`**, after the bring-up loop (whether it worked or not) and before `IntelXeReady`:
   `nmxgpu-claimed` false (NMXAccel does not start), the PCI device's `name` and `IOName` back to `display`, class-code
   `03 00 00`, `IntelXeHeldDisplay` on the device, `registerService()`, `waitQuiet` up to 15 s, then it looks for the
   IOFramebuffer client. IONDRVFramebuffer (`IONameMatch display`, score 20000, category IOFramebuffer) takes the
   card. When `IntelXeReady` re-registers the device, IntelXeFB is in the same category and is not started; NMXAccel
   refuses an unclaimed GPU; NMXAGDC stays idle (it waits for an IntelXeFramebuffer). The GPU itself is still up and
   draws nothing. If IONDRVFramebuffer does not attach, `handoff=` says so and IntelXeFB starts unaccelerated, as in
   0.1.4.
4. **Visible state**: `sysctl debug.intelxe.bootguard` (and the `IntelXeBootGuard` property on IntelXe), key=value
   lines: `state`, `held_by` (count, accel-off, boot-arg, safe-mode), `reason`, `accel_fails`, `fb_fails`, `previous`,
   `held_display`, `display` (intelxefb, firmware, intelxefb-unaccelerated), `handoff`. IntelXeFB adds
   `IntelXeFBGuardState`. The IntelXe Manager's Overview page has a *Boot guard* section with the state and *Clear the
   boot guard*, which now writes `intelxe-accel-off=ok intelxe-guard=ok` (both, so a power cut in the held boot does
   not bring the hold back); the banner says when the boot runs on the firmware framebuffer. The diagnostics report
   prints the sysctl and warns `held-firmware`. The guard's LaunchDaemon (`guard-clear.sh`) stops waiting at once on
   such a boot (no IntelXeFB to clear). The macOS update guard clears both variables too.

Timing: IONDRVFramebuffer appears after the bring-up (about 15-20 s into the boot, the same moment IntelXeFB appears
on a normal boot), so WindowServer meets a late framebuffer exactly as it does today. Until then the firmware's
picture (the Apple logo) stays.

## 5. Risks and limits

- **Not run on hardware.** The registry change has to make IONDRVFramebuffer match late (a second `registerService`
  on a nub whose IOFramebuffer category is free). IOKit re-runs matching for unmatched categories on
  `registerService`; IntelXe already relies on that for IntelXeFB and NMXAccel. IONameMatch on an IOPCIDevice
  compares the `name` property (`IODTCompareNubName`), which is what is restored.
- **WindowServer might still not draw on IONDRVFramebuffer** with our other parts installed (the Metal plugin on the
  system volume, NMXAGDC loaded, the display configuration saved by accelerated boots). The installer had none of
  them. If so, section 6 step 7 collects what is needed; the way out stays Reset NVRAM, as today.
- **Sleep**: SSDT-ARCOFF exists partly because IONDRVFramebuffer's controller held system sleep for 90 s on this card.
  A held boot is for repairing; the Manager says not to sleep it. On a wake IntelXe clears VRAM below 32 MB (the
  firmware's surface) as it does today.
- **One resolution** (the GOP's), no acceleration, no Metal, no HiDPI, no Adaptive-Sync, no hot-plug.
- The renamed device (`display`) is offered to every driver once more; in the installer nothing but
  IONDRVFramebuffer took it.
- A panic inside the hand-off would repeat on every held boot: Reset NVRAM clears the hold (normal boots never run
  the hand-off), or `intelxe-held-display=fb`, or the USB stick's EFI.
- Safe mode: IntelXe.kext is `OSBundleRequired Root`; if it loads in safe mode, safe mode now gets the firmware
  framebuffer too (before, nothing drove the display). Not checked.

## 6. Hardware test (B580, by hand; nothing runs by itself)

Before: Remote Login on; a second machine with SSH; the USB stick keeps the proven EFI (F8); OpenCore's picker shows
Reset NVRAM (Space). Each remote command is short (under a minute). Build: `build.py --release` for both kexts;
IntelXe.kext into the SN550 EFI (`efi.sh` / the EFI page), IntelXeFB.kext with `install.sh --apply` (the AuxKC is
rebuilt), restart. Keep `TALLogoutSavesState`/`LoginwindowLaunchesRelaunchApps` false before each restart.

1. **Normal boot.** `sysctl debug.intelxe.bootguard`: `state=normal`, `display=intelxefb`, `handoff=not needed`.
   Accelerated desktop as before; the Manager's Overview shows *Boot guard: Normal*.
2. **Held by `intelxe-accel-off`.** `sudo nvram intelxe-accel-off=test`, restart. Expect the Apple logo for about
   20 s, then the login window on the firmware framebuffer (no transparency, 2560x1440 at the GOP's rate). Over SSH:
   `sysctl debug.intelxe.bootguard` (`state=held`, `held_by=accel-off`, `display=firmware`,
   `handoff=IONDRVFramebuffer attached`); `ioreg -rc IONDRVFramebuffer -d1 | head -20`;
   `ioreg -rc IntelXeFramebuffer -d1 | head -3` (nothing); `kmutil showloaded --list-only | grep -i -e nmx -e intelxe`;
   `log show --last boot --style compact --predicate 'eventMessage CONTAINS "boot guard"' | tail -5`. Log in; the
   Manager shows *Acceleration held* with the reason `test`. Do not sleep the machine.
3. **Clear from that desktop.** The Manager's *Clear the boot guard* (password), then restart. Expect a normal
   accelerated boot; `nvram intelxe-accel-off intelxe-guard` both `ok`.
4. **Held by the count.** `sudo nvram intelxe-guard=accel4`, restart. Expect step 2's desktop with `held_by=count`;
   `nvram intelxe-guard` reads `ok` and `intelxe-accel-off` reads "4 accelerated boots in a row ended inside their
   first minute" (the kernel's writes). Restart once more without clearing: still held, now `held_by=accel-off`.
   Clear over SSH with `sudo nvram intelxe-accel-off=ok intelxe-guard=ok`, restart: normal.
5. **Safe state shown.** `sudo nvram intelxe-guard=accel2`, restart: accelerated login window (as on 5 Nov),
   `state=safe`, the Manager shows *Safe settings*; after a minute on the desktop `nvram intelxe-guard` is `ok`.
6. **The old behaviour on request (optional).** `sudo nvram intelxe-accel-off=test intelxe-held-display=fb`,
   restart: the logo stays (0.1.4's held boot), `display=intelxefb-unaccelerated`. Undo over SSH:
   `sudo nvram intelxe-accel-off=ok intelxe-held-display=ok`, restart.
7. **If step 2 stays at the logo**: it is not a hang; over SSH collect `sysctl debug.intelxe.bootguard`,
   `ioreg -rc IOFramebuffer -d1 | grep -E 'class|IOFBDependent' | head`,
   `ioreg -l -w0 | grep -E '"(name|IOName|class-code)" = .*(display|0300)' | head`,
   `log show --last boot --style compact --predicate 'process == "WindowServer"' | grep -iE 'display|framebuffer|compositor' | tail -40`,
   then `sudo nvram intelxe-accel-off=ok intelxe-guard=ok && sudo reboot`. Without SSH: Reset NVRAM in the picker.
   A panic loop: Reset NVRAM (clears the hold), or F8 and the USB stick's EFI.

Recovery in every case: `sudo nvram intelxe-accel-off=ok intelxe-guard=ok intelxe-held-display=ok` and restart, or
Reset NVRAM, or the USB stick.

## 7. Files and tests

`kernel/IntelXe/uapi/intelxe_bootguard.h` (rules), `kernel/IntelXe/IntelXe.cpp` (`bootGuardDecide`,
`bootGuardHandOff`, `debug.intelxe.bootguard`), `kernel/IntelXeFB/IntelXeFB.cpp` (`guardBegin` on the shared rules,
`IntelXeFBGuardState`), `release/templates/install.sh` (`guard-clear.sh`), `app/Sources/Model.swift`
(`BootGuardStatus`), `Views.swift` (*Boot guard* section, the clear), `Health.swift`, `SelfTest.swift`,
`app/Resources/osupdate-guard.sh`, `tools/report/collect-macos.sh`.

- `kernel/IntelXe/tests/bootguard_test.cpp` (550 checks): parsing; `decide` against IntelXeFB's 0.1.4 rules and
  NMXAccel's over every combination of guard value, `intelxe-accel-off` and the two boot-args; the held display
  setting; several boots in a row (crashes to safe and held, the held boot's writes, clearing from its desktop, a
  power cut that loses them, the old one-variable clear). A mutation of the hold count or of the guard clear fails it.
- `IntelXeManager --selftest` (the status parser and texts; the `system-held` snapshot),
  `app/tests/test-osupdate-guard.sh` (both variables cleared), `release/test-install-flow.sh` (the daemon script
  parses).
- Kext builds: no new warnings (22 IntelXe, 24 IntelXeFB).
