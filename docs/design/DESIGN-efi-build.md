# Building the OpenCore EFI from scratch: OpCore Simplify without questions, for the Windows app

> Design note with its date (2026-11-13, branch `ocsimplify`): it records the plan and the state of that day, not the
> current status. Code: `windows/efibuild/`. Related: `docs/design/DESIGN-efi-rules.md` (stages 2 and 4),
> `windows/app/README.md` (the Windows app).

Today the user builds stage 1 of the install guide by hand: a plain OpenCore EFI made with Dortania's guide, the
sample of the test machine and SSDTTime/USBToolBox/GenSMBIOS. Our tools take over from stage 2. This note adds the
missing first step for the Windows app:

1. read this PC's hardware (Hardware Sniffer: `Report.json` and the ACPI tables);
2. generate an OpenCore EFI for it (OpCore Simplify, every question answered by a recorded rule);
3. apply stage 2 through the shared rules (`release/templates/efi-rules.json`), so the EFI is ready for the driver;
4. validate it (`ocvalidate` from the same OpenCore release; our own checks).

NullMoth's 1401 does the same for its NVIDIA driver; its `docs/HOW-IT-WORKS.md`, `p1401/engine.py`, `policy.py`,
`validate.py` and `upstream/PINNED.json` were read for ideas (PolyForm Noncommercial: no code was taken). What we
share with it: a pinned OpCore Simplify driven without prompts, an answer per question with a stop on an unknown one,
`ocvalidate` from the same release, a validator that cannot run being a failure.

```
Windows PC                              windows/efibuild (Python, MIT)                       output folder
-----------                             ------------------------------                       -------------
Hardware Sniffer (pinned)  ── Report.json ──▶ engine.py: OpCore Simplify (pinned), headless ──▶ EFI/ (stage 1)
  WMI, acpidump.exe (pinned) ─ ACPI/ ──────▶   answers.py: one rule per question, logged
                                              pins.py: only downloads.lock.json, SHA-256
                                            policy.py: the guide's stage-1 rules          ──▶ EFI/ (stage 1 as tested)
                                            rules.py: efi-rules.json "settings" + "safari" ──▶ EFI/ (stage 2 done)
                                            validate.py: ocvalidate (same release), checks ──▶ build-log.txt, result.json
```

Stage 3 (the driver on macOS) and stage 4 (IntelXe.kext, SSDT-ARCOFF.aml, HybridPM.kext) stay where they are:
after macOS is installed, through IntelXe Manager, `efi.sh`, `IntelXe-EFI.ps1` or the Windows app's EFI page.

## 1. Upstream: fetched at a pinned commit, not vendored

| | pinned |
|---|---|
| OpCore Simplify | `lzhoang2801/OpCore-Simplify` `9aed67e7b41a1648e9b8d4d11db08c029535ff17` (2026-11-08, "Fix C422 quirks default and missing HEDT CPU detection"), BSD-3-Clause, 34 files, manifest SHA-256 `3516e86b…` |
| Hardware Sniffer | `lzhoang2801/Hardware-Sniffer` `ff3b9edd89c4076982a0c8522be9c5dbec94925d` (2026-11-02), BSD-3-Clause, 25 files, manifest SHA-256 `f970e69b…` |

`windows/efibuild/upstream/PINNED.json` names the commits; `OpCore-Simplify.sha256` and `Hardware-Sniffer.sha256`
beside it list every file with its SHA-256, and PINNED.json pins each list by its own SHA-256. `upstream.py` downloads
the commit's source archive from GitHub, unpacks it (regular files only) and compares every file with the list before
every build; a missing, changed or extra file stops the build. The archive's own bytes are not pinned (GitHub does not
promise stable archive bytes); its contents are. Each build runs on a fresh copy, so nothing the program writes into
its own folder (macserial, caches) carries over.

Why not vendored under `windows/efibuild/upstream/`:

- **The licence boundary stays simple.** The repository is MIT plus the parts listed in `docs/LICENSING.md`; vendoring
  adds 3 MB of BSD-3 code (a binary `.icns` among it) that every licence table and scan would have to carry. Fetched,
  it is like `firmware/`: pinned, checked, not in git. The Windows release ships the checked tree with its `LICENSE`.
- **Upgrades are reviewable.** Moving the pin is one commit: the new commit in PINNED.json and the manifest diff,
  which shows exactly which upstream files changed (`efibuild.py upstream --manifest NAME` writes it).
