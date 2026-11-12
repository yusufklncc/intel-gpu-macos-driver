# Diagnostics reports

Three scripts collect what we need to debug the driver on your machine. Each one writes a single file named after a **report ID**, for example `xe-report-XR-1A2B3C4D.tar.gz`. The bug form asks for that ID, and you attach the file to the issue.

Nothing is uploaded. The scripts only read; they change no settings and install nothing. Each takes about a minute.

| Script | Run it on | What it is for |
|---|---|---|
| `collect-macos.sh` | macOS, with or without our driver | The main report: hardware, driver state, logs, panics, OpenCore config |
| `collect-linux.sh` | a Linux live USB, on the same machine | Reference: what Linux's own driver finds and programs on your hardware |
| `collect-windows.ps1` | Windows, on the same machine | Hardware inventory, if you dual-boot and have no Linux USB |

For a bug, the macOS report is required. The Linux and Windows reports are optional extras, but they help a lot for display and boot problems, and for hardware we have not seen yet.

## macOS

The easiest way: **IntelXe Manager > Report > Create a diagnostics report**. It runs the same script, puts the file on
your Desktop, lists the warnings it found with a fix where the app has one, and **Report a bug** (or **Ask for a new
GPU**) opens the issue form with the report ID filled in.

Or open Terminal in the folder that holds the script and run:

```sh
sh collect-macos.sh
```

At the end it prints the report ID and the file's path. The first section of `summary.txt`, **Warnings**, lists the
known problems the script recognised on your machine, each with what to do: acceleration held off by the boot guard,
a system-volume copy of the Metal plugin older than the rest of the install, a half-done HiDPI switch, a kext whose
loaded version differs from the installed file, a hybrid Intel CPU without HybridPM, the display redrawing at full rate
with nothing moving, and recent kernel panics. The options are:
- `--days N`: include panic and crash reports from the last N days (default 7);
- `--tests`: also run our quick GPU self-tests (`vk_smoke`, `mtl_tests`) if they are installed (off by default);
- `--no-mount`: do not try to mount an EFI partition to read the OpenCore config;
- `--sudo-mount`: if `sudo` works without a password, mount EFI partitions read-only as root and unmount them again. With a known boot partition, only that one is mounted; otherwise every EFI partition a user could not mount;
- `--acpi`: also the DSDT and SSDT tables as macOS sees them, after OpenCore's patches and with the SSDTs it adds
  (from the I/O Registry, no root needed). Never the MSDM or SLIC table. macOS 26 and later no longer publish the tables
  there, so on Tahoe the option collects nothing (IntelXe Manager greys it out); dump them from Windows or Linux;
- `--probe`: also collect the results of the staged probe (`XeProbe`, see `docs/PROBE-MODE.md`) and the NVRAM breadcrumb the previous boot left;
- `--out DIR`: write the file somewhere else.

You do not need `sudo`. A few driver counters can only be read by root. If `sudo` works without a password, the script adds them; otherwise it skips them and says so in the summary.

The script uses only tools that ship with macOS. It does not need Xcode or the Command Line Tools, and it does not start their install dialog.

**OpenCore config.** The script reads `EFI/OC/config.plist` from any EFI partition that is already mounted. It also tries to mount the other EFI partitions read-only, which usually needs root. Run as root (IntelXe Manager's
administrator mode), it also reads an EFI partition that macOS refuses to mount read-only because OpenCore left it
marked dirty at boot (it mounts it with `mount_msdos -o rdonly` instead). If it found no config, mount the EFI partition you boot from, for example with `sudo diskutil mount disk0s1` or MountEFI, and run the script again.

**Which EFI booted.** The script works this out without root, in this order:
1. the firmware's `BootCurrent` load option (its partition GUID and file path);
2. OpenCore's `boot-path`, if `ExposeSensitiveData` exposes it;
3. otherwise, it compares each EFI it could read with the running system: whether the OpenCore log was written during this boot, and whether the boot-args, `csr-active-config` and OpenCore version match.

A comparison only names an EFI when the OpenCore log of this boot is on it, or when every EFI partition was read. A copy of the same EFI on a USB stick would match the settings just as well. The result is in `opencore/booted-efi.txt` and on the "Booted from" line of the summary.

## Linux (live USB)

Our kernel driver follows Linux's `xe` and `i915` drivers. When something differs on your machine, the quickest way to find out why is to see what Linux does on the same hardware:
- the firmware it loads;
- how it sets up PCI and Resizable BAR;
- the GuC/HuC state;
- the display link, the VBT and the EDIDs.

