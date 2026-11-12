# The macOS update guard: IntelXe Manager before and after a macOS update

> Design note with its date (2026-11-13, branch `osguard`): it records the plan and the state of that day, not the
> current status. Game mode, built on the same branch, has its own note: [`DESIGN-game-mode.md`](DESIGN-game-mode.md).

A macOS update replaces the system volume and the kernel collections Apple ships. Part of this stack lives there or
is linked against it. Until now the user guide (`site/guide/upgrading.html`, `release/README-INSTALL.md` "If it does
not come up") said what to do afterwards, and IntelXe Manager noticed one consequence, the system-volume copy that is
gone (`Health.swift` `ssv-gone`). This note designs a guard: the Manager warns before an update the driver does not
support, prepares a one-shot check for one it does, and that check repairs what it can at the first start of the new
macOS.

NullMoth's 1401 app has an "OS update guard" (`mac/Resources/nullmoth-setup.sh`, `--update prepare/finish`,
`com.nullmoth.osupdate`). It was read for ideas only (PolyForm Noncommercial); no code comes from it. Its approach:
park the driver with a boot-arg before a major update, a LaunchDaemon on the new macOS that rebuilds the kernel
collection, reinstalls and un-parks, a bounded number of tries, an expiry for a preparation that never saw an update,
and a log. Section 2 explains why this stack does not park; the rest (one-shot daemon, bounded tries, expiry, log)
is the same idea.

## 1. What the stack needs across an update

| Part | Where it lives | Minor update (26.7.x → 26.7.y) | Major update (26 → 27) |
|---|---|---|---|
| `IntelXe.kext` | the EFI; OpenCore's `Kernel > Add` injects it into every boot, the installer's included | stays. Links against `com.apple.kpi.*` 10.0.0 and up (`kernel/IntelXe/build.py`), which every newer kernel exports | stays and is injected; `com.apple.kpi.unsupported` may differ: it may fail to link (OpenCore logs and skips it) or start |
| `IntelXeFB`, `NMXAccel`, `NMXAGDC` | `/Library/Extensions`, in the Auxiliary Kernel Collection that `install.sh` builds with `kmutil create -n aux` against the boot and system collections | the files stay. The update replaces the boot and system collections the AuxKC was linked against; whether the update rebuilds the AuxKC with our kexts in it (they were put there by `kmutil` with SIP relaxed, not through the approval database) was **not recorded** on the test machine. The guard checks that they load instead of assuming it | NMXAccel is built for macOS 26's IOAcceleratorFamily2: its vtable forwarding is generated from 26's binary (`nullmoth/kexts/NVRM/accel/gen_tahoe_fwd.py`, `tahoe_fwd.h`). On 27 it either fails to link (no accelerator) or links with wrong slots (a panic in the first accelerated minute). Without the accelerator WindowServer has no desktop on the Arc (ROADMAP, boot guard) |
| the Metal driver | `/Library/GPUBundles` (data volume) | stays and is expected to work (the plugin interface did not change within 26) | built against 26's private Metal driver interface: unknown |
| the system-volume copy (Safari on the GPU) | `/System/Library/Extensions/NVMTLDriver.bundle` (sealed system volume) | **gone**: the update replaces the snapshot. Installing it again needs `SecureBootModel Disabled` and makes a new snapshot: a user decision (`ssv-install.sh --apply`) | gone |
| boot-args, `csr-active-config` | OpenCore's `config.plist` (`NVRAM > Add` and `Delete`) | stay | stay |
| the driver's NVRAM settings (`intelxe-*`, `intelxefb-*`) | NVRAM | stay (an update does not reset NVRAM; OpenCore's Reset NVRAM does) | stay |
| boot guard daemon (`guard-clear.sh`) | `/Library/LaunchDaemons` | stays | stays |
| `HybridPM.kext` | the EFI | stays; if the update changed the XCPM routine it corrects, it stands aside by itself (guide, route 3) | Lilu's range ends at Tahoe (`kernel/HybridPM/HybridPM.cpp`): it does not start |
| Zink, the Vulkan provider | `/Library/IntelXe` | stay | built against 26's frameworks: unknown |

So a minor update needs at most: the AuxKC rebuilt (`install.sh --apply`, which also puts back anything else that
went missing) and a restart, and the system-volume copy installed again if it was there. A major update the release
does not support cannot be repaired by reinstalling; it needs a release built for it.

