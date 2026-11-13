// SPDX-License-Identifier: MIT
// The EFI page's facts and commands. efi.sh decides what changes in the EFI (the package's copy, or the one inside this app
// for packages made before it): the app shows its plan, asks, and runs it with the plan's SHA-256, so that efi.sh writes
// nothing unless the EFI is still in the state the plan was made from.
import CryptoKit
import Foundation

/// Which EFI efi.sh works on: the one OpenCore recorded in NVRAM, a partition chosen by its UUID, or a folder that holds
/// EFI/OC (one the user chose, such as a USB stick's; the tests' fake EFI).
enum EFITarget: Hashable {
    case recorded
    case partition(String)
    case folder(String)
    var arguments: [String] {
        switch self {
        case .recorded: return []
        case .partition(let uuid): return ["--part", uuid]
        case .folder(let path): return ["--efi", path]
        }
    }
}

/// The folder to give efi.sh --efi for a folder the user picked: the picked folder when it holds EFI/OC (a USB stick, a
/// mounted partition), its parent when it is the EFI folder itself (it holds OC); nil for anything else.
func efiFolderRoot(for picked: URL) -> String? {
    let fm = FileManager.default
    var isDir: ObjCBool = false
    func dir(_ url: URL) -> Bool { fm.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue }
    let url = picked.standardizedFileURL
    if dir(url.appendingPathComponent("EFI/OC")) { return url.path }
    let parent = url.deletingLastPathComponent()
    if dir(url.appendingPathComponent("OC")) && dir(parent.appendingPathComponent("EFI/OC")) { return parent.path }
    return nil
}

/// An EFI partition efi.sh found: "candidate=UUID|device|disk|size|mount point|OpenCore".
struct EFICandidate: Hashable, Identifiable {
    let uuid: String
    let device: String
    let disk: String
    let size: String
    let mountPoint: String
    let openCore: String
    var id: String { uuid }
    init?(_ text: String) {
        let fields = text.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard fields.count >= 6, !fields[0].isEmpty else { return nil }
        (uuid, device, disk, size, mountPoint, openCore) = (fields[0], fields[1], fields[2], fields[3], fields[4], fields[5])
    }
    var label: String { "\(disk.isEmpty ? "?" : disk), \(size) (\(device))" }
}

/// efi.sh --status.
struct EFIStatus {
    var values: [String: String] = [:]
    var candidates: [EFICandidate] = []
    init() {}
    init(text: String) {
        for line in text.split(separator: "\n") {
            let pair = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            guard pair.count == 2 else { continue }
            if pair[0] == "candidate" {
                if let candidate = EFICandidate(pair[1]) { candidates.append(candidate) }
            } else {
                values[pair[0]] = pair[1]
            }
        }
    }
    subscript(key: String) -> String { values[key] ?? "" }
    var known: Bool { !values.isEmpty }
    var found: Bool { !["", "none"].contains(self["efi_source"]) }
    var mounted: Bool { self["efi_mounted"] == "yes" }
    var readable: Bool { self["efi_readable"] == "yes" }
    var isOpenCore: Bool { self["opencore"] == "yes" }
    var fileChanges: Int { Int(self["plan_changes"]) ?? 0 }
    var settingsChanges: Int { Int(self["settings_changes"]) ?? 0 }
    var backup: String? { ["", "none"].contains(self["backup"]) ? nil : self["backup"] }
    /// boot logging: on, off, partial, missing (no Misc > Debug), or "" (not read)
    var bootLogging: String { self["boot_logging"] }
    /// HybridPM.kext's rows on the EFI page: for a CPU that needs it, or when the EFI has it
    var showsHybridPM: Bool { self["hybridpm_needed"] == "yes" || ["same", "different"].contains(self["hybridpm_file"]) }
}