`collect-linux.sh` records that ground truth.

1. Boot any recent live USB on the same machine, with the same monitor connected. A distribution with kernel 6.12 or newer is best; Ubuntu 25.04+ and Fedora 41+ both work. Battlemage needs the `xe` driver.
2. Copy `collect-linux.sh` to the live system, open a terminal in that folder, and run:

   ```sh
   sudo sh collect-linux.sh
   ```

   It also runs without `sudo`, but then it misses the debugfs state, the full PCI capability list (which holds Resizable BAR) and, on some systems, the kernel log.
3. Copy the resulting `xe-report-XR-….tar.gz` to a USB stick or to your macOS/Windows disk before you reboot. A live system forgets everything.

Install the optional tools first; most live systems lack them, and the register dump in particular saves a second
report later:

```sh
sudo apt install intel-gpu-tools vainfo clinfo vulkan-tools mesa-utils         # Ubuntu, Debian
sudo dnf install igt-gpu-tools libva-utils clinfo vulkan-tools glx-utils       # Fedora
```

For **a GPU we do not support yet**, run it as `sudo sh collect-linux.sh --acpi --bench`: `--acpi` adds the DSDT and
SSDT tables (the GPU's ACPI device, its backlight and `_DSM` methods; never the MSDM or SLIC table, which hold the
Windows licence), and on a laptop the backlight's range is recorded either way.

