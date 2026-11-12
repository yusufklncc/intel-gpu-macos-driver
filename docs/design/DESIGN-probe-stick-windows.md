# The Xe probe stick in IntelXe Manager for Windows (design note)

Status: **built** (13 November 2026), on the Report page of the WinUI app (`windows/app/`); how it works and what was
tested: `windows/app/README.md`, *How the probe stick works*. Built as planned below, except where *As built* (at the
end) says otherwise. Not yet run on Windows: the page, the elevated helper and the raw write were tested only through the
Core's xUnit tests on macOS. The stick itself is `tools/probe-stick/` (README there).

## What the app should do

Most testers with a new Intel GPU run Windows. Today they need balenaEtcher or Rufus to write the stick, and they must
find the report on it themselves. The app can do both ends:

1. **Write a stick**: pick a USB disk, check the image, write it, read it back.
2. **Read a stick**: find the reports on a stick, check their seals, show the summary, and open the issue form with the
   report ID, as the Report page does for `collect-windows.ps1`.

It does not boot anything, does not change Secure Boot (it shows the state the Overview already reads, says the stick
needs it off, and why), and uploads nothing.

## Where it goes

A **Probe stick** section on the Report page (group Help), below *Create a diagnostics report*: one card to write,
one to read. Both texts in `Strings/en-US` and `Strings/tr-TR`, held to the same keys by the existing test. The
Overview's "another Intel GPU is named as not supported yet" line links to it.

## The image

- From a release asset `xe-probe-stick-VERSION.img` with its `.img.sha256` (no release ships it yet: that is a
  separate change), or a local `.img` the user picks. The app checks the SHA-256 before it writes, and refuses a
  mismatch, as `write-stick.sh` does; once releases are signed (`docs/RELEASE-KEYS.md`), the signature too.
