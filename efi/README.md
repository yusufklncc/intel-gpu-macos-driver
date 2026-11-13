# OpenCore notes

This directory holds notes and the source of one ACPI table (`SSDT-ARCOFF.dsl`): no `config.plist` and no SMBIOS data. These are the settings the IntelXe stack depends on, as used on the test machine: an Intel Z790 desktop with an i7-14700K and an Arc B580 in the x16 slot (the CPU's integrated GPU is not used), running macOS 26 (x86_64) through OpenCore 1.0.8. The step-by-step install is `release/README-INSTALL.md`; the test machine's whole configuration, without its serial numbers, is in `sample-z790/`.

## Firmware (BIOS)

- **Above 4G decoding and Resizable BAR on.** The firmware then gives the B580 a 16 GB BAR2.

## Booter quirks

- **`ResizeAppleGpuBars = 10`** (2^10 MB = 1 GB). macOS cannot map a 16 GB BAR, so without this quirk BAR2 stays unassigned. With `0` (256 MB) the driver also works, but the CPU-visible VRAM window is smaller. IntelXe.kext reads the window size from BAR2's length. VRAM beyond the window is used for buffers the CPU does not map, and allocations that do not fit spill to system memory.
  - OpenCore's `ocvalidate` reports 10 as "unstable with macOS sleep-wake" (it is silent only for `0` and `-1`; 1-8 are "not useful", 9 and 10 "unstable"). Sleep resets the card's Resizable BAR size and macOS's PCI driver does not restore it, which Apple's own GPU drivers do not handle. IntelXe.kext restores it on wake before it touches BAR2 (as Linux's `pci_restore_rebar_state`), and sleep works on the test machine. The quirk applies to every GPU in the PC, so a second GPU driven by Apple's drivers would be exposed. Research, options and a test plan: `docs/design/DESIGN-rebar-window.md`.

## UEFI output

- **`ReconnectGraphicsOnConnect = false`.** With `true`, OpenCore re-binds the graphics drivers while it connects
  drivers, and its RELEASE build then sets up the console before the B580's GOP is back ("OCC: Missing compatible GOP"
  in the OpenCore log). macOS boots without a console framebuffer, the kernel never enables the boot screen, Lilu waits
  for that to complete its start and never does, VirtualSMC's SMC device is not hooked into AppleSMC, there is no
  `SMCWatchDogTimer`, and `watchdogd` panics the boot after about 20 seconds (`watchdogd ... exited`). OpenCore's DEBUG
  build is about a second slower before the kernel and hides it. With `false`, OpenCore, Lilu and VirtualSMC RELEASE
  boot cleanly (6 of 6 boots on the test machine, 4 of them warm restarts).
- `ProvideConsoleGop = true`, `Resolution = Max` as in Dortania's guide.

## Kernel quirks

- **`DisableIoMapper = true`.** Keep it.
  - IntelXe.kext's DMA goes through `IODMACommand` and is ready for VT-d.
  - Other kexts on such systems are not (AppleIGC, for example, uses raw physical DMA).

## ACPI

- **An SSDT that sets the card's `class-code` to `FFFFFFFF`** (here: `SSDT-ARCOFF`; `efi.sh` and the app write the card's own ACPI path into a copy of it). It keeps `IONDRVFramebuffer` and Apple's display drivers off the card.
  - `IOPCIMatch` still lets IntelXe.kext match.
  - Once IntelXeFB holds the IOFramebuffer category, it writes a display class code back to the PCI nub, because IOPresentment's GPU node match needs one.

## Kext placement

| Kext | Where | Why |
|---|---|---|
| `IntelXe.kext` | OpenCore `Kernel > Add` (EFI), `OSBundleRequired = Root` | plain IOKit, no Lilu; matches `8086:e20b` (B580), `8086:e20c` (B570), `8086:e211` (Pro B60) and `8086:e212` (Pro B50), brings the GPU up at start; BMG-G31 (`8086:e220`-`e223`, Pro B70 = `e223`) in a personality of its own, refused at start unless `-intelxe_allow_g31` is set |
| `HybridPM.kext` (optional) | OpenCore `Kernel > Add` (EFI), below `Lilu.kext` | Lilu plugin for 12th-14th gen hybrid Intel CPUs (`kernel/HybridPM/`); `efi.sh` and the app add it where the CPU needs it |
| `IntelXeFB.kext` | `/Library/Extensions` + Auxiliary KC | links IOGraphicsFamily, which is not available to OpenCore-injected kexts at that stage; matches at boot from the AuxKC |
| `NMXAccel.kext`, `NMXAGDC.kext` | `/Library/Extensions` + Auxiliary KC | IOAccelerator and AppleGraphicsDeviceControl services for WindowServer |
| `NVMTLDriverLib.bundle` | `/Library/GPUBundles/` | Metal driver plugin; `/Library/GPUBundles/nvmtl/` holds Mesa's `libvulkan_intel.dylib` and the codecs, `/Library/GPUBundles/nvmtl-allow.txt` the allow-list. The optional system-volume copy (Safari WebGL) is `/System/Library/Extensions/NVMTLDriver.bundle` |

