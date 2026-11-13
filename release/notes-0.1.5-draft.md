<!-- Draft of the 0.1.5 release notes, written from git log v0.1.4..main (up to 6adbb5b, the sycl merge) on 13 November
     2026. Before the release: check each item against what the packages really carry (which binaries were rebuilt from
     main: IntelXe.kext, IntelXeFB.kext, NMXAccel.kext, HybridPM.kext, the Metal driver, Mesa/ANV, the video decoders and
     encoders, Zink), fill in the measurements of the release build, settle every "decision pending" below, remove what
     did not ship, delete this comment and rename the file to notes-0.1.5.md (release.sh refuses a notes file that still
     says draft or still holds a "decision pending" marker). -->
The Windows release: **IntelXe Manager for Windows** prepares the OpenCore EFI from Windows with the same plans,
backups and undo as the macOS app, and **one set of EFI rules** now drives every EFI tool (the macOS app, `efi.sh`, the
Windows app and `IntelXe-EFI.ps1`), with four fixes found by comparing them. Also: direct submission is on by default,
the kernel driver's memory and fence handling follows Linux `xe` more closely, uploads to the card use PCIe Extended
Tags, opt-in **boot logging** for a PC that does not start, a **macOS update guard** and an opt-in **game mode** in
IntelXe Manager, the **Arc Pro B60 and B50** (untested), and more of Metal's API.