- Downloads go to `%LOCALAPPDATA%\IntelXe Manager\probe-stick\` and are reused while the hash matches.

## Choosing the disk (Core, testable anywhere)

`IntelXe.Core/ProbeStick/StickDiskRules.cs`: a pure function from the facts about one disk to *accept* or *refuse,
with the reason*, the same rules as `write-stick.sh`:

- the bus is USB (`MSFT_Disk.BusType` 7) or the media is removable; never NVMe, SATA, RAID, SAS, virtual (VHD, 15) or
  Storage Spaces;
- not `IsBoot`, not `IsSystem`, and not the disk that holds the Windows volume, its page file, or the EFI partition
  Windows booted from (the Overview already knows that one);
- a whole disk (the app lists disks, never volumes), at least as large as the image, at most 128 GB unless the user
  ticks that they mean a larger one;
- the user confirms by typing the disk number shown with its model and size (the default button is *Cancel*).

The rules get shared test vectors, as the EFI rules do (`release/efi-rules-vectors.json`): a small
`tools/probe-stick/stick-disk-vectors.json` of disk facts and expected answers, run by `write-stick.sh`'s checks in a
test and by xUnit in `IntelXe.Core.Tests`.

## Writing (the elevated helper)

Raw disk writes need administrator rights. The app already starts itself elevated as a helper for the EFI page
(`Program.cs --efi-helper`, `Services/EfiHelperHost.cs`); the stick adds a second command, `--stick-helper`, with the
same request/answer files and the same rule that the helper re-checks everything the window decided:

1. re-read the disk's facts and run `StickDiskRules` again (the disk may have changed since the user chose it);
2. lock and dismount each volume on the disk (`FSCTL_LOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`), since Windows refuses
   writes to the sectors of a mounted volume;
3. open `\\.\PhysicalDriveN` for writing (`FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`) and write the image in
   aligned 4 MiB blocks, reporting progress;
4. `IOCTL_DISK_UPDATE_PROPERTIES`, then read the image's length back and compare its SHA-256;
5. answer with the result; the window then shows the next steps (Secure Boot off, the boot menu key, what the scan
   does and how long it takes).

## Reading a stick

The stick's FAT32 partition gets a drive letter on Windows. The app finds volumes labelled `XEPROBE` that hold
`xe-probe-stick.marker`, lists `REPORTS\xe-stick-XR-*.tar.gz`, and for each checks the `.sha256` next to it and the
`SEAL.sha256` inside (`System.Formats.Tar` over a `GZipStream`, no extraction to disk), shows the `.txt` summary,
and offers *Copy the ID*, *Show in Explorer* and *Ask for a new GPU* (the issue form with the ID filled in, as for the
Windows report). A `xe-stick-…-privacy-check-failed.txt` is shown as such: there is no report to attach.

## Tests

- Core: `StickDiskRules` against the shared vectors; the report reader against a report made by
  `tools/probe-stick/test/qemu-test.sh` (a small one, committed as a test vector once its contents have been read:
  it holds only QEMU's planted values, already redacted).
- The helper: a CI runner has no USB stick. A test-only switch lets the helper accept a VHD (`New-VHD`/`Mount-VHD`),
  which the rules refuse otherwise; the test writes the image to it and compares.
- By hand, once: a real stick written by the app boots the B580 test machine.

## Not in scope

Signing the stick's boot loader (a shim would let Secure Boot stay on, but needs a key Microsoft signs), a Windows
build of the image (the build runs in Docker on any host), and sending the report anywhere.

## As built

- **One set of disk rules, not two.** The installer stick already had `Stick/StickDisk.cs` (`StickRules`); there is no
  separate `ProbeStick/StickDiskRules.cs`. `StickRules` now serves both sticks: each refusal carries a code, and the codes
  `write-stick.sh` shares (`partition`, `virtual`, `not-usb`, `not-removable`, `system`, `too-small`, `too-large`) are
  held to `tools/probe-stick/stick-disk-vectors.json` by xUnit (`StickDiskVectorTests`) and by
  `tools/probe-stick/test/disk-rules-test.sh` (`write-stick.sh --rules`, with sh, dash and bash). The rules are the
  stricter union of the two: **USB and removable** (the installer's), not "USB or removable" as planned above; a USB disk
  that reports fixed media is refused by the app and, since this change, by `write-stick.sh` too. The 128 GB limit
  (with a tick to lift it) applies to both sticks.
- **A stick that holds an EFI** (an EFI system partition, or a volume with OpenCore or another `EFI\BOOT\BOOTx64.efi`
  that is not a probe stick's) is not refused but needs, besides the typed name and size, a box of its own in the erase
  dialog under an error bar that says it holds an EFI and will be erased; the helpers refuse such a disk unless they
  are told (`--erase-efi`). This came from the test machine: its recovery EFI lives on a USB stick. The installer stick's
  dialog and helper do the same.
- **The confirmation** is the installer stick's: the disk's name and size typed (`SanDisk Ultra 16.0 GB`), not its
  number; the default button is *Cancel*.
- **A helper of its own**, `--probe-helper check|write`, rather than a verb of `--stick-helper`: the probe stick needs no
  recovery files, and the two command lines stay simple. It shares the runner (`StickRunner`), the answer format, the
  identity hash and `StickWriter.Reinspect`. The raw device is `IRawDisk` (`PhysicalDisk` in the app, `FileRawDisk` in
  the tests). Besides the plan: the disk's last MiB is zeroed (a stale backup GPT), and the first 4 MiB block is
  written last.
- **The image** must have its `.sha256` (the plan followed `write-stick.sh`, which writes an unchecked image with a
  note) and the stick's layout (MBR, FAT32, label `XEPROBE`). It is found beside the app, in its `probe-stick\` folder
  or in `%LOCALAPPDATA%\IntelXe Manager\probe-stick\`, or picked; **no download** (no release ships the image yet) and
  no signature check (releases are not signed with `docs/RELEASE-KEYS.md` yet).
- **Not built:** the Overview's link from "not supported yet" to the section; the VHD test in CI (the helper takes
  `--test-vhdx` like the installer stick's, by hand).
- **The report vector** is the safe scan of `qemu-test.sh` (boot 4, `XR-954586D6`), committed as
  `windows/app/IntelXe.Core.Tests/Fixtures/probe-stick/` after reading it: QEMU's planted names only, the serials and
  UUID redacted.
