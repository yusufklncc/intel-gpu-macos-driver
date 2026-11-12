# Xe probe stick

A USB stick that a tester boots on their PC to collect what we need to bring up an Intel GPU, without installing
macOS, Linux or anything else. It runs Linux from RAM, lets Linux's own `xe` (or `i915`) driver start the GPU while
the kernel records every register access the driver makes (mmiotrace), and writes one report to the stick. The tester
uploads that report to a GitHub issue.

It is the wide data-collection step before XeProbe (`docs/PROBE-MODE.md`): XeProbe answers "does our code drive this
GPU" inside macOS; the stick answers "what does this GPU look like, and what does Linux do to start it" on any PC that
boots from USB.

## For testers

1. **Write the stick** (it erases the stick; 256 MB or larger):
   - macOS or Linux: `sh tools/probe-stick/write-stick.sh --list`, then
     `sh tools/probe-stick/write-stick.sh xe-probe-stick-VERSION.img disk4` (Linux: `sdb`). It refuses disks that are
     not USB removable media (internal disks, USB hard disks and SSDs, Thunderbolt disks), the system disk, partitions,
     virtual disks and disks over 128 GB (`--allow-large`), asks you to type the disk's name, and reads the stick back
     to compare.
   - Windows: **IntelXe Manager**, Report page, *Probe stick* (`windows/app/README.md`, *How the probe stick works*). Put
     `xe-probe-stick-VERSION.img` and its `.img.sha256` beside `IntelXeManager.exe` (or choose the image), choose the
     stick, type its name and size to confirm; Windows asks for administrator rights, and the app writes the image raw
     and reads it back. The same rules as `write-stick.sh` (below), and a stick that holds an EFI (OpenCore, an EFI
     system partition) needs a box of its own ticked. balenaEtcher, or Rufus in "DD image" mode, work too, without
     those checks.
2. **Turn Secure Boot off** in the firmware setup. The stick's boot loader is not signed (no shim), so with Secure Boot
   on the firmware will not start it. Turn it back on afterwards if you use it.
3. **Boot from the stick** (the boot menu key is usually F8, F11 or F12) and choose *scan this computer*. It takes
   three to five minutes. The screen may go black or flicker while the GPU driver starts. The computer turns itself
   off at the end.
4. **Send the report.** Put the stick into any computer. On Windows, IntelXe Manager's Report page lists the stick's
   reports, checks their seals, shows the summary and opens the issue form with the report ID.
   Otherwise: `REPORTS/` holds three files per scan:
   `xe-stick-XR-XXXXXXXX.tar.gz` (the report), `.txt` (its summary: read it first) and `.sha256`. Open an issue with the
   *new hardware* or *something is broken* form, put the report ID (`XR-…`) in the form, and drag the `.tar.gz` into
   the description. A report stays under 25 MB, GitHub's limit.

**If the computer freezes**, hold the power button, and boot the stick again: the stick remembers which step froze
this computer and leaves it out from then on (below). If it freezes again, choose *safe scan*: it starts no GPU
driver at all.

## What it collects

Everything `tools/report/collect-linux.sh` collects (see `tools/report/README.md`), twice, with `--acpi`:

- `before/`: without any GPU driver: PCI setup and capabilities (Resizable BAR), the bridges above the GPU, DMI names,
  the firmware files on the stick, the ACPI DSDT and SSDTs;
- `after/`: with the driver bound: the same, plus the driver's debugfs (GuC/HuC/GSC state and versions, hwconfig,
  engines, topology, workarounds, display info, DMC, the VBT, per-connector DPCD/PSR/DSC), the connectors, EDIDs
  (without their serial number), the backlight, the GT clocks;
- `trace/`: per Intel display device and driver, `DRIVER-BDF.mmio.gz` (the mmiotrace: every MMIO read and write of
  the driver's bring-up), `DRIVER-BDF.txt` (what happened: bind result, time, trace size, ring-buffer overruns, what
  was left out) and `DRIVER-BDF.dmesg.txt` (the kernel log of that bind). Accesses to the GPU's VRAM or aperture BAR
  are left out by default (the console's frame-buffer writes would bury the register traffic; their count is kept);
  BAR0 (registers and the GGTT) is kept whole;
- `stick/`: the scan's own log, the full kernel log of this boot, the disk locks, the network checks, the mounts, the
  modules, the kernel command line and the stick's build;
- `summary.txt`, and `SEAL.sha256`: the SHA-256 of every file in the report.

**Never collected**: any file from the computer's disks (they are never mounted), serial numbers (machine, board, disks,
USB devices, monitors), UUIDs, MAC addresses, user and host names, Wi-Fi names, the Windows product key (the MSDM and
SLIC tables are never read), screen contents. Before writing, the stick runs collect-linux.sh's redaction over every
file, mmiotrace text included, with the private values both collections found, then searches the whole report once
more. If any value is still there, it writes no report, only a note that the privacy check failed. The private values
themselves never leave RAM.