## 2. Is parking needed? No

Parking means: before the update, make the driver not load (NullMoth writes `-nvoff` into the boot-args of the EFI's
`config.plist`), and turn it back on once the new macOS is set up. For this stack:

1. **A boot-arg cannot be set from macOS here.** Stage 2 of the install guide puts `boot-args` and
   `csr-active-config` in OpenCore's `NVRAM > Delete`, so OpenCore writes the config's values at every boot; an
   `nvram boot-args=...` from macOS is gone at the next start. Parking by boot-arg is an edit of the EFI's
   `config.plist` (efi.sh), before and after the update, on an EFI that may not be mounted at either moment.
2. **Parking costs the desktop.** On macOS 26 WindowServer has no desktop on the Arc without the accelerator (a held
   boot stays at the Apple logo: ROADMAP "Boot guard", README-INSTALL "If it does not come up"). The Arc has no Apple
   driver to fall back to. A parked first boot of the new macOS is certainly dark; an unparked one of a minor update
   is expected to work. For the common case parking is strictly worse.
3. **The kexts on an unknown macOS.** None of them checks the macOS version (no `version_major` or `osrelease` test
   in `kernel/IntelXe`, `kernel/IntelXeFB`; the OSBundleLibraries ask for `com.apple.kpi.*` 8.0 or 10.0 and newer).
   They load wherever their symbols resolve; what can go wrong is NMXAccel on a new major version (section 1).
4. **The boot guard already bounds a crash loop.** IntelXeFB counts accelerated boots that end inside their first
   minute (`intelxe-guard`): after two the next boots arm NMXAccel's safe settings, after four acceleration is held
   (`intelxe-accel-off`) until cleared from the Manager, over SSH (`sudo nvram intelxe-accel-off=ok`) or by Reset
   NVRAM in the picker. A new macOS that crashes the accelerator ends in a held, dark but running system, the same
   end state as parking, after at most four boots.
5. **What parking would add on a major update**: up to four panicking boots fewer. It would not give a desktop.

Decision: no parking. Instead:

- **Before**: warn about an offered update the installed release does not support (the Overview page, a banner, the
  check at login). The advice is to wait for a release; the guide's way back after a major update is Time Machine.
- **After**: check, and repair what reinstalling repairs (section 4).
- **The boot guard after an update**: the update's own restarts can end boots inside their first minute (a
  shutdown before the desktop was steady leaves `intelxe-guard` set). If the guard holds acceleration after the
  update, it was clear when the update was prepared, no kernel panic report was written since, and this is the first
  check after the update, the daemon clears it once (`intelxe-accel-off=ok`) and restarts. A panic report, or a
  second time, leaves it to the user.
- **The one real parking knob**, for whoever must run an unsupported major version anyway (with another GPU for the
  desktop): `MaxKernel` on IntelXe.kext's `Kernel > Add` entry (`25.99.99` keeps it out of Darwin 26 = macOS 27),
  and `install.sh --uninstall` for the /Library part. An EFI edit; the guard does not make it.

## 3. Detecting a pending and a finished update

| What | How | Cost |
|---|---|---|
| updates on offer | Software Update's record `/Library/Preferences/com.apple.SoftwareUpdate.plist`: `RecommendedUpdates` items with `Display Name`, `Display Version`, `Identifier` (`MSU_UPDATE_<build>_patch_<version>_<minor\|major>`), `MobileSoftwareUpdate`; `AutomaticallyInstallMacOSUpdates`. World-readable, written by softwareupdated's background scan. Seen on the development Mac (macOS 27.0.1): `MSU_UPDATE_26B131_patch_27.2.1_minor` with `Display Version` 27.2.1 | no network, no root, instant |
| updates on offer, now | `softwareupdate --list` (`* Label: macOS Tahoe 26.7.2-25H123` / `Title: ..., Version: 26.7.2, ...`): the "Check now" button | asks Apple's catalog: tens of seconds |
| an update happened | `ProductBuildVersion` in `/System/Library/CoreServices/SystemVersion.plist` against the build the preparation recorded (the daemon), or `kern.osversion` against the build the Manager saw at its last start (`lastSeenMacOSBuild`) | instant |
| an update is downloaded and staged | **not detectable without private APIs.** The download and preparation state belongs to MobileSoftwareUpdate (`/System/Library/AssetsV2/com_apple_MobileAsset_MacSoftwareUpdate/*.xml` lists the offered assets, not whether one is staged; `/System/Volumes/Update` holds the preparation in root-only places). The Manager therefore says "ready to install", not "downloaded" | |