- **The same check covers the release.** The app verifies the shipped tree with the same manifest at run time, so a
  changed file in the user's install folder is refused as a changed download is.

## 2. OpCore Simplify at 9aed67e

### How it runs

`OpCore-Simplify.py` is a console program around a main menu (`OCPE.main`): `1` selects the report and asks everything
else in order (compatibility check, macOS version, hardware customisation, SMBIOS, ACPI tables, ACPI patches, kexts);
`2`–`5` change one choice; `6` downloads OpenCore and the kexts and builds the EFI into its `Results` folder. Every
question goes through `Scripts/utils.py` `Utils.request_input`; `Utils.exit_program` quits after a final "Press Enter".
`updater.py` updates the program from GitHub when started as a script; the builder never runs it (it loads the module
and calls `OCPE().main()`).

### Every question

Source lines are at 9aed67e. "Our answer" is `windows/efibuild/intelxe_efibuild/answers.py`; each answer is logged
with its rule and reason. Any question not listed, or one asked again in a row after it was answered (the program
refused the answer), stops the build.

| where | question | upstream default | our answer |
|---|---|---|---|
| `OpCore-Simplify.py:377` main menu | Select an option | — | `1`; `5` when the Mac model is not the policy's; `6`; after the build the engine stops. The menu's macOS and SMBIOS lines are checked; "Requires OpenCore Legacy Patcher" there stops |
| `:52` Select hardware report | Drag and drop your hardware report (`E` exports on Windows) | — | the `Report.json` given; asked again = refused: stop |
| `:106` Validation Report | Press Enter to go back (report invalid) | — | stop: the report did not pass the program's checks |
| `compatibility_checker.py:491` | Press Enter (after the compatibility list) | — | notice (logged) |
| `compatibility_checker.py:99,113,243,346,369,396` | Press Enter before quitting (no SSE4, no GPU, no supported GPU, Intel VMD, no usable storage) | — | stop with what it printed |
| `:167` Select macOS Version | Please enter the macOS version | the newest native version | `25` (macOS 26) or `24` (macOS 15); stop if not on the menu or marked as needing OCLP |
| `:131` OpenCore Legacy Patcher Warning | Continue with OpenCore Legacy Patcher? | — | stop: a system patched by OCLP is not one the driver is tested on |
| `hardware_customizer.py:39` | Build EFI for UEFI? (report says BIOS/Legacy) | — | `yes` (and the firmware steps say UEFI, CSM off) |
| `hardware_customizer.py:250` | Select a GPU combination | — | the combination that is exactly the Arc card(s): the iGPU is turned off for macOS |
| `hardware_customizer.py:282` | Select a WiFi / Bluetooth device | — | `1`, the first listed (macOS wants one) |
| `hardware_customizer.py:92` | Press Enter (Device Selection Summary) | — | notice |
| `kext_maestro.py:180` (macOS 26, an AppleALC codec) | Select audio kext: 1 AppleALC + SimpleHDA, 2 AppleALC + OCLP | — | `1`: no OCLP root patch |
| `kext_maestro.py:225` (Broadcom Wi-Fi, macOS 14+) | Select Broadcom Wi-Fi kext: 1 SimpleBCM, 2 IOSkywalkFamily + OCLP | SimpleBCM | `1` (checked on the menu) |
| `kext_maestro.py:283` (Intel Wi-Fi) | Select Intel Wi-Fi kext: laobamac AirportItlwm / AirportItlwm / itlwm | laobamac where offered | laobamac AirportItlwm if listed, else itlwm; never plain AirportItlwm on 15+ (it needs OCLP) |
| `kext_maestro.py:504` (AMD Navi 21/23) | Select kext for your AMD GPU | WhateverGreen | the default (not reached when the Arc card is chosen alone) |
| `kext_maestro.py:448,463,501` | Press Enter (WhateverXe, NVIDIA, NootRX notices) | — | notice |
| `kext_maestro.py:805` Kext Compatibility Check | force load kexts on an unsupported macOS? | No | `no` |
| `wifi_profile_extractor.py:301` (itlwm) | Would you like to scan for WiFi profiles? | Yes | `no`: Wi-Fi passwords never go into the EFI. The other prompts of that file (`:115,159,170,180,336,350`) are then never reached |
| `acpi_guru.py:3223` Select ACPI Tables | Please drag and drop ACPI Tables folder | — | the ACPI folder given; asked again = unreadable: stop |
| `acpi_guru.py:146,162,186,200,252` Loading ACPI Table(s) | Press Enter (no valid tables, two DSDTs, not disassembled) | — | stop |
| `smbios.py:219` Customize SMBIOS Model (option 5) | Select your option | — | `a` (show all) when the policy's model is not among the "compatible" ones, its number, `b` |
| `config_prodigy.py:313` Choosing Codec Layout ID | Enter the ID of the codec layout | the default layout | the default (logged with its number) |
| `gathering_files.py:319,353,383` Gathering Files | Press Enter after a failed download | — | stop |
| `:428` (option 6) | Press Enter after an error | — | stop (no rule on that screen) |
| `:341` Before Using EFI | Press Enter (BIOS steps, USB mapping) | — | notice |
| `:439` Result | Press Enter to main menu | — | the EFI is built |
| `acpi_guru.py:3345`, `kext_maestro.py:871` | the ACPI-patch and kext menus (options 3, 4) | — | never opened; a prompt there would stop |

