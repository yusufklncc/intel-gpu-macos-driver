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
| `bootlog` | the items of opt-in boot logging (section 7), applied in order; readers before revision 2 ignore it |
| `bootlog-off` | the items that turn boot logging off again (section 7); readers before revision 3 ignore it |
| `probe.device_pattern` | the device id the `xeprobe` list's `boot_arg_value` takes (section 8); readers before revision 5 ignore it |
| `xeprobe`, `xeprobe-off` | the items of the XeProbe boot-arg and of its off change (section 8); readers before revision 5 ignore them |
| `backup_kinds` | the `kind` written into a backup per plan: `driver`, `settings`, `safari`, `settings-safari`, `bootlog`, `bootlog-off`, `xeprobe`, `xeprobe-off` |
| `undo_paths` | the only paths under `EFI/OC` that undo restores or removes |

### Items (`settings`, `safari`, `bootlog`, `bootlog-off`, `xeprobe`, `xeprobe-off`)

Each item has `kind` and `path`; `why`, when not empty, ends the plan's line in parentheses.

| kind | fields | the plan |
|---|---|---|
| `value` | `type` (`integer`, `bool`, `string`), `value` | set when the current value (as `plutil -extract raw` prints it) differs; the line shows `missing` for none |
| `value_if` | `type`, `if`, `value` | set only when the current value is `if` (ReconnectGraphicsOnConnect) |
| `require` | `note` | when `path` is not there: the note, and the rest of the list is skipped |
| `note` | `note` | the note, always (`path` names what it is about and is not read) |
| `dict` | | a dictionary made when `path` is not there |
| `bits` | `type` (`data`: four bytes little-endian, or `integer`), `bits` | the bits added when one is missing; the line shows both values in hex |
| `clear_bits` | `type`, `bits` (as for `bits`) | the bits cleared when one is set (a missing value counts as 0); the same line (revision 3) |
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