For a program that packs more files around this report (the Xe probe stick does), the script also takes `--id XR-…`
(use this report ID), `--time SECONDS` (the time budget), `--literals-out FILE` (also write the private values it
found to FILE, for redacting the caller's own files; keep that file out of every archive) and
`--redact FILE PATH…` (collect nothing: apply the same redaction, with those values, to the text files under PATH,
then run the privacy check over them).

Optional extras, used if they are installed: `vulkaninfo` (package `vulkan-tools`), `glxinfo` (`mesa-utils`), `vainfo`
(the video codecs the media driver offers), `clinfo`
(the OpenCL platforms: Mesa's rusticl and Intel's compute runtime), and `intel_reg` (`intel-gpu-tools`, for a register
dump).

`--bench` also runs `clbench` (`tools/clbench/`: the same OpenCL kernels we time on macOS and Windows) on every OpenCL
platform that has the card, about a minute each. Put `clbench-linux`, or `clbench.c` and a C compiler, next to the
script. With both rusticl (Mesa's compiler, the one the macOS driver uses) and Intel's runtime (Intel's compiler, as on
Windows) installed, this compares the two compilers on the same card and kernel driver.

## Windows

Open PowerShell in the folder that holds the script and run:

```powershell
powershell -ExecutionPolicy Bypass -File collect-windows.ps1
```

Add `-Acpi` to include the DSDT and the first SSDT (Windows hands out no more without a driver; the Linux report has
them all).

`-ExecutionPolicy Bypass` applies to this one run only; it does not change your system's policy. The script needs no administrator rights. Run it as administrator only if you want the TPM details too. It writes `xe-report-XR-….zip` in the current folder.

It records:
- Windows, the machine and its board, the BIOS version and date, and the CPU;
- the GPUs with their hardware IDs, driver versions and the memory ranges Windows assigned them (from the device's
  resource list: WMI lists only ranges below 4 GB and so hides a resized VRAM BAR). From them it tells whether
  Resizable BAR and Above 4G decoding are active;
- the monitors (EDID);
- Secure Boot and the TPM;
- the disk and network adapter models;
- a `dxdiag /t` report.

## What is collected

**macOS:**
- macOS version and build; CPU, SMBIOS model and board-id; the OpenCore version and boot-args from NVRAM; SIP state;
- every PCI device's IDs. For GPUs, also the BAR sizes and the PCIe path to the root port. A discrete Arc card sits behind its own PCIe switch, whose downstream port and GPU function always report Gen1 x1, so the summary gives the link into the root port (in `gpu/pcie-links.txt`, each hop in the path);
- framebuffer and display properties from the I/O Registry, the EDIDs, and `system_profiler SPDisplaysDataType` (Metal devices);
- loaded kexts: ours, Lilu, WhateverGreen and WhateverXe, plus the other graphics kexts;
- the stack's state (`driver/stack.txt`): which Metal plugin the accelerator names and the copies installed (the
  system-volume copy, the `/Library` copy, `NVMTLDriver.use-library`, the system-volume record), the boot guard and the
  driver's NVRAM keys, the HiDPI switch (NVRAM flag, display override, `DisplayResolutionEnabled`), the installed
  package and IntelXe Manager versions, Zink GL, the CPU power-management kexts (HybridPM, CpuTopologyRebuild), and
  how many frames the display presented while the report ran; with root, the CPU clock from `powermetrics`;
- the Metal plugin's own log, where a test build writes one (`/tmp/nvmtl.log`, the last 3000 lines);
- our driver state: `debug.intelxe.*`, `debug.intelxefb_*` and `debug.nmxaccel*`. Only read-only values are read: never a screen capture (`intelxefb_thumb`/`crop`), register peeks or batch dumps. If the kernel has `debug.intelxe.dump`, that one call replaces the list;
- the version and SHA-256 of each installed piece: kexts, NVMTLDriver.bundle, Mesa, the Vulkan loader and the VideoToolbox codecs;
- `vulkaninfo --summary`, if installed;
- kernel log lines from our kexts, Lilu, WhateverGreen/WhateverXe and IOGraphics, since the last boot (the first 2000 and the last 3000);
- WindowServer errors and log lines from our user-space parts: since the boot, or from the last 2 hours if the machine has been up longer. Both log queries run in the background while the rest is collected;
- panic reports: the list of all of them, and the panic text of those from the last 7 days;
- crash, hang and GPU-restart reports that involve our stack;
- power settings and recent sleep/wake events;
- from a mounted EFI: `config.plist`, the newest OpenCore log, and the names, versions and hashes of the kexts and drivers.

**Linux:**
- kernel and distribution; the machine, board and BIOS (DMI, without serials); Secure Boot;
- `lspci -nnvv` for each GPU and the bridges above it; sysfs PCI and link data; BAR sizes;
- the loaded driver, `modinfo xe`/`i915`, module parameters, and the firmware files present;
- kernel log lines about xe/i915/drm/GuC/HuC/DMC;
- with root, selected debugfs files: device info, GuC/HuC info and log, hwconfig, frequency, engines, topology, workarounds, display info, DMC, the VBT, and per-connector DPCD/PSR/DSC state;
- connectors with their status, modes and EDIDs;
- `vulkaninfo --summary` / `glxinfo -B`, `clinfo`, and `intel_reg dump` if installed;
- the GT clock limits and the current clock from sysfs; `vainfo`; the backlight interfaces (type and range);
- with `--acpi`, the DSDT and SSDT tables (binary, as the firmware has them) and the names of all tables;
- with `--bench`, the `clbench` results on each OpenCL platform.

Each archive also holds `summary.txt`, a one-page overview to read first, and `manifest.txt` with the SHA-256 of every file.

## Privacy

Before the archive is written, every script removes:
- serial numbers: the machine, board, disks and USB devices, and the monitor's serial inside its EDID;
- the platform UUID and machine IDs: on macOS the hardware UUID, MLB and ROM; on Linux the machine-id; on Windows the MachineGuid;
- MAC addresses;
- your user name and full name, and your user name in file paths;
- the computer's host names;
- the names of Wi-Fi networks your machine knows;
- in the OpenCore config: `SystemSerialNumber`, `MLB`, `SystemUUID`, `ROM`, `BoardSerialNumber`, `ChassisSerialNumber` and `ApECID`. The keys stay, the values become `REDACTED`.

The scripts first read these values from your own machine, then remove each one from every file. Before packing, they search the output once more. If any value is still there, or anything shaped like a Windows product key, they stop without writing a file and tell you.

**Never collected:**
- ACPI tables, unless you ask for them (`--acpi` on Linux and macOS, `-Acpi` on Windows), and then only the DSDT and
  SSDTs: never the MSDM or SLIC table, which carry your Windows product key, nor Windows' WPBT, and never a file in an
  EFI `ACPI` folder (on macOS only their names are listed). The final privacy check reads the collected tables too.
- The Windows product key and licensing data.
- Screen contents.
- Wi-Fi passwords.
- Your documents.
- Full system logs: only the log lines that concern graphics are kept.

The archive is plain text plus a few small binaries: the VBT on Linux, and the GuC hwconfig table on macOS. You can unpack it and read every file before you share it:

```sh
tar -xzf xe-report-XR-1A2B3C4D.tar.gz
```

## Attaching the report to an issue

1. Open a new issue with the **Something is broken** form.
2. Put the report ID (`XR-…`) in the first field. If you made more than one report, for example macOS and Linux, list all their IDs, separated by commas.
3. Drag the `.tar.gz` or `.zip` file into the description box. GitHub accepts both, up to 25 MB.

If GitHub rejects the file as too large, rerun the macOS script with `--days 2`.