### What it downloads, and how it checks it

| what | upstream source at 9aed67e | upstream check | the builder |
|---|---|---|---|
| OpenCorePkg (EFI base, Sample.plist, ocvalidate, macserial) | Dortania's nightly build list `dortania/build-repo` `builds/latest.json`, newest entry (a build of OpenCorePkg's latest commit, 1.0.9 pre-release on 2026-11-13) | SHA-256 from that JSON | the **1.0.8 release** zip of acidanthera/OpenCorePkg, SHA-256 `2011e8b7…` (GitHub's asset digest) |
| kexts in Dortania's list (Lilu, VirtualSMC, AppleALC, NVMeFix, RestrictEvents, BrcmPatchRAM, …) | the same nightly list | SHA-256 from that JSON | the repository's latest **release** asset (Lilu 1.7.2, VirtualSMC 1.3.8, AppleALC 1.9.8, …) with GitHub's digest |
| other kexts (AppleIGC, CpuTopologyRebuild, USBToolBox, SimpleHDA, RTL812xLucy, the laobamac AirportItlwm, …) | the GitHub releases page, scraped (`github.py`) | the digest GitHub prints, when there is one | the URL and SHA-256 recorded when the lock was made (computed where GitHub publishes none) |
| kexts with a fixed URL in `datasets/kext_data.py` (OCLP payloads, nightly.link artifacts) | `main` branches and "latest artifact" links | none | only if in the lock; a nightly.link artifact changes under the same URL, so the build stops on its hash. None is needed for the Arc builds |
| OcBinaryData (HfsPlus.efi, picker resources) | `archive/refs/heads/master.zip` | **none** | the archive of the commit `640e7e92…`, SHA-256 pinned; the zip's top folder is renamed to the `OcBinaryData-master` the program expects |
| kernel patch lists (CpuTopologyRebuild `patches_ht.plist`, AMD Vanilla, Aquantia) | `refs/heads` URLs | **none** | the file at a commit, SHA-256 pinned (`patches_ht.plist` at `fe58fd88…`) |
| iasl | MaciASL `master` (macOS), corpnewt (Linux), ACPICA 20251212 (Windows), downloaded on first use | **none** | never downloaded: the builder is given iasl (Windows: ACPICA 20260408 `iasl.exe`, pinned) |
| macserial | moved out of the OpenCorePkg zip into the program's folder | that zip's | from the pinned OpenCorePkg zip, its own SHA-256 pinned per OS |
| Hardware-Sniffer-CLI.exe (`E` on Windows) | the latest Hardware Sniffer release | digest from the releases page | never: the builder runs the pinned Hardware Sniffer source |
| the program itself (`updater.py`) | GitHub `main` | | never run |

All of it is in `windows/efibuild/downloads.lock.json`: `products` (by the name the program looks up), `files` (a
branch URL and the commit URL used instead), `tools` (ocvalidate and macserial for Windows, macOS and Linux inside the
OpenCorePkg zip) and `windows_tools` (ACPICA 20260408 `iasl.exe` and `acpidump.exe`, GitHub digests). The builder
replaces the program's download functions (`ResourceFetcher.download_and_save_file`, `fetch_and_parse_content`,
`gatheringFiles.fetch_latest_products_info`): a URL that is not in the lock is refused, every file is read from a
cache by its SHA-256 and checked again each time it is read. The program's product cache (`OCK_Files`) is fresh for
every build, unpacked again from the checked zips. `efibuild.py lock` (maintainer only, `lock.py`) runs a build in
which the program resolves its downloads as it would alone, swaps Dortania nightlies for the repository's release,
pins branch files to their commit, downloads everything once and writes the lock; its diff is reviewed before commit.
The lock made on 2026-11-13 covers the seven fixtures (15 products, 2 files).