`release/efi-rules-vectors.json` (format `intelxe-efi-rules-vectors` 1, MIT): 41 vectors, 24 CPU names, 6 PCI
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
- A vector: `name`, `about`, `plan` (`driver`, `settings`, `safari`, `settings-safari`, `bootlog`, `bootlog-off`), `edits` (steps in the language
  below, or `{"op": "remove", "path"}`, on the stage 1 `config.plist`), `package` (fixtures that replace the base
  package's), `efi` (fixtures in the EFI folder), `facts`:
  - `card`: `{"pci": "8086:e20b", "port": "\\_SB.PC00.PEG1.PEGP"}`; `"port": "none"` when the port has no ACPI name; no
    `card` when there is none. Each runner turns it into what its platform reads: efi.sh an ioreg listing with the
    device `cards.port_levels_above_gpu` levels below the port; the script a device instance id
    (`PCI\VEN_8086&DEV_E20B&...`) and, for a card, the Location path Device Manager shows, which the user confirms;
  - `cpu`: the CPU's name;
  - `boot_path_recorded`: whether OpenCore records its boot path (efi.sh: a boot-path in `INTELXE_BOOT_PATH`; the
    script: `BootPathRecorded`).
- `expect.status`: status values (`{"boot_logging": "off"}`, as `--status` and `-Status` print them) before the plan;
  `expect.after.status` the same after the change.
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
(alone, with stage 2, no `Misc > Security`), boot logging (no logging, the sample's onscreen logging, already on with
wider bits, missing keys, no `Misc > Debug`) and off (the sample, after on, already off, no `Misc > Debug`), stage 4 (no card, the shipped port, a B570, an unsupported device and
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
- **D13**: the engine's texts stay English and the script's (the vectors and the plan's SHA-256 are made from them);
  the app shows them through a table, `IntelXe.Core/Efi/EngineText.cs`, that maps each step, note, output line and
  error message of the engine, by its template, to a key of the app's string tables with the variable parts as
  arguments (the rules' "why" texts, the backups' kinds and the reasons inside a note are keys too). The English table
  words them for the app ("tick the box under the path box", not "-CardAcpiPath"; "for IntelXe Manager and efi.sh on
  macOS"), the Turkish one translates them; a line no template matches is shown as the engine wrote it.
  `EngineTextTests` reads the engine's source between the markers and holds every `p.Notes.Add`, `p.Lines.Add` and
  `say` there, the rules' notes and whys, and every line the 32 vectors produce, to a template.
- Not tested: a real EFI partition (mounting, writing, unmounting); only the helper's logic with the mount faked, and the
  helper itself on folders.

## 7. Boot logging (13 November, branch `bootlogs`)

When a PC fails to reach the macOS desktop there is usually nothing to report but a photo of the screen. Boot logging is
an opt-in change, like the Safari setting, that makes OpenCore leave its log and any macOS panic on the EFI, where the
diagnostics collectors find them later (`tools/report/README.md`, "Boot logs"). The idea is NullMoth's 1401 (its
"boot-logs-on-stick" rule and "Scan for logs and send them"); nothing of its code is used.

efi.sh `--bootlog-plan`/`--bootlog-apply`, IntelXe-EFI.ps1 `-Plan bootlog`/`-Apply bootlog`, the engine's
`MakePlan("bootlog")` (the Windows app's `EfiChange.BootLog`). The change backs up the whole EFI folder (kind `boot
logging`), as every change does. Both apps' EFI pages offer it in a "Boot logging (for troubleshooting)" section: its
state, "Turn on" and "Turn off" (each a plan in a dialog or sheet, made with the plan's hash), the note on the slower
boots and a link to the Report page's "Boot logs" option (13 November, branch `bootlogui`).

**Turning it off is its own change, `bootlog-off`, not undo.** Undo puts back the newest backup not yet put back, and
from it the whole `config.plist`. That is boot logging's backup only while no other change came after it: with stage 4
made after the logs were turned on, undo takes back stage 4 and leaves the logging on; with `config.plist` edited in
another tool since, undo loses those edits (its plan only notes it). efi.sh `--bootlog-off-plan`/`--bootlog-off-apply`,
`-Plan bootlog-off`/`-Apply bootlog-off`, `EfiChange.BootLogOff` touch only what boot logging is about, whatever came
before or after; the change has its own backup (kind `boot logging off`), so undo can take it back in turn:

| item | key | value | why |
|---|---|---|---|
| `require` | `Misc > Debug` | | as above |
| `clear_bits` | `Misc > Debug > Target` | `0x40` cleared (67 -> 3, 65 -> 1) | no log file, the slow part. 0x01 and the other targets stay: someone's onscreen log (0x02) is theirs, and 0x01 with no target writes nothing |
| `value_if` | `Misc > Debug > AppleDebug` | `false` when `true` | a missing key stays missing (OpenCore's default is false) |
| `value_if` | `Misc > Debug > ApplePanic` | `false` when `true` | as AppleDebug |

`DisplayLevel` stays: it filters only the onscreen output. So off is not always the exact config before on (the 0x01 of
an EFI that logged nothing, DEBUG_INFO in DisplayLevel); for that exact config, undo right after turning it on still
works.

**Status.** `--status` and `-Status` print `boot_logging=`: `on` when the `bootlog` plan has nothing left to change,
`off` when `bootlog-off` has nothing left, `partial` otherwise (the sample logs to a file with AppleDebug and ApplePanic
but DisplayLevel 0x80000002), `missing` without `Misc > Debug`, empty when `config.plist` cannot be read. The pages
offer "Turn on" unless it is on and "Turn off" unless it is off; neither when missing.

| item | key | value | why |
|---|---|---|---|
| `require` | `Misc > Debug` | | every OpenCore config has it; without it nothing is changed |
| `note` | | | the plan always says that logging slows every boot and to turn it off once the logs are collected |
| `bits` | `Misc > Debug > Target` | `0x41` added (65 from 0) | 0x01 logging on, 0x40 to a file: `opencore-YYYY-MM-DD-HHMMSS.txt` (UTC) in the root of the EFI volume OpenCore started from. Other bits stay (the sample's 67 = 0x43 also shows it on screen) |
| `bits` | `Misc > Debug > DisplayLevel` | `0x80000042` added (2147483714 from the sample's 0x80000002) | Dortania's value: DEBUG_ERROR, DEBUG_WARN, DEBUG_INFO. It filters only the onscreen output (OcLog.c checks it for the console only); the file takes every line the build prints |
| `value` | `Misc > Debug > AppleDebug` | `true` | boot.efi's log (macOS 10.15.4 and later) goes into OpenCore's log as `AAPL:` lines, with `EBPF:` performance lines |
| `value` | `Misc > Debug > ApplePanic` | `true` | at the next boot through OpenCore after a panic, macOS's panic record in NVRAM is saved as `panic-YYYY-MM-DD-HHMMSS.txt` in the root of that EFI volume |

**RELEASE or DEBUG.** No DEBUG build is needed for what the collectors look for, checked in OpenCorePkg's source:

- the file target works in every build (`OcConfigureLogProtocol`); a RELEASE build compiles its own messages with
  `PcdFixedDebugPrintErrorLevel` 0x80000002, so its log holds OpenCore's warnings and errors only, no DEBUG_INFO lines
  (the picker's entries, the kexts injected, the quirks applied). The DEBUG and NOOPT builds add those;
- AppleDebug's lines go to the log through `OcLog->AddEntry` directly (`OcAppleLog.c`), not through the build's debug
  library, so boot.efi's log is in the file with a RELEASE build too;
- ApplePanic (`SavePanicLog` in `OpenCoreMisc.c`) is not tied to the build either. Its own "Saving ... panic log" line is
  DEBUG_INFO, so a RELEASE log does not mention it; the file is there all the same;
- `SysReport` would need a DEBUG build; boot logging does not set it.

**Limits.** The log starts when OpenCore has read `config.plist`: a firmware that never starts OpenCore, or a config
that does not parse, leaves no log. Nothing after boot.efi hands over to the kernel is in it; a hang in the kernel
before a panic leaves only boot.efi's last lines, and a panic is saved only when the machine next boots through
OpenCore (with ApplePanic still on). OpenCore writes to the volume it started from when that one is writable, else to
the first writable file system it finds. `keepsyms=1` in the boot-args (the sample config has it) makes the panic's
backtrace readable; boot logging does not add it.

**The cost.** Dortania's debugging page gives no figure: it warns only that onscreen logging (0x02, which this change
does not add) "can heavily increase boot times on firmwares with poor GOP implementations". OpenCore's manual says file
logging is written "in the safest manner and thus, is very slow", and to set `DisableWatchDog` on a slow drive (the
sample has it; this change does not set it). In the source, the safe mode rewrites the whole 256 KB log buffer to the
file after every line (`OcSetFileData` with `OC_LOG_BUFFER_SIZE`); with AppleDebug that is some hundreds of lines per
boot, so tens of MB written through the firmware's FAT driver: seconds from an internal SSD, tens of seconds or more
from a slow USB stick (an estimate from the write volume, not measured). The fast mode (0x80) is not used: the manual
warns that it can corrupt the FAT on firmware with a poor FAT32 driver. Each boot also leaves a new 256 KB file in the
volume's root. The plan says so (the `note`), and `bootlog-off` takes the change back.

**Tests.** Nine vectors (`bootlog-*`, four of them `bootlog-off-*`), each with `boot_logging` before (and after, when it changes something);
`release/test-efi-flow.sh` plans, applies and undoes it with efi.sh, then turns it on, makes stage 2, checks that undo
would take back stage 2, turns it off (stage 2 stays) and undoes the off change; the Windows Core's `BootLoggingTests`
do the same through the app's planner; the macOS app's `--selftest` turns it on and off on its fake EFI.
**Not tested yet:** a real boot on the B580 machine with a USB-stick EFI copy (the night run): that the RELEASE log has
the `AAPL:` lines, that a panic is saved, how many seconds the boot takes with and without it.


## 8. The XeProbe boot-arg (13 November, branch `xeprobe`)

XeProbe ([docs/PROBE-MODE.md](../PROBE-MODE.md)), the staged probe in `IntelXe.kext` for an Intel GPU the driver does
not support yet, attaches only to the device the boot-arg `xeprobe=0xDEVICE` names. With stage 2's config, OpenCore
deletes and rewrites `boot-args` at every boot, so `sudo nvram boot-args=...` does not last: the boot-arg has to go into
`config.plist`. Two opt-in changes do it, like boot logging: `xeprobe` and `xeprobe-off` (revision 5).

efi.sh `--xeprobe-plan`/`--xeprobe-apply --probe-device 0xDEVICE` and `--xeprobe-off-plan`/`--xeprobe-off-apply`;
IntelXe-EFI.ps1 `-Plan xeprobe -ProbeDevice 0xDEVICE` and `-Plan xeprobe-off`; the engine's `MakePlan("xeprobe")` with
`EfiFolder.ProbeDevice` (the Windows Core's `EfiChange.XeProbe`, `EfiOptions.ProbeDevice`, the helper's
`--probe-device`). IntelXe Manager's Probe section on macOS offers them; the Windows app only plans them (vectors,
helper) and shows XeProbe results.

| item | key | value | why |
|---|---|---|---|
| `require` | `NVRAM > Add` | | as stage 2 |
| `dict` | `NVRAM > Add > 7C436110-…` | | as stage 2 |
| `boot_arg_value` | `… > boot-args` | `xeprobe=` + the fact `probe_device` | XeProbe attaches to that GPU only and does nothing until a stage is run |
| `dict`, `strings` | `NVRAM > Delete > 7C436110-…` | `boot-args` | so that the new value replaces the old one at boot |

`xeprobe-off` has one item, `boot_args_remove` with `^(xeprobe=.*|-xeprobecrumb)$`: both tokens out, the others kept in
their order, `NVRAM > Delete` kept. Each change has its own backup kind (`XeProbe boot-arg`, `XeProbe boot-arg off`), so
undo takes it back too. `--status`/`-Status` print `probe_boot_arg=`: the device the config's boot-args name, or empty.

Two new item kinds (readers before revision 5 ignore the two lists, as they ignored `bootlog` before revision 2):

| kind | fields | the plan |
|---|---|---|
| `boot_arg_value` | `name`, `value` (the fact it comes from: `probe_device`, the only one) | `name=VALUE`: the first `name=` token takes the new value in its place and later ones go, else it is appended as `boot_args` appends; nothing when the one token already is it. The line: `set name=VALUE ('cur' -> 'new') (why)` |
| `boot_args_remove` | `remove` (a regular expression, matched against each token) | the matching tokens out; the line `remove TOKENS ('cur' -> 'new') (why)` |

Tokens are the boot-args split at spaces, tabs and line breaks (bash `read -a`). The device id is lower-cased and must
match `probe.device_pattern` (`^0x[0-9a-f]{4}$`); anything else stops the plan with an error that names it.

**Tests.** Nine vectors (`xeprobe-*`, three of them `xeprobe-off-*`): the stage 1 config (the id given in upper case,
`boot-args` added to `NVRAM > Delete`), another id named twice, already set, no `boot-args`, no `NVRAM > Delete`, no
`NVRAM > Add`, off with `-xeprobecrumb`, off with nothing to do, off without `boot-args`. On 13 November: efi.sh 53 of
53 (`release/test-efi-vectors.sh`), IntelXe-EFI.ps1 53 of 53 (`Test-EfiVectors.ps1`, PowerShell 7.6 on macOS), the
Windows Core's `EfiVectorTests` 53 of 53 (dotnet 10 on macOS).
