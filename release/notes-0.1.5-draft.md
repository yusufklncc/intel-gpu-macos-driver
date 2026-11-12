<!-- Draft of the 0.1.5 release notes, written from git log v0.1.4..main on 12 November 2026. Before the release:
     check each item against what the packages really carry (which binaries were rebuilt), fill in the measurements of
     the release build, remove what did not ship, delete this comment and rename the file to notes-0.1.5.md
     (release.sh refuses a notes file that still says draft). -->
The Windows release: **IntelXe Manager for Windows** prepares the OpenCore EFI from Windows with the same plans,
backups and undo as the macOS app, and **one set of EFI rules** now drives every EFI tool (the macOS app, `efi.sh`, the
Windows app and `IntelXe-EFI.ps1`), with four fixes found by comparing them. Also: direct submission is on by default,
and the kernel driver's memory and fence handling follows Linux `xe` more closely.

> [!WARNING]
> Experimental. Tested on one machine with one GPU (Arc B580); the Arc B570 is supported but untested. Not signed by Apple; needs
> relaxed SIP/AMFI and OpenCore. Keep a USB stick with a working EFI before installing.

> [!NOTE]
> Developed with AI assistance: the code was written by Anthropic's Claude Opus 5.5 (High effort level) under the
> direction of the maintainer, who tests every build on the real hardware.

### Highlights

