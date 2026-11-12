# Quick start: the IntelXe Manager way

[Türkçe](QUICKSTART.tr.md)

One page for the usual case: a PC with an Arc B580 or B570 that boots macOS with OpenCore, set up with the
**IntelXe Manager** app. Each step names the section of the full guide,
[`release/README-INSTALL.md`](../release/README-INSTALL.md), that explains it, gives the Terminal commands and says what
to do when something goes wrong.

Experimental and unsigned: read the warning at the top of the full guide first.

## Before you start

- **A USB stick with a working OpenCore EFI**, and your firmware's boot-menu key. If the PC does not come up after a
  change, you boot from the stick.
- **One monitor on the card's DisplayPort.**
- macOS 26 Tahoe or 15 Sequoia, installed with a plain OpenCore EFI ([Dortania's guide](https://dortania.github.io/OpenCore-Install-Guide/);
  full guide, stage 1). The screen is slow until the end: macOS runs on the card's firmware output.
- No `IntelXe.kext` or `SSDT-ARCOFF.aml` in the EFI yet. They come last (step 5).

## 1. Get the app

Download `IntelXe-Manager-<version>.zip` from the [releases page](https://github.com/yusufklncc/intel-gpu-macos-driver/releases)
into Downloads, then in Terminal:

```sh
mkdir -p ~/Applications && ditto -x -k ~/Downloads/IntelXe-Manager-*.zip ~/Applications
xattr -dr com.apple.quarantine ~/Applications/"IntelXe Manager.app"
open ~/Applications/"IntelXe Manager.app"
```

The second line is needed because the app is not signed with an Apple developer ID (full guide, 3.1).

<p align="center"><img src="images/manager-overview.png" width="70%" alt="IntelXe Manager's Overview page with the driver working"></p>

## 2. The setup assistant opens

On its first start the app shows its setup assistant. It does the steps below one page at a time, shows what each
change will be before it asks for your password, and continues where it stopped when you open the app again after a
restart.

## 3. Prepare OpenCore (stage 2)

The assistant changes OpenCore's `config.plist` in the EFI you booted from: relaxed SIP, the AMFI boot-arg, two quirks
and one console setting. Tick *Also prepare Safari on the GPU* if you want Safari on the card later. It backs up the
whole EFI folder first. When OpenCore did not record which EFI it booted from, the app lists the EFI partitions to
choose from.

**Restart**, then open the app again.

Preparing the EFI from Windows instead: IntelXe Manager for Windows (`IntelXeManager-VERSION-win-x64.zip`, from
0.1.5), or `IntelXe-EFI.ps1` in the `-efi.zip` as a script (full guide, *On Windows* and stage 2).

## 4. Install the driver (stage 3)

The assistant downloads the package for your macOS, checks the release's signature and the package's checksum, and
installs it. It
backs up what it replaces and can undo it.

**Do not restart yet.**

## 5. The EFI (stage 4)

The assistant adds `IntelXe.kext` and `SSDT-ARCOFF.aml` to the EFI. It rewrites the table for your card's ACPI path when
it is not the shipped one. On a 12th-14th gen Intel CPU with E-cores it also adds `HybridPM.kext` below `Lilu.kext`;
HybridPM needs Lilu, which this project does not ship (full guide, *Hybrid Intel CPUs*).

**Restart.** The desktop should now move at your monitor's refresh rate.

## 6. Check

- The app's Overview page says the driver is working, and About This Mac names the card.
- The assistant's last page runs a GPU test and offers the extras: Safari on the GPU (the system-volume copy; full
  guide, 3.3), OpenGL through Zink for Minecraft Java and other LWJGL games, HiDPI modes, a menu bar item.

## If it does not come up

Boot from the USB stick, or press Shift with the OpenCore entry selected for safe mode, and undo the driver
(`sudo ./install.sh --undo` in the package, or the app's Driver page). The full guide's *If it does not come up* has
the details, and *Reporting a problem* says how to make a diagnostics report.

## Later releases

The app's Driver page downloads and installs a new release over the old one; then its EFI page replaces
`IntelXe.kext` (full guide, *Upgrading*). The release notes say when a step is needed.