Any build change counts as an update (a security response that changes the build too). The record says
`_major`/`_minor`; the guard does not rely on it and compares major version numbers itself.

## 4. The guard

### 4.1 Which macOS versions a release supports

`data/platforms.json` has no macOS list (it lists PCI devices and features); the app's only list is
`packageSuffix` (26 and 15). The authority is the package that gets installed, so `install.sh` answers:
`install.sh --supports-macos VERSION` prints `ok`, `untested` or `no`, from the same `macos_support` function its
`check` uses (Tahoe package: 26.7.x ok, another 26.x untested, anything else no; Sequoia package: 15.x ok), and
`--status` adds `macos_support=` for the running macOS. Packages before this branch (0.1.4) do not have it: their
own major version (`package_macos`) is supported, nothing else. The Manager (`macOSSupport`) and the daemon
(`support`) use the same rule. `untested` is treated as supported (install.sh itself only warns).

### 4.2 Before: Prepare

The Overview page's *macOS updates* section lists the offered updates with the verdict for each. **Prepare** (a
supported update, the installed release's package chosen on the Driver page) runs
`osupdate-guard.sh --prepare PACKAGE TARGET` as root through the password dialog (the app's usual path, no helper
tool). It:

1. checks the package against its `SHA256SUMS` and that it is the installed release (`installed-version`);
2. copies it to `/Library/Application Support/intel-gpu-macos-driver/osupdate/package`, root-owned: the daemon runs
   as root and must not run a script from the user's downloads folder, which the user (or anything running as the
   user) can change; the copy is checked against `SHA256SUMS` again before it is used;
3. copies the guard script itself next to it (the app may move or be replaced before the update);
4. records the state (`state`): versions, build, target, package version and macOS, the `restart` choice, and
   `--check` as it is now (kexts loaded, AuxKC, Metal driver, system-volume copy, boot guard), prefixed `before_`;
5. writes `/Library/LaunchDaemons/io.github.yusufklncc.intelxe.osupdate.plist` (`RunAtLoad`, `/bin/bash
   <kept script> --finish`), checked with `plutil -lint`; a file launchd could not read would leave a preparation
   that never runs, so then nothing is prepared.

The daemon is not loaded now: launchd loads it at the next start. An unsupported target can still be prepared (the
log notes it); the daemon then only reports. **Cancel the preparation** (`--cancel`) removes the daemon, the record
and the kept copy.

### 4.3 After: the daemon, once per start until it is done

| Situation | What it does | Result | Daemon |
|---|---|---|---|
| the build is the recorded one | logs "waiting" | – | stays |
| the same, 14 days after the preparation | gives up waiting | `expired` | removed |
| a new build: the count of checks goes up; the AuxKC's kexts get up to 90 s to load | `--check` into the log | | |
| the release does not support the new macOS | installs nothing | `unsupported` | removed |
| IntelXe.kext (from the EFI) is not loaded | installs nothing (the /Library kexts would have nothing to attach to) | `efi-kext` | removed |
| the boot guard holds acceleration, it was clear before, no panic report since the preparation, first check | `nvram intelxe-accel-off=ok` | `guard-cleared` | stays (checks again) |
| a kext installed but not loaded, the Metal driver missing, or installed files that differ from the package | checks the kept package's `SHA256SUMS` (else `refused`, removed), `install.sh --apply` (backs up, copies, rebuilds the AuxKC) | `repaired` | stays (checks again) |
| `--apply` failed | | `retrying`; `gave-up` at the third | stays; removed |
| still not loaded at the fourth check | | `gave-up` | removed |
| everything loads | notes a system-volume copy that is gone, a guard that holds after a panic | `ok` | removed |

After `repaired` or `guard-cleared` at the first check it restarts once by itself (`shutdown -r +1`), unless the
preparation said `--no-restart` (the Overview page's switch, on by default: without the AuxKC's kexts the Arc may
show no desktop to restart from). Bounds: at most three installs, one automatic restart, one guard clear. When it
is done it removes the LaunchDaemon file, the record and the kept package; `result` and `osupdate.log` stay. It never
touches the EFI.

