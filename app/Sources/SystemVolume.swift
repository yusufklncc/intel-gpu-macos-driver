// SPDX-License-Identifier: MIT
// The optional system-volume copy of the Metal driver (Safari and WebKit on the GPU): its state, read from the files
// themselves, and the package's system-volume/ssv-install.sh for every change. Safari's GPU process may map code only
// from the system volume, so without this copy Safari draws in software. The script mounts the system volume, copies the
// bundle and blesses a new boot snapshot (as OpenCore Legacy Patcher does); a macOS update replaces the snapshot and
// with it the copy. Its removal boots the last sealed snapshot again, which takes back every root change, not only ours.
import Foundation
import SwiftUI

let ssvBundle = "/System/Library/Extensions/NVMTLDriver.bundle"
let ssvRecord = "/Library/GPUBundles/nvmtl/ssv-component.plist"
let useLibraryFlag = "/Library/GPUBundles/NVMTLDriver.use-library"
/// install.sh --apply's record that it made useLibraryFlag (0.1.6; 0.1.5 and earlier: created-use-library in its backup)
let useLibraryRecord = "/Library/Application Support/intel-gpu-macos-driver/use-library-by-install"

/// useLibraryFlag is there and install.sh --apply made it, the same test as install.sh's and ssv-install.sh's uselib_ours:
/// its record is not older than the file (one the user made later is not taken for install.sh's). nil: no file.
func useLibraryMadeByInstall() -> Bool? {
    let fm = FileManager.default
    func mtime(_ p: String) -> Date? { (try? fm.attributesOfItem(atPath: p))?[.modificationDate] as? Date }
    guard let flag = mtime(useLibraryFlag) else { return nil }
    var record = useLibraryRecord
    if !fm.fileExists(atPath: record) {
        let bk = "/Library/Application Support/intel-gpu-macos-driver/last-backup"
        guard let last = (try? String(contentsOfFile: bk, encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines),
              !last.isEmpty else { return false }
        record = last + "/created-use-library"
    }
    guard let made = mtime(record) else { return false }
    return Int(flag.timeIntervalSince1970) <= Int(made.timeIntervalSince1970)
}

struct SSVStatus {
    var installed = false             // the bundle is on the booted system volume
    var recordedBuild: String?        // the macOS build it was installed for (ssv-install.sh's record)
    var currentBuild = ""
    var useLibrary = false            // NMXAccel told to take the /Library copy anyway
    var useLibraryByInstall = false   // install.sh --apply made that file (for a copy of another release), not the user
    var pending = false               // this package's copy waits in a new boot snapshot (ssv-install.sh --apply): restart
    var shadowed = false              // /Library/GPUBundles/NVMTLDriver.bundle: Metal finds it first
    var mesaMatchesLibrary: Bool?     // its Mesa is the one in /Library (same release)
    var matchesPackage: Bool?         // its plugin and Mesa are the chosen package's
    var loadedFrom: String?           // NMXAccel's NMXMetalPluginFrom
    var packageScript: URL?           // the package's ssv-install.sh, when the package has one

    enum State { case absent, gone, otherRelease, notUsed, shadowed, inUse, notPackage, pending }
    var state: State {
        if pending { return .pending }
        if !installed { return recordedBuild != nil ? .gone : .absent }
        if shadowed { return .shadowed }
        if mesaMatchesLibrary == false { return .otherRelease }
        if useLibrary { return .notUsed }
        // in use and matching /Library's Mesa, but not the chosen package's build (a newer or an older one)
        if matchesPackage == false { return .notPackage }
        return .inUse
    }

    static func read(package: Package?) -> SSVStatus {
        let fm = FileManager.default
        var s = SSVStatus()
        s.installed = fm.fileExists(atPath: ssvBundle + "/Contents/MacOS/NVMTLDriver")
        s.recordedBuild = (NSDictionary(contentsOfFile: ssvRecord)?["build"] as? String)
        s.currentBuild = sysctlString("kern.osversion") ?? ""
        s.useLibrary = fm.fileExists(atPath: useLibraryFlag)
        s.useLibraryByInstall = useLibraryMadeByInstall() == true
        s.shadowed = fm.fileExists(atPath: "/Library/GPUBundles/NVMTLDriver.bundle")
        if let package {
            let script = package.directory.appendingPathComponent("system-volume/ssv-install.sh")
            if fm.isExecutableFile(atPath: script.path) { s.packageScript = script }
        }
        let mesa = "/Contents/Frameworks/nvmtl/libvulkan_intel.dylib", plugin = "/Contents/MacOS/NVMTLDriver"
        let ssvMesa = s.installed ? fileHash(ssvBundle + mesa) : nil, libMesa = fileHash("/Library/GPUBundles/nvmtl/libvulkan_intel.dylib")
        // ssv-install.sh (0.1.6 and later) records the Mesa it installed: the /Library part's, on this macOS build, but not
        // the booted copy's means the new snapshot is not booted yet
        if let recorded = NSDictionary(contentsOfFile: ssvRecord)?["mesa"] as? String, let lib = libMesa {
            s.pending = recorded == lib && ssvMesa != recorded && s.recordedBuild == s.currentBuild
        }
        guard s.installed else { return s }
        if let a = ssvMesa, let b = libMesa { s.mesaMatchesLibrary = a == b }
        if let script = s.packageScript {
            let layout = script.deletingLastPathComponent().appendingPathComponent("system-layout/NVMTLDriver.bundle").path
            if let a = ssvMesa, let b = fileHash(layout + mesa), let c = fileHash(ssvBundle + plugin), let d = fileHash(layout + plugin) {
                s.matchesPackage = a == b && c == d
            }
        }
        if let out = commandOutput("/usr/sbin/ioreg", ["-r", "-c", "NMXAccel", "-d", "1", "-w0"]),
           let line = out.split(separator: "\n").first(where: { $0.contains("\"NMXMetalPluginFrom\"") }),
           let value = line.split(separator: "=", maxSplits: 1).last {
            s.loadedFrom = value.trimmingCharacters(in: CharacterSet(charactersIn: " \""))
        }
        return s
    }
}

private func fileHash(_ path: String) -> String? {
    guard FileManager.default.fileExists(atPath: path), let out = commandOutput("/usr/bin/shasum", ["-a", "256", path]) else { return nil }
    return out.split(separator: " ").first.map(String.init)
}

enum SSVAction: String, Identifiable {
    case preflight, install, use, uninstall
    var id: String { rawValue }
    var title: String {
        switch self {
        case .preflight: return String(localized: "Check the requirements")
        case .install: return String(localized: "Install the system-volume copy")
        case .use: return String(localized: "Use the system-volume copy")
        case .uninstall: return String(localized: "Remove the system-volume copy")
        }
    }
    var explanation: String {
        switch self {
        case .preflight: return String(localized: "Reads SIP, AMFI, FileVault, the boot snapshot and the free space. Changes nothing.")
        case .install: return String(localized: "Mounts the system volume, copies this package's Metal driver into /System/Library/Extensions and makes a new boot snapshot, as OpenCore Legacy Patcher does. The current snapshot stays as the way back. Then restart. A macOS update removes the copy again: install it once more after each update.")
        case .use: return String(localized: "The system-volume copy is this package's, but the driver is told to load the /Library copy. This removes that setting. Then restart.")
        case .uninstall: return String(localized: "Boots the last sealed snapshot of macOS again. That takes back EVERY change made to the system volume since the last macOS update: this copy, and any other root patch (OpenCore Legacy Patcher's too). The /Library copy of the driver is used from then on. Then restart.")
        }
    }
}

extension Manager {
    func performSSV(_ action: SSVAction) {
        guard let script = ssv.packageScript else { return }
        let dir = shellQuoted(script.deletingLastPathComponent().path)
        let command: String
        switch action {
        case .preflight: command = "cd \(dir) && ./ssv-install.sh --preflight"
        // after a successful --apply the copy is this package's (its preflight refuses another Mesa than /Library's):
        // the /Library override install.sh may have made for an older copy is no longer wanted. ssv-install.sh 0.1.6
        // removes install.sh's file itself; this also covers older packages' scripts, and a file made by hand (the
        // user asked the app for the system-volume copy).
        // A /Library/GPUBundles/NVMTLDriver.bundle (ssv-install.sh --uninstall leaves one for older NMXAccel builds) would
        // hide the system copy, and the script refuses while it is there: install.sh --apply removes it the same way, when
        // NVMTLDriverLib.bundle, the copy NMXAccel falls back to, is in place.
        case .install: command = "cd \(dir) && { [ ! -d /Library/GPUBundles/NVMTLDriverLib.bundle ] || rm -rf /Library/GPUBundles/NVMTLDriver.bundle; } && ./ssv-install.sh --apply && rm -f \(shellQuoted(useLibraryFlag))"
        case .use: command = "rm -f \(shellQuoted(useLibraryFlag))"
        case .uninstall: command = "cd \(dir) && ./ssv-install.sh --uninstall"
        }
        perform(title: action.title, command: command, needsRoot: action != .preflight, then: { result in
            if result.cancelled { self.lastResult = String(localized: "Cancelled. Nothing changed."); return }
            switch action {
            case .preflight:
                self.lastResult = result.succeeded ? String(localized: "All requirements are met.") : String(localized: "Some requirements are not met: the log says which.")
            default:
                self.lastResult = result.succeeded ? String(localized: "Finished. Restart to use the change.") : String(localized: "It did not finish. See the log.")
                if result.succeeded { self.rebootNeeded = true }
            }
        })
    }
}

final class SSVPageState: ObservableObject {
    @Published var confirming: SSVAction?
    @Published var loginCheck = loginCheckEnabled()
}

/// The Driver page's section for the system-volume copy.
struct SSVSection: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = SSVPageState()

    var body: some View {
        let s = manager.ssv
        Section2(title: "Safari and WebKit", symbol: "safari") {
            Caption("Safari on the GPU needs a second copy of the Metal driver on the system volume.",
                    detail: "Safari draws its pages in a process that may load code only from the system volume. To use the GPU there, the Metal driver needs a second copy on the system volume; without it Safari works, drawing in software.")
            LabeledContent("System-volume copy") {
                Label(stateText(s), systemImage: symbol(s.state)).foregroundStyle(color(s.state))
                    .multilineTextAlignment(.trailing)
            }
            if s.installed, let from = s.loadedFrom {
                LabeledContent("Metal loads the driver from") { Text(from).foregroundStyle(.secondary) }
            }
            let installable = s.state != .inUse && s.state != .notUsed && s.state != .pending
            if s.packageScript != nil && installable {
                LabeledContent("OpenCore's Apple Secure Boot") {
                    HStack(spacing: 8) {
                        Label(secureBootText, systemImage: secureBootOK == true ? "checkmark.circle.fill" : secureBootOK == false ? "exclamationmark.triangle.fill" : "questionmark.circle")
                            .foregroundStyle(secureBootOK == true ? Color.green : secureBootOK == false ? Color.orange : Color.secondary)
                            .multilineTextAlignment(.trailing)
                        if secureBootOK != true { Button("Open the EFI page") { manager.requestedSection = .efi }.fixedSize() }
                    }
                }
            }
            if s.installed || s.recordedBuild != nil {
                CaptionedToggle(title: "Tell me at login when a macOS update removed it", caption: "A macOS update replaces the system volume, and this copy with it.",
                                detail: "A macOS update replaces the system volume, and this copy with it; Safari then draws in software again until it is installed once more. The same check as Check the driver at login on the Power and app page.",
                                isOn: Binding(get: { page.loginCheck }, set: { setLoginCheck($0); page.loginCheck = loginCheckEnabled() }))
            }
            if s.packageScript == nil {
                Text(manager.package == nil ? String(localized: "Choose a package above: the system-volume copy comes from it.")
                                            : String(localized: "This package has no system-volume copy."))
                    .foregroundStyle(.secondary)
            } else {
                HStack(spacing: 10) {
                    Button(SSVAction.preflight.title) { manager.performSSV(.preflight) }
                    Spacer()
                    // accent only when the driver itself is installed as the package has it: that comes first
                    if s.state == .notUsed {
                        Button(SSVAction.use.title) { page.confirming = .use }.buttonStyle(ProminentUnless(plain: manager.installRecommended))
                    }
                    // a changed seal does not boot with Apple Secure Boot on: install only once the EFI says it is off
                    if s.state == .notPackage {
                        Button(String(localized: "Install this package's copy")) { page.confirming = .install }.disabled(secureBootOK != true)
                    } else if installable {
                        Button(s.installed || s.recordedBuild != nil ? String(localized: "Install this package's copy") : SSVAction.install.title) { page.confirming = .install }
                            .buttonStyle(ProminentUnless(plain: manager.installRecommended)).disabled(secureBootOK != true)
                    }
                    if s.installed {
                        Button(String(localized: "Remove…"), role: .destructive) { page.confirming = .uninstall }
                            .foregroundStyle(.red)
                    }
                }
            }
        }
        .alert(page.confirming?.title ?? "", isPresented: Binding(get: { page.confirming != nil }, set: { if !$0 { page.confirming = nil } }), presenting: page.confirming) { action in
            Button(action.title, role: action == .uninstall ? .destructive : nil) { manager.performSSV(action) }
            Button("Cancel", role: .cancel) {}
        } message: { action in
            Text(action.explanation)
        }
    }

    /// SecureBootModel in the config.plist of the EFI OpenCore booted from: nil when it could not be read (not mounted)
    var secureBootOK: Bool? {
        let e = manager.efi
        guard e.readable, e.isOpenCore else { return nil }
        return e["secure_boot_model"] == "Disabled"
    }
    var secureBootText: String {
        switch secureBootOK {
        case true?: return String(localized: "Off (SecureBootModel Disabled)")
        case false?: return String(localized: "On (SecureBootModel \(manager.efi["secure_boot_model"].ifEmpty("?"))): macOS would not start; turn it off first")
        case nil: return String(localized: "Not checked: mount the EFI partition")
        }
    }

    func stateText(_ s: SSVStatus) -> String {
        switch s.state {
        case .absent: return String(localized: "Not installed: Safari draws in software")
        case .gone: return String(localized: "Removed by a macOS update (installed for \(s.recordedBuild ?? "?"), now \(s.currentBuild)): install it again")
        case .otherRelease: return s.useLibrary
            ? String(localized: "From another release than the rest of the driver, so the /Library copy is used (NVMTLDriver.use-library): install this package's copy, which removes that file")
            : String(localized: "From another release than the rest of the driver: install this package's copy")
        case .notPackage: return String(localized: "Installed and in use; another build than this package's")
        case .notUsed: return s.useLibraryByInstall
            ? String(localized: "Installed, but the /Library copy is used instead: install.sh made NVMTLDriver.use-library for an older copy, and it is no longer needed")
            : String(localized: "Installed, but the /Library copy is used instead (NVMTLDriver.use-library, made by hand)")
        case .pending: return String(localized: "This package's copy is installed in a new boot snapshot: restart to use it")
        case .shadowed: return String(localized: "Hidden by /Library/GPUBundles/NVMTLDriver.bundle, which Metal finds first: install this package's copy")
        case .inUse: return String(localized: "Installed and in use")
        }
    }
    func symbol(_ state: SSVStatus.State) -> String {
        switch state {
        case .inUse: return "checkmark.circle.fill"
        case .notPackage: return "checkmark.circle"
        case .absent: return "circle.dashed"
        case .pending: return "arrow.clockwise.circle"
        default: return "exclamationmark.triangle.fill"
        }
    }
    func color(_ state: SSVStatus.State) -> Color {
        switch state {
        case .inUse: return .green
        case .absent, .notPackage, .pending: return .secondary
        default: return .orange
        }
    }
}
