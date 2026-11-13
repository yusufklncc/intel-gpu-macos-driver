# Screenshots for the Windows guide

The pictures `docs/WINDOWS-GUIDE.md` and `docs/WINDOWS-GUIDE.tr.md` refer to. None exists yet: take them on the Windows
machine and save them here under these names (PNG). Both guides use the same files (English interface); Turkish
copies can follow later as `NAME.tr.png`, the guides would then need their paths changed. The site page
(`site/guide/windows-app.html`) has no pictures yet; copy the finished ones to `site/images/win/` to add them there.

## How to take them

- Light theme, English, one size for every app window, through the app's own options, for example:

  ```powershell
  .\IntelXeManager.exe --section overview --lang en-US --theme light --size 1280x860
  ```

  `--section setup|overview|bios|build|stick|efi|report|about`, `--scroll PIXELS` for a lower part of a page.
- Capture **the app's window only** (Alt+PrintScreen, or Snipping Tool in window mode): no taskbar, no desktop, no
  other windows.
- The B580 test machine, with the state described below. Photos of the PC's screen (picker, verbose boot) are fine as
  phone photos, cropped to the screen and saved as PNG.

## Hide in every picture

- **Your user name.** It appears in every full path the app prints: `C:\Users\<name>\...` in the EFI page's backup
  folder (*The backup of the whole EFI folder goes to …*), the package folder, the USB installer's *Kept in …*, the
  probe stick's download folder, *Saved to …*, the Report output. Crop or blur those lines, or use a test account
  with a neutral name; `--report-out C:\IntelXe\reports` keeps the report's path neutral.
- **Serial numbers, MLB, ROM, UUID:** the app never shows them, but keep *PlatformInfo (identity masked)* collapsed,
  do not show `config.plist`, the build log or the EFI folder in Explorer.
- **The administrator prompt's details** (*Show more details* shows the program's path): keep them collapsed.
- **The report ID** may stay (it is random), but blur it if in doubt; no GitHub issue pages with your account.
- Computer name, Wi-Fi names, IP addresses: none of the pages below shows them; check the edges of photos.

## The list

| File | Page (`--section`) | State to show | Hide / note |
|---|---|---|---|
| `00-setup.png` | Set up (`setup`) | fresh start (*Start over* first): step 1 *This PC* **Next** with the B580 found, steps 2 to 5 **Blocked**; the step list and step 1's section visible | nothing special |
| `00a-smartscreen.png` | Windows, not the app | SmartScreen's "Windows protected your PC" **after** *More info*, with *Run anyway* visible (first start of a freshly extracted, not unblocked copy) | the folder path if it shows the user name |
| `00b-uac.png` | Windows, not the app | the administrator prompt for `IntelXeManager.exe`, "Unknown publisher" (for example from *Scan this PC…*) | details collapsed (program location) |
| `01-overview.png` | Overview (`overview`) | *At a glance* and the top of *This PC*: Graphics card Arc B580 with its driver, Processor, HybridPM.kext *Needed*, Firmware mode UEFI, Secure Boot, Resizable BAR *On* | nothing special (disk models are fine) |
| `02-bios.png` | BIOS settings (`bios`) | this board's maker (ASUS) with the keys line, the counts (*N to do*, *N done*), the *Windows first* group and the first firmware items with their states | nothing special |
| `03-build-efi.png` | Build the EFI (`build`) | after a scan: *This PC was scanned: the report and N ACPI tables.*, *2. macOS version* on Tahoe, the *Build the EFI* button ready | the scan folder path, if shown |
| `03b-build-result.png` | Build the EFI (`build`, `--scroll` to *4. Result*) | a finished build: *Built: macOS 26, OpenCore 1.0.8, Mac model MacBookPro16,2.*, ocvalidate passed with 1 finding, stage 2 complete, the answers/changes expanders closed | **PlatformInfo collapsed**; no log or folder windows |
| `04-usb.png` | USB installer (`stick`) | Tahoe chosen, *Recovery image* downloaded and checked, a real USB stick listed as *Can be erased and written.* and chosen, *Use the EFI built on this PC* chosen, the *Erase and write the stick…* button | *Kept in …* path (user name) |
| `04a-usb-confirm.png` | USB installer, erase dialog | the dialog *Erase …?* with the stick's name and size typed, the *Erase and write* button enabled (take it, then *Cancel*, unless you are writing anyway) | nothing special |
| `05-report.png` | Report (`report`) | *Diagnostics report* with *Boot logs* on, after a report: *Last report* with the ID, *Copy the ID*, *Report a bug* | the output pane's paths (user name); blur the ID if in doubt |
| `06-probe.png` | Report (`report`, `--scroll` to *Probe stick*) | the probe stick card: the image checked (*Image … : the stick's layout, and its SHA-256 matches its .sha256.*), a USB stick chosen, *Write the probe stick…*, and the Secure Boot line | the download folder path |
| `07-picker.png` | photo of the PC's screen | the OpenCore picker booted from the stick, the macOS installer entry ending in **(dmg)** selected | nothing on screen besides the picker |
| `08-efi.png` | EFI (OpenCore) (`efi`) | the stick's EFI folder chosen (*Use an EFI folder…*), *Read the EFI* done, *Package* with *All files match*, the stage 4 section with the card's ACPI path and the box ticked | package folder and backup paths (user name) |
| `08a-efi-plan.png` | EFI (OpenCore), plan dialog | the stage 4 dialog *Make these changes*: the steps, *The backup of the whole EFI folder goes to …*, *Plan SHA-256: …*, *Back up and apply* / *Cancel*. `--efi-folder E:\EFI --card-confirmed --plan driver` opens it at start | **the backup path** (user name) |

Optional, if they come out well: `03a-build-running.png` (the build's screens and rules scrolling, *Cancel* visible),
`04b-usb-done.png` (*The stick is ready.*), `07a-verbose.png` (photo: the verbose text of the first boot),
`09-efi-bootlog.png` (*Boot logging (for troubleshooting)* with *Turn on…*), `10-about.png` (About with *Language*
and *Appearance*). The guides do not refer to these yet.

## Taken (14 November 2026)

Taken on the B580 test PC with the app's own options (`--section`, `--lang en-US`, `--theme light|dark`,
`--size 1280x860`), the window only: `00-setup`, `01-overview`, `02-bios`, `03-build-efi`, `03b-build-result`,
`04-usb`, `05-report`, `06-probe`, `08-efi`, each also as `NAME.dark.png` (the guides pick it with `<picture>` for a
reader in dark mode). In `04-usb` and `06-probe` the folder path was redrawn as `%LOCALAPPDATA%\...` (it showed the
user name). Still missing, so their places in the guides are commented out: `00a-smartscreen`, `00b-uac` (Windows'
own dialogs), `04a-usb-confirm` and `08a-efi-plan` (the app's dialogs: `--plan driver` did not open the plan at start
with an EFI folder that has no IntelXe yet), `07-picker` (a photo).