/// A cheap fingerprint of what efi.sh reads in an EFI (EFI/OC/config.plist, ACPI, Kexts with their contents, Drivers):
/// paths, sizes and modification times, no file contents. The EFI page compares it to notice changes made outside the
/// app (another tool, a text editor, a second copy of the app) and reads the EFI again. nil when it cannot be read.
func efiFingerprint(root: String) -> String? {
    guard !root.isEmpty else { return nil }
    let oc = URL(fileURLWithPath: root).appendingPathComponent("EFI/OC")
    let fm = FileManager.default
    func stamp(_ url: URL) -> String? {
        guard let a = try? fm.attributesOfItem(atPath: url.path) else { return nil }
        let size = (a[.size] as? NSNumber)?.int64Value ?? -1
        let time = (a[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return "\(url.path.dropFirst(oc.path.count))=\(size)@\(time)"
    }
    guard let config = stamp(oc.appendingPathComponent("config.plist")) else { return nil }
    var parts = [config]
    for sub in ["ACPI", "Kexts", "Drivers"] {
        let dir = oc.appendingPathComponent(sub)
        guard let walk = fm.enumerator(at: dir, includingPropertiesForKeys: nil) else { continue }
        var entries: [String] = []
        for case let url as URL in walk { if let s = stamp(url) { entries.append(s) } }
        parts += entries.sorted()
    }
    return parts.joined(separator: "\n")
}

/// A plan as efi.sh prints it. Its hash goes back to efi.sh with the command.
struct EFIPlan: Hashable {
    let text: String
    var hash: String { SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined() }
    var lines: [String] { text.split(separator: "\n").map(String.init) }
    var changes: Bool { lines.dropFirst().contains { !$0.hasPrefix("note ") && $0 != "nothing to change" } }
}

enum EFIAction: String, Identifiable {
    case mount, apply, settings, safari, settingsSafari, bootLog, bootLogOff, xeprobe, xeprobeOff, undo
    var id: String { rawValue }
    var argument: String {
        switch self {
        case .mount: return "--mount"
        case .apply: return "--apply"
        case .settings: return "--settings-apply"
        case .safari: return "--safari-apply"
        case .settingsSafari: return "--settings-safari-apply"
        case .bootLog: return "--bootlog-apply"
        case .bootLogOff: return "--bootlog-off-apply"
        case .xeprobe: return "--xeprobe-apply"
        case .xeprobeOff: return "--xeprobe-off-apply"
        case .undo: return "--undo"
        }
    }
    /// what the action's efi.sh command takes besides its argument: the GPU's device id for the XeProbe boot-arg
    func extraArguments(probeDevice: String?) -> [String] {
        self == .xeprobe ? ["--probe-device", probeDevice ?? ""] : []
    }
    var planArgument: String? {
        switch self {
        case .mount: return nil
        case .apply: return "--plan"
        case .settings: return "--settings-plan"
        case .safari: return "--safari-plan"
        case .settingsSafari: return "--settings-safari-plan"
        case .bootLog: return "--bootlog-plan"
        case .bootLogOff: return "--bootlog-off-plan"
        case .xeprobe: return "--xeprobe-plan"
        case .xeprobeOff: return "--xeprobe-off-plan"
        case .undo: return "--undo-plan"
        }
    }
}

/// The package's efi.sh, or the copy in this app when the package's is older than it (made before efi.sh, or before
/// the XeProbe boot-arg change and its probe_boot_arg status): the files still come from the package (--package), only the steps
/// are the newer script's. Each efi.sh reads the efi-rules.json beside it (the package's, or the one in this app's
/// Resources), never the other one's.
func efiScript(for package: Package) -> URL? {
    let own = package.directory.appendingPathComponent("efi.sh")
    if let text = try? String(contentsOf: own, encoding: .utf8), text.contains("--xeprobe-off-plan"), text.contains("probe_boot_arg=") { return own }
    return Bundle.main.url(forResource: "efi", withExtension: "sh") ?? (FileManager.default.fileExists(atPath: own.path) ? own : nil)
}

/// efi.sh's command line, as words.
func efiWords(script: URL, package: Package, target: EFITarget, _ argument: String) -> [String] {
    ["/bin/bash", script.path, argument, "--package", package.directory.path] + target.arguments
}

func efiStatus(script: URL, package: Package, target: EFITarget) -> EFIStatus {
    let words = efiWords(script: script, package: package, target: target, "--status")
    return EFIStatus(text: commandOutput(words[0], Array(words.dropFirst()), seconds: 90) ?? "")
}

/// A plan, or nil when efi.sh could not make one (the EFI is not mounted, not OpenCore, ...). `extra`: more arguments
/// (--probe-device for the XeProbe boot-arg).
func efiPlan(script: URL, package: Package, target: EFITarget, _ argument: String, extra: [String] = []) -> EFIPlan? {
    let words = efiWords(script: script, package: package, target: target, argument) + extra
    guard let text = commandOutput(words[0], Array(words.dropFirst()), seconds: 90), text.hasPrefix("EFI: ") else { return nil }
    return EFIPlan(text: text)
}

/// What to say about the EFI after a driver action. Only when the EFI's IntelXe.kext is not this package's, or not on,
/// or could not be read.
enum EFINotice: Equatable {
    case kextDiffers, kextMissing, unchecked, removeKext

    static func after(_ action: DriverAction, status: EFIStatus) -> EFINotice? {
        switch action {
        case .install, .repair:
            switch status["kext_file"] {
            case "same": return ["enabled", "unknown", ""].contains(status["kext_entry"]) ? nil : .kextMissing
            case "different": return .kextDiffers
            case "absent": return .kextMissing
            default: return .unchecked
            }
        case .uninstall:
            return status["kext_entry"] == "enabled" || status["kext_entry"] == "unknown" || !status.known ? .removeKext : nil
        case .check, .undo:
            return nil
        }
    }
}
