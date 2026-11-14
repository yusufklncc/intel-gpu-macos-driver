// SPDX-License-Identifier: MIT
// Problems the app can see by itself at start, the same ones the diagnostics report warns about (collect-macos.sh's
// codes): a kernel panic since the last look, the boot guard holding acceleration off, a system-volume copy of the Metal
// plugin out of step with /Library, a half-done HiDPI switch, a kext loaded at another version than its file, a hybrid
// Intel CPU without HybridPM, HybridPM loaded but off. Each shows as a banner with what to do; a dismissed one stays quiet until it changes.
import Foundation

struct HealthIssue: Identifiable, Hashable {
    let code: String
    let text: String
    /// what changes when the issue is a new one (a panic file's name, a version): a dismissal holds for this key only
    let key: String
    var id: String { code }
}

private func sha256Prefix(_ path: String) -> String? {
    guard let out = commandOutput("/usr/bin/shasum", ["-a", "256", path]) else { return nil }
    let s = out.split(separator: " ").first.map(String.init) ?? ""
    return s.count >= 16 ? String(s.prefix(16)) : nil
}

private func plistValue(_ path: String, _ key: String) -> String? {
    guard let d = NSDictionary(contentsOfFile: path) else { return nil }
    return d[key] as? String
}

/// 12th-14th gen Core with efficiency cores, by the CPU's name (machdep.cpu.model is what OpenCore's Cpuid1Data makes it):
/// i3/i5/i7/i9-12xxx..14xxx, without Core Ultra (HybridPM leaves it alone) and the desktop models with P-cores only. The
/// same list as efi.sh and the diagnostics report.
func hybridCPU(_ brand: String) -> Bool {
    brand.range(of: #"\bi[3579]-1[2-4]([0-9]{3}|[0-9]{2}[UP])"#, options: .regularExpression) != nil
        && brand.range(of: #"Ultra|i3-1[234]100|i3-12300|i5-12400|i5-12490|i5-12500(?!H)|i5-12600(?![KH])"#, options: .regularExpression) == nil
}

func healthIssues(_ facts: SystemFacts) -> [HealthIssue] {
    var list: [HealthIssue] = []
    let fm = FileManager.default
    // a kernel panic report newer than the boot before this one (macOS writes it at the boot after the panic)
    let dr = "/Library/Logs/DiagnosticReports"
    var newest: (String, Date)? = nil
    for dir in [dr, dr + "/Retired"] {
        for name in (try? fm.contentsOfDirectory(atPath: dir)) ?? [] where name.hasSuffix(".panic") && !name.hasPrefix(".") {
            let p = dir + "/" + name
            if let m = (try? fm.attributesOfItem(atPath: p))?[.modificationDate] as? Date, m > (newest?.1 ?? .distantPast) { newest = (name, m) }
        }
    }
    if let (name, date) = newest, Date().timeIntervalSince(date) < 7 * 86400 {
        let f = DateFormatter(); f.dateStyle = .medium; f.timeStyle = .short
        list.append(HealthIssue(code: "panics", text: String(localized: "macOS restarted after a kernel panic (\(f.string(from: date))). A diagnostics report holds its text."), key: name))
    }
    if facts.bootGuardOff {
        list.append(HealthIssue(code: "accel-off", text: facts.bootGuard.firmwareDisplay
            ? String(localized: "The boot guard holds acceleration off: earlier boots did not reach a healthy desktop, and this one runs on the firmware framebuffer. Clear it once you know why.")
            : String(localized: "The boot guard holds acceleration off: earlier boots did not reach a healthy desktop. Clear it once you know why."), key: "on"))
    }
    let ssv = "/System/Library/Extensions/NVMTLDriver.bundle", mark = "/Library/GPUBundles/nvmtl/ssv-component.plist"
    let ssvMesa = sha256Prefix(ssv + "/Contents/Frameworks/nvmtl/libvulkan_intel.dylib")
    let libMesa = sha256Prefix("/Library/GPUBundles/nvmtl/libvulkan_intel.dylib")
    // this package's copy waits in a new boot snapshot (ssv-install.sh --apply records its Mesa): the restart is all
    let pendingMesa = plistValue(mark, "mesa").map { String($0.prefix(16)) }
    let ssvPending = pendingMesa != nil && pendingMesa == libMesa && pendingMesa != ssvMesa
    if fm.fileExists(atPath: ssv) && fm.fileExists(atPath: useLibraryFlag) && ssvMesa != nil && ssvMesa == libMesa {
        // the copy is the /Library part's, but NMXAccel is told to load the /Library copy: Safari draws in software
        list.append(HealthIssue(code: "ssv-use-library", text: useLibraryMadeByInstall() == true
            ? String(localized: "The system-volume copy of the Metal driver is this release's, but NVMTLDriver.use-library, which install.sh made for an older copy, still makes every process use the /Library copy, and Safari draws in software. Use the system-volume copy on the Driver page, then restart.")
            : String(localized: "The system-volume copy of the Metal driver is this release's, but /Library/GPUBundles/NVMTLDriver.use-library makes every process use the /Library copy, and Safari draws in software. Remove it on the Driver page (Use the system-volume copy) if you did not make it on purpose."), key: ssvMesa ?? "use"))
    } else if ssvPending {
        // nothing to do but restart (the app's restart banner says so after its own install)
    } else if fm.fileExists(atPath: ssv) && !fm.fileExists(atPath: useLibraryFlag) {
        if let a = ssvMesa, let b = libMesa, a != b {
            list.append(HealthIssue(code: "ssv-mesa", text: String(localized: "The system-volume copy of the Metal driver is older than the rest of the install, and WindowServer uses it. Install the package's system-volume copy too (system-volume/ssv-install.sh --apply) and restart."), key: a + b))
        }
    } else if !fm.fileExists(atPath: ssv) && fm.fileExists(atPath: mark) {
        list.append(HealthIssue(code: "ssv-gone", text: String(localized: "The system-volume copy of the Metal driver is gone (a macOS update replaces the system volume). Install it again, or use the /Library copy."), key: plistValue(mark, "build") ?? "gone"))
    }
    let nv = commandOutput("/usr/sbin/nvram", ["intelxefb-hidpi"]) ?? ""
    let hidpiOn = nv.contains("\t1")
    let ws = (CFPreferencesCopyValue("DisplayResolutionEnabled" as CFString, "/Library/Preferences/com.apple.windowserver" as CFString,
                                     kCFPreferencesAnyUser, kCFPreferencesCurrentHost) as? Bool) ?? ((NSDictionary(contentsOfFile: "/Library/Preferences/com.apple.windowserver.plist")?["DisplayResolutionEnabled"] as? Bool) ?? false)
    var ours = 0
    if let e = fm.enumerator(atPath: "/Library/Displays/Contents/Resources/Overrides") {
        for case let rel as String in e {
            let p = "/Library/Displays/Contents/Resources/Overrides/" + rel
            if NSDictionary(contentsOfFile: p)?["IntelXeHiDPI"] != nil { ours += 1 }
        }
    }
    if hidpiOn && (ours == 0 || !ws) {
        list.append(HealthIssue(code: "hidpi-half", text: String(localized: "The HiDPI switch is only half on. Turn it on again on the Display page, then restart."), key: "\(ours)\(ws)"))
    } else if !hidpiOn && ours > 0 {
        list.append(HealthIssue(code: "hidpi-stale", text: String(localized: "A HiDPI display override is left without the framebuffer's HiDPI modes. Turn HiDPI off or on again on the Display page."), key: "\(ours)"))
    }
    let loaded = commandOutput("/usr/bin/kmutil", ["showloaded", "--list-only"], seconds: 30) ?? ""
    for (kext, id) in [("IntelXeFB", "io.github.yusufklncc.IntelXeFB"), ("NMXAccel", "com.nullmoth.NMXAccel"), ("NMXAGDC", "com.nullmoth.NMXAGDC")] {
        guard let line = loaded.split(separator: "\n").first(where: { $0.contains(id + " ") }),
              let open = line.firstIndex(of: "("), let close = line[open...].firstIndex(of: ")") else { continue }
        let lv = String(line[line.index(after: open)..<close])
        if let fv = plistValue("/Library/Extensions/\(kext).kext/Contents/Info.plist", "CFBundleShortVersionString"), fv != lv {
            list.append(HealthIssue(code: "kext-version", text: String(localized: "\(kext).kext \(lv) is loaded, but \(fv) is installed. Restart; if it stays, run Install this package again."), key: lv + fv))
            break
        }
    }
    // installed in /Library but not loaded while IntelXe.kext (from the EFI) is: the Auxiliary Kernel Collection was not
    // rebuilt for this macOS (an update), or the restart after an install has not happened yet
    if facts.kextLoaded && fm.fileExists(atPath: "/Library/Extensions/IntelXeFB.kext") && !loaded.contains("io.github.yusufklncc.IntelXeFB ") {
        list.append(HealthIssue(code: "kexts-not-loaded", text: String(localized: "IntelXeFB.kext is installed but not loaded. After a macOS update the Auxiliary Kernel Collection needs rebuilding: install this package again on the Driver page, then restart. Right after an install, just restart."), key: sysctlString("kern.osversion") ?? "on"))
    }
    list += osUpdateIssues(facts)
    let brand = commandOutput("/usr/sbin/sysctl", ["-n", "machdep.cpu.brand_string"]) ?? ""
    if !loaded.lowercased().contains("hybridpm") {
        if hybridCPU(brand) {
            list.append(HealthIssue(code: "cpu-hybridpm", text: String(localized: "This CPU has performance and efficiency cores (12th-14th gen Core) and HybridPM.kext is not loaded: macOS can hold the cores at low clocks, which makes the GPU look slow too. Add HybridPM.kext from the -efi.zip below Lilu in your EFI (the EFI page does it; install guide: Hybrid Intel CPUs)."), key: "on"))
        }
    } else if commandOutput("/usr/sbin/sysctl", ["-n", "debug.hybridpm.enabled"])?.trimmingCharacters(in: .whitespacesAndNewlines) == "0" {
        // loaded but not converting: an unsupported CPU, or a macOS whose XCPM routine it does not find (before 20 s after
        // boot it is still waiting, which the reason says)
        let why = commandOutput("/usr/sbin/sysctl", ["-n", "debug.hybridpm.why"])?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !why.contains("waiting") && !why.contains("routed: on 20 s") {
            list.append(HealthIssue(code: "hybridpm-off", text: String(localized: "HybridPM.kext is loaded but does nothing: \(why.isEmpty ? "–" : why)."), key: why))
        }
    }
    return list
}

/// The macOS update guard's warnings (OSUpdate.swift): a macOS update on offer that the installed driver does not support
/// (its macOS major version differs), and a result of the guard's check after an update that needs the user.
func osUpdateIssues(_ facts: SystemFacts) -> [HealthIssue] {
    guard facts.installedVersion != nil else { return [] }
    var list: [HealthIssue] = []
    let record = NSDictionary(contentsOfFile: softwareUpdatePlist) as? [String: Any] ?? [:]
    if let other = offeredMacOSUpdates(record).first(where: { supportByMajor($0.version, packageMajor: facts.macOSMajor) == .no }) {
        list.append(HealthIssue(code: "osupdate-unsupported", text: String(localized: "\(other.title) is offered, and the installed driver does not support it: after that update the Arc card would show no desktop. Wait for a release that supports it."), key: other.version))
    }
    if let text = try? String(contentsOfFile: osUpdateDirectory + "/result", encoding: .utf8) {
        var v = keyValues(text)
        let result = v["result"] ?? ""
        if ["unsupported", "efi-kext", "gave-up", "refused", "retrying"].contains(result) {
            for (k, value) in v { v["last_" + k] = value }
            list.append(HealthIssue(code: "osupdate-result", text: osUpdateResultText(result, v), key: v["when"] ?? result))
        }
    }
    return list
}
