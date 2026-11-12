# The EFI rules in one file: efi-rules.json for efi.sh, IntelXe-EFI.ps1 and the Windows app

> Design note with its date (2026-11-12, branch `efirules`): it records the plan and the state of that day, not the
> current status. The current state is in the README, `windows/README.md` and `release/README-INSTALL.md`.

Two tools make the OpenCore part of the install guide, and IntelXe Manager adds a third reader:

- `release/templates/efi.sh` (bash) on macOS, shipped in the Tahoe and Sequoia packages. IntelXe Manager
  (`app/Sources/EFI.swift`, `EFIView.swift`) decides nothing itself: it shows efi.sh's plan, asks, and runs efi.sh
  with the plan's SHA-256. The app carries its own copy of efi.sh for packages made before the package's copy could
  do everything (`efiScript(for:)`).
- `windows/IntelXe-EFI.ps1` on Windows, shipped in the `-efi.zip`. Its engine is C# 5 inside the script
  (`// BEGIN ENGINE` … `// END ENGINE`, compiled by `Add-Type`), with a Windows Forms window around it.

Until now each implemented the rules on its own; the parity was kept by tests (`windows/test/make-expected.sh` runs
efi.sh and the engine on the same steps and compares plans, `config.plist` contents and adapted tables). A native
Windows app (WinUI 3, C#) is planned to replace the script. Before it is written the rules move into one data file,
`release/templates/efi-rules.json`, that efi.sh and the script both read, with test vectors
(`release/efi-rules-vectors.json`) that every implementation runs, the WinUI app included.

## 1. The rules, as they were implemented

What efi.sh (652 lines) and IntelXe-EFI.ps1 (2,307 lines) did before this change. "Same" means the same lines, steps
and values; the parity tests already compared them.

### Stage 4 (efi.sh `--plan`/`--apply`; the script `-Plan driver`/`-Apply driver`)

| # | rule | efi.sh | IntelXe-EFI.ps1 |
|---|---|---|---|
| 4.1 | `IntelXe.kext` copied when absent, replaced when its executable or `Info.plist` differs byte for byte from the package's | same | same |
| 4.2 | `Kernel > Add` entry: added at the end with Arch Any, BundlePath, Comment, Enabled, ExecutablePath `Contents/MacOS/IntelXe`, MaxKernel/MinKernel empty, PlistPath `Contents/Info.plist`; an existing entry (BundlePath compared without case) gets Enabled, ExecutablePath and PlistPath set | same | same |
| 4.3 | an enabled `B580.kext` entry (the kext's earlier name) is turned off | same | same |
| 4.4 | `SSDT-ARCOFF.aml` only when the card's upstream port is known and a table names it: the package's (for `\_SB.PC00.PEG1.PEGP`), the package's adapted to the port, or the EFI's own table naming the port (kept); `ACPI > Add` entry added or enabled; a note for no card, a port without an ACPI name, a table that cannot be adapted | same | same, but the port is the one the user confirms (see D3) |
| 4.5 | adapting the table: the AML is parsed as `efi/SSDT-ARCOFF.dsl` builds it, rebuilt from its parts with the shipped path (must give the same bytes), then with the card's (NamePath, three PkgLengths, length, checksum) | `adapt_ssdt` | `Aml.Adapt`, the same bytes |
| 4.6 | the card: PCI vendor 8086, device e20b or e20c; its upstream port is the bridge two PCI levels above the GPU | from ioreg | from Device Manager's Location path (see D3) |
| 4.7 | `HybridPM.kext` on a hybrid CPU (by name: i3/i5/i7/i9 12xxx-14xxx and the U/P laptop models, not Core Ultra, not the P-core-only desktop models), only when the package has it and the EFI loads Lilu (file in `Kexts`, entry enabled); copied or replaced (an earlier build, `org.hackintosh.HybridPM`, is named in the plan); entry inserted right below Lilu's or brought to Enabled/ExecutablePath/PlistPath; a note when its entry is above Lilu's | glob patterns | regular expressions with look-ahead; the same answer for the 20 names of `expected/efi-sh/hybrid-cpus.txt` |
| 4.8 | `Misc > Security > ExposeSensitiveData` gets bit 0 (an integer value only) so that OpenCore records which EFI it booted from | only when the boot path is not recorded | always (see D4) |
| 4.9 | the package's kexts and table are checked against its `SHA256SUMS` before anything is copied | only the lines listed (see D2) | every file of the kexts must be listed |

### Stage 2 (efi.sh `--settings-plan`; the script `-Plan settings`)

| # | rule | both |
|---|---|---|
| 2.1 | `Booter > Quirks > ResizeAppleGpuBars` = 10 | same |
| 2.2 | `Kernel > Quirks > DisableIoMapper` = true | same |
| 2.3 | `UEFI > Output > ReconnectGraphicsOnConnect`: true becomes false (only then) | same |
| 2.4 | nothing in NVRAM without `NVRAM > Add` (a note) | same |
| 2.5 | `NVRAM > Add > 7C436110-…` made when missing | same |
| 2.6 | `csr-active-config` gets the bits 0x803 (a wider value stays) | same |
| 2.7 | boot-args: `amfi_get_out_of_my_way=0x1` unless `amfi_get_out_of_my_way=1/0x1` or `amfi=0x80/128` is there; `intelxe_surfvram=2` unless an `intelxe_surfvram=` is there; the others kept | same |
| 2.8 | `NVRAM > Delete > 7C436110-…` holds `csr-active-config` and `boot-args` | the script also made a missing `NVRAM > Delete` (see D1) |

### The Safari setting, undo, backups

| # | rule | efi.sh | IntelXe-EFI.ps1 |
|---|---|---|---|
| S.1 | `Misc > Security > SecureBootModel` = Disabled (the system-volume copy), apart from stage 2 and together with it (`--settings-safari-plan`: one backup, one restart) | yes | missing (see D5) |
| U.1 | a change backs up the whole EFI folder first; its backup lists what the change touched (`changed`), the copies first, `config.plist` last | listed at the end (see D6) | listed before each step |
| U.2 | undo puts back the newest backup not yet put back: each listed file restored, or removed when it was not there; warns when `config.plist` changed after the change; one change per run; the backup stays | same | same |
| U.3 | the plan's SHA-256: nothing is written unless the plan is still the one shown | `--expect` optional | `-Expect` required (see D9) |

## 2. The differences and what was decided

Each difference is either a bug or a gap in one implementation (fixed in its own commit; the bugs before the rules
moved) or a platform difference that stays in the implementation.

| | difference | decision |
|---|---|---|
| D1 | efi.sh: no `NVRAM > Delete` in `config.plist` made `--settings-apply` stop after its backup (`plutil -insert` makes no missing parent) | **bug in efi.sh**, fixed (d19762f): the plan adds the dictionary, as the script did. In the rules: a `dict` item for `NVRAM.Delete` |
| D2 | efi.sh checked only the `SHA256SUMS` lines it found: a file added to a kext after packaging was copied unchecked; the script also noted a package without `SHA256SUMS` | **bug in efi.sh**, fixed (1181b9f): every file of the two kexts and the table must be listed and match; the same note |
| D3 | the card's port: efi.sh reads ioreg on the Mac it runs on; the script takes the path the user confirms (`-CardAcpiPath`, or the window's box), because it may run on another PC and before macOS is installed (the table must not be added before stage 3) | **platform**: stays. The rule (vendor, ids, two levels above the GPU) is shared (`cards`). The notes for "no card" differ in wording |
| D4 | `ExposeSensitiveData`: efi.sh only when OpenCore's `boot-path` NVRAM variable is not there; the script cannot read it | **platform**: the fact stays per implementation (`BootPathRecorded` is false on Windows), the rule is shared (`boot_path`) |
| D5 | the script had no Safari plan | **gap in the script**, closed: `-Plan safari` / `settings-safari` from the same `safari` list (the window is unchanged) |
| D6 | efi.sh wrote the backup's list of changes after the last step: a change that stopped on the way left a backup without a list, and `--undo-plan`/`--undo` then failed on it, so no earlier change could be undone | **bug in efi.sh**, fixed (a5a09fe): listed before each step, as the script does; a backup without a list is passed over |
| D7 | the script checked the edited `config.plist` by reading each `set` back at its path after all steps; the HybridPM insert moves the entries below Lilu's, so the check looked at another entry and refused the change when that one was off | **bug in the script**, fixed (cfc7a11): each value is checked as it is set, then the written bytes against the edited document |
| D8 | hybrid CPU names: globs (`*i5-12500[!H]*`) against regular expressions with look-ahead (`i5-12500(?!H)`) | the same answers; now one list of POSIX extended expressions without look-ahead (`i5-12500([^H]|$)`), which bash `[[ =~ ]]` and .NET read alike |
| D9 | `--expect` is optional for efi.sh (the Manager always passes it), required for the script | **interface**: stays |
| D10 | backups: efi.sh in `/Library/Application Support/intel-gpu-macos-driver/efi-backups/<partition UUID>/` (root), numbered by count; the script next to the EFI folder or in `%LOCALAPPDATA%` for a small volume, keyed by the volume's serial number, numbered by the highest number, with a free-space check and a completeness check | **platform**: stays. The format inside a backup (`EFI/`, `kind`, `date`, `where`, `changed`, `config-after.sha256`, `restored`) is the same, and so are the `kind` words (now in the rules) |
| D11 | writing `config.plist`: efi.sh through `plutil` (XML, keys sorted, its own layout); the script edits the XML text and keeps the file's layout, comments and line ends | **platform**: stays; both give the same keys and values (the tests compare them) |
| D12 | the script needs no package for stage 2 and for `-Status` ("present" for a kext it cannot compare); efi.sh always runs from a package | **interface**: stays |
| D13 | wording: "for this script" / "for efi.sh on macOS" in the ExposeSensitiveData line, the no-card note | **platform**: the texts stay in the code; tests and vectors compare the steps, and the lines where the wording is shared |
| D14 | neither checked the backup's list before acting on it: undo would restore or remove any path it named under `EFI/OC` | both now act only on `undo_paths` of the rules (`config.plist`, the two kexts, the table) and name any other line in a note |

## 3. efi-rules.json

`release/templates/efi-rules.json`, MIT (its first key, `"SPDX-License-Identifier"`). It ships next to each reader:
next to efi.sh in the Tahoe and Sequoia packages and in IntelXe Manager's Resources (`app/build.sh`), next to
IntelXe-EFI.ps1 in the `-efi.zip` (`release/package.sh`; `release/release.sh` checks all four copies against the
repository's). Each reader takes the file next to itself, never another reader's: the app's own efi.sh takes the
app's rules, a package's efi.sh the package's.

It is data. Each implementation keeps the code that applies it: finding and mounting the EFI, NVRAM, privileges,
backups on disk, reading and writing `config.plist`, comparing files, the AML rebuild, the plan's wording.

### Conventions

- **Key paths** of `config.plist` are dotted strings (`NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args`):
  no key the rules name contains a dot. The plan shows them with ` > `.
- **Values** are strings, integers and booleans; no fractions, no null. An integer written as a string with `0x` is
  a bit mask (`"0x803"`). A key ending in `about` explains its neighbour and is not read.
- **Regular expressions** (`cpu_include`, `cpu_exclude`, `unless`, `port_pattern`) are POSIX extended expressions
  that mean the same in .NET: literals, `[...]` and `[^...]`, `{n}` and `{m,n}`, `(a|b)`, `*`, `^`, `$`. No
  look-around, no `\d`, no lazy quantifiers. They are searched (not anchored unless they say so).
- Strings hold no tab and no line break (efi.sh reads the file as one value per line).
- An entry (`kext.entry`, `ssdt.entry`, `hybridpm.entry`) is written in the order the file gives (alphabetical, as
  plutil writes it anyway).

### Fields

| field | meaning |
|---|---|
| `format`, `format_version` | `"intelxe-efi-rules"`, `1`. A reader refuses another format or version. A change that an older reader would misread raises `format_version`; one that it reads correctly (a new card, a new boot-arg, a new item of a kind it knows) does not |
| `revision` | raised with every change of the rules, for messages and reports |
| `cards.vendor_id`, `cards.device_ids` | the PCI devices the tools look for (`8086`; `e20b`, `e20c`): data/platforms.json "supported", checked by `tools/parity/devices.py` |
| `cards.port_levels_above_gpu` | the card's upstream port is the PCI bridge this many levels above the GPU (2: the GPU sits behind its own switch) |
| `kext.bundle`, `kext.folder`, `kext.executable`, `kext.info_plist` | `IntelXe.kext` in `EFI/OC/Kexts`; the two files compared byte for byte (4.1) |
| `kext.entry`, `kext.fields` | the `Kernel > Add` entry added when there is none; the fields an existing entry is brought to, in order (4.2) |
| `kext.earlier_bundles` | entries turned off when enabled (`B580.kext`, 4.3) |
| `ssdt.file`, `ssdt.folder` | `SSDT-ARCOFF.aml` in `EFI/OC/ACPI` |
| `ssdt.shipped_port` | the port the package's table names (`\_SB.PC00.PEG1.PEGP`); the adaptation starts from it (4.5) |
| `ssdt.port_pattern` | what counts as an ACPI path that can be written into the table |
| `ssdt.entry`, `ssdt.fields` | the `ACPI > Add` entry; the fields an existing one is brought to |
| `hybridpm.bundle`, `.folder`, `.executable`, `.info_plist`, `.entry`, `.fields` | as for the kext (4.7) |
| `hybridpm.after` | the kext it needs loaded and is placed right below (`Lilu.kext`, in the same folder) |
| `hybridpm.earlier_builds` | bundle identifiers of earlier builds under the same name, named as such in the plan |
| `hybridpm.cpu_include`, `hybridpm.cpu_exclude` | the CPU's name needs HybridPM.kext when it matches one `include` and no `exclude` |
| `boot_path.path`, `boot_path.bits` | stage 4 sets these bits of that integer when the boot path is not recorded (4.8) |
| `settings`, `safari` | the items of stage 2 and of the Safari setting, applied in order (below) |
| `backup_kinds` | the `kind` written into a backup per plan: `driver`, `settings`, `safari`, `settings-safari` |
| `undo_paths` | the only paths under `EFI/OC` that undo restores or removes |

### Items (`settings`, `safari`)

Each item has `kind` and `path`; `why`, when not empty, ends the plan's line in parentheses.

| kind | fields | the plan |
|---|---|---|
| `value` | `type` (`integer`, `bool`, `string`), `value` | set when the current value (as `plutil -extract raw` prints it) differs; the line shows `missing` for none |
| `value_if` | `type`, `if`, `value` | set only when the current value is `if` (ReconnectGraphicsOnConnect) |
| `require` | `note` | when `path` is not there: the note, and the rest of the list is skipped |
| `dict` | | a dictionary made when `path` is not there |
| `bits` | `type` (`data`: four bytes little-endian, or `integer`), `bits` | the bits added when one is missing; the line shows both values in hex |
| `boot_args` | `args`: `{add, unless}` | each `add` appended unless `unless` matches `" " + current + " "`; one `set` for all |
| `strings` | `strings` | the array made when missing; each string appended when not in it |

A reader that meets a kind it does not know stops with an error instead of planning half a change.

## 4. Reading it

**efi.sh** (no new dependency; stock macOS 15 and 26): `plutil -convert xml1 -o - efi-rules.json` (plutil has read
JSON for as long as it has had `-convert`), then one `awk` pass lists every value by its key path into three bash
arrays (`RK`, `RT`, `RV`), so a lookup costs no process. Measured on this Mac (macOS 27): `--settings-plan` 0.16 s
(0.10 s before), `--status` about 1.0 s (0.9 s). The entry dictionaries go to `plutil -insert -json` as JSON built
from those arrays. bash 3.2 only (`printf -v`, arrays, `[[ =~ ]]`). `INTELXE_EFI_RULES` names another file (tests);
`INTELXE_EFI_OPS` makes a plan write its steps to a file (the vectors).

**IntelXe-EFI.ps1**: `ConvertFrom-Json` (Windows PowerShell 5.1 and PowerShell 7), then `ConvertTo-RulesTree` turns
the objects into plain dictionaries, lists, strings, integers and booleans, which the engine's `Rules` class reads
(`Rules.FromTree`). The engine stays free of PowerShell types, so the same class takes a tree made with
System.Text.Json (`windows/test/macos/EngineTest.cs`, `RulesJson`). `-RulesFile` names another file; by default the
one next to the script, else the repository's.

## 5. Shared test vectors

`release/efi-rules-vectors.json` (format `intelxe-efi-rules-vectors` 1, MIT): 32 vectors, 24 CPU names, 6 PCI
devices. Like the rules it holds no null and no key with a dot in it (plutil reads it too), so files are named in lists
(`{"file": "Kexts/IntelXe.kext", ...}`), not as keys.

- `config` and `stage1`: `efi/sample-z790/config.plist`, set back to how stage 1 leaves it by the `stage1` edits.
- `tables`: the tables the fixtures use, as hex: `stand-in` (names the shipped port, not adaptable), `own-peg0`
  (a user's own table for `\_SB.PC00.PEG0.PEGP`), `arcoff-0-1-3` (the 0.1.3 `SSDT-ARCOFF.aml`, which iasl builds from
  `efi/SSDT-ARCOFF.dsl`).
- `package`: the package every vector starts from, as fixtures under `EFI/OC`. A fixture is
  `{"file", "type": "kext", "executable", "text", "bundle_id", "version"}` (the executable holds `text` and a line feed;
  `Info.plist` the identifier and version when not empty), `{"type": "table", "table"}`, `{"type": "dir"}` (an empty
  kext, for Lilu), `{"type": "package", "append"}` (a copy of the package's file, with a line appended to its
  executable), `{"type": "none"}` (no such file). `{"file": "SHA256SUMS", "type": "sums"}`: the package's sums over
  every file under `EFI`, written last; a vector's `{"file": "SHA256SUMS", "type": "none"}` leaves them out.
- A vector: `name`, `about`, `plan` (`driver`, `settings`, `safari`, `settings-safari`), `edits` (steps in the language
  below, or `{"op": "remove", "path"}`, on the stage 1 `config.plist`), `package` (fixtures that replace the base
  package's), `efi` (fixtures in the EFI folder), `facts`:
  - `card`: `{"pci": "8086:e20b", "port": "\\_SB.PC00.PEG1.PEGP"}`; `"port": "none"` when the port has no ACPI name; no
    `card` when there is none. Each runner turns it into what its platform reads: efi.sh an ioreg listing with the
    device `cards.port_levels_above_gpu` levels below the port; the script a device instance id
    (`PCI\VEN_8086&DEV_E20B&...`) and, for a card, the Location path Device Manager shows, which the user confirms;
  - `cpu`: the CPU's name;
  - `boot_path_recorded`: whether OpenCore records its boot path (efi.sh: a boot-path in `INTELXE_BOOT_PATH`; the
    script: `BootPathRecorded`).
- `expect.ops`: the plan's steps, exactly and in order:
  `{"op": "copy", "to": "Kexts/IntelXe.kext", "from": "package" | "adapted"}`,
  `{"op": "set", "path", "type": "bool" | "integer" | "string" | "data", "value"}` (data as base64),
  `{"op": "append", "path", "type": "string" | "dict", "value"}`, `{"op": "insert", "path", "index", "type": "dict",
  "value"}`, `{"op": "mkdict" | "mkarray", "path"}`. A dictionary value compares by its keys and values.
- `expect.notes`: one substring per note, in order, and exactly that many notes. The substrings are the parts both
  platforms share (D3, D13).
- `expect.after` (when there are steps, the change is made with the plan's SHA-256): `config` values
  (`{"path", "type", "value"}` as `plutil -extract raw` prints them, or `{"path", "absent": true}`), `files`
  (`{"file", "state": "package" | "absent"}`, `{"file", "sha256"}`, `{"file", "table"}`), `changed` (the backup's
  list, in order).
- `expect.undo`: after one undo `config.plist` holds the same keys and values as before, and every other file under
  `EFI` has the same bytes.
- `cpus`: `{"cpu", "hybridpm"}`; `cards`: `{"pci", "card"}`.

They cover stage 2 (the sample, no `NVRAM > Delete`, values that already do, no `NVRAM > Add`), the Safari setting
(alone, with stage 2, no `Misc > Security`), stage 4 (no card, the shipped port, a B570, an unsupported device and
vendor, a recorded boot path, missing entries, older entries and kext with an enabled `B580.kext` entry, no
`SHA256SUMS`), HybridPM (the sample's entry, an entry inserted below Lilu's with the IntelXe entry moving, no Lilu,
Lilu off, an earlier build, an entry above Lilu's, a package without it, a Core Ultra), SSDT-ARCOFF (adapted to three
ports with the bytes iasl builds, the 0.1.3 table at its own port, a port without a name, a table that cannot be
adapted, a shorter path, the user's own table kept) and undo (each vector that changes something and says `undo`).

Runners: `release/test-efi-vectors.sh` (efi.sh through its command line; `test-efi-flow.sh` runs it) and
`windows/test/Test-EfiVectors.ps1` (IntelXe-EFI.ps1's rules loader and engine, in Windows PowerShell 5.1 or
PowerShell 7, on Windows or macOS; `Test-IntelXe-EFI.ps1` runs it). On 12 November both pass all 32 vectors on macOS
(bash 3.2 with the system plutil; PowerShell 7.6 as a dotnet tool), and both report a changed step, note, file
digest, CPU answer and card answer as failures.

## 6. What the WinUI app must do itself

The app reads `efi-rules.json` (System.Text.Json; `RulesJson` in `EngineTest.cs` is the conversion the engine
takes) and runs `release/efi-rules-vectors.json` in its tests. The plan logic can be the engine as it is (C# 5, no
PowerShell types: `Rules`, `EfiFolder`, `Plist`, `Aml`), or a port of it that passes the same vectors. It must
implement on its own, as the script does now:

- the EFI partitions (listing, mounting, unmounting: `Get-Partition`/`mountvol` in the script), administrator rights
  and elevation;
- the card's Location path from the device manager (`DEVPKEY_Device_LocationPaths`) and the user's confirmation of
  the port before SSDT-ARCOFF is added; the cards by instance id (`Rules.IsCardInstance`);
- the CPU's name (`Win32_Processor.Name`);
- whether OpenCore recorded its boot path (false unless the app reads OpenCore's NVRAM variable, which needs
  administrator rights and booting through OpenCore);
- backups (where, the key per volume, free space, completeness), the undo history and the `restored` mark, in the
  format of section 2 (D10);
- writing `config.plist` with its layout kept (the engine's `Plist`) and checking it as it is written (D7);
- the `SHA256SUMS` check of the package (D2);
- the plan's text and its SHA-256, the confirmation, and the window.

### As built (12 November, branch `winapp2`)

IntelXe Manager for Windows (`windows/app`, part 2) does the list above as follows; `windows/app/README.md` has the
details.

- **The engine, shared, not ported.** `IntelXe.Core.csproj` copies the text between `// BEGIN ENGINE` and
  `// END ENGINE` of `windows/IntelXe-EFI.ps1` into `obj/IntelXeEfiEngine.g.cs` at build time (an inline MSBuild task)
  and compiles it; the script stays one file, its engine stays C# 5 for `Add-Type` under Windows PowerShell 5.1, and an
  edit there changes both. `efi-rules.json` is embedded in the Core like `data/platforms.json`; `EfiRules` reads it
  with System.Text.Json into the engine's tree (the `RulesJson` conversion of `EngineTest.cs`).
- **Vectors.** `EfiVectorTests` runs every vector through the Core's planner (`FolderEfiPlanner`): the engine's steps
  against `expect.ops`, the planner's notes, hash and text against the engine's, the change with the plan's hash, the
  values, files and backup list after it, and undo. 32 of 32 on macOS (dotnet 10) and on Windows 11.
- **Partitions** go through an elevated helper: the app's own exe with `--efi-helper status|plan|apply`, started with
  `runas` for one mount + read/plan or one mount + backup + change + unmount, given the plan's hash (`--expect`) and
  the drive letter the plan was made at (the plan's `EFI:` line names it). The mounting (`IEspMounter`) is `mountvol`
  as in `Mount-Esp`/`Dismount-Esp`; the tests fake it. A folder needs no elevation.
- **Backups** are made by the engine, so they are the script's in place and format: `Test-AppInterop.ps1` undoes the
  script's changes with the app and the app's with the script (stage 2, the Safari setting, stage 4 with the 0.1.4
  package), on fake EFI folders.
- **D3**: the port Windows reports fills a box; `SSDT-ARCOFF.aml` is planned only when the user ticks that macOS is
  installed, stage 3 is done and the path is checked. **D4**: `BootPathRecorded` stays false, as in the script.
- **D13**: the notes are the engine's English text, the script's wording included ("-CardAcpiPath, or the
  SSDT-ARCOFF box in the window"); the page's own texts around them are translated.
- Not tested: a real EFI partition (mounting, writing, unmounting); only the helper's logic with the mount faked, and the
  helper itself on folders.