### What it needs from the Hardware Sniffer report

`Scripts/report_validator.py` requires `Motherboard`, `BIOS`, `CPU`, `GPU`, `Network`, `USB Controllers`, `Input` and
`Storage Controllers`; `Monitor`, `Sound`, `Biometric`, `Bluetooth`, `SD Controller` and `System Devices` are optional.
What the steps read:

| section | keys | used for |
|---|---|---|
| Motherboard | Name, Chipset, Platform (Desktop/Laptop) | quirks by chipset, laptop kexts/patches, MMIO whitelist, HP/ASUS specials, SMBIOS |
| BIOS | Firmware Type, Secure Boot | the UEFI question, the BIOS steps |
| CPU | Manufacturer, Processor Name, Codename, Core Count, CPU Count, SIMD Features | macOS range (SSE4.x, AVX2), CPUID spoof, CpuTopologyRebuild, quirks, SMBIOS |
| GPU | Manufacturer, Codename, Device ID, Device Type (Integrated/Discrete), PCI Path, ACPI Path, Resizable BAR | compatibility, GPU combination, WhateverGreen/NootRX, iGPU properties, `disable-gpu`, `ResizeAppleGpuBars` (0 with Resizable BAR on, else -1), SSDT to turn a dGPU off |
| Monitor | Connector Type, Resolution, Connected GPU | iGPU framebuffer choices, VGA |
| Network | Device ID, Bus Type, PCI Path, ACPI Path | Ethernet and Wi-Fi kexts, `built-in`, disabling unsupported devices |
| Sound | Device ID, Bus Type, Controller Device ID | AppleALC and the layout |
| Storage Controllers | Device ID, Subsystem ID, Disk Drives | VMD and unsupported NVMe stop the build, NVMeFix, SATA kexts |
| USB Controllers, Input, Bluetooth, SD Controller, System Devices | Device ID and friends | XHCI/USB kexts, laptop input, Bluetooth kexts, card readers, IMEI, Yoga/Chromebook specials |

Plus the ACPI tables: the DSDT is required (SSDTTime's code in `dsdt.py`/`acpi_guru.py` disassembles it with iasl and
writes SSDT-EC, -PLUG, -USBX, -RTCAWAC, …); SSDTs, APIC and DMAR are read too. The builder passes only those four
signatures to the program (`sniff.keep_build_tables`): the rest of a dump can hold data a build has no use for (MSDM
holds the Windows product key). The report holds no serial numbers; device names, disk models and, in Windows'
language, some device names.

### macOS versions

`datasets/os_data.py` lists High Sierra 10.13 (Darwin 17) to Tahoe 26 (Darwin 25), all "final". The menu offers the
range the CPU, GPUs, Wi-Fi and storage allow natively, plus an OCLP range; the default is the newest native version
(lower when a device suggests it). The policy asks for Darwin 25 (macOS 26, default) or 24 (macOS 15, `--macos 15`):
the two versions the driver ships for. A target the menu does not offer, or offers only with OCLP, stops the build.

## 3. Our rules for the driver

### The Arc card

OpCore Simplify knows the Arc B580/B570 (Battlemage, `gpu_data.py`) but no macOS driver for it: the compatibility
check marks it unsupported and quits when it is the only GPU ("You cannot install macOS without a supported GPU"), and
with another usable GPU it turns the card off (`disable-gpu` in DeviceProperties, or an `SSDT-Disable_GPU_*` that calls
`_OFF`). Both are wrong here: the installer and macOS until stage 4 run on the card's firmware output (UEFI GOP)
through macOS's generic display driver (install guide, stage 1; the test machine installed macOS on the B580 alone).
The engine therefore (`engine.py`, `Patches.install`):

- in the compatibility check, counts the card (by `efi-rules.json` `cards`: 8086 + e20b/e20c) as working on every
  macOS, and runs the program's check on the other GPUs only (so a supported iGPU is still judged as before);
- hides the card from the kext selection, so nothing is chosen for it: no WhateverGreen, no WhateverXe, no
  DeviceProperties;
- answers the GPU combination question with the card alone.

