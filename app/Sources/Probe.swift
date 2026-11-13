// SPDX-License-Identifier: MIT
// The Probe page: XeProbe (docs/PROBE-MODE.md), the staged probe in IntelXe.kext for an Intel GPU the driver does not
// support yet. The page names the Intel GPUs on this Mac, puts the boot-arg xeprobe=0xDEVICE into the EFI's config.plist
// and takes it out again (efi.sh --xeprobe-plan / --xeprobe-off-plan, shown and confirmed as on the EFI page), runs one
// stage at a time as root (sysctl debug.xeprobe.stage=N, through the password dialog), keeps a record of each run in
// ~/Library/Application Support/IntelXe Manager/xeprobe-history.txt (the results of a boot that hung are only there and in
// the NVRAM breadcrumb), and makes the diagnostics report with the probe's files (collect-macos.sh --probe, with the
// record as XE_PROBE_HISTORY; the report removes private details as always).
import AppKit
import IOKit
import SwiftUI

/// An Intel display-class PCI function on this Mac (class 03, or 0xffffff when an SSDT hid it, as XeProbe::probe takes).
struct IntelGPU: Identifiable, Hashable {
    let device: String   // "0x64a0", as the boot-arg takes it
    let name: String
    var supported: Bool { supportedCards.contains { "0x" + $0.id == device } }
    var id: String { device }
    var label: String { name.isEmpty ? "8086:\(device.dropFirst(2))" : "\(name) (8086:\(device.dropFirst(2)))" }
}

func intelDisplayDevices() -> [IntelGPU] {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &iterator) == KERN_SUCCESS else { return [] }
    defer { IOObjectRelease(iterator) }
    var found: [IntelGPU] = []
    var service = IOIteratorNext(iterator)
    while service != 0 {
        func bytes(_ key: String) -> [UInt8] {
            (IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? Data).map { [UInt8]($0) } ?? []
        }
        let vendor = bytes("vendor-id"), device = bytes("device-id"), cls = bytes("class-code")
        if vendor.count >= 2, device.count >= 2, cls.count >= 3, vendor[0] == 0x86, vendor[1] == 0x80,
           cls[2] == 0x03 || (cls[0] == 0xff && cls[1] == 0xff && cls[2] == 0xff) {
            let model = bytes("model").prefix { $0 != 0 }
            let name = String(decoding: model, as: UTF8.self).trimmingCharacters(in: .whitespaces)
            let id = String(format: "0x%02x%02x", device[1], device[0])
            if !found.contains(where: { $0.device == id }) { found.append(IntelGPU(device: id, name: name)) }
        }
        IOObjectRelease(service)
        service = IOIteratorNext(iterator)
    }
    return found
}

/// One line of debug.xeprobe.results: "stage %d %-9s %-8s %5u ms  %s" (xe_probe.cpp resultsHandler).
struct ProbeStageResult: Hashable {
    let number: Int
    let status: String   // not run, ok, FAILED, refused, STALLED, running
    let ms: Int
    let detail: String
    var passed: Bool { status == "ok" }
}