## Safety

| Risk | What the stick does |
|---|---|
| Writing to the computer's disks | Before anything is mounted, every block device that is not USB or removable is set read-only in the kernel (`BLKROSET`, busybox `blockdev --setro`). Once the stick is found, every other device is, USB drives included, and a background loop locks devices that appear later. The image has no NVMe driver at all; SATA and virtio disks (built into the kernel) appear and are locked. The stick is found by its label on USB/removable devices only, mounted read-only to check its marker file, and only then made writable. No other file system is ever mounted. |
| The network | No network, Wi-Fi or Bluetooth driver is in the image (the build checks the kernel's built-in list and deletes every such module), no network tool either. The scan records the interfaces at start, after the drivers load, after the trace and at the end: only `lo`. |
| Firmware variables | efivarfs is mounted read-only; `efi_pstore.pstore_disable=1` keeps a panic from writing the kernel log into NVRAM. |
| The GPU driver bind hangs the machine | Each bind is a *step*: before it starts, `running KEY STEP` is written to `REPORTS/xe-stick-state.txt` and synced; after it, the line is removed. A line still there at the next boot means the computer froze in that step: it becomes `skip KEY STEP`, and that step is left out on that computer from then on (KEY: a hash of the machine and board names, BIOS version and Intel GPU IDs; no serial number). A *dead-man* process powers the computer off if a step does not end in time (180 s per bind, 420 s per collection, 40 min for the whole scan). A wedged kernel cannot run the dead-man; for that, the kernel command line has `panic=30 softlockup_panic=1 hung_task_panic=1 nmi_watchdog=panic`, so a stuck CPU or a driver stuck for two minutes becomes a reboot after 30 s. A total freeze that none of these catch needs the power button; the next boot then skips the step all the same. |
| mmiotrace itself | It takes all CPUs but one offline while it runs (the kernel's own behaviour), and the drivers bind one device at a time (`drivers_autoprobe` off). The raw trace is capped at a tenth of RAM (at most 768 MB). |

The rest of the report (both collections) is not at risk from the trace step: a freeze there leaves no report, but the
next boot skips the frozen step and finishes.

Boot menu: *scan this computer* (`xestick.mode=full`), *safe scan* (`nomodeset xestick.mode=safe`: one collection,
no GPU driver), *shell* (`xestick.mode=shell`: disks locked, stick mounted at `/mnt/stick`, a root shell on tty1; for
developers), *turn this computer off*. Kernel command-line switches, for developers: `xestick.wait=SECONDS` before
the power-off (default 30), `xestick.trace=full` (keep the VRAM/aperture accesses), `xestick.retry=1` (try the steps
that froze before), `xe.force_probe=DEVICE` (Linux's own switch, for an ID `xe` does not drive by default), and, for the
tests, `xestick.testhang=STEP`.

## The image

| | |
|---|---|
| Boot | GRUB 2.14 (`grub-mkimage`, x86_64-efi, no shim), UEFI only, MBR with one FAT32 partition (label `XEPROBE`) so that Windows and macOS mount it |
| Kernel | Arch Linux `linux` 7.3.4.arch1-1 (below) |
| Initramfs | busybox 1.37, musl, kmod 34.2 (modprobe/modinfo, honouring module blacklists and `xe.*` options on the command line), pciutils 3.15 with `pci.ids`; the scan (`/init`, `scan.sh`, `trace-filter.awk`) and `collect-linux.sh` |
| Modules | 23: `xe`, `i915` and what they need (drm helpers, ttm, gpu-sched, cec, video, wmi, intel-vsec, ...), `usb-storage`, `uas`, `vfat` and two NLS tables. No network, Wi-Fi, Bluetooth, NVMe, MEI or sound module |
| Firmware | linux-firmware 20261019's `i915/` and `xe/` directories (136 files, 13 MB, zstd-compressed as packaged) with `LICENSE.i915` and `LICENSE.xe`. Every firmware file `xe` and `i915` name is present |
| Sizes | files on the stick 38 MB (vmlinuz 17 MB, initramfs 21 MB, BOOTX64.EFI 0.7 MB); the raw image 208 MB, of which 160 MB is free space for reports |
| Licences | `THIRD-PARTY-NOTICES.txt` (every package, version, licence and source) and `LICENSES/` on the stick |

The orchestrator is POSIX sh on busybox rather than Python: the same language as `collect-linux.sh`, every operation it
needs is a busybox applet (`blockdev --setro` is the `BLKROSET` ioctl, plus `sha256sum`, `tar`, `gzip`, `awk`), and the
image needs no interpreter besides (Python would add about 40 MB). Busybox's network applets and every applet that
formats, partitions or copies raw to a drive (`dd`, `mkfs.*`, `fdisk`, `mkswap`, ...) are removed from the image.

### The kernel

A kernel build under x86_64 emulation on an arm64 Mac takes well over an hour, so the stick ships a distribution kernel:
Arch Linux's `linux` package, because the Arch Linux Archive keeps every package version at a fixed URL (Alpine and
Debian mirrors drop superseded files), because Arch follows the newest stable kernel (7.3, the tree
`data/platforms.json` reads `xe` from: BMG, LNL and PTL are all driven by `xe` there), and because its configuration has
what the stick needs. `image/kconfig-check.sh` reads the configuration back out of the shipped `vmlinuz` itself
(`CONFIG_IKCONFIG`) at every build and stops the build if any of these differs:

- `MMIOTRACE=y`, `HOTPLUG_CPU=y`, `TRACING=y`; `DRM_XE=m` with `DRM_XE_DISPLAY=y`, `DRM_I915=m`; `DEBUG_FS=y`;
  `EFIVAR_FS=y`; `EFI_STUB`, `BLK_DEV_INITRD`, `RD_ZSTD`, `DEVTMPFS`, `TMPFS`, `FW_LOADER` with
  `FW_LOADER_COMPRESS_ZSTD=y` and no user-space helper; `MAGIC_SYSRQ`, `SOFTLOCKUP_DETECTOR`, `DETECT_HUNG_TASK`;
  `MSDOS_PARTITION`, USB storage and VFAT;
- not set: `MODULE_SIG_FORCE` (the image's modules are decompressed copies of the signed ones),
  `LOCK_DOWN_KERNEL_FORCE_INTEGRITY` and `..._CONFIDENTIALITY` (lockdown forbids mmiotrace; Arch has
  `LOCK_DOWN_KERNEL_FORCE_NONE=y`), and `EFI_VARS_PSTORE_DEFAULT_DISABLE=y`;
- no Ethernet, Wi-Fi, Bluetooth or USB-network driver built in: `modules.builtin` lists only `netkit` (a virtual BPF
  device) under `drivers/net/`.

## Building

```sh
tools/probe-stick/build.sh          # out/xe-probe-stick-VERSION.img, .img.sha256, .manifest.txt; out/usb/ (the files)
```

It needs only Docker; the build runs in the pinned Alpine image as `linux/amd64` (emulated on an arm64 Mac) and takes
about a minute once the downloads (about 300 MB, kept in `out/cache/`) are there. Every input is pinned in
`sources.lock`: the Docker images by digest; the kernel and firmware packages (Arch Linux Archive), the Alpine package
files that go into the image, GRUB, and the licence texts by URL and SHA-256. The packages are unpacked, never
installed, so no package script runs. File times come from the lock's `epoch`, the cpio archive is written with
`--reproducible`, the FAT file system with `--invariant`: two builds give a byte-identical image. The manifest lists
the inputs, the build tools' versions, the kernel configuration checks, the modules, and the SHA-256 of every file in
the initramfs and on the stick.

To move to a newer kernel or firmware, change its line in `sources.lock` (URL and SHA-256) and the `epoch`, build,
and run the tests. Alpine removes a package file from its mirror when it ships a fix; `build.sh lock` then prints new
`apk`/`tool` lines (the cached files keep older builds working meanwhile).

## Tests

```sh
tools/probe-stick/test/unit-test.sh    # about 30 s
tools/probe-stick/test/qemu-test.sh    # after build.sh; about 3 minutes
tools/probe-stick/test/disk-rules-test.sh   # a second; no Docker
```

`disk-rules-test.sh` runs every vector of `stick-disk-vectors.json` (the facts about one disk and the reasons to refuse
it, as codes) through `write-stick.sh --rules` with each shell it finds (sh, dash, bash, busybox). IntelXe Manager's
`StickRules` runs the same vectors in `windows/app/IntelXe.Core.Tests`, so the two writers refuse the same disks.

`unit-test.sh` runs in the pinned Alpine image (busybox, as on the stick) and in the pinned Debian image (dash and GNU
tools, as on a live USB): `trace-filter.awk` on a synthetic trace (VRAM accesses dropped at 64-bit addresses,
registers and MAP/MARK/PCIDEV lines kept, counts), `collect-linux.sh --redact` on trace and log text (values replaced,
the rest byte-identical, a value or product key in a `.bin` failing the privacy check), and `collect-linux.sh` as the
standalone script (as root and as a user, `--help`, `--id`, `--literals-out`).

`qemu-test.sh` boots the real image four times on an emulated q35 PC with OVMF (QEMU in the pinned Debian image; on
an arm64 Mac it emulates x86_64 in software, about 30 s a boot). The PC carries planted values: SMBIOS system and board
serials and a UUID, the stick's USB serial, a SATA disk with a serial, the same serial written inside that disk (which
also holds a decoy FAT volume labelled like the stick, with the marker file), and an e1000e network card with a set MAC
address. It fails if a planted value appears anywhere in a report (every file, traces unpacked, the summary, the state
file), if an interface other than `lo` appears, if the SATA disk changes by one byte, if a report is missing or its
seals do not verify, or if the trace step does not skip cleanly (QEMU has no Intel GPU: no trace file, no GPU driver
loaded, and the summary says so). With sanity checks that the redaction learned the serials and the kernel log really
held the stick's serial, that the network card is on the bus, that the disk was locked and only the stick mounted.
Boot 2 hangs in a step on purpose (`xestick.testhang=collect-after`): the dead-man must power the PC off; boot 3 must
then leave that step out and say so, while a freeze planted for another computer changes nothing. Boot 4 is the
menu's *safe scan*: one collection, no GPU step, a report.

### Not tested on real hardware

Everything that needs an Intel GPU is so far tested only by reading: the `xe`/`i915` binds under mmiotrace, the
trace sizes and the VRAM filter on a real B580 (whose BAR2 is 16 GB with Resizable BAR), the dead-man around a real
bind, the panic switches, the after-bind collection, and booting on real UEFI firmware from a real USB stick
(including firmware that dislikes FAT32 on MBR). `write-stick.sh` was checked against this Mac's own disks (it refuses
them); writing a real stick was not run, by it or by IntelXe Manager (whose writer was tested with a file for the
disk). The first real runs: the B580 test machine, then an iGPU (LNL or PTL) laptop.

## Files

| Path | What it is |
|---|---|
| `build.sh`, `image/build-inside.sh` | the build (host side, and inside Docker) |
| `image/kconfig-check.sh` | reads the kernel's configuration out of `vmlinuz` and checks it |
| `image/grub.cfg`, `image/grub-embedded.cfg` | the boot menu; the config built into BOOTX64.EFI that finds it |
| `rootfs/init` | PID 1: mounts, then `scan.sh`, then power-off |
| `rootfs/usr/lib/xe-stick/scan.sh` | the scan: disk locks, the stick, the collections, the traces, the report |
| `rootfs/usr/lib/xe-stick/trace-filter.awk` | drops the accesses inside the VRAM/aperture BARs from a trace |
| `rootfs/etc/modprobe.d/xe-stick.conf` | GPU drivers load only by name |
| `stick/` | the README files that go on the stick |
| `sources.lock`, `VERSION` | the pinned inputs; the stick's version |
| `write-stick.sh` | writes the image to a USB stick on macOS or Linux |
| `stick-disk-vectors.json` | the disk rules' answers, shared by `write-stick.sh` and IntelXe Manager for Windows |
| `test/` | `unit-test.sh`, `qemu-test.sh`, `disk-rules-test.sh` and their helpers |

The stick's programs and these scripts are MIT licensed. Prior art: NullMoth's `1401-probe` (GPL-3.0) was read for its
ideas (an initramfs-only scanner, disks locked with `BLKROSET`, freeze checkpoints on the stick, a dead-man power-off,
mmiotrace during a driver re-bind, a QEMU test with planted serials); no code was taken from it.
