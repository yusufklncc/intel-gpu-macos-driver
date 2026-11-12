# IntelXe Manager for Windows

A native Windows app (WinUI 3, Windows App SDK, C#) with the look and wording of the macOS app **IntelXe Manager**
(`app/`): the same sidebar groups, section titles, accent colour, status colours and texts, in Fluent form (Mica,
NavigationView, light and dark following Windows). It is meant to replace `windows/IntelXe-EFI.ps1` once the EFI steps
are wired to the shared rules: check this PC, prepare the OpenCore EFI, collect a report.

**This version is the skeleton and the read-only parts.** It reads and shows; it mounts nothing, writes nothing to a
disk, a partition or the firmware, and uploads nothing.

## Layout

| Folder | What it is |
|---|---|
| `IntelXe.Core/` | Plain .NET 10 class library, no Windows APIs: the models and rules. Supported cards by PCI ID from `data/platforms.json` (embedded at build time), PCI instance-ID parsing, the CPU family and the hybrid-CPU rule HybridPM.kext needs (the same regex as `efi.sh` and `IntelXe-EFI.ps1`), the ACPI-path conversion of a Device Manager location path, the REG_RESOURCE_LIST parser for the card's BARs (as `tools/report/collect-windows.ps1`), the Overview's checks and their limits, the report script's output, the issue-form URLs, the `.resw` key checks, and the `IEfiPlanner` seam. |
| `IntelXe.Core.Tests/` | xUnit tests of the Core. They run on macOS, Linux and Windows. |
| `IntelXe.App/` | The WinUI 3 app: unpackaged (`WindowsPackageType=None`) and self-contained (`WindowsAppSDKSelfContained`, `SelfContained`), `net10.0-windows10.0.19041.0`, x64, Microsoft.WindowsAppSDK 2.6.5. No MSIX: there is no signing certificate, and the folder runs without installing .NET or the Windows App Runtime. |
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

The result is `out\IntelXeManager-VERSION-win-x64\IntelXeManager.exe` (about 230 MB unpacked) and
`out\IntelXeManager-VERSION-win-x64.zip` (about 90 MB). The version is the repository's `VERSION`. Most of the size is the
Windows App SDK carried inside (it includes its AI/ML components, onnxruntime and DirectML, about 45 MB), plus the .NET
runtime. Referencing only the SDK's WinUI, Foundation and InteractiveExperiences packages instead of the
`Microsoft.WindowsAppSDK` metapackage would make it smaller; that is left for later.

On macOS or Linux only the Core builds (the WinUI XAML compiler runs on Windows only):

```sh
dotnet test windows/app/IntelXe.Core.Tests
```

For screenshots and support instructions the app takes, like the macOS app's `--section`:
`--section overview|efi|report|about`, `--lang en-US|tr-TR`, `--theme light|dark`, `--size WIDTHxHEIGHT`,
`--scroll PIXELS` and `--report-out FOLDER` (the report goes there instead of the Desktop).

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
  read by a program that is not an administrator, and this version mounts nothing, so it says "needs mounting to read".

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

## What is a placeholder

**EFI (OpenCore)** (group Install) describes stage 2 (the settings) and stage 4 (IntelXe.kext, SSDT-ARCOFF.aml,
ExposeSensitiveData, HybridPM.kext) as the macOS EFI page and `IntelXe-EFI.ps1` make them, shows the card's ACPI path
as Windows reports it (`\_SB.PC00.PEG1.PEGP` on the test machine) and whether HybridPM.kext applies, and lets you choose
the target (an EFI partition, or an EFI folder such as a USB stick's). Its *Show the plan* buttons call `IEfiPlanner`;
the planner in this version, `PendingEfiPlanner`, answers that this comes with the shared rules. *Apply* and *Undo* are
off. Until then `IntelXe-EFI.ps1` in the `-efi.zip` makes these changes.

## Wiring in the shared EFI rules

The EFI rules are moving into one rules file with test vectors that `efi.sh`, `IntelXe-EFI.ps1` and this app will
share. To wire them in:

1. Implement `IntelXe.Core.Efi.IEfiPlanner` in the Core (plain .NET, so that the same test vectors run on macOS and
   Windows): `ReadStatusAsync` for the `key=value` facts `-Status` prints, `PlanAsync` for the plan lines, notes and the
   plan's SHA-256, `ApplyAsync` for backup-then-write, refusing when the plan's hash changed. Load the rules file as
   `data/platforms.json` is loaded (embedded at build time) and add a test that runs every vector.
2. The parts the rules need from this PC are already in the Core: `CpuInfo.IsHybrid` (HybridPM.kext),
   `AcpiPath.FromLocationPath` / `Normalize` (SSDT-ARCOFF.aml's path), `SupportedCards` (the device IDs).
3. Mounting an EFI partition needs administrator rights: run the planner's mount and write steps in an elevated helper
   (the app itself stays `asInvoker`), for `EfiTarget.Partition`; an `EfiTarget.Folder` (a USB stick) needs none.
4. Set `AppState.Planner` to the new planner. On the EFI page (`Pages/EfiPage.xaml.cs`, `Actions`), turn *Apply* and
   *Undo* on behind a confirmation dialog that shows the plan and then calls `ApplyAsync` with the plan's hash, and show
   `ReadStatusAsync`'s facts for the chosen EFI, as the macOS EFI page does.
5. When the app makes every change `IntelXe-EFI.ps1` makes, the `-efi.zip` can carry the app instead of the script.