> [!WARNING]
> Experimental. Tested on one machine with one GPU (Arc B580); the Arc B570, Pro B60 and Pro B50 are supported but
> untested. Not signed by Apple; needs relaxed SIP/AMFI and OpenCore. Keep a USB stick with a working EFI before
> installing.

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
    hybrid CPU), boot logging (below) and undo, on an EFI partition of the PC or an EFI folder (a USB stick's). Each
    change shows its plan first (steps, notes, where the backup goes, the plan's SHA-256), backs up the whole EFI
    folder, and is made only while the EFI still gives that plan. Plans and results are in English or Turkish.
  - **Report** runs the Windows diagnostics script (`collect-windows.ps1`, shipped with the app; *Boot logs* adds
    OpenCore's logs from the EFI) and opens the bug or new-hardware issue form with the report's ID. Nothing is
    uploaded. It also writes and reads the **Xe probe stick** (below).
  - **Set up** (first in the sidebar, experimental): the steps from a Windows PC to an installed macOS in one list:
    this PC, the BIOS settings, building the EFI, the USB installer, what to do after the install; each step done,
    next or blocked, kept between starts. Its pages:
    - **BIOS settings** for the board's maker (ASUS, MSI, Gigabyte, ASRock or generic menu names and keys): Secure
      Boot, CSM, Above 4G, Resizable BAR, the integrated GPU, VT-d, CFG Lock and Dortania's other Intel settings, each
      marked done, to do or "cannot tell from Windows", with its sources, and a page to save for a phone. It changes no
      setting (and not BitLocker). Ran on the test machine (an ASUS board).
    - **Build the EFI** <!-- decision pending: ship this page in 0.1.5, or hide it until it has run on Windows -->:
      a stage-1 OpenCore EFI for this PC, made by [OpCore Simplify](https://github.com/lzhoang2801/OpCore-Simplify)
      at a pinned commit, driven without questions (every answer a recorded rule), from pinned downloads only
      (OpenCore 1.0.8 and release kexts, each checked by SHA-256), with stage 2 from `efi-rules.json` and `ocvalidate`
      of the same OpenCore release. The scan of the PC (Hardware Sniffer, administrator rights) keeps only the
      DSDT/SSDT/APIC/DMAR tables; the SMBIOS serials are generated, never logged, and masked on screen. **Not yet run
      on Windows**: tested on macOS with seven fixtures (the test machine's Z790 + B580, public B660 and Z490 reports).
    - **USB installer**: macOS 26 Tahoe's or 15 Sequoia's recovery from Apple's servers (checked piece by piece against
      Apple's chunklist), written with an EFI (the one built on this PC, or one you choose) to a USB stick you confirm
      by name and size. Tested on a virtual disk (VHDX); **not yet on a real stick** that a PC then booted.
  - It runs from its folder: nothing to install; .NET, the Windows App SDK and the EFI builder (an embedded Python)
    are inside (about 205 MB unpacked, 83 MB zipped). Windows 10 (2004 or later) or 11, x64.
  - **Administrator rights only for disks:** the app runs with your rights, and starts a helper of its own with
    administrator rights (Windows asks) only to mount, read and change an EFI partition, to scan the PC for the EFI
    builder and to write a USB stick. An EFI folder on a USB stick needs no prompt.
  - **Not code-signed.** The project has no code-signing certificate, so SmartScreen may stop the first start ("Windows
    protected your PC": *More info* > *Run anyway*) and the administrator prompt shows "Unknown publisher". Windows
    would show the same for a changed copy: check the zip's SHA-256 against `intel-gpu-macos-driver-0.1.5.sha256`
    (`Get-FileHash .\IntelXeManager-0.1.5-win-x64.zip -Algorithm SHA256`), which is signed with the project's key.
  - `IntelXe-EFI.ps1` in the `-efi.zip` stays: the scripted way (command line, or when the app cannot run). It now
    also makes the Safari setting (`-Plan safari`, `-Plan settings-safari`) and boot logging (`-Plan bootlog`), as
    `efi.sh` does. Each tool undoes the other's changes.
- **One set of EFI rules.** What the driver changes in an OpenCore EFI (stage 2, the Safari setting, stage 4, boot
  logging, what undo puts back) is now one file, `efi-rules.json`, shipped beside each tool that reads it (the macOS
  packages and the app with `efi.sh`, the `-efi.zip` with `IntelXe-EFI.ps1`, the Windows app built in). 44 shared test
  vectors run against `efi.sh`, the PowerShell script and the Windows app; `release.sh` checks that every shipped copy
  is the repository's. Comparing the two implementations found four fixes:
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
- **Boot logging (opt-in, for troubleshooting).** When macOS did not start there was nothing to report. The EFI pages
  of both apps (*Boot logging (for troubleshooting)*), `efi.sh --bootlog-plan` / `--bootlog-apply` and `IntelXe-EFI.ps1 -Plan bootlog` turn
  on OpenCore's log file, boot.efi's lines in it (`AppleDebug`) and the panic files OpenCore saves at the next boot
  (`ApplePanic`), in the root of the EFI; a RELEASE OpenCore is enough. Every boot is slower while it is on (OpenCore
  rewrites its log after each line), so turn it off (its own change, `bootlog-off`) once the logs are collected. The
  reports collect them: the apps' Report page (*Boot logs*), `collect-macos.sh --boot-logs`, `collect-windows.ps1
  -BootLogs`; serial numbers, MLB, ROM and UUIDs are removed.
- **Direct submission on by default.** The fast job start of 0.1.4 (`debug.intelxe.direct`, opt-in there) is on from
  the start; `-intelxe_nodirect` in the boot-args turns it off. Unattended on 0.1.4 with it on (11-12 November):
  three rounds of regression tests, Geekbench, 30 minutes idle, sleep and wake, no fault; Geekbench 7 Metal
  97.5-98.9k, OpenCL 93.3-94.0k.
- **Faster uploads to the card (PCIe Extended Tags).** macOS never turns on the GPU's Extended Tag field, which Linux
  sets for every device that has it; with only 32 tags for its reads of system memory the card copied host-to-VRAM
  (Shared buffers, texture uploads) well below the link's speed. `IntelXe.kext` now sets it at start and after wake
  (`-intelxe_noexttag` leaves it alone). Measured by a contributor under Linux on a Core Ultra 7 265KF with a B580
  (PCIe 4.0 x8, 256 MB copy): 13.4 GB/s on the copy engine and 11.8 GB/s on the render engine with it, 9.5 and 2.4 GB/s
  without. Not yet measured under macOS on the test machine.
- **IntelXe Manager for macOS:**
  - **macOS update guard.** The Overview's *macOS updates* section says, for each update Software Update offers,
    whether this release supports it (`install.sh --supports-macos`). *Prepare for the macOS update* keeps a checked copy of the installed
    package and a one-shot check that runs at the first start after the update: it checks the kexts, the Metal
    driver, the system-volume copy and the boot guard, reinstalls (`install.sh --apply`) and restarts once only when
    the release supports the new macOS, at most three tries, then removes itself. It never touches the EFI. An update
    made without it is noticed at the next start, with what the check finds and an install button. **Not yet
    run across a real macOS update.**
  - **Game mode (opt-in, off at every start).** While a game is in front (the App Store's games category, or an app you
    add from the menu bar) the GPU's lowest clock is raised (2000 MHz, 2400 MHz or the card's highest), and put back
    when it leaves. With this release's kext it uses the new `debug.intelxe.slpc_boost` (below): no password, no
    helper, and the floor ends by itself within 10 seconds if the app dies. Optionally Time Machine backups are
    stopped while a game runs (that asks for the administrator password). **Not yet run on the B580:** the effect on
    frame times is still to be measured.
  - The Zink page installs, tests and removes the Vulkan provider for Wine (the `-vulkan.zip`, below).
- **HybridPM for Arrow Lake desktop.** `HybridPM.kext` now also corrects macOS's CPU power management on Core Ultra
  200S desktop CPUs (Arrow Lake, model 0xC6), with each core type's HWP scale as measured from Linux on a Core Ultra 7
  265KF; without it a single-threaded loop took 8.4-9.0 s on macOS there, against 2.5 s on Linux. Measured by a
  contributor on their machine, not by us. The EFI tools do not add it on a Core Ultra yet (they go by the CPU's name):
  add it by hand as the guide's *Hybrid Intel CPUs* section says. Other Core Ultra models stay unsupported.
  <!-- decision pending: let efi-rules.json's hybridpm cpu_include take Arrow Lake desktop (Core Ultra 2xxK/KF/F) -->

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
- These waits cost Geekbench 6 OpenCL about 3.5% on the B580 (86.7k -> 83.7k, no resets; night of 12-13 November).
  Each can be switched off for an A/B (`debug.intelxe.bind_tlb_fence`, `exec_tlb_dep`, `defer_jobs`,
  `ggtt_inval_wait`; default 1, the behaviour above), and `debug.intelxe.stats` counts what they cost (the "rb8 cost"
  line). The likely cause, an EXEC that waits for a TLB invalidation and so leaves direct submission, has a fix behind
  `debug.intelxe.tlb_inring` (default 0): the EXEC waits for the invalidation in the ring and its queue stays armed.
  Not yet run on the B580. <!-- decision pending: tlb_inring default (0 or 1) and the four rb8 knobs' defaults, after the night A/B -->
- **`debug.intelxe.slpc_boost`:** a GPU minimum clock that any user may ask for, as a 10-second lease (renewed by
  writing again, ended by writing 0, by sleep or when it runs out): one lease for the GPU, clamped to the
  administrator's ceiling (`slpc_max`, else the card's highest) and never below `slpc_min`, at most 10 writes a second.
  IntelXe Manager's game mode uses it. Host-tested (48 checks); not yet run on the B580.
- **Scan-out of modes whose frame does not end on a page** (3440x1440 is 4837.5 pages): every frame of such a mode
  was refused and went the CPU copy path (66 fps on a 165 Hz panel). The frame is now rounded up to whole pages, as
  IntelXeFB maps it. Found on a contributor's 3440x1440 monitor.
- **Arc Pro B60 (8086:E211) and Pro B50 (8086:E212)**: the B580's chip (BMG-G21), so the kexts, `install.sh`, both
  apps and the EFI tools accept them and About This Mac names them; VRAM and the BAR are read from the card. **We have
  no Pro card and have not run one.** BMG-G31 (Arc Pro B70 and its siblings) stays unsupported; for testers only, the
  boot-arg `-intelxe_allow_g31` lets the kexts try it, with warnings in the log. `Wa_14022085890` (the SLPC minimum of
  1200 MHz) is applied per platform as Linux does (unchanged on the B580 and B570).
- Asynchronous binds behind `debug.intelxe.bind_async` (off by default).
- Lunar Lake groundwork (no hardware; Lunar Lake stays refused at start): its platform entry checked against Linux,
  the GSC firmware names of Meteor Lake, Lunar Lake and Panther Lake fixed, a memory model per platform; XeProbe reads
  an integrated GPU's stolen memory and GGTT (`docs/design/DESIGN-lunar-lake.md`).

### The Metal driver

- **Texture arrays in argument buffers:** a sampled `array<texture2d<T>, N>` in an argument buffer was one array
  variable that the driver declared with a single descriptor, the first element's texture, so the other elements did
  not read their own textures. Each element now reads its own bindless slot, as a single texture field does.
  Translator suite: 2508 passed.
- **Pipelines with an object stage** (object + mesh) lost the GPU on the B580 (20 s, then the device was gone); they
  are now refused with an error (`NSError` code 42) the application can handle, and the GPU keeps running. Mesh
  pipelines without an object stage are unchanged. `NVMTL_MESH_OBJECT=1` builds them as before.
- A kernel the translator cannot take for indirect command buffer commands is now a function whose pipeline fails with
  an error, as on Apple GPUs (it was `nil`, which an application passed on and crashed).
- **More of Metal's API, untested on the B580** (each checked against Apple's GPU where it has the feature):
  - Pipeline reflection as Apple's: threadgroup arguments of kernels, the declared access of buffers and textures,
    the AIR's alignment.
  - Binary functions and dynamic linking through the visible-function-table emulation (`binaryFunctions`, pipelines
    with additional binary functions, Metal 4's binary-function and linking descriptors). Tile pipelines, machine
    learning pipelines and pipeline scripts are refused with `MTLDeviceErrorNotSupported`.
  - `MTL4Compiler`: reflection, static linking, required threads per threadgroup and archives (with the Metal 4 entry
    points, which stay off unless `NVMTL_METAL4=1`).

### Experimental and off by default

Each of these is in the release's binaries, does nothing unless turned on, and has not yet been run on the B580 unless
it says so:

- **The descriptor-buffer model** (`NVMTL_DESC_MODEL=buffer`) now has everything the classic model has (heap bindings
  2-4, pushed table addresses, GL-UBO, GPU-encoded indirect command buffers) and no longer writes a new region per
  dispatch (1.000 -> 0.165 regions per bind in its model of Geekbench's compute). The default stays the classic model:
  on the night of 12 November, before that change, Geekbench 6 Metal scored 31,969 in the buffer model against 93,475
  in the classic one.
- **H.264 decoding straight through Vulkan Video** (`INTELXEDEC_DIRECT=h264` in the application's environment):
  progressive 8-bit Baseline/Main/High streams decoded with our own parameter-set and slice-header code and one command
  buffer per picture instead of libavcodec's decoder; everything else stays on libavcodec. Off the GPU its pictures
  and reference lists match libavcodec's on 6 recorded streams and 89 of the JVT conformance streams.
  <!-- decision pending: default after the B580 A/B (tests/vtdirect-ab.sh: identical pictures, CPU per picture) -->
- The VideoToolbox encoder's upload through a cached import and the copy engine (`INTELXEVENC_UPLOAD=import`).
  <!-- decision pending: default (copy or import) after the night A/B (vtencq --upload copy,import) -->
- Metal sparse textures, tier 1 (`NVMTL_SPARSE_TEXTURES=1`), now with **placement sparse buffers**
  (`MTLBufferSparseTier1`, mapped through Metal 4 queues) and **tier 2** (`NVMTL_SPARSE_TIER2=1` as well: per-tile
  access counters, emulated with atomics in the shaders that sample a sparse texture; with it off every translation
  is byte for byte as before); shader-encoded indirect command buffer state through
  `VK_EXT_device_generated_commands` (`NVMTL_ICB_DGC=1`; on its first B580 run the compute slots read zeros: a bug,
  open).
- Fence polling before the kernel wait (`NVMTL_FENCE_SPIN_US`), importing unaligned host pointers
  (`NVMTL_IMPORT_UNALIGNED=1`: Geekbench 7 OpenCL 87.7k -> 89.0-89.4k on the B580, but one GPU fault in six runs, so
  it stays off until an unattended A/B), ANV completion through user fences (`ANV_UFENCE=1`), `VK_EXT_memory_priority`
  as a recorded hint.
- A second display pipe, `-intelxefb_pipeb` (needs a second monitor; `sysctl debug.intelxefb_pipeb_plan` is a dry run
  for one), and NMXAccel accelerating a second head behind `nmxheads=1`.
- Native OpenGL apps through Zink (`cglzink-app.sh`): `CGLSetOffScreen`, drawing into IOSurface textures, CGL
  pbuffers.
- **Vulkan for Windows games in Wine** (the optional `-vulkan.zip`, when the release has it): a Vulkan provider on
  ANV for Wine, DXVK and vkd3d-proton, with `intelxe-vk`; IntelXe Manager installs, tests and removes it on its Zink
  page.
- **The Xe probe stick** (`xe-probe-stick-0.1.0.img.zip`, when the release has it)
  <!-- decision pending: ship the image in 0.1.5; it has not run on real hardware -->: a bootable USB image for people
  with an Intel GPU the driver does not support yet. It starts the PC offline into Linux 7.3, keeps every internal disk
  read-only, lets Linux's `xe` or `i915` driver start the GPU under `mmiotrace` and writes one redacted, sealed report
  to the stick, for a *Support for my Intel GPU* issue. IntelXe Manager for Windows downloads it (checked against the
  release's signed list of hashes), writes it and reads the reports back; `tools/probe-stick/write-stick.sh` writes it
  on macOS and Linux. Tested in QEMU only.

### Packages

| File | For | Unpack on |
|---|---|---|
| `IntelXe-Manager-0.1.5.zip` | the IntelXe Manager app (macOS 15 or later) | macOS |
| `IntelXeManager-0.1.5-win-x64.zip` | **new:** IntelXe Manager for Windows: stages 2 and 4 from Windows, a check of the PC, the setup steps, reports | Windows 10 or 11 (x64) |
| `intel-gpu-macos-driver-0.1.5-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.5-sequoia.zip` | macOS 15 Sequoia (same sources; not re-tested on Sequoia) | macOS |
| `intel-gpu-macos-driver-0.1.5-efi.zip` | the OpenCore pieces: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (optional), `IntelXe-EFI.ps1` and `efi-rules.json` | any system |
| `intel-gpu-macos-driver-0.1.5-zink-gl.zip` | optional: OpenGL through Mesa's Zink | macOS |
| `intel-gpu-macos-driver-0.1.5-vulkan.zip` | optional, experimental (when present): Vulkan for Wine | macOS |
| `xe-probe-stick-0.1.0.img.zip` | optional, experimental (when present): the Xe probe stick's image, for Intel GPUs the driver does not support yet | write it to a USB stick |

Checksums: `intel-gpu-macos-driver-0.1.5.sha256`, signed with the project's key in
`intel-gpu-macos-driver-0.1.5.sha256.sig`; it lists the Windows app's zip and the probe stick's image too. Install
guide: `README-INSTALL.md` in each macOS package and
[in the repository](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.5/release/README-INSTALL.md)
(Turkish: `README-INSTALL.tr.md`); its new section *On Windows: IntelXe Manager for Windows* covers the Windows app.

### Upgrading from 0.1.4

1. Install the package (the app: *Download v0.1.5*, then *Install this package*; or `sudo ./install.sh --apply`).
2. **Replace `IntelXe.kext` in the EFI** before you restart: the app's EFI page or `sudo ./efi.sh --apply` on macOS;
   on Windows IntelXe Manager for Windows (EFI page, *IntelXe.kext and SSDT-ARCOFF (stage 4)*) or `IntelXe-EFI.ps1`;
   or by hand. Its `config.plist` entry stays as it is. The same step replaces `HybridPM.kext` where it is used.
3. If you installed the system-volume copy for Safari, install this package's too (the app's Driver page, *Safari and
   WebKit*; or `sudo ./system-volume/ssv-install.sh --apply`).
4. Restart. To go back to 0.1.4's behaviour for job submission only, add `-intelxe_nodirect` to the boot-args.

From 0.1.3 or earlier, follow the guide's upgrade section first.

### Fixed

- `IntelXe.kext`: a mode whose frame does not end on a page (3440x1440) was drawn through the CPU copy path (above).
- The Metal driver: texture arrays in argument buffers, object + mesh pipelines that lost the GPU, a `nil` function
  for indirect command buffer kernels (above).
- The Metal driver's bundle in 0.1.4 said version 0.1.2 (its `Info.plist` was older than its binaries), so IntelXe
  Manager and bug reports showed the wrong version; `package.sh` now refuses a bundle of another version.
- `efi.sh` and `IntelXe-EFI.ps1`: the four fixes under *One set of EFI rules*.
- `tools/probe-stick/write-stick.sh` names a stick that holds an EFI and wants `ERASE EFI` typed (or `--erase-efi`)
  before it overwrites it; the Windows app's two stick writers ask for that in a confirmation of its own.

### For developers

- **Intel's compute stack on macOS** (not in the packages): Level Zero 1.18 and the OpenCL ICD (`compute-runtime`,
  gmmlib, the loader) build for macOS x86_64 over `xemac`, with Intel's graphics compiler (IGC, `build-igc.sh`), and
  now **SYCL**: Intel's DPC++ v7.2.0 (clang `-fsycl` with SPIR-V and ahead-of-time code for the B580, `libsycl`, the
  Unified Runtime's Level Zero adapters) with seven small macOS patches (`compute/build-sycl.sh`,
  `docs/design/DESIGN-sycl.md`). On the B580 every Level Zero step of `ze_hello` passed and `cl_hello` ran with the IGC
  build (13 November); `sycl_hello` has run against a simulated device only.
- **The EFI builder** (`windows/efibuild`, Python, MIT) behind the Windows app's *Build the EFI* page also runs on its
  own: `efibuild.py build --report Report.json --acpi ACPI --out DIR` (`docs/design/DESIGN-efi-build.md`).
- **The Xe probe stick** builds in Docker from pinned inputs, byte for byte the same each time
  (`tools/probe-stick/build.sh`; a QEMU end-to-end test); `collect-linux.sh` v3 has one redaction for the archive and
  for callers (`--redact`).
- **Release signing keys can be replaced without breaking installed apps**, each step checked:
  `release/release-keys.sh` adds a successor key to both apps' key lists together and later retires the old one; the
  release that brings the new key is signed by both (`.sha256.sig` with the old key, which every installed app reads,
  and `.sha256.sigs` with a line per key; `SIGN_KEY_NEXT`). `package.sh` refuses signing keys the apps do not list and
  an app build that does not carry them; `release.sh` checks the `.sig` against the previous release's app and that a
  new key has its `.sigs` line; `release/test-signing.sh` runs the whole three-release change with throwaway keys. No
  successor key exists yet (`docs/RELEASE-KEYS.md`). `package.sh` also refuses a Metal driver bundle of another version, takes the Windows app's zip
  as built on Windows (`WINAPP_ZIP`), checking its version, files and SHA-256, and zips the probe stick's image
  (`PROBE_IMG`) into the signed list.
- The Windows app's Core (models, rules, the EFI engine of `IntelXe-EFI.ps1` compiled in, the setup steps, the disk
  rules of both sticks, its xUnit tests with the shared vectors) builds and tests on macOS and Linux too:
  `dotnet test windows/app/IntelXe.Core.Tests`.
- `docs/design/DESIGN-rebar-window.md`: why `ocvalidate` calls `ResizeAppleGpuBars = 10` unstable with sleep and wake
  (macOS does not restore the Resizable BAR size after sleep; `IntelXe.kext` does) and why the guide keeps 10.
- Tests and tools: `bindstress` (binds and unbinds under running jobs), `unbindfence`, `cl_gbmicro` (a
  Geekbench-OpenCL-shaped microbenchmark), `mtl_desc_paths` (Metal's binding paths in both descriptor models; 29
  cases, all pass on Apple's GPU), `mtl_desc_cost`, `dbuf_model`, `mtl_sparse_tier2`, `mtl4_linking`, `vtdec --lib`,
  `tests/vtdirect-ab.sh`, `sycl_hello`, and host tests for the boost lease (48 checks) and the in-ring TLB wait (3.8
  million). `docs/design/DESIGN-anv-descbuf-cost.md`: what ANV emits for descriptor buffers on Xe2.
- The project site (GitHub Pages) has an install guide by path, an Apps and a Drivers page and a symptom index;
  `tools/site-guide.py --check` keeps the guide true to the packages' install guide.

### Known issues

- **IntelXe Manager for Windows has not yet changed a real EFI partition**: its EFI page was tested on Windows 11 on
  test EFI folders (its administrator helper included) and by the shared vectors, the partition mounting only with a
  fake. Neither Windows tool has yet prepared an EFI that a PC then booted. Keep a USB stick with a working EFI and
  read each plan before you confirm it.
- **The Windows app's Set up, Build the EFI and probe stick pages have not run on Windows yet** (they were checked on
  macOS against the Windows App SDK, their disk writers with files and a virtual disk), and no USB installer or probe
  stick they wrote has booted a PC yet.
- **The Windows app is not code-signed** (above): SmartScreen and the administrator prompt name an unknown publisher.
- **GPU faults after a long idle (cause open)**, as in 0.1.4: the GuC low-latency hint stays off by default. The
  kernel changes above follow the leading hypotheses; if you see a `MEMORY_CAT_ERROR`, please send a report.
- **OpenCL is about 3.5% slower than with 0.1.4's kernel driver** in Geekbench 6 (*The kernel driver*, above).
- **`ocvalidate` warns about `ResizeAppleGpuBars = 10`** ("unstable with macOS sleep-wake"). Keep 10: the driver
  restores the BAR size after wake (`docs/design/DESIGN-rebar-window.md`); whether that restore is ever needed on the
  test board has not been seen yet.
- The rest as in 0.1.4: the B570 is untested (so are the Pro B60 and B50); PCIe ASPM (`debug.intelxe.aspm_l1`) has
  not run on a B580 (the test machine's root port advertises no L1); one monitor on DisplayPort; a held boot stays at
  the Apple logo (Reset NVRAM in the OpenCore picker); Safari offers no WebGPU `timestamp-query` on Intel GPUs.

### Credits

- `@td512` for the scan-out fix for 3440x1440 modes, PCIe Extended Tags and HybridPM's Arrow Lake support, each
  measured on their machine (issue #3).
- [OpCore Simplify](https://github.com/lzhoang2801/OpCore-Simplify) and
  [Hardware Sniffer](https://github.com/lzhoang2801/Hardware-Sniffer) (lzhoang2801, BSD-3-Clause), which the Windows
  app's EFI builder drives at a pinned commit; python.org's embeddable Python and pywin32, which it carries.
- The Xe probe stick boots Arch Linux's kernel and Intel GPU firmware packages with Alpine Linux's busybox; their
  licences are on the stick.
- As in every release: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` and i915, Mesa, FFmpeg,
  Emil J. Tywoniak's Xe2 VDEnc work and Lilu (acidanthera); see the README.
- The Windows app is built on Microsoft's .NET and Windows App SDK (WinUI 3), which its folder carries.
