# IntelXe Manager for Windows

A native Windows app (WinUI 3, Windows App SDK, C#) with the look and wording of the macOS app **IntelXe Manager**
(`app/`): the same sidebar groups, section titles, accent colour, status colours and texts, in Fluent form (Mica,
NavigationView, light and dark following Windows): a guided setup from this PC to a macOS install, check this PC, list
the firmware (BIOS) settings it needs, build an OpenCore EFI for this PC, make a macOS installer USB stick, prepare the
OpenCore EFI for the driver, collect a report. Its EFI page makes the same changes as
`windows/IntelXe-EFI.ps1`, by the same rules (`release/templates/efi-rules.json`) and the same engine, so it can replace
the script.

It writes only on the EFI page (only after showing the plan and getting a confirmation, and only to the EFI folder or
EFI partition chosen there, after a backup of the whole EFI folder) and on the USB installer page (only to the USB stick
chosen there, after the user typed its name and size). The Build the EFI page writes into the app's own data folder and,
when asked, a new `EFI` folder inside a folder the user chooses; the Set up page writes only its progress into the
settings file. It never writes to the firmware, never changes BitLocker, and uploads nothing; the USB installer page
downloads macOS recovery from Apple, the EFI builder OpenCore and the kexts from their GitHub releases (each pinned by
SHA-256).

## Layout

| Folder | What it is |
|---|---|
| `IntelXe.Core/` | Plain .NET 10 class library, no Windows APIs: the models and rules. Supported cards by PCI ID from `data/platforms.json` (embedded at build time), PCI instance-ID parsing, the CPU family and the hybrid-CPU rule HybridPM.kext needs (the same regex as `efi.sh` and `IntelXe-EFI.ps1`), the ACPI-path conversion of a Device Manager location path, the REG_RESOURCE_LIST parser for the card's BARs (as `tools/report/collect-windows.ps1`), the Overview's checks and their limits, the report script's output, the issue-form URLs, the `.resw` key checks. The BIOS part (`Bios/`): `bios-steps.json` (embedded at build time), the
items' states from this PC's facts, the printable page. The stick part (`Stick/`): `macos-recovery.json` (embedded),
Apple's recovery protocol and chunklist check, the disk rules, the stick writer and its helper's command line, the
`IEfiSource` seam. The EFI builder part (`EfiBuild/`): `IEfiBuilder` and `PythonEfiBuilder` (the builder's command lines, its process, its result JSON), the engine folder and its manifest, the elevated scan's request, work and answer, the SMBIOS masking, the build folders and the built EFI as an `IEfiSource`. The setup part (`Setup/`): the wizard's steps and their states. The EFI part (`Efi/`): `efi-rules.json` (embedded at build time), IntelXe-EFI.ps1's engine (compiled in at build time, see below), the planner, the package (folder or zip, SHA256SUMS), and the elevated helper's command line, work and answer. |
| `IntelXe.Core.Tests/` | xUnit tests of the Core. They run on macOS, Linux and Windows. `EfiVectorTests` runs every vector of `release/efi-rules-vectors.json` through the Core's planner and apply; `EfiHelperTests` the helper with the mounting faked, the package and the command line. `BiosStepsTests`,
`RecoveryTests` and `StickTests` the BIOS data, the recovery protocol (with answers and chunklists recorded from Apple's
server in `Fixtures/recovery/`) and the stick writer with the disks faked. `EfiBuildTests`, `SniffTests`,
`EfiBuildStoreTests` and `SetupWizardTests` the EFI builder with a fake Python (and a shell script as a real process on
macOS and Linux), two result files of windows/efibuild's fixture runs (`Fixtures/efibuild/`), the scan, the masking and
the wizard. |
| `IntelXe.App/` | The WinUI 3 app: unpackaged (`WindowsPackageType=None`) and self-contained (`WindowsAppSDKSelfContained`, `SelfContained`), `net10.0-windows10.0.19041.0`, x64, Windows App SDK 2.6.5 (only its WinUI and Foundation packages). No MSIX: there is no signing certificate, and the folder runs without installing .NET or the Windows App Runtime. `Program.cs` is its entry point: `--efi-helper` (and `--efi-helper sniff`) and `--stick-helper` run the helpers without a window. |
| `IntelXe.App/Strings/{en-US,tr-TR}/Resources.resw` | English and Turkish. The Turkish texts are the macOS app's (`app/Resources/tr.lproj`) wherever the same text appears there. A test holds both files to the same keys and `{n}` placeholders, and to every key the app's sources and the Core's checks name. |
| `build.ps1` | Restore, build, test, publish a self-contained folder, put the EFI builder beside it (`engine\`), zip it (`out/`). |
| `engine.lock.json` | What `build.ps1` puts into `engine\python\`: python.org's embeddable Python 3.12.10 and the wheels pywin32 311, WMI 1.5.1 and certifi 2026.10.23, each with its URL and SHA-256, and what is left out of them. |

The CI workflow `.github/workflows/windows-app.yml` runs `build.ps1` on `windows-latest` for pushes and pull requests
that touch `windows/app/**` (or `data/platforms.json`, `tools/report/collect-windows.ps1`) and keeps the zip as a build
artifact. It releases nothing.

## Building

On Windows, with the .NET 10 SDK (nothing else; the NuGet packages go to the user's cache):

```powershell
cd windows\app
powershell -ExecutionPolicy Bypass -File .\build.ps1          # or: pwsh ./build.ps1
```

The result is `out\IntelXeManager-VERSION-win-x64\IntelXeManager.exe` (about 205 MB unpacked),
`out\IntelXeManager-VERSION-win-x64.zip` (about 83 MB) and the zip's SHA-256 beside it (`.zip.sha256`). Of that, the
EFI builder in `engine\` is about 35 MB in 572 files (29 MB Python with its packages, 5 MB windows/efibuild and the two
upstream trees) and about 17 MB of the zip: measured by laying out the same downloads on macOS without running them;
`build.ps1` prints the real size. `-SkipEngine` leaves it out (the Build the EFI page then says it is missing). A release
publishes that zip as it is, as its own asset: `release/package.sh` takes it as `WINAPP_ZIP` (with the `.sha256`
beside it), checks it and lists it in the release's signed `.sha256`. The version is the repository's `VERSION`. Most of the size is the
.NET runtime and the Windows App SDK carried inside. The app references only the SDK's `Microsoft.WindowsAppSDK.WinUI`
and `Microsoft.WindowsAppSDK.Foundation` packages (which bring InteractiveExperiences and Base), not the
`Microsoft.WindowsAppSDK` metapackage, whose AI, ML (onnxruntime, DirectML), Widgets, Search and DWrite parts the app
does not use: that took the folder from 233 MB to 171 MB and the zip from 91 MB to 66 MB.

On macOS or Linux only the Core builds (the WinUI XAML compiler runs on Windows only):

```sh
dotnet test windows/app/IntelXe.Core.Tests
```

For screenshots and support instructions the app takes, like the macOS app's `--section`:
`--section setup|overview|bios|build|stick|efi|report|about` (none: Set up, the first item), `--lang en-US|tr-TR`, `--theme light|dark`, `--size WIDTHxHEIGHT`,
`--scroll PIXELS` and `--report-out FOLDER` (the report goes there instead of the Desktop). For the EFI page:
`--efi-folder FOLDER` (chosen as the EFI), `--package FOLDER|ZIP`, `--card-confirmed` (the ACPI path box ticked),
`--plan settings|safari|settings-safari|driver|undo` (that plan's dialog opened at start) and `--helper-for-folders`
(an EFI folder goes through the elevated helper too, to test that path without an EFI partition). For the BIOS page:
`--bios-vendor asus|msi|gigabyte|asrock|generic` (that maker's menu names) and `--bios-html FILE` (the printable page
written there). For the USB installer page: `--recovery-dir FOLDER` (the downloads kept there instead of
`%LOCALAPPDATA%\IntelXe Manager\recovery`), `--stick-confirm` (the erase dialog opened, for the test disk only) and,
**for tests only**, `--test-vhdx PATH`: that attached VHD/VHDX file is offered and may be written although Windows
reports it as a file-backed virtual disk, not as USB. Only a file-backed virtual disk whose backing file is that path
passes; every other rule still applies.

## What works

**Set up** (the first item): one guided flow from this PC to a macOS install, as NullMoth's 1401 does it for its
driver (ideas only; its code is PolyForm Noncommercial and none of it is used). A step list shows each step as done,
next or blocked (by an earlier step, or by its own reason), and each step has its section:

1. *This PC*: the Overview's facts, what stops the install first (no B580/B570; a CPU that is not Intel, which the
   builder refuses), then problems, warnings and the rest;
2. *BIOS settings*: opens the BIOS page; done when the user clicks *I did these*;
3. *Build the EFI*: opens the Build the EFI page; done while the built EFI is still on disk;
4. *The installer USB stick*: opens the USB installer page, which offers the built EFI (*Use the EFI built on this PC*,
   through `IEfiSource`); done when the stick page wrote a stick with that EFI, or when the user says they made the
   stick another way. Building a new EFI makes this step to do again;
5. *After macOS is installed*: booting the stick, stage 3 on macOS (IntelXe Manager for Mac), stage 4 before the next
   restart, on macOS or from Windows on the EFI page (the stick's EFI folder or the disk's EFI partition), the build's
   own after-install notes; *Mark as done*.

The progress (`Setup` in `settings.json`: when the BIOS steps were confirmed, the built EFI's folder, macOS version and
time, which build the stick carries, the last scan's folder) is kept across restarts; *Do this step again* and *Start
over* clear it, never anything on disk. The page itself changes nothing else: erasing the stick and changing an EFI stay
on their pages, behind their confirmations (`Core/Setup/SetupWizard.cs`).

**Build the EFI** (group Install): an OpenCore EFI for this PC, made by `windows/efibuild` (OpCore Simplify at a pinned
commit, every question answered by a recorded rule; `docs/design/DESIGN-efi-build.md`):

1. *Scan this PC*: Hardware Sniffer's report and the ACPI tables, through the elevated helper (one UAC prompt); only
   the DSDT, SSDT, APIC and DMAR tables are kept, never MSDM (the Windows product key) or SLIC;
2. the macOS version: 26 Tahoe (default) or 15 Sequoia;
3. *Build the EFI*: as the user, the program's screens and the rule that answered each question shown as they come,
   *Cancel* kills the builder's process tree;
4. the result: built or the reason it stopped; ocvalidate's verdict (and the finding accepted on purpose, with its
   reason); the app's own EFI engine planning stage 2 and the Safari setting on the built EFI, which must have nothing
   left to change (the Python and C# readers of `efi-rules.json` agree); every recorded answer with its rule and reason;
   the builder's changes; the firmware steps; the after-install notes; PlatformInfo with the identity masked; the build
   log and the folder. The builder's own lines are in English;
5. *Make the installer stick…* (the USB installer page, which offers this EFI) or *Save the EFI folder…* (copied as
   `EFI` into a chosen folder, never over an existing `EFI`, read back).

Scans go to `%LOCALAPPDATA%\IntelXe Manager\efibuild\scans\`, builds to `...\efibuild\builds\` (older builds the builder
made are deleted after a new one: each holds a generated serial number); the builder's download cache is its own
`%LOCALAPPDATA%\IntelXe\efibuild`.

**Overview** (sidebar group Status), read with WMI/CIM, the registry and Windows APIs, without administrator rights:

- the graphics card: the first supported one (`8086:e20b` B580, tested; `8086:e20c` B570, supported but untested), with
  its Windows driver version; another Intel GPU is named as "not supported yet";
- the CPU (cleaned name, family, P- and E-cores by efficiency class) and whether HybridPM.kext applies;
- the motherboard (maker and model only);
- the firmware mode (`GetFirmwareType`: UEFI or legacy) and Secure Boot (`SecureBoot\State`);
- Resizable BAR and Above 4G decoding, from the card's `AllocConfig` (the same reading as `collect-windows.ps1`);
- the Windows version and build; BitLocker on the system drive (`Win32_EncryptableVolume` as administrator, else the
  shell's `System.Volume.BitLockerProtection`, which needs no rights);
- the EFI system partitions (by GPT type, as `Get-EspList` in `IntelXe-EFI.ps1`): disk, bus, size, which one Windows
  booted from, drive letter, and whether it holds `EFI\OC\OpenCore.efi`. A partition without a drive letter cannot be
  read by a program that is not an administrator, and the Overview mounts nothing, so it says "needs mounting to read" (the
  EFI page mounts it, through its elevated helper).

Each fact has an OK, warning, problem or neutral state and one line on what it means; the limits and wording come from
`site/guide/hardware.html`, `site/guide/windows.html` and `release/README-INSTALL.md` (UEFI with CSM off, Above 4G
decoding and Resizable BAR on, Secure Boot off for OpenCore, the recovery key when BitLocker is on). Serial numbers,
UUIDs, MAC addresses and the product key are never queried; a volume's GUID path is used to look inside a partition and
never shown.

**Report** (group Help): runs `tools/report/collect-windows.ps1`, which ships beside the app in `tools\`, in Windows
PowerShell, shows its progress and output, then shows the zip in Explorer. The Last report section copies the report
ID and opens the bug and new-GPU issue forms with it filled in (`?template=1-bug.yml&report=ID`,
`?template=2-new-hardware.yml&reports=ID`, as the macOS Report page builds them). Nothing is uploaded.

**About**: version, what the app does, the links, the licence text and credits as the macOS About window has them, and
the app's two settings: language (Windows', English, Turkish) and appearance (Windows', light, dark), kept in
`%LOCALAPPDATA%\IntelXe Manager\settings.json`. Both change at once.

**BIOS settings** (group Install): the firmware settings this PC needs for macOS with the driver, worded with the board
maker's menu names and paths (ASUS, MSI, Gigabyte, ASRock; a generic hint for other makers; the box chooses another
maker's names), with the keys for setup, advanced mode, save and exit, and the boot menu. The Windows traps come
first: BitLocker (save the recovery key, suspend protection), a Windows installed in legacy mode (MBR2GPT before CSM goes
off), RAID / Intel RST / VMD to AHCI. Then Secure Boot off, CSM off, Above 4G decoding on, Resizable BAR on (with
`ResizeAppleGpuBars = 10`, the guide's value, not Dortania's), the first display on the PCIe card with integrated
graphics off, VT-d off (or `DisableIoMapper`), CFG Lock off (or `AppleXcpmCfgLock`), XHCI hand-off on, Fast Boot off,
VT-x, Hyper-Threading and Execute Disable on, SGX, the serial port and Thunderbolt (for the install) off, and Intel
Platform Trust (Dortania lists it off; the page says what that does to Windows 11 and BitLocker). The items, their
levels (required, recommended, optional), both languages and their sources are `IntelXe.Core/Bios/bios-steps.json`:
each cites the guide (`site/guide/hardware.html`, `macos.html`, `windows.html`, `release/README-INSTALL.md`,
`efi/sample-z790/README.md`), Dortania's Intel BIOS settings (the Comet Lake page, which the guide uses for 10th to
14th gen, or the Coffee Lake page for 8th and 9th gen) or Microsoft. Each item is marked done, to do, or cannot tell
from what Windows reports: BitLocker, the firmware mode, Secure Boot (on means CSM is off), the card's BARs (Above 4G,
Resizable BAR), the storage controllers' names (RAID, RST, VMD), the integrated GPUs Windows lists, the ACPI DMAR
table (VT-d), `Win32_Processor`'s virtualization flag and core and thread counts; settings Windows cannot see (CSM
when Secure Boot is off, CFG Lock, XHCI hand-off, Fast Boot ...) are "cannot tell". The page changes no setting and has
no BitLocker button. *Save as a page for your phone* writes the steps as one HTML file (no scripts, no external files,
light and dark, readable on a phone, printable) in the language shown. Menu paths are the usual ones of each maker's
current UEFI setup for Intel 600/700-series boards and move between BIOS versions; the page and the file say so.

**USB installer** (group Install): a macOS installer stick made from Windows, as Dortania's Windows guide makes it with
`macrecovery.py`, in the app:

1. the macOS version: 26 Tahoe (recommended, tested) or 15 Sequoia (not re-tested), from
   `IntelXe.Core/Stick/macos-recovery.json`;
2. its recovery image from Apple's recovery server, every piece checked against Apple's signed chunklist (below), kept
   in `%LOCALAPPDATA%\IntelXe Manager\recovery\VERSION\`;
3. the USB stick: only USB disks are listed, and the rules below refuse a disk with their reasons;
4. optionally an OpenCore EFI folder the user chose (it must hold `BOOT\BOOTx64.efi`, `OC\OpenCore.efi` and
   `OC\config.plist`), copied as `EFI\`;
5. *Erase and write the stick...*: a dialog names the disk, its size, bus and what is on it now, and enables its button
   only when the user typed the disk's name and size (`SanDisk Ultra 16.0 GB`); the elevated helper then writes it.

**EFI (OpenCore)** (group Install): stage 2 (the settings), the Safari setting (alone, or with stage 2 as one change),
stage 4 (IntelXe.kext, SSDT-ARCOFF.aml, ExposeSensitiveData, HybridPM.kext) and undo, on an EFI partition of this PC or
an EFI folder (a USB stick's, or a copy), as `IntelXe-EFI.ps1` and the macOS EFI page make them:

- *Read the EFI* shows what `-Status` says: OpenCore, what stage 2 and stage 4 would still change, IntelXe.kext,
  SSDT-ARCOFF.aml and HybridPM.kext in the EFI (against the package's) and their `config.plist` entries, the Safari
  setting, the last backup.
- The **package** section takes the release's `intel-gpu-macos-driver-VERSION-efi.zip` (unpacked into
  `%LOCALAPPDATA%\IntelXe Manager\packages\`) or the folder it was unpacked to, or finds one beside the app, and shows
  its kext versions and its `SHA256SUMS` check (every file of the two kexts and the table listed and matching, as the
  rules and the script require; a change refuses a package that does not match).
- **Stage 4** shows the card's ACPI path as Windows reports it (Device Manager's Location path, two bridges above the
  GPU) in a box the user can correct, and adds `SSDT-ARCOFF.aml` only when the user ticks that macOS is installed,
  stage 3 is done and the path is checked (D3 of `docs/design/DESIGN-efi-rules.md`). Unticked, the plan says why the
  table is not added, as the script's does.
- Each section's *Show the plan…* opens a dialog with the plan's `EFI:` line, its steps, its notes, where the backup
  goes and the plan's SHA-256 (the steps, notes and output in the app's language: `Efi/EngineText.cs` maps the
  engine's English lines to string-table keys, D13 of `docs/design/DESIGN-efi-rules.md`; a line it does not know is
  shown as the engine wrote it); *Back up and apply* (the default button is *Cancel*) makes the change with that hash
  only, and a dialog shows what was done. Undo has its own plan (*Show what undo puts back…*).

## How the EFI page works

**One engine.** `IntelXe.Core.csproj` copies the C# engine of `windows/IntelXe-EFI.ps1` (the text between
`// BEGIN ENGINE` and `// END ENGINE`: `Rules`, `EfiFolder`, `Plist`, `Aml`, namespace `IntelXeEfi`) into
`obj/IntelXeEfiEngine.g.cs` at build time and compiles it. The script stays one file that Windows PowerShell 5.1 compiles
with `Add-Type` (so the engine stays C# 5), and the app runs the same code: the same plan lines, notes and SHA-256
(`Plan.Hash`), the same order-preserving `config.plist` writing with each value checked as it is set, the same
`SHA256SUMS` check, backups and undo. `Efi/EfiRules.cs` reads `efi-rules.json` with System.Text.Json into the tree the
engine takes (as the script's `ConvertTo-RulesTree`); `EfiWork`/`FolderEfiPlanner` call the engine. The page's
planner is `IEfiPlanner`; Windows-only parts are in `Services/EfiHelperHost.cs`.

**Backups** are the script's, made by the same code: next to the EFI folder (`IntelXe-EFI-backups\<volume serial>-<path>\`)
or, for an EFI partition (a volume under 2 GB), in `%LOCALAPPDATA%\intel-gpu-macos-driver\efi-backups\`, numbered,
with `EFI/`, `kind`, `date`, `where`, `changed`, `config-after.sha256` and `restored`. A change made by the script is
undone by the app and the other way round (`windows/test/Test-AppInterop.ps1`).

**Administrator rights only in the helper.** The app is `asInvoker`. An EFI folder is planned and changed in the app's
own process. An EFI partition can be mounted only by an administrator, so for each reading, plan and change of a
partition the app starts its own exe again through `runas` (the UAC prompt) as
`IntelXeManager.exe --efi-helper status|plan|apply --disk N --partition N [--change KIND] [--letter X:] [--expect SHA256] ... --out FILE`.
The helper (`Program.cs` → `HelperMain`, `Core/Efi/EfiHelper.cs`) opens no window and does one thing: finds the
partition by disk and partition number (`MSFT_Partition`), refuses it unless its GPT type is the EFI system
partition's, mounts it with `mountvol` at a free letter as the script's `Mount-Esp` does (or uses the letter it already
has), reads or plans or backs up and changes, and unmounts it again with `mountvol /D` (only a partition it mounted,
and only when that letter still holds that volume, as `Dismount-Esp` checks), whatever happened. Its answer goes to the
`--out` file as JSON. A change is given the plan's hash (`--expect`) and the letter the plan was made at (the plan's
`EFI:` line names it): it refuses when the EFI no longer gives that plan, so nothing is written that the user did not
see. A partition needs two UAC prompts per change (the plan, then the change); a USB stick needs none.

**Tested.** On macOS and Windows: the xUnit tests (all 32 vectors, the helper with a fake mounter: plan, change,
refusals, unmounting after a failure, a letter that moved, a partition that is not an ESP; the package folder, zip and
sums; quoting). On Windows: `Test-AppInterop.ps1` and the app itself on fake EFI folders, including the helper started
through `runas` (`--helper-for-folders`, from an elevated session so that no prompt appears). **Not tested: mounting and
changing a real EFI partition** (`MountvolMounter`); the test machine boots macOS from one, so the mount code was run
only with the mounting abstracted.

## How the USB installer page works

**Apple's recovery protocol**, as OpenCorePkg's `Utilities/macrecovery/macrecovery.py` (BSD-3-Clause) speaks it,
written again in C# (`Stick/Recovery.cs`): a GET of `http://osrecovery.apple.com/` for the `session` cookie; a POST to
`/InstallationPayload/RecoveryImage` with the lines `cid`, `sn`, `bid`, `k`, `fg`, `os` (random hex for `cid`, `k`, `fg`);
the answer's `AU`/`AT` and `CU`/`CT` give the image's and the chunklist's URL and token, fetched with the cookie
`AssetToken=TOKEN`, all with the tool's `User-Agent: InternetRecovery/1.0`. The board IDs come from the tool's
`recovery_urls.txt` and `boards.json`: Tahoe `Mac-CFF7D910A743CAAF` with `os=latest`, Sequoia `Mac-7BA5B2D9E42DDD94`
with `os=default`. `sn` is the tool's default all-zero MLB (`00000000000000000`); nothing about the PC is sent. The
answer is refused unless both URLs are Apple's (`*.apple.com`) and name `NAME.dmg` and `NAME.chunklist`.

**The chunklist** (`Stick/Chunklist.cs`) is fetched before the image and must be version 1, chunk method 1 and
signature method 1: an RSA-2048 PKCS#1 v1.5 SHA-256 signature, stored little endian, of the header and the chunk hashes,
checked with Apple's EFI ROM public key (the key macrecovery.py uses). An unsigned chunklist (method 2) is refused,
like any other shape. The image is then downloaded into a `.part` file while each piece is hashed as it arrives; the
first piece that does not match stops the download and deletes the file; the image becomes `BaseSystem.dmg` only when
every piece matched and it ended where the last piece ends. A marker file records the download; the page finds it again
by checking the chunklist's signature and the image's size, and the helper checks every piece once more before writing.

**The disks** (`Stick/StickDisk.cs`, read through `MSFT_Disk`, `MSFT_Partition` and `Win32_DiskDrive`, without
administrator rights and without mounting anything) are listed only when they are USB. A USB disk is still refused when
Windows booted from it or it holds the system (EFI) partition Windows started from, when Windows does not report it as
removable media, when it is read-only or offline, when it is smaller than the image plus 256 MB, when it holds an
operating system (a volume with `Windows\System32\ntoskrnl.exe`, an APFS container, an Apple boot partition, a Windows
recovery partition, Linux root, file system or LVM partitions; EFI system partitions are never
opened), when it holds the files to copy or the app, or when Windows reports neither a serial number nor a unique ID.
The disk is identified by its serial number (or unique ID) and size: the page keeps a salted SHA-256 of them, the
helper's command line carries only that hash and the salt, and the helper reads the disk twice, once after checking the
files and once right before erasing, and refuses it unless the hash and the size match and no rule refuses it. The
serial number is never shown, logged or written anywhere.

**The writing** (`Stick/StickWriter.cs`) runs only in the helper: the page starts its own exe through `runas` as
`IntelXeManager.exe --stick-helper write --disk N --size BYTES --id HASH --salt SALT --image FOLDER [--efi FOLDER]
--progress FILE --out FILE` and shows the progress the helper writes. The helper refuses without administrator rights,
checks the image against the chunklist, the EFI folder, the disk (twice, as above), then, through Windows' storage API
(`MSFT_Disk.Clear`, `Initialize`, `CreatePartition`, `MSFT_Volume.Format`), clears the disk, makes it **GPT** (Windows adds its 16 MB
reserved partition) with one basic-data partition of up to 16 GiB, formats it **FAT32** (label `MACOS`), copies `com.apple.recovery.boot\BaseSystem.dmg`
and `.chunklist` and the EFI folder, and reads them back (the image against the chunklist, the EFI files by SHA-256).
GPT and not MBR: the stick is only ever started by UEFI firmware (macOS needs CSM off), UEFI reads FAT32 on GPT as on
MBR, and GPT is what Dortania's Windows guide makes (`convert gpt`; it reports MBR sticks failing to boot with one of its
tools). Up to 16 GiB because Windows formats FAT32 only up to 32 GB. `--stick-helper check` does everything before the
erase and nothing after; `list` prints the disks with their refusals; `download --version tahoe|sequoia --image FOLDER`
downloads without the window.

**The EFI seam.** The EFI folder comes from an `IEfiSource` (`Stick/IEfiSource.cs`): a folder the user chooses
(`FolderEfiSource`), or the EFI built on the Build the EFI page (`EfiBuild/BuiltEfiSource`, the build's `EFI\`, checked
to hold OpenCore). A stick written with the built EFI completes the Set up page's stick step.


**Tested.** On macOS and Windows: the xUnit tests (the BIOS data in both languages, its sources and anchors, a path for
every maker, the states from facts, the printable page; the recovery requests, the two answers and the two real
chunklists recorded from Apple's server, a changed byte, an unsigned chunklist, a bad piece, a short image, the
download against a fake server; the disk rules, the identity hash, the command line, the writer with the disks faked:
written and read back, a check that writes nothing, the boot disk, another disk at the same number, a disk swapped
between the check and the erase, a changed image, a folder that is not an EFI). On Windows 11 with the app itself: the
Tahoe recovery image downloaded from Apple by `--stick-helper download` (92 of 92 pieces matched Apple's signed
chunklist), `check` refused each internal NVMe disk (the boot disk, the macOS disk, the third disk) and the VHDX
without `--test-vhdx`, and listed nothing that is not USB; a 2 GB VHDX written through `--test-vhdx` by the helper
and by the page (typed confirmation, `runas`): GPT, one FAT32 partition `MACOS` with `com.apple.recovery.boot\`
and the EFI folder, the image on it the same SHA-256 as the download. **Not tested: a real USB stick** (written, then
booted by a PC), a USB disk that Windows reports as fixed (it is refused), and Sequoia's download (its request and
chunklist were checked, not the whole image). The BIOS page ran on the test machine (an ASUS Z790 board) in both
languages and themes, and its exported page was read at phone width.

## How the Build the EFI page works

**The builder beside the app.** `build.ps1` puts `engine\` into the published folder: `engine\python\` is python.org's
embeddable Python 3.12.10 (its `python.exe` and `python312.dll` signed by the Python Software Foundation) with pywin32,
WMI and certifi unpacked into `Lib\site-packages` (pywin32's IDE, help and the wheels' `.data` folders left out) and a
`python312._pth` that names the search path; `engine\efibuild\` is `windows/efibuild` without its tests, with
`upstream\OpCore-Simplify-<commit>\` and `upstream\Hardware-Sniffer-<commit>\` (each source archive's files checked
against the manifest pinned in `PINNED.json`; `upstream.py` uses a shipped tree instead of its cache and never replaces it
by a download); `engine\MANIFEST.sha256` lists every file. The app checks the engine against that manifest before every
scan and build: a changed, missing or extra file (a `json.py` next to `efibuild.py` would be imported by it) refuses it.
Embedded Python and not PyInstaller, as the design note recommends: no unsigned executable of ours, the code stays
inspectable and checked, and no PyInstaller false positives. Nothing downloaded is run on the machine that builds the
release. Licences: Python PSF-2.0 (`LICENSE.txt` inside), pywin32 PSF-2.0, WMI MIT, certifi MPL-2.0, OpCore Simplify and
Hardware Sniffer BSD-3-Clause (`LICENSE` in their trees).

**The build** runs as the user (`Core/EfiBuild/PythonEfiBuilder.cs`): `engine\python\python.exe -I -B -u -X utf8
engine\efibuild\efibuild.py build --report R --acpi A --out O --macos 26|15 --result O\result.json --verbose` without a
console window, the arguments as a list (never through a shell), standard input closed. `-I` keeps the user's
environment and folders out of Python, `-B` writes no `__pycache__` beside the engine, `-X utf8` makes the output UTF-8
whatever the code page. The window shows only the lines that name a screen (`== ...`) or the rule that answered
(`[rule: why]`); the program's own text stays in the build log. Exit code 0 with a result that is ok is a build; 1 is a
build that stopped (the result's `error`); 2 a pin or input error (its `error:` line); anything else, or no result, shows
the last lines. Cancelling kills the process tree.

**The scan** needs administrator rights (acpidump reads the firmware's tables), so the app starts its own exe through
`runas` as `IntelXeManager.exe --efi-helper sniff --scan DIR --out FILE` (`Core/EfiBuild/SniffHelper.cs`,
`Services/EfiBuildHost.cs`). The helper takes no program or engine path from its command line: it runs the engine beside
the exe, after the manifest check, as `efibuild.py sniff --out DIR` (the pinned Hardware Sniffer and the pinned
`acpidump.exe`). It refuses a scan folder that exists or lies under a link, deletes every table that is not DSDT, SSDT,
APIC or DMAR (by signature, also when the scan fails; the builder already copies only those and now removes the whole
dump from its cache whatever happens), and answers in JSON.

**The SMBIOS identity** (serial number, MLB, UUID, ROM) is generated by macserial into `config.plist` and stays there.
The builder removes every form of it from its log and result; the app reads the values from the built `config.plist` and
removes every form again (text in either case, a UUID without dashes, data as hex, colon hex and base64:
`EfiBuild/SmbiosMask.cs`) from all it shows, and shows PlatformInfo with those keys masked (`••••••••`). Nothing is sent.

**Tested** on macOS: the xUnit tests (the command lines and refusals, both fixture results, exit codes 0/1/2 and a crash,
a result left from an earlier build, cancellation, a changed or planted engine file, the identity in every form removed
and masked, the scan's folder rules and tables, saving, pruning, the stick's `IEfiSource`, the app's engine on the
sample config before and after stage 2, every wizard state); `windows/efibuild/tests/test_upstream.py`. The App's C# was
type-checked against the Windows App SDK's reference assemblies with the XAML parts stubbed. **Not tested: anything on
Windows** (the XAML compile, the pages, the scan with Hardware Sniffer, WMI and acpidump, the embedded Python with
pywin32, a build with `iasl.exe`/`ocvalidate.exe`/`macserial.exe`, `build.ps1`'s engine step), and booting a built EFI.