func parseProbeResults(_ text: String) -> [Int: ProbeStageResult] {
    guard let re = try? NSRegularExpression(pattern: #"^stage (\d) +(\S+) +(not run|ok|FAILED|refused|STALLED|running) +(\d+) ms {1,2}(.*)$"#) else { return [:] }
    var out: [Int: ProbeStageResult] = [:]
    for line in text.split(separator: "\n").map(String.init) {
        let ns = line as NSString
        guard let m = re.firstMatch(in: line, range: NSRange(location: 0, length: ns.length)) else { continue }
        let group = { (i: Int) in ns.substring(with: m.range(at: i)) }
        guard let n = Int(group(1)), let ms = Int(group(4)) else { continue }
        out[n] = ProbeStageResult(number: n, status: group(3), ms: ms, detail: group(5))
    }
    return out
}

/// What the page shows: the Intel GPUs, XeProbe's state this boot (debug.xeprobe.info and .results, nothing when it is
/// not attached), the previous boot's breadcrumb and this boot's boot-args.
struct ProbeStatus {
    var gpus: [IntelGPU] = []
    var info: [String: String] = [:]
    var results = ""
    var log = ""
    var breadcrumb = ""
    var bootArgs = ""
    var historyRuns = 0
    var attached: Bool { info["attached"] == "yes" }
    var stages: [Int: ProbeStageResult] { parseProbeResults(results) }
    /// the device this boot's boot-args name for XeProbe, or nil
    var bootArgDevice: String? {
        bootArgs.split(separator: " ").first { $0.hasPrefix("xeprobe=") }.map { String($0.dropFirst(8)).lowercased() }
    }

    static func read() -> ProbeStatus {
        var s = ProbeStatus()
        s.gpus = intelDisplayDevices()
        func sysctl(_ name: String) -> String { commandOutput("/usr/sbin/sysctl", ["-n", name], seconds: 5) ?? "" }
        for line in sysctl("debug.xeprobe.info").split(separator: "\n") {
            let pair = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            if pair.count == 2 { s.info[pair[0]] = pair[1] }
        }
        if s.attached {
            s.results = sysctl("debug.xeprobe.results").trimmingCharacters(in: CharacterSet(charactersIn: "\0\n"))
            s.log = sysctl("debug.xeprobe.log").trimmingCharacters(in: CharacterSet(charactersIn: "\0\n"))
        }
        s.bootArgs = sysctl("kern.bootargs").trimmingCharacters(in: .whitespacesAndNewlines)
        let crumb = commandOutput("/usr/sbin/nvram", ["xeprobe-crumb"], seconds: 5) ?? ""
        if let tab = crumb.firstIndex(of: "\t") {
            s.breadcrumb = String(crumb[crumb.index(after: tab)...]).replacingOccurrences(of: "%00", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
        }
        s.historyRuns = ((try? String(contentsOf: probeHistoryFile, encoding: .utf8)) ?? "").split(separator: "\n").filter { $0.hasPrefix("== ") }.count
        return s
    }
}

/// The record of the stage runs this app made, over boots: the file collect-macos.sh puts into probe/history.txt.
/// Each run: "== <when> boot <boot time> stage <N> exit <code>" and the results after it (docs/PROBE-MODE.md).
let probeHistoryFile = dataDirectory.appendingPathComponent("xeprobe-history.txt")

func bootTime() -> Date? {
    var tv = timeval()
    var size = MemoryLayout<timeval>.size
    guard sysctlbyname("kern.boottime", &tv, &size, nil, 0) == 0 else { return nil }
    return Date(timeIntervalSince1970: TimeInterval(tv.tv_sec))
}

func appendProbeHistory(when: Date, boot: Date?, stage: Int, exit: Int, results: String) {
    let iso = ISO8601DateFormatter()
    var text = ""
    if !FileManager.default.fileExists(atPath: probeHistoryFile.path) {
        try? FileManager.default.createDirectory(at: dataDirectory, withIntermediateDirectories: true)
        text = "# XeProbe stage runs by IntelXe Manager, newest last (docs/PROBE-MODE.md)\n"
    }
    text += "== \(iso.string(from: when)) boot \(boot.map { iso.string(from: $0) } ?? "unknown") stage \(stage) exit \(exit)\n"
    text += results.trimmingCharacters(in: .whitespacesAndNewlines) + "\n"
    if let handle = try? FileHandle(forWritingTo: probeHistoryFile) {
        handle.seekToEndOfFile(); handle.write(Data(text.utf8)); try? handle.close()
    } else {
        try? text.write(to: probeHistoryFile, atomically: true, encoding: .utf8)
    }
}

/// The stages, what each does and what it risks (docs/PROBE-MODE.md, Stages and Safety).
struct ProbeStage: Identifiable {
    enum Risk { case none, low, medium, cleanup }
    let number: Int
    let name: String
    let what: String
    let risk: Risk
    var id: Int { number }
    var riskText: String {
        switch risk {
        case .none: return String(localized: "No risk: reads only")
        case .low: return String(localized: "Low risk")
        case .medium: return String(localized: "Can freeze or restart the Mac")
        case .cleanup: return String(localized: "Puts back what the stages changed")
        }
    }
    var riskColor: Color { risk == .none || risk == .cleanup ? .green : risk == .low ? .orange : .red }

    static let all: [ProbeStage] = [
        ProbeStage(number: 1, name: "identity", what: String(localized: "Reads how the GPU identifies itself: PCI IDs, IP versions, stolen memory, the GGTT and the screen the firmware lit. It writes nothing (on Lunar Lake and Panther Lake only the dummy writes Linux makes before each read)."), risk: .none),
        ProbeStage(number: 2, name: "forcewake", what: String(localized: "Wakes the GPU's graphics tile, reads its fuses and engines, and lets it sleep again."), risk: .low),
        ProbeStage(number: 3, name: "guc", what: String(localized: "Loads the GuC firmware: resets the GuC and sets its memory layout, which stays until the next restart. A hang becomes a kernel panic that names the step."), risk: .medium),
        ProbeStage(number: 4, name: "store", what: String(localized: "Gives each engine one small job through the GuC: write one number into memory."), risk: .medium),
        ProbeStage(number: 5, name: "readback", what: String(localized: "Reads those numbers back (and one page of video memory on a graphics card)."), risk: .low),
        ProbeStage(number: 9, name: "release", what: String(localized: "Stops the GuC, puts back what the probe changed in the GPU's address table and frees its memory. Restart afterwards in any case."), risk: .cleanup),
    ]
}

extension Manager {
    func refreshProbe() async {
        // a stage that started in an earlier boot and never finished: the machine hung or panicked there
        if let pending = UserDefaults.standard.string(forKey: "xeprobePending") {
            let parts = pending.split(separator: "|").map(String.init)
            let now = bootTime().map { String(Int($0.timeIntervalSince1970)) } ?? ""
            if parts.count == 3, parts[1] != now, let stage = Int(parts[0]), let started = Double(parts[2]) {
                appendProbeHistory(when: Date(timeIntervalSince1970: started), boot: Double(parts[1]).map { Date(timeIntervalSince1970: $0) },
                                   stage: stage, exit: -1, results: "the Mac did not come back from this stage: no results (the breadcrumb and any panic report are in the diagnostics report)")
                UserDefaults.standard.removeObject(forKey: "xeprobePending")
            }
        }
        probe = await Task.detached { ProbeStatus.read() }.value
        if probeDevice == nil || !probe.gpus.contains(where: { $0.device == probeDevice }) {
            // the boot-arg's device, else the first Intel GPU the driver does not support, else the first one
            probeDevice = probe.bootArgDevice.flatMap { d in probe.gpus.first { $0.device == d } }?.device
                ?? probe.gpus.first { !$0.supported }?.device ?? probe.gpus.first?.device
        }
    }

    func chooseProbeDevice(_ device: String) {
        probeDevice = device
        UserDefaults.standard.set(device, forKey: "probeDevice")
        Task { await refreshEFI() }
    }

    /// One stage as root. The breadcrumb switch is set first; sysctl's exit status goes into the log as "xeprobe-exit=N"
    /// (the run itself always ends with 0, so that a refused stage is not taken for a cancelled password dialog).
    func runProbeStage(_ stage: Int, breadcrumb: Bool) {
        let started = Date(), boot = bootTime()
        UserDefaults.standard.set("\(stage)|\(boot.map { String(Int($0.timeIntervalSince1970)) } ?? "")|\(Int(started.timeIntervalSince1970))", forKey: "xeprobePending")
        let command = "/usr/sbin/sysctl -w debug.xeprobe.nvram=\(breadcrumb ? 1 : 0) > /dev/null; /usr/sbin/sysctl -w debug.xeprobe.stage=\(stage); "
            + "rc=$?; echo \"xeprobe-exit=$rc\"; /usr/sbin/sysctl -n debug.xeprobe.results; exit 0"
        let title = String(localized: "XeProbe stage \(stage)")
        let start = log.count
        perform(title: title, command: command, needsRoot: true, then: { result in
            UserDefaults.standard.removeObject(forKey: "xeprobePending")
            if result.cancelled { self.lastResult = String(localized: "Cancelled. Nothing ran."); return }
            guard result.succeeded else { self.lastResult = String(localized: "It did not finish. See the log."); return }
            let output = String(self.log.dropFirst(start))
            let exit = output.split(separator: "\n").last { $0.hasPrefix("xeprobe-exit=") }.flatMap { Int($0.dropFirst(13)) } ?? -1
            let results = commandOutput("/usr/sbin/sysctl", ["-n", "debug.xeprobe.results"], seconds: 5)?
                .trimmingCharacters(in: CharacterSet(charactersIn: "\0\n")) ?? ""
            appendProbeHistory(when: started, boot: boot, stage: stage, exit: exit, results: results)
            let r = parseProbeResults(results)[stage]
            self.lastResult = r?.passed == true ? String(localized: "Stage \(stage) passed.")
                : String(localized: "Stage \(stage): \(r?.status ?? "no result"). See the results below.")
        })
    }
}

final class ProbePageState: ObservableObject {
    @Published var confirming: EFIAction?
    @Published var risky: Int?
}

struct ProbeView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = ProbePageState()
    @AppStorage("xeprobeBreadcrumb") var breadcrumb = false
    /// false for --snapshot, whose made-up state the page's own read of this Mac would replace
    var refreshes = true
    var probe: ProbeStatus { manager.probe }
    var device: String? { manager.probeDevice }

    var body: some View {
        Form {
            Section2(title: "Staged probe (XeProbe)", symbol: "stethoscope") {
                Caption("For an Intel GPU the driver does not support yet: test it one step at a time and send us the results.",
                        detail: "XeProbe is part of IntelXe.kext. It attaches only to the GPU the boot-arg xeprobe= names, and does nothing until you run a stage. Each stage needs the ones before it. Stages 1, 2 and 5 only read or barely touch the GPU; stages 3 and 4 load firmware and run commands on it, and can freeze or restart the Mac. Before you start, keep a working EFI on a USB stick. Afterwards, restart, and make a diagnostics report with the results.")
                if probe.gpus.isEmpty {
                    Text("No Intel GPU found on this Mac.").foregroundStyle(.secondary)
                } else {
                    Picker("GPU", selection: Binding(get: { device ?? "" }, set: { manager.chooseProbeDevice($0) })) {
                        ForEach(probe.gpus) { gpu in
                            Text(gpu.supported ? String(localized: "\(gpu.label), supported by the driver") : gpu.label).tag(gpu.device)
                        }
                    }
                }
                if let gpu = probe.gpus.first(where: { $0.device == device }), gpu.supported {
                    Text("The driver supports this GPU: the probe refuses stages 2 to 5 while IntelXe drives it.").font(.caption).foregroundStyle(.secondary)
                }
            }
            bootArg
            stages
            results
            if let result = manager.lastResult {
                Text(result).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .task { if refreshes { await manager.refreshProbe() } }
        .sheet(item: $page.confirming) { action in
            EFIConfirmSheet(action: action, plan: manager.efiPlans[action], disk: manager.efi["efi_description"]) { manager.performEFI(action) }
        }
        .alert(String(localized: "This stage can freeze or restart the Mac"), isPresented: Binding(get: { page.risky != nil }, set: { if !$0 { page.risky = nil } })) {
            Button("Cancel", role: .cancel) { page.risky = nil }
            Button(String(localized: "Run it")) {
                if let n = page.risky { manager.runProbeStage(n, breadcrumb: breadcrumb) }
                page.risky = nil
            }
        } message: {
            Text("Save your work and close other apps first. If the Mac freezes, hold the power button, start it again and make a diagnostics report: the breadcrumb and the panic report tell us where it stopped.")
        }
    }

    // ---- the boot-arg ----------------------------------------------------------------------------------------------------
    var bootArg: some View {
        Section2(title: "Boot-arg", symbol: "text.badge.plus") {
            Caption("xeprobe=0xDEVICE in your EFI's config.plist makes XeProbe attach to that GPU at the next start.")
            LabeledContent("In the EFI") { Text(efiBootArgText) }
            LabeledContent("This start") { Text(probe.bootArgDevice.map { "xeprobe=\($0)" } ?? String(localized: "not set")) }
            LabeledContent("XeProbe") {
                Text(probe.attached ? String(localized: "attached to 8086:\(String((probe.info["device"] ?? "").suffix(4)))") : String(localized: "not attached this start"))
                    .foregroundStyle(probe.attached ? .green : .secondary)
            }
            if !probe.attached, probe.bootArgDevice != nil {
                Text("The boot-arg is set but XeProbe did not attach: IntelXe.kext must be in the EFI (the EFI page's stage 4) and from a release with XeProbe, and the device id must be the GPU's.")
                    .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
            if manager.package == nil {
                HStack {
                    Text("Choose a package on the Driver page first: efi.sh comes from it.").font(.callout).foregroundStyle(.secondary)
                    Spacer()
                    Button("Open the Driver page") { manager.requestedSection = .driver }
                }
            } else if !(manager.efi.readable && manager.efi.isOpenCore) {
                HStack {
                    Text("The EFI cannot be read: mount it or choose it on the EFI page.").font(.callout).foregroundStyle(.secondary)
                    Spacer()
                    Button("Open the EFI page") { manager.requestedSection = .efi }
                }
            } else {
                if manager.efi["kext_entry"] != "enabled" {
                    Text("Your EFI does not load IntelXe.kext: add it on the EFI page (stage 4) first, or XeProbe is not there to attach.")
                        .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                }
                HStack {
                    Spacer()
                    Button(String(localized: "Remove it") + "…") { page.confirming = .xeprobeOff }
                        .disabled(!(manager.efiPlans[.xeprobeOff]?.changes ?? false) || manager.busy != nil)
                    Button(String(localized: "Set xeprobe=\(device ?? "?")") + "…") { page.confirming = .xeprobe }
                        .buttonStyle(.borderedProminent)
                        .disabled(!(manager.efiPlans[.xeprobe]?.changes ?? false) || manager.busy != nil)
                }
            }
        }
    }

    var efiBootArgText: String {
        guard manager.efi.readable else { return "–" }
        let v = manager.efi["probe_boot_arg"]
        return v.isEmpty ? String(localized: "not set") : "xeprobe=\(v)"
    }

    // ---- the stages ------------------------------------------------------------------------------------------------------
    var stages: some View {
        Section2(title: "Stages", symbol: "list.number") {
            if let guc = probe.info["guc_image"], !guc.isEmpty {
                let built = probe.info["guc_built_in"] ?? "no"
                LabeledContent("GuC firmware for stage 3") {
                    Text(built == "no" ? String(localized: "\(guc) is not in this IntelXe.kext: stage 3 cannot run") : "\(guc) \(built)")
                        .foregroundStyle(built == "no" ? .orange : .primary).multilineTextAlignment(.trailing)
                }
            }
            if probe.attached && probe.info["family"] == "unknown" {
                Text("The probe does not know this device id: stages 3 to 5 need its family (sudo sysctl debug.xeprobe.family=NAME).")
                    .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
            CaptionedToggle(title: "NVRAM breadcrumb", caption: "Each step writes its name to NVRAM first, so that a freeze still shows where it stopped. It wears the NVRAM flash: only when looking for a freeze.", isOn: $breadcrumb)
            ForEach(ProbeStage.all) { stage in
                let r = probe.stages[stage.number]
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .firstTextBaseline) {
                        Text("\(stage.number) \(stage.name)").font(.body.monospaced()).bold()
                        Text(stage.riskText).font(.caption).foregroundStyle(stage.riskColor)
                        Spacer()
                        if let r { Text(statusText(r)).font(.callout).foregroundStyle(statusColor(r)) }
                        Button(String(localized: "Run")) { run(stage) }
                            .disabled(!canRun(stage.number) || manager.busy != nil)
                    }
                    Text(stage.what).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    if let r, !r.detail.isEmpty {
                        Text(r.detail).font(.caption.monospaced()).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.vertical, 2)
            }
        }
    }

    func canRun(_ n: Int) -> Bool {
        guard probe.attached else { return false }
        if n == 1 || n == 9 { return true }
        return probe.stages[n - 1]?.passed ?? false
    }

    func run(_ stage: ProbeStage) {
        if stage.risk == .medium { page.risky = stage.number } else { manager.runProbeStage(stage.number, breadcrumb: breadcrumb) }
    }

    func statusText(_ r: ProbeStageResult) -> String {
        switch r.status {
        case "ok": return String(localized: "passed, \(r.ms) ms")
        case "FAILED": return String(localized: "failed")
        case "refused": return String(localized: "refused")
        case "STALLED": return String(localized: "stalled")
        case "running": return String(localized: "running")
        default: return String(localized: "not run")
        }
    }
    func statusColor(_ r: ProbeStageResult) -> Color {
        r.status == "ok" ? .green : r.status == "FAILED" || r.status == "STALLED" ? .red : r.status == "refused" ? .orange : .secondary
    }

    // ---- the results -----------------------------------------------------------------------------------------------------
    var results: some View {
        Section2(title: "Results", symbol: "doc.text") {
            if !probe.breadcrumb.isEmpty {
                LabeledContent("Previous start's breadcrumb") { Text(probe.breadcrumb).font(.caption.monospaced()).textSelection(.enabled).multilineTextAlignment(.trailing) }
            }
            if !probe.results.isEmpty {
                Text(probe.results).font(.caption.monospaced()).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
            }
            if !probe.log.isEmpty {
                DisclosureGroup("What the stages printed") {
                    ScrollView { Text(probe.log).font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
                        .frame(maxHeight: 260)
                }
            }
            LabeledContent("Runs recorded") { Text("\(probe.historyRuns)") }
            HStack {
                Text("The report holds the results of this start, the record of every run and any panic. Private details are removed.").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Create a report with the results") {
                    manager.createReport(acpi: false, probe: true, admin: false)
                    manager.requestedSection = .report
                }
                .disabled(manager.busy != nil)
            }
        }
    }
}