Rebuild the AuxKC after changing any of the AuxKC kexts (`kmutil create` with all `/Library/Extensions` kexts, as `install.sh --apply` does), then reboot.

**Recovery:** Shift+Enter in the OpenCore picker boots safe mode, which skips the AuxKC. Keep a known-good EFI on a USB stick.

## Security settings

- **SIP must allow unsigned kexts:** `csr-active-config` `0x803` (custom), so no approval dialog appears.
- **Boot-arg `amfi_get_out_of_my_way=0x1`:** WindowServer accepts the ad-hoc-signed Metal plugin only with it. Meant
  to go once the plugin is signed properly. (The development machine also had `amfi=0x80` for the virtual HID test
  tools' entitlements; the driver does not need it.)

## Driver boot-args (all optional)

| Boot-arg | Effect |
|---|---|
| `intelxe_surfvram=N` | VRAM-shared IOSurfaces: 0 off, 1 applications, 2 applications and WindowServer (the install guide sets 2); 3 is accepted, the kernel treats it as 2 and reports the level to the Metal plugin |
| `intelxe_svwsfull=N` | 1 (default): WindowServer only imports, never makes, a VRAM backing of at least one scan-out frame; 0: it may make them |
| `intelxe_fds=1` | object file descriptors (syncobj fds, sync files, PRIME) through `/dev/intelxeobj`; off by default |
| `-intelxe_off` | IntelXe.kext does nothing |
| `-intelxe_allow_g31` | for testers: IntelXe.kext starts on a BMG-G31 card (Arc Pro B70, `8086:e220`-`e223`), which is not supported and has never run this driver; it logs a warning. Without it the card is refused at start. NMXAccel does not match G31, so there is no Metal on it yet ([DESIGN-platforms.md](../docs/design/DESIGN-platforms.md#how-to-enable-another-bmg-card)) |
| `-intelxe_nometal` | no `nmxgpu-claimed` / NMXAccel |
| `-intelxe_novideo` | no media GT |
| `-intelxe_noaccel` | no in-kernel arming of acceleration |
| `-intelxe_lazy` | bring-up on first use instead of at start |
| `-intelxe_nohugepages` | 4 KB page-table leaves only |
| `-intelxe_noexttag` | PCIe Extended Tags not turned on (they lift host-to-VRAM copies; registry `IntelXePCIeExtTag` says what was done) |
| `-intelxe_nofb` / `-intelxefb_off` | IntelXeFB stays off |
| `-intelxefb_noscale`, `-intelxefb_norates`, `-intelxefb_novrr`, `-intelxefb_nohwcursor`, `-intelxefb_nogamma`, `-intelxefb_noirq`, `-intelxefb_noedidfix` | turn off single IntelXeFB features |
| `intelxefb_vrr=`, `intelxefb_vrrmodes=` | VRR policy |
| `-nvoff` | NMXAccel stays off |
| `nmxgamma=`, `nmxearlyreg=` | NMXAccel gamma / early registration |

NVRAM variables: `intelxefb-vrr-modes` (Adaptive-Sync modes, the "Variable" refresh in Displays: on by default from 0.1.3; `0` turns them off, as does the boot-arg `intelxefb_vrrmodes=0`), `intelxefb-hidpi` (the opt-in HiDPI modes, 0.1.3; also the boot-arg `intelxefb_hidpi=1`), `intelxe-guard` / `intelxe-accel-off` (panic-loop guard of the boot-time arming). From 0.1.4 the app's *Power and app* and *Display* pages write the user settings the kexts read at start: `intelxe-gpu-profile`, `intelxe-gpu-min-mhz`, `intelxe-gpu-max-mhz`, `intelxefb-hwcursor`, `intelxefb-dpms`, `intelxefb-vrr-lfc`, `intelxefb-vrr-floor`.

Before 0.1.0 (private builds) these boot-args started with `b580` (`-b580off`, `b580surfvram=`, `-b580fbnovrr`, ...; the full list is in <!-- intelxe-rename: keep -->
`kernel/IntelXe/uapi/intelxe_bootargs.h`). Release 0.1.x still reads an earlier name when the new one is absent and
logs once that it is deprecated; later releases read only the names above. The NVRAM variables have only the new names. <!-- intelxe-rename: keep -->