`validate.py` then checks that the EFI has no IntelXe.kext, no SSDT-ARCOFF.aml (both stage 4: the table hides the card
from the display driver the installer needs), no WhateverGreen, no `SSDT-Disable_GPU`, no DeviceProperties at the
card's PCI path and no Kernel > Block of IONDRVSupport.

A report without an Arc B580/B570 is refused before the program runs, as is a non-Intel CPU (the driver is tested on
Intel only).

### The integrated GPU

The test machine runs with its UHD 770 off in the firmware. With an iGPU in the report the program asks for a GPU
combination (12th–14th gen iGPUs count as supported through the experimental WhateverXe + OCLP); the rule takes the
card alone, so the iGPU is turned off for macOS (`disable-gpu`, no WhateverXe, no OCLP). Because WhateverGreen is not
loaded, `disable-gpu` itself has no effect; the firmware steps of the result therefore say to turn the iGPU off (or
at least not make it the primary display) and to put the monitor on the card's DisplayPort. macOS has no driver for
the 12th–14th gen iGPUs without WhateverXe. Untested: a PC that keeps its iGPU on during the install, and whether
macOS's own driver attaches to an older, natively supported iGPU (the Z490 fixture's UHD 630) without WhateverGreen.

### SMBIOS

`MacBookPro16,2`, the model the driver is tested with on macOS 15 and 26 (`efi/sample-z790`). The program would pick
`MacPro7,1` (macOS 26) or `iMacPro1,1` (macOS 15) for a desktop whose last GPU is discrete; neither has been run with
the B580. The model is chosen through the program's own SMBIOS menu (option 5), so its model-specific steps run as
for any other choice; `--smbios` overrides it. The serial number, MLB and UUID come from macserial of the pinned
OpenCore release (the program calls it); the ROM is 6 random bytes (the program's choice). They are written only into
`config.plist`. The builder reads them back after the build and removes every form of them (text, hex, base64) from
the log and the result; the tests check that none appears in anything printed or written besides `config.plist`.
Generated EFIs are never committed, and the fixtures hold no serials.

### Stage 1 as the test machine installed (`policy.py`)

Applied after the program, each change logged with its reason:

- `CpuTopologyRebuild.kext` off, `ctrsmt=` removed from the boot-args, and the "force HT enabled" kernel patches the
  program adds with it off: the test machine installed macOS without them (CpuTopologyRebuild warns of random boot
  failures with `-v`, and the test machine saw them). The result's after-install notes say to turn it back on with
  `ctrsmt=off` and to remove `-v`, as `efi/sample-z790/README.md` describes. `-v debug=0x100 keepsyms=1` stay for
  the install;
- `Misc > Security > ExposeSensitiveData` gets bit 0 (the rules' `boot_path`), so OpenCore records which EFI it booted
  from and efi.sh / IntelXe Manager find it for stages 2 to 4 on macOS;
- the firmware steps (stage 1, step 1): Above 4G decoding on, Resizable BAR on, CSM off, Secure Boot off, and the iGPU
  line above. The program's own advice for 600/700-series desktops without Resizable BAR in the report ("Disable
  Resizable BAR") is replaced: the guide asks for it on, with `ResizeAppleGpuBars = 10`.

Kept from the program: its kext choice, its ACPI patches, its CPUID spoof (Comet Lake EAX only, the sample's macOS 15
form), `SecureBootModel = Disabled` (it sets that for macOS 14 and later), `csr-active-config 030A0000` (0xA03, which
contains stage 2's 0x803), `-v debug=0x100 keepsyms=1`, `UTBDefault.kext` until the user maps USB.

### Stage 2 through the shared rules (`rules.py`)

`efi-rules.json` is read by a fourth reader in Python: the `settings` list and, unless `--no-safari`, the `safari` list
(`settings-safari`, as the setup assistant's first step with Safari chosen), with the same item kinds, the same order
and the same steps as efi.sh (DESIGN-efi-rules.md section 3). It passes the 7 stage-2 and Safari vectors of
`release/efi-rules-vectors.json` (`efibuild.py vectors`; ops exactly, notes, values after, a second plan empty), and
the tests then run efi.sh itself on every built EFI: `--settings-safari-plan` must say "nothing to change". Stage 4 is
not implemented in Python; it is not part of the build.

What stage 2 changes in a fresh build: `ResizeAppleGpuBars` 0/-1 → 10, the boot-args `amfi_get_out_of_my_way=0x1
intelxe_surfvram=2`; the rest is already as the rules want (the program sets `DisableIoMapper`, SIP 0xA03, both
names in `NVRAM > Delete` and `SecureBootModel = Disabled`; OpenCore's Sample.plist has `ReconnectGraphicsOnConnect =
false`).

### Validation (`validate.py`)

- **ocvalidate** from the pinned OpenCorePkg 1.0.8 zip (the build's own OpenCore), taken out of the zip and checked
  against its pinned SHA-256 before every run. If it cannot run, the build fails. Every finding fails the build except
  the one our rules cause on purpose: `Booter->Quirks->ResizeAppleGpuBars is set to 10, which is unstable with macOS
  sleep-wake!` (ocvalidate flags any value above 8). It is accepted only for the exact value of `efi-rules.json`, only
  when ocvalidate's error count equals the accepted lines, and the result lists it with the reason (a 1 GB window on
  the card's VRAM, `efi/README.md`; sleep and wake work with it on the test machine). This is a finding for the
  maintainer: the sample config and every EFI our tools touch carry it too.
- OpenCore.efi, BOOTx64.efi and the OpenCorePkg drivers are byte for byte the pinned release's.
- Stage 2 complete (the rules plan nothing more), stage 4 not yet and nothing in its way (section 3, the card), every
  enabled Kernel > Add / ACPI > Add entry has its files, Lilu is there when HybridPM.kext will be wanted.

### The result

`OUT/EFI/` (OpenCore 1.0.8, about 18 MB), `OUT/build-log.txt` (the program's whole transcript with each answer and its
rule, our changes, the checks; SMBIOS values and the user's home folder removed) and, with `--result`, the same as
JSON: `ok`, `error`, `policy`, `facts`, `decisions[]` (`rule`, `screen`, `question`, `answer`, `why`), `notices[]`,
`changes[]`, `firmware[]`, `after_install[]`, `downloads[]` (URL and SHA-256 of each), `acpi_tables[]`, `upstream`,
`opencore`, `checks` (`ok`, `failures[]`, `notes[]`, `checks.ocvalidate` with `found`, `accepted[]`, `issues[]`, `text`).
A folder the builder did not make (no `.intelxe-efibuild` marker) is never written into.

## 4. Tests on this Mac (13 November)

`windows/efibuild/tests/run-fixtures.sh WORK [PACKAGE]` (Python 3.12 venv outside the repository, Homebrew's iasl
20260408, the 0.1.4 `-efi.zip` unpacked as the package): 39 checks passed, 0 failed.

- `test_answers.py`: 12 tests of the rules without the program (an unknown question stops; a refused answer stops; an
  unexpected "Press Enter" stops; macOS 15 and 26; not offered or OCLP-only stops; the GPU combination; Intel Wi-Fi
  never needing OCLP; no Wi-Fi passwords; OCLP stops; the SMBIOS menu; the main menu's checks).
- `efibuild.py vectors`: 7 of 7 stage-2 and Safari vectors.

Fixtures (`tests/fixtures/fixtures.json`): public Hardware Sniffer reports and ACPI dumps fetched at a pinned commit
and checked by SHA-256 (not copied into the repository), and one report of ours. No public Hardware Sniffer report or
ACPI dump of a Z790 board was found (GitHub code and repository search); the Z790 report is the test machine
(ASUS ROG STRIX Z790-A GAMING WIFI II, i7-14700K, Arc B580, I226-V, BE202, SN550) written from `efi/sample-z790`'s facts
in Hardware Sniffer's format, without serials, and it is built with the ASUS B660 dump (same maker, 600/700-series
PCH, the slot named `\_SB.PC00.PEG1` as on the test machine).

| fixture | report, tables | result | answers beyond macOS/SMBIOS |
|---|---|---|---|
| `z790-b580` | ours; ASUS PRIME B660-PLUS D4 dump | built, macOS 26 | none needed (BE202 unsupported, turned off by the program) |
| `z790-b580-sequoia` | the same, `--macos 15` | built, macOS 15 | — |
| `z790-b580-igpu` | ours with the UHD 770 on | built | GPU combination: the B580 alone (UHD 770 off for macOS, no WhateverXe) |
| `z790-b580-ax210` | ours with an AX210 instead of the BE202 | built | Intel Wi-Fi: laobamac AirportItlwm |
| `b660-b580` | klich3/macOS-on-B660-PLUS-D4-i9-12900KF @ 837d726, B580 in place of its RX 6700 XT | built | audio: AppleALC + SimpleHDA; layout default (98) |
| `z490-b580-uhd630` | 5T33Z0/Gigabyte-Z490-Vision-G-Hackintosh-OpenCore @ 90efe42 (BSD-3), B580 in place of its RX 580, UHD 630 on; its Focusrite entry left out | built | GPU combination: the B580 alone; audio; layout default (5) |
| `z490-report-refused` | the same report as published | stops: "the report did not pass OpCore Simplify's checks" (an older Hardware Sniffer wrote a Sound entry without Device ID) | — |

For every built EFI: ocvalidate 1.0.8 found one issue, the accepted ResizeAppleGpuBars line; efi.sh
`--settings-safari-plan` had nothing to change and `--status` showed `settings_changes=0`, no IntelXe.kext or
SSDT-ARCOFF.aml, Lilu enabled; stage 4 was then made on a copy by efi.sh (`--plan`, `--apply --expect`) with the
fixture's CPU and card port as facts (IntelXe.kext, SSDT-ARCOFF.aml as shipped or adapted to `\_SB.PCI0.PEG1.PEGP` on
the Z490, HybridPM.kext below Lilu on the 12th–14th gen CPUs) and `efibuild.py validate --after-stage4` passed
(ocvalidate again with only the accepted finding); no SMBIOS value appeared in the logs, results or efi.sh output. A
build takes about 11 s with the downloads cached.

Not tested: booting any of these EFIs (no test machine was used), Hardware Sniffer itself (Windows only), the
builder on Windows (its Windows paths: `iasl.exe`/`acpidump.exe` from the lock, `ocvalidate.exe`, `macserial.exe`).

## 5. The Windows app

`windows/app/` is not changed by this branch. This is how it will use the builder.

### The interface

In `IntelXe.Core` (no Windows APIs), beside `IEfiPlanner`:

```csharp
public sealed record HardwareScan(string ReportPath, string AcpiDir, IReadOnlyList<string> Tables);
public sealed record EfiBuildRequest(string ReportPath, string AcpiDir, string OutDir,
    string MacOS = "26", string? Smbios = null, bool Safari = true);
public sealed record EfiBuildDecision(string Rule, string Screen, string Question, string Answer, string Why);
public sealed record EfiBuildResult(bool Ok, string? Error, string OpenCore, string Smbios,
    IReadOnlyList<EfiBuildDecision> Decisions, IReadOnlyList<string> Changes, IReadOnlyList<string> Firmware,
    IReadOnlyList<string> AfterInstall, IReadOnlyList<string> Failures, IReadOnlyList<string> Notes,
    string LogPath, string ResultJsonPath);

public interface IEfiBuilder
{
    bool IsAvailable { get; }              // the engine folder is there and its manifests check
    string Versions { get; }               // "OpCore Simplify 9aed67e, OpenCore 1.0.8, rules 1"
    Task<HardwareScan> ScanAsync(string outDir, IProgress<string> progress, CancellationToken cancel);   // elevated
    Task<EfiBuildResult> BuildAsync(EfiBuildRequest request, IProgress<string> progress, CancellationToken cancel);
}
```

`PythonEfiBuilder` (in the app) starts `engine\python\python.exe -I -u engine\efibuild\efibuild.py build --report …
--acpi … --out … --macos 26 --result result.json --verbose` as a child process without a console, streams stderr
lines to `progress` (the program's screens and each answer), and reads `result.json` (section 3, "The result"). Exit
code 0 is success, 1 a build that stopped (the reason is `error`), 2 a pin or input error. Cancel kills the process
tree; the output folder is then incomplete and the marker lets the next build replace it.

`ScanAsync` needs administrator rights (acpidump reads the firmware tables). It goes through the app's existing
elevated helper, which is the app's own exe started with `runas`: `IntelXeManager.exe --efi-helper sniff --out DIR`
runs `efibuild.py sniff --out DIR` and exits; the build itself runs as the user. After the build, the app's EFI page
can plan `settings-safari` on the new folder with its own engine (`FolderEfiPlanner`): it must report no changes,
which checks the Python reader against the C# one on every real build. Writing the EFI to a USB stick or the EFI
partition is a separate step (not part of this branch).

### Hardware Sniffer on Windows

Hardware Sniffer collects through WMI (`Win32_*` classes, `MSFT_*`, device properties such as
`DEVPKEY_Device_LocationPaths` for the PCI and ACPI paths; `cpuid` through ctypes for the CPU's codename) with the
`wmi` package (it needs pywin32), and dumps the tables with `acpidump.exe -b` in an administrator session. Its own
code downloads `acpidump.exe` (ACPICA R2024_12_12) without a checksum; `sniff.py` replaces that with the pinned
ACPICA 20260408 `acpidump.exe` (`windows_tools`, SHA-256 `a0095a57…`) and keeps only the DSDT, SSDT, APIC and DMAR
tables. A Windows without the card's driver lists the card as "Microsoft Basic Display Adapter" with the right
device ID, which is all the rules need. Its report holds no serial numbers; it does not upload anything.

### Packaging: embedded Python (recommended) or a PyInstaller exe

| | embedded Python (as 1401) | PyInstaller (one file or one folder) |
|---|---|---|
| what ships | `engine\python\` = python.org's embeddable zip (3.12) + `wmi`, `pywin32`, `certifi`; `engine\efibuild\`, the two upstream trees (5 MB), their LICENSEs. Roughly 30–40 MB unpacked (to be measured when packaging) | one exe (or folder) with the interpreter, the same packages and sources inside: a similar size, compressed into the exe |
| signing | `python.exe` and `python312.dll` of the embeddable zip are Authenticode-signed by the Python Software Foundation (pywin32's DLLs to be checked); what we add is `.py` text, checked at run time by our manifests (upstream) and a manifest of `efibuild` made at release | the bootloader exe is ours: unsigned (the app has no code-signing certificate, `windows/app/README.md`) |
| antivirus | a signed python.exe running scripts is common and rarely flagged | PyInstaller bootloaders are a well-known source of false positives (Defender heuristics; one-file builds unpack into `%TEMP%` on every start), and a flagged exe blocks the whole feature |
| updating upstream | replace a folder; the manifest says what changed | rebuild and re-release the exe |
| reproducibility | the same files as on the development machine, checked by SHA-256 | the frozen archive is opaque to the user and to our checks |
| start time | immediate | one-file: unpacking on every run |

**Recommendation: embedded Python.** The interpreter stays signed by its publisher and we add no unsigned
executable of our own; our part stays inspectable and checked by manifest at run time; it avoids the PyInstaller
false positives an unsigned exe would attract; and it is what 1401 runs in the field. (ocvalidate.exe, macserial.exe,
iasl.exe and acpidump.exe are downloaded by the builder from their releases and checked by SHA-256 either way.) The
release step (`windows/app/build.ps1`) would fetch the embeddable zip and the wheels pinned by SHA-256 (as
`downloads.lock.json` pins the rest), copy `windows/efibuild` and the checked upstream trees into `engine\`, and write
`engine\MANIFEST.sha256`. The app's folder grows by those tens of MB over today's 170 MB (66 MB zipped). The engine's
cache and working files go to `%LOCALAPPDATA%\IntelXe\efibuild`, never next to the exe (a OneDrive or read-only folder
would break it).

## 6. Not done

- The Windows side: `IEfiBuilder`, `PythonEfiBuilder`, the helper's `sniff` verb, packaging the embedded Python,
  writing the EFI to a stick or partition (windows/app is other agents' work; this note is the seam).
- Running anything on Windows: Hardware Sniffer, `sniff.py`, the builder with `iasl.exe`/`ocvalidate.exe`/
  `macserial.exe` from the lock. The Windows tool hashes are GitHub's digests and the OpenCore zip's members, not
  measured on Windows.
- Booting a built EFI on a real PC; the Z790 fixture uses another board's ACPI tables.
- Stage 4 in Python (not needed for the build; efi.sh, IntelXe-EFI.ps1 and the app do it after the install).
- A fixture with a B570, an AMD CPU (refused by design), a laptop, a BIOS-mode report.
- The lock covers what the seven fixtures need. Another PC can need a kext that is not pinned: the build then stops
  ("… which downloads.lock.json does not pin") and the maintainer runs `efibuild.py lock` with that report. A wider
  lock (every kext OpCore Simplify can choose) is possible but each entry should be looked at.
- Products whose URL is a moving artifact (nightly.link) cannot be pinned for long.
- SimpleHDA (chosen for audio on macOS 26) states no licence; the builder downloads it on the user's PC from its
  release, it is not redistributed by us. Worth a look before the app ships the feature.
- Findings upstream could use: the DSDT signature is compared as bytes against a str in `acpi_guru.read_acpi_tables`,
  so its DSDT pre-patch path never runs (the builder wraps `_table_signature`, as 1401 also found); OcBinaryData,
  branch patch lists, iasl and Hardware Sniffer's acpidump are downloaded without a checksum.