- **IntelXe Manager for Windows** (`IntelXeManager-0.1.5-win-x64.zip`, new). A native Windows app (WinUI 3) with the
  look and wording of the macOS app, now the recommended way to make stages 2 and 4 from Windows:
  - **Overview** reads the PC without changing anything: the Arc card and its Windows driver, the CPU and whether it
    needs `HybridPM.kext`, the board, UEFI and Secure Boot, Resizable BAR and Above 4G decoding, BitLocker, and the
    EFI partitions (which one Windows booted from, which one holds OpenCore).
  - **EFI (OpenCore)**: stage 2, the Safari setting (alone or with stage 2), stage 4 (`IntelXe.kext`,
    `SSDT-ARCOFF.aml` for the ACPI path you confirm, adapted to it when it is not the shipped one, `HybridPM.kext` on a
    hybrid CPU) and undo, on an EFI partition of the PC or an EFI folder (a USB stick's). Each change shows its plan
    first (steps, notes, where the backup goes, the plan's SHA-256), backs up the whole EFI folder, and is made only
    while the EFI still gives that plan. Plans and results are in English or Turkish.
  - **Report** runs the Windows diagnostics script (`collect-windows.ps1`, shipped with the app) and opens the bug or
    new-hardware issue form with the report's ID. Nothing is uploaded.
  - It runs from its folder: nothing to install, .NET and the Windows App SDK are inside (about 170 MB unpacked,
    66 MB zipped). Windows 10 (2004 or later) or 11, x64.
  - **Administrator rights only for an EFI partition:** the app runs with your rights, and starts a helper of its own
    with administrator rights (Windows asks) only to mount, read and change an EFI partition. An EFI folder on a USB
    stick needs no prompt.
  - **Not code-signed.** The project has no code-signing certificate, so SmartScreen may stop the first start ("Windows
    protected your PC": *More info* > *Run anyway*) and the administrator prompt shows "Unknown publisher". Windows
    would show the same for a changed copy: check the zip's SHA-256 against `intel-gpu-macos-driver-0.1.5.sha256`
    (`Get-FileHash .\IntelXeManager-0.1.5-win-x64.zip -Algorithm SHA256`), which is signed with the project's key.
  - `IntelXe-EFI.ps1` in the `-efi.zip` stays: the scripted way (command line, or when the app cannot run). It now
    also makes the Safari setting (`-Plan safari`, `-Plan settings-safari`), as `efi.sh` does. Each tool undoes the
    other's changes.
- **One set of EFI rules.** What the driver changes in an OpenCore EFI (stage 2, the Safari setting, stage 4, what undo
  puts back) is now one file, `efi-rules.json`, shipped beside each tool that reads it (the macOS packages and the app
  with `efi.sh`, the `-efi.zip` with `IntelXe-EFI.ps1`, the Windows app built in). 32 shared test vectors run against
  `efi.sh`, the PowerShell script and the Windows app; `release.sh` checks that every shipped copy is the
  repository's. Comparing the two implementations found four fixes:
  - `efi.sh`: stage 2 adds `NVRAM > Delete` when `config.plist` has none, as `IntelXe-EFI.ps1` did. Before, on such a
    `config.plist` stage 2 made its backup and then stopped, the EFI unchanged but the backup without its list of
    changes.
  - `efi.sh`: the backup's list of changes is written step by step, before each step, so that undo also puts back an
    interrupted change; a backup without a list no longer blocks undoing the changes before it.
  - `efi.sh`: every file of the package's kexts and the table must be listed in its `SHA256SUMS` and match before
    anything is copied (a file added to a kext after packaging was copied unchecked); a plan says when the package has
    no `SHA256SUMS`.
  - `IntelXe-EFI.ps1`: the edited `config.plist` is checked value by value as each is set. On a hybrid CPU, the
    `HybridPM.kext` entry inserted below Lilu moved `IntelXe.kext`'s entry, the check afterwards read the wrong item,
    and the tool refused a change `efi.sh` made.
- **Direct submission on by default.** The fast job start of 0.1.4 (`debug.intelxe.direct`, opt-in there) is on from
  the start; `-intelxe_nodirect` in the boot-args turns it off. Unattended on 0.1.4 with it on (11-12 November):
  three rounds of regression tests, Geekbench, 30 minutes idle, sleep and wake, no fault; Geekbench 7 Metal
  97.5-98.9k, OpenCL 93.3-94.0k.

### The kernel driver (IntelXe.kext)

Memory and synchronisation brought closer to Linux `xe`, aimed at the GPU fault after a long idle (still open, see
Known issues):

- Freed memory and page tables wait for the jobs of their VM; a split huge page invalidates its whole span; new page
  tables are written before they are linked.
- An unbind's out-fence signals only after its TLB invalidation; GGTT invalidations go through the GuC and are waited
  for, on both GTs; the copy windows of suspend, resume, surface copies and BO moves are waited for like every unbind.
- After a GuC context reset, a job that never starts resets the GT promptly instead of after 10 s; a timeline point
  with several fences is reached only when all have signalled; the TLB fence timeout is Linux's.
- Asynchronous bind user fences, per-bind-queue out-fences, `EBUSY` for unbinding a range whose user fence is pending,
  user fences kept and written after their file is closed, user fences in BO memory.
- Long-running VMs (`DRM_XE_VM_CREATE_FLAG_LR_MODE`): accepted as Linux does, without a job timeout, their memory
  protected by preemption (for Intel's compute runtime, below).
- Asynchronous binds behind `debug.intelxe.bind_async` (off by default).

### Experimental and off by default

Each of these is in the release's binaries, does nothing unless turned on, and has not yet been run on the B580 unless
it says so:

- A second display pipe, `-intelxefb_pipeb` (needs a second monitor; `sysctl debug.intelxefb_pipeb_plan` is a dry run
  for one), and NMXAccel accelerating a second head behind `nmxheads=1`.
- Metal sparse textures, tier 1 (`NVMTL_SPARSE_TEXTURES=1`), and shader-encoded indirect command buffer state through
  `VK_EXT_device_generated_commands` (`NVMTL_ICB_DGC=1`).
- Fence polling before the kernel wait (`NVMTL_FENCE_SPIN_US`), importing unaligned host pointers
  (`NVMTL_IMPORT_UNALIGNED=1`: Geekbench 7 OpenCL 87.7k -> 89.0-89.4k on the B580), ANV completion through user fences
  (`ANV_UFENCE=1`), `VK_EXT_memory_priority` as a recorded hint.
- The VideoToolbox encoder's upload through a cached import and the copy engine (`INTELXEVENC_UPLOAD=import`).
- Native OpenGL apps through Zink (`cglzink-app.sh`): `CGLSetOffScreen`, drawing into IOSurface textures, CGL
  pbuffers.
- **Vulkan for Windows games in Wine** (the optional `-vulkan.zip`, when the release has it): a Vulkan provider on
  ANV for Wine, DXVK and vkd3d-proton, with `intelxe-vk`; IntelXe Manager installs, tests and removes it on its Zink
  page.

### Packages

| File | For | Unpack on |
|---|---|---|
| `IntelXe-Manager-0.1.5.zip` | the IntelXe Manager app (macOS 15 or later) | macOS |
| `IntelXeManager-0.1.5-win-x64.zip` | **new:** IntelXe Manager for Windows: stages 2 and 4 from Windows, a check of the PC, reports | Windows 10 or 11 (x64) |
| `intel-gpu-macos-driver-0.1.5-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.5-sequoia.zip` | macOS 15 Sequoia (same sources; not re-tested on Sequoia) | macOS |
| `intel-gpu-macos-driver-0.1.5-efi.zip` | the OpenCore pieces: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (optional), `IntelXe-EFI.ps1` and `efi-rules.json` | any system |
| `intel-gpu-macos-driver-0.1.5-zink-gl.zip` | optional: OpenGL through Mesa's Zink | macOS |
| `intel-gpu-macos-driver-0.1.5-vulkan.zip` | optional, experimental (when present): Vulkan for Wine | macOS |

Checksums: `intel-gpu-macos-driver-0.1.5.sha256`, signed with the project's key in
`intel-gpu-macos-driver-0.1.5.sha256.sig`; it lists the Windows app's zip too. Install guide: `README-INSTALL.md` in
each macOS package and
[in the repository](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.5/release/README-INSTALL.md)
(Turkish: `README-INSTALL.tr.md`); its new section *On Windows: IntelXe Manager for Windows* covers the Windows app.

### Upgrading from 0.1.4

1. Install the package (the app: *Download v0.1.5*, then *Install this package*; or `sudo ./install.sh --apply`).
2. **Replace `IntelXe.kext` in the EFI** before you restart: the app's EFI page or `sudo ./efi.sh --apply` on macOS;
   on Windows IntelXe Manager for Windows (EFI page, *IntelXe.kext and SSDT-ARCOFF (stage 4)*) or `IntelXe-EFI.ps1`;
   or by hand. Its `config.plist` entry stays as it is.
3. If you installed the system-volume copy for Safari, install this package's too (the app's Driver page, *Safari and
   WebKit*; or `sudo ./system-volume/ssv-install.sh --apply`).
4. Restart. To go back to 0.1.4's behaviour for job submission only, add `-intelxe_nodirect` to the boot-args.

From 0.1.3 or earlier, follow the guide's upgrade section first.

### For developers

- **Intel's compute runtime on macOS** (not in the packages): Level Zero 1.18 and the OpenCL ICD (`compute-runtime`,
  gmmlib, the loader) build for macOS x86_64 over `xemac`, with Intel's graphics compiler (IGC) built for macOS
  (`build-igc.sh`); `ze_hello` and `cl_hello` (`docs/design/DESIGN-level-zero.md`).
- Release signing keys can change without breaking installed apps (`.sha256.sigs`, `SIGN_KEY_NEXT`,
  `docs/RELEASE-KEYS.md`); `package.sh` refuses a Metal driver bundle of another version, and takes the Windows app's
  zip as built on Windows (`WINAPP_ZIP`), checking its version, files and SHA-256.
- The Windows app's Core (models, rules, the EFI engine of `IntelXe-EFI.ps1` compiled in, its xUnit tests with the
  shared vectors) builds and tests on macOS and Linux too: `dotnet test windows/app/IntelXe.Core.Tests`.
- The project site (GitHub Pages) has an install guide by path, an Apps and a Drivers page and a symptom index;
  `tools/site-guide.py --check` keeps the guide true to the packages' install guide.

### Known issues

- **IntelXe Manager for Windows has not yet changed a real EFI partition**: its EFI page was tested on Windows 11 on
  test EFI folders (its administrator helper included) and by the shared vectors, the partition mounting only with a
  fake. Neither Windows tool has yet prepared an EFI that a PC then booted. Keep a USB stick with a working EFI and
  read each plan before you confirm it.
- **The Windows app is not code-signed** (above): SmartScreen and the administrator prompt name an unknown publisher.
- **GPU faults after a long idle (cause open)**, as in 0.1.4: the GuC low-latency hint stays off by default. The
  kernel changes above follow the leading hypotheses; if you see a `MEMORY_CAT_ERROR`, please send a report.
- The rest as in 0.1.4: the B570 is untested; PCIe ASPM (`debug.intelxe.aspm_l1`) has not run on a B580 (the test
  machine's root port advertises no L1); one monitor on DisplayPort; a held boot stays at the Apple logo (Reset NVRAM
  in the OpenCore picker); Safari offers no WebGPU `timestamp-query` on Intel GPUs.

### Credits

- As in every release: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` and i915, Mesa, FFmpeg,
  Emil J. Tywoniak's Xe2 VDEnc work and Lilu (acidanthera); see the README.
- The Windows app is built on Microsoft's .NET and Windows App SDK (WinUI 3), which its folder carries.