### 4.4 After, without a preparation

The Manager remembers the build it saw at its last start. When it starts on another build that no daemon result
covers, the *macOS updates* section says so and shows `osupdate-guard.sh --check` (no root): kexts installed but not
loaded, the Metal driver missing, IntelXe.kext not loaded, the boot guard. When something is missing and the chosen
package supports the macOS, its button runs the Driver page's *Install this package*. A new health check,
`kexts-not-loaded` (IntelXeFB.kext in /Library/Extensions, IntelXe loaded, IntelXeFB not), puts the same thing in a
banner and in the dialog of the optional check at login. `ssv-gone` already covered the system-volume copy.

### 4.5 What the user sees

- The Overview page, *macOS updates*: the last result (with *Done*), the system-volume copy that went with the
  update (*Open the Driver page*), the preparation (*Cancel the preparation*), the offered updates with their
  verdict, *Prepare…*, the restart switch, *Check now*, and a note when Software Update installs updates by itself.
- Banners and the check at login (`healthIssues`): `osupdate-unsupported` (an offered update of another major
  version than the running one; key: its version, so a dismissal holds until another is offered), `osupdate-result`
  (a result that needs the user: `unsupported`, `efi-kext`, `gave-up`, `refused`, `retrying`), `kexts-not-loaded`.
- The Report page: `collect-macos.sh` adds `driver/osupdate-state.txt` (record, result, tries, daemon) and
  `logs/osupdate.log` (its last 400 lines).

## 5. Tests

- `app/tests/test-osupdate-guard.sh` (89 checks, about 15 s, no root): the guard against a throwaway root, with a
  package made from `release/templates/install.sh` (paths under the throwaway root, root and kmutil stubbed as
  `release/test-install-flow.sh` does) and stubs for `kmutil`, `nvram` and `shutdown`. It covers
  `install.sh --supports-macos` and `--status`; `--check`; Prepare refused without an installed driver, without a
  package, with another release's package, with a damaged one, and outside the test root without root; the record,
  the plist (`plutil -lint`), the kept copies; waiting and expiry; a minor update with everything loaded (nothing
  installed); one that lost the AuxKC and the system-volume copy (repaired, one restart, `ok` and `ssv=gone` after
  it); `--no-restart`; kexts that never load (three tries, one restart, `gave-up`); an unsupported major version
  (nothing installed, no restart); IntelXe.kext missing; the boot guard after the update with and without a panic
  report; a damaged kept package; `--apply` failing (retrying, gave-up); cancel; and a 0.1.4 package (from git
  history) without `--supports-macos`.
- `release/test-install-flow.sh` still passes with the changed `install.sh`.
- `IntelXeManager --selftest`: Software Update's record and `softwareupdate --list` parsed, support by major version,
  the result texts, `--status` read from the carried script; with a package, `--supports-macos` through the app.
  `--snapshot` adds `system-osupdate.png` (the Overview page with an offered supported update, an unsupported one and
  a result).

## 6. What needs a real update or the test machine

Not tested without one: the daemon at the first start of a really updated macOS (launchd loading it, the kexts'
load timing against the 90 s wait, `kmutil` at that moment), whether a minor update keeps our kexts in the AuxKC by
itself (the answer decides how often the repair runs), `install.sh --apply` from the daemon with no user logged in,
the automatic restart, the guard clear after real update restarts, Software Update's record on macOS 26 (it was read
on 27), the password dialog, and the banners and login dialog on the test machine. The next macOS 26.7.x update on
the test machine is the test: prepare it from the Manager, update, and read `osupdate.log`.

## 7. Files

`app/Resources/osupdate-guard.sh` (the guard), `app/Sources/OSUpdate.swift` (detection, verdicts, the Overview
section), `app/Sources/Health.swift` (`kexts-not-loaded`, `osUpdateIssues`), `app/Sources/Views.swift` (state,
refresh, banner fixes), `release/templates/install.sh` (`--supports-macos`, `macos_support=`),
`tools/report/collect-macos.sh` (the guard's state and log), `app/tests/test-osupdate-guard.sh`.
