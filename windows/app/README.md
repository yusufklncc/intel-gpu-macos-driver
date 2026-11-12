# IntelXe Manager for Windows

A native Windows app (WinUI 3, Windows App SDK, C#) with the look and wording of the macOS app **IntelXe Manager**
(`app/`): the same sidebar groups, section titles, accent colour, status colours and texts, in Fluent form (Mica,
NavigationView, light and dark following Windows): check this PC, prepare the OpenCore EFI, collect a report. Its EFI
page makes the same changes as `windows/IntelXe-EFI.ps1`, by the same rules (`release/templates/efi-rules.json`) and
the same engine, so it can replace the script.

It writes only on the EFI page, only after showing the plan and getting a confirmation, and only to the EFI folder or
EFI partition chosen there, after a backup of the whole EFI folder. It never writes to the firmware and uploads
nothing.

## Layout

| Folder | What it is |
|---|---|
| `IntelXe.Core/` | Plain .NET 10 class library, no Windows APIs: the models and rules. Supported cards by PCI ID from `data/platforms.json` (embedded at build time), PCI instance-ID parsing, the CPU family and the hybrid-CPU rule HybridPM.kext needs (the same regex as `efi.sh` and `IntelXe-EFI.ps1`), the ACPI-path conversion of a Device Manager location path, the REG_RESOURCE_LIST parser for the card's BARs (as `tools/report/collect-windows.ps1`), the Overview's checks and their limits, the report script's output, the issue-form URLs, the `.resw` key checks. The EFI part (`Efi/`): `efi-rules.json` (embedded at build time), IntelXe-EFI.ps1's engine (compiled in at build time, see below), the planner, the package (folder or zip, SHA256SUMS), and the elevated helper's command line, work and answer. |
| `IntelXe.Core.Tests/` | xUnit tests of the Core. They run on macOS, Linux and Windows. `EfiVectorTests` runs every vector of `release/efi-rules-vectors.json` through the Core's planner and apply; `EfiHelperTests` the helper with the mounting faked, the package and the command line. |
| `IntelXe.App/` | The WinUI 3 app: unpackaged (`WindowsPackageType=None`) and self-contained (`WindowsAppSDKSelfContained`, `SelfContained`), `net10.0-windows10.0.19041.0`, x64, Windows App SDK 2.6.5 (only its WinUI and Foundation packages). No MSIX: there is no signing certificate, and the folder runs without installing .NET or the Windows App Runtime. `Program.cs` is its entry point: `--efi-helper` runs the elevated helper without a window. |
| `IntelXe.App/Strings/{en-US,tr-TR}/Resources.resw` | English and Turkish. The Turkish texts are the macOS app's (`app/Resources/tr.lproj`) wherever the same text appears there. A test holds both files to the same keys and `{n}` placeholders, and to every key the app's sources and the Core's checks name. |
| `build.ps1` | Restore, build, test, publish a self-contained folder and zip it (`out/`). |

The CI workflow `.github/workflows/windows-app.yml` runs `build.ps1` on `windows-latest` for pushes and pull requests
that touch `windows/app/**` (or `data/platforms.json`, `tools/report/collect-windows.ps1`) and keeps the zip as a build
artifact. It releases nothing.

## Building

On Windows, with the .NET 10 SDK (nothing else; the NuGet packages go to the user's cache):

```powershell
cd windows\app
powershell -ExecutionPolicy Bypass -File .\build.ps1          # or: pwsh ./build.ps1
```

The result is `out\IntelXeManager-VERSION-win-x64\IntelXeManager.exe` (about 170 MB unpacked),
`out\IntelXeManager-VERSION-win-x64.zip` (about 66 MB) and the zip's SHA-256 beside it (`.zip.sha256`). A release
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
`--section overview|efi|report|about`, `--lang en-US|tr-TR`, `--theme light|dark`, `--size WIDTHxHEIGHT`,
`--scroll PIXELS` and `--report-out FOLDER` (the report goes there instead of the Desktop). For the EFI page:
`--efi-folder FOLDER` (chosen as the EFI), `--package FOLDER|ZIP`, `--card-confirmed` (the ACPI path box ticked),
`--plan settings|safari|settings-safari|driver|undo` (that plan's dialog opened at start) and `--helper-for-folders`
(an EFI folder goes through the elevated helper too, to test that path without an EFI partition).

## What works

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
