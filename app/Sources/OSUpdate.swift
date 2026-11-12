// SPDX-License-Identifier: MIT
// The macOS update guard (docs/design/DESIGN-os-update-guard.md). Before an update: the Overview page shows the macOS
// updates Software Update offers (its own record, /Library/Preferences/com.apple.SoftwareUpdate.plist: no network, no
// root; "Check now" asks softwareupdate --list), says whether the installed release supports each (the package's
// install.sh --supports-macos, or its macOS major version), and "Prepare" runs osupdate-guard.sh --prepare as root: it
// records the state, keeps a copy of the package and installs a one-shot LaunchDaemon. After the update that daemon
// checks and repairs what it can, and leaves a result the page shows. Without a preparation the page still notices a
// new macOS build at the next start and shows what osupdate-guard.sh --check finds.
import Foundation
import SwiftUI

let osUpdateDirectory = "\(installerDirectory)/osupdate"
let softwareUpdatePlist = "/Library/Preferences/com.apple.SoftwareUpdate.plist"

/// A macOS update Software Update offers.
struct OfferedUpdate: Identifiable, Hashable {
    let version: String
    var build: String?
    var title: String
    var id: String { version + (build ?? "") }
    var major: Int { Int(version.split(separator: ".").first ?? "") ?? 0 }
}

/// The macOS updates in Software Update's record (its RecommendedUpdates; the identifiers look like
/// MSU_UPDATE_25H123_patch_26.7.2_minor), newest first.
func offeredMacOSUpdates(_ record: [String: Any]) -> [OfferedUpdate] {
    var list: [OfferedUpdate] = []
    for item in (record["RecommendedUpdates"] as? [[String: Any]]) ?? [] {
        let identifier = item["Identifier"] as? String ?? "", name = item["Display Name"] as? String ?? ""
        let msu = (item["MobileSoftwareUpdate"] as? Bool) == true || (item["MobileSoftwareUpdate"] as? Int) == 1
        guard msu || identifier.hasPrefix("MSU_UPDATE_") || name.hasPrefix("macOS") else { continue }
        var build: String?, version = item["Display Version"] as? String
        if let m = identifier.firstMatch(of: #/^MSU_UPDATE_([0-9A-Za-z]+)_patch_([0-9.]+)_/#) {
            build = String(m.1); version = version ?? String(m.2)
        }
        guard let version, Int(version.split(separator: ".").first ?? "") != nil else { continue }
        list.append(OfferedUpdate(version: version, build: build, title: name.isEmpty ? "macOS \(version)" : name))
    }
    return list.sorted { Version(padded: $0.version) > Version(padded: $1.version) }
}

