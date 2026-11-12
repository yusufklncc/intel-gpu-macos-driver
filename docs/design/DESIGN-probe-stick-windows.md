# The Xe probe stick in IntelXe Manager for Windows (design note)

Status: a plan; nothing of it is built. The stick itself is `tools/probe-stick/` (README there). This note is for
whoever adds it to the WinUI app (`windows/app/`); the app is not changed by the stick's work.

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