/// softwareupdate --list: "* Label: macOS Tahoe 26.7.2-25H123" followed by "Title: macOS Tahoe 26.7.2, Version: 26.7.2, ...".
func parseSoftwareUpdateList(_ text: String) -> [OfferedUpdate] {
    var list: [OfferedUpdate] = [], label: String?
    for line in text.split(separator: "\n").map({ $0.trimmingCharacters(in: .whitespaces) }) {
        if line.hasPrefix("* Label: ") { label = String(line.dropFirst(9)); continue }
        guard let m = line.firstMatch(of: #/^Title: (macOS[^,]*), Version: ([0-9.]+)/#) else { continue }
        var build: String?
        if let label, let dash = label.lastIndex(of: "-") { build = String(label[label.index(after: dash)...]) }
        list.append(OfferedUpdate(version: String(m.2), build: build, title: String(m.1)))
        label = nil
    }
    return list
}

extension Version {
    /// "26.7" as 26.7.0, "26" as 26.0.0; anything else as 0.0.0
    init(padded text: String) {
        var parts = text.split(separator: ".").map(String.init)
        while parts.count < 3 { parts.append("0") }
        self = Version(parts.prefix(3).joined(separator: ".")) ?? Version("0.0.0")!
    }
}

enum MacOSSupport: String { case ok, untested, no }

/// Whether a package supports a macOS version: its install.sh says (--supports-macos, from 0.1.5); an earlier package
/// supports its own major version only.
func macOSSupport(_ version: String, package: Package?, packageMajor: Int?) -> MacOSSupport {
    if let package, let text = try? String(contentsOf: package.script, encoding: .utf8), text.contains("--supports-macos)"),
       let out = commandOutput(package.script.path, ["--supports-macos", version]),
       let answer = MacOSSupport(rawValue: out.trimmingCharacters(in: .whitespacesAndNewlines)) {
        return answer
    }
    return supportByMajor(version, packageMajor: packageMajor)
}

func supportByMajor(_ version: String, packageMajor: Int?) -> MacOSSupport {
    guard let packageMajor, let major = Int(version.split(separator: ".").first ?? "") else { return .no }
    return major == packageMajor ? .ok : .no
}

/// osupdate-guard.sh: the copy the app carries (its --prepare installs a root-owned copy for the daemon).
func osGuardScript() -> URL? { Bundle.main.url(forResource: "osupdate-guard", withExtension: "sh") }

/// "key=value" lines.
func keyValues(_ text: String) -> [String: String] {
    var values: [String: String] = [:]
    for line in text.split(separator: "\n") {
        let pair = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
        if pair.count == 2 && values[pair[0]] == nil { values[pair[0]] = pair[1] }
    }
    return values
}

struct OSUpdateState {
    var offered: [OfferedUpdate] = []
    var support: [String: MacOSSupport] = [:]   // by version
    var autoInstall = false                     // Software Update installs macOS updates by itself
    var guardStatus: [String: String] = [:]     // osupdate-guard.sh --status
    var checking = false
    var checkError: String?
    /// a new macOS build since the app last ran, without a preparation: (from, to) and osupdate-guard.sh --check
    var unprepared: (from: String, to: String)?
    var afterCheck: [String: String] = [:]

    var prepared: Bool { guardStatus["prepared"] == "yes" }
    var result: String? { guardStatus["last_result"].flatMap { $0.isEmpty ? nil : $0 } }
    var resultKey: String { guardStatus["last_when"] ?? "" }

    static func read(package: Package?, packageMajor: Int?) -> OSUpdateState {
        var s = OSUpdateState()
        let record = NSDictionary(contentsOfFile: softwareUpdatePlist) as? [String: Any] ?? [:]
        s.autoInstall = (record["AutomaticallyInstallMacOSUpdates"] as? Bool) == true
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let current = Version(padded: "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)")
        s.offered = offeredMacOSUpdates(record).filter { Version(padded: $0.version) > current }
        for o in s.offered { s.support[o.version] = macOSSupport(o.version, package: package, packageMajor: packageMajor) }
        if let script = osGuardScript(), let out = commandOutput("/bin/bash", [script.path, "--status"]) { s.guardStatus = keyValues(out) }
        return s
    }
}

/// The build macOS runs now (kern.osversion), and the one the app saw at its last start.
let lastSeenBuildKey = "lastSeenMacOSBuild"

extension Manager {
    /// The installed release's package is chosen: what Prepare keeps.
    var packageIsInstalled: Bool {
        guard let installed = facts.installedVersion, let v = packageStatus["package_version"] else { return false }
        return installed == v
    }

    func refreshOSUpdate() async {
        let chosen = package, major = Int(packageStatus["package_macos"] ?? "") ?? (facts.macOSMajor > 0 ? facts.macOSMajor : nil)
        let checking = osUpdate.checking, listed = osUpdate.offered
        var s = await Task.detached { OSUpdateState.read(package: chosen, packageMajor: major) }.value
        if checking || s.offered.isEmpty && !listed.isEmpty {   // keep what Check now found
            s.offered = listed
            for o in listed where s.support[o.version] == nil { s.support[o.version] = await Task.detached { macOSSupport(o.version, package: chosen, packageMajor: major) }.value }
        }
        // a new build since the last start that no preparation covered: show what --check finds
        let build = sysctlString("kern.osversion") ?? "", defaults = UserDefaults.standard
        let seen = defaults.string(forKey: lastSeenBuildKey)
        let covered = s.guardStatus["last_to"]?.contains("(\(build))") == true || s.prepared
        if seen == nil || seen == build || covered || !driverInstalled {
            if !build.isEmpty && (seen == nil || seen == build || !driverInstalled) {
                defaults.set(build, forKey: lastSeenBuildKey); defaults.set(facts.macOS, forKey: lastSeenBuildKey + "Version")
            }
        } else if let seen {
            s.unprepared = (from: "\(defaults.string(forKey: lastSeenBuildKey + "Version") ?? "?") (\(seen))", to: "\(facts.macOS) (\(build))")
            if let script = osGuardScript(), let out = await Task.detached(operation: { commandOutput("/bin/bash", [script.path, "--check"], seconds: 60) }).value {
                s.afterCheck = keyValues(out)
            }
        }
        s.checking = osUpdate.checking
        s.checkError = osUpdate.checkError
        osUpdate = s
    }

    /// The unprepared update has been looked at: the next start compares with this build.
    func acknowledgeUpdate() {
        UserDefaults.standard.set(sysctlString("kern.osversion") ?? "", forKey: lastSeenBuildKey)
        UserDefaults.standard.set(facts.macOS, forKey: lastSeenBuildKey + "Version")
        UserDefaults.standard.set(osUpdate.resultKey, forKey: "osUpdateResultSeen")
        osUpdate.unprepared = nil
        osUpdate.guardStatus["last_result"] = nil
        Task { await refresh() }
    }

    var updateResultSeen: Bool { UserDefaults.standard.string(forKey: "osUpdateResultSeen") == osUpdate.resultKey }

    /// softwareupdate --list (asks Apple's servers; tens of seconds).
    func checkSoftwareUpdate() async {
        osUpdate.checking = true; osUpdate.checkError = nil
        let out = await Task.detached { commandOutput("/usr/sbin/softwareupdate", ["--list"], seconds: 180) }.value
        osUpdate.checking = false
        guard let out else { osUpdate.checkError = String(localized: "softwareupdate did not answer."); return }
        let found = parseSoftwareUpdateList(out)
        let current = Version(padded: facts.macOS)
        osUpdate.offered = found.filter { Version(padded: $0.version) > current }
        await refreshOSUpdate()
    }

    func prepareForUpdate(_ target: OfferedUpdate?, restart: Bool) {
        guard let script = osGuardScript(), let package else { return }
        var words = [script.path, "--prepare", package.directory.path]
        if let target { words.append(target.version) }
        if !restart { words.append("--no-restart") }
        perform(title: String(localized: "Prepare for the macOS update"), command: "/bin/bash " + words.map(shellQuoted).joined(separator: " "), needsRoot: true, then: { result in
            if result.cancelled { self.lastResult = String(localized: "Cancelled. Nothing changed.") }
            else if result.succeeded { self.lastResult = String(localized: "Prepared. Install the update when you like: the first start of the new macOS checks the driver.") }
            else { self.lastResult = String(localized: "It did not finish. See the log.") }
        }, afterRefresh: { Task { await self.refreshOSUpdate() } })
    }

    func cancelUpdatePreparation() {
        guard let script = osGuardScript() else { return }
        perform(title: String(localized: "Cancel the update preparation"), command: "/bin/bash \(shellQuoted(script.path)) --cancel", needsRoot: true, then: { result in
            self.lastResult = result.cancelled ? String(localized: "Cancelled. Nothing changed.") : result.succeeded ? String(localized: "The preparation is removed.") : String(localized: "It did not finish. See the log.")
        }, afterRefresh: { Task { await self.refreshOSUpdate() } })
    }
}

/// What a result of the daemon means, in a line.
func osUpdateResultText(_ result: String, _ s: [String: String]) -> String {
    let to = s["last_to"] ?? "?", from = s["last_from"] ?? "?"
    switch result {
    case "ok": return String(localized: "macOS was updated from \(from) to \(to). The driver works on it.")
    case "repaired": return String(localized: "macOS was updated to \(to). The driver was installed again; the next start checks that it loads.")
    case "guard-cleared": return String(localized: "macOS was updated to \(to). The update's restarts had set the boot guard; it was cleared once.")
    case "retrying": return String(localized: "macOS was updated to \(to). Installing the driver again failed; the next start tries again.")
    case "unsupported": return String(localized: "macOS was updated to \(to), which release \(s["last_package"] ?? "?") does not support. Nothing was installed: install a release that supports it.")
    case "efi-kext": return String(localized: "macOS was updated to \(to), and IntelXe.kext from the EFI did not load. Check the EFI page.")
    case "gave-up": return String(localized: "macOS was updated to \(to). Three tries did not bring the driver back: see the Report page.")
    case "refused": return String(localized: "macOS was updated to \(to). The kept package was damaged, so nothing was installed: install the package again on the Driver page.")
    case "expired": return String(localized: "The preparation for a macOS update was removed: no update came within 14 days.")
    default: return result
    }
}

/// The Overview page's section: the offered updates, the preparation, and what the last update left.
struct OSUpdateSection: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = OSUpdatePage()

    var body: some View {
        let u = manager.osUpdate
        Section2(title: "macOS updates", symbol: "arrow.down.app") {
            if let r = u.result, !manager.updateResultSeen {
                let good = r == "ok" || r == "repaired" || r == "guard-cleared" || r == "expired"
                HStack(alignment: .firstTextBaseline) {
                    Label(osUpdateResultText(r, u.guardStatus), systemImage: good ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(good ? Color.green : Color.orange)
                    Spacer()
                    Button("Done") { manager.acknowledgeUpdate() }.controlSize(.small)
                }
                if u.guardStatus["last_ssv"] == "gone" {
                    HStack {
                        Text("The system-volume copy (Safari on the GPU) is gone with the update.").font(.callout).foregroundStyle(.secondary)
                        Spacer()
                        Button("Open the Driver page") { manager.requestedSection = .driver }.controlSize(.small)
                    }
                }
                if r == "repaired" || r == "guard-cleared" { Text("Restart to finish (the guard may already have restarted once by itself).").font(.caption).foregroundStyle(.secondary) }
            }
            if let up = u.unprepared {
                AfterUpdateRows(from: up.from, to: up.to, check: u.afterCheck)
            }
            if u.prepared {
                let date = Double(u.guardStatus["state_prepared_at"] ?? "").map { Date(timeIntervalSince1970: $0).formatted(date: .abbreviated, time: .shortened) } ?? "?"
                HStack(alignment: .firstTextBaseline) {
                    Label(String(localized: "Prepared on \(date) (from macOS \(u.guardStatus["state_from_version"] ?? "?")): the first start of the new macOS checks the driver and repairs what it can."),
                          systemImage: "checkmark.shield").foregroundStyle(.secondary)
                    Spacer()
                    Button("Cancel the preparation") { manager.cancelUpdatePreparation() }.controlSize(.small)
                }
            }
            if u.offered.isEmpty {
                HStack {
                    Text(u.checking ? String(localized: "Asking Software Update…") : u.checkError ?? String(localized: "Software Update offers no macOS update.")).foregroundStyle(.secondary)
                    Spacer()
                    Button("Check now") { Task { await manager.checkSoftwareUpdate() } }.controlSize(.small).disabled(u.checking)
                }
            }
            ForEach(u.offered) { o in
                OfferedUpdateRow(update: o, support: u.support[o.version] ?? .no, prepared: u.prepared, restart: $page.restart)
            }
            if !u.offered.isEmpty && u.autoInstall {
                Text("Software Update installs macOS updates by itself on this Mac: prepare now, or turn that off in System Settings > General > Software Update.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

final class OSUpdatePage: ObservableObject {
    @Published var restart = true
}

struct OfferedUpdateRow: View {
    @EnvironmentObject var manager: Manager
    let update: OfferedUpdate
    let support: MacOSSupport
    let prepared: Bool
    @Binding var restart: Bool
    var body: some View {
        let installed = manager.facts.installedVersion ?? "?"
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Label(support == .no ? String(localized: "\(update.title) is offered.") : String(localized: "\(update.title) is ready to install."), systemImage: support == .no ? "exclamationmark.octagon.fill" : "arrow.down.circle")
                    .foregroundStyle(support == .no ? Color.red : Color.primary)
                Spacer()
                if !prepared && support != .no {
                    Button("Prepare…") { manager.prepareForUpdate(update, restart: restart) }
                        .buttonStyle(.borderedProminent).controlSize(.small).disabled(!manager.packageIsInstalled)
                }
            }
            switch support {
            case .ok:
                Text("Release \(installed) supports it. Prepare keeps a copy of its package and adds a check that runs once at the first start of the new macOS: it reinstalls the driver if the update left it out, and tells you here.")
                    .font(.caption).foregroundStyle(.secondary)
            case .untested:
                Text("Release \(installed) was not tested on macOS \(update.version), but is for this major version. Prepare as for any update; the Report page helps if something goes wrong.")
                    .font(.caption).foregroundStyle(.secondary)
            case .no:
                Text("Release \(installed) does not support macOS \(update.version). After this update macOS would start without the driver: the Arc card would show no desktop. Wait for a release that supports it.")
                    .font(.callout).foregroundStyle(.red)
            }
            if !prepared && support != .no {
                if manager.packageIsInstalled {
                    Toggle("Restart once by itself if the check reinstalled the driver", isOn: $restart).controlSize(.small).font(.caption)
                } else {
                    Text("Choose the installed release's package on the Driver page first (\(installed)): Prepare keeps a copy of it.").font(.caption).foregroundStyle(.orange)
                }
            }
        }
    }
}

/// A new macOS build since the app last ran, with no preparation: what --check finds, and the repair.
struct AfterUpdateRows: View {
    @EnvironmentObject var manager: Manager
    let from: String
    let to: String
    let check: [String: String]
    var body: some View {
        let notLoaded = check["not_loaded"] ?? "", plugin = check["plugin"] ?? "?"
        let broken = !notLoaded.isEmpty || plugin != "present"
        HStack(alignment: .firstTextBaseline) {
            Label(String(localized: "macOS was updated from \(from) to \(to)."), systemImage: broken ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                .foregroundStyle(broken ? Color.orange : Color.green)
            Spacer()
            Button("Done") { manager.acknowledgeUpdate() }.controlSize(.small)
        }
        if !notLoaded.isEmpty {
            Text("Installed but not loaded: \(notLoaded). The Auxiliary Kernel Collection needs rebuilding for this macOS: install the package again, then restart.").font(.callout)
        }
        if plugin != "present" { Text("The Metal driver is missing from /Library/GPUBundles: install the package again.").font(.callout) }
        if check["kext_IntelXe"] == "absent" { Text("IntelXe.kext from the EFI is not loaded: check the EFI page.").font(.callout) }
        if (check["boot_guard"] ?? "").hasPrefix("held") { Text("The boot guard holds acceleration off: clear it above once you know why.").font(.callout) }
        if broken {
            let support = macOSSupport(manager.facts.macOS, package: manager.package, packageMajor: Int(manager.packageStatus["package_macos"] ?? ""))
            HStack {
                Text(support == .no ? String(localized: "The chosen package does not support macOS \(manager.facts.macOS).") : String(localized: "The Driver page's Install this package does it."))
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                if support != .no && manager.package != nil {
                    Button(DriverAction.install.title) { manager.perform(.install) }.controlSize(.small)
                }
            }
        }
    }
}
