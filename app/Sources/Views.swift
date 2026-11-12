// SPDX-License-Identifier: MIT
import AppKit
import SwiftUI

@MainActor
final class Manager: ObservableObject {
    @Published var facts = SystemFacts()
    @Published var probing = false
    @Published var release: Release?
    @Published var releaseError: String?
    @Published var checkingRelease = false
    @Published var package: Package?
    @Published var packageStatus: [String: String] = [:]
    @Published var zinkPackage: Package?
    @Published var vulkanPackage: Package?
    @Published var busy: String?
    @Published var log = ""
    @Published var lastResult: String?
    @Published var reportID: String?
    @Published var reportFile: String?
    @Published var reportWarnings: [ReportWarning] = []
    @Published var health: [HealthIssue] = []
    @Published var showSetup = false
    @Published var testResults: [TestResult] = []
    @Published var testing: String?
    func runTest(long: Bool = false) {
        testing = String(localized: "Starting…"); testResults = []
        Task.detached {
            let report: (String) -> Void = { step in Task { @MainActor in self.testing = step } }
            let r = long ? runLongTest(progress: report) : runGPUTest(progress: report)
            if long { await MainActor.run { self.testResults = r; self.testing = nil }; return }
            await MainActor.run { self.testResults = r; self.testing = String(localized: "Smoothness (a window opens for 5 seconds)…") }
            let smooth = await runSmoothTest()   // SmoothTest.swift, on the main thread
            await MainActor.run { self.testResults = r + smooth; self.testing = nil }
        }
    }
    /// dismissed health issues: code -> the key it had (a new key shows it again)
    var healthDismissed: [String: String] {
        get { UserDefaults.standard.dictionary(forKey: "healthDismissed") as? [String: String] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: "healthDismissed") }
    }
    func dismiss(_ issue: HealthIssue) { var d = healthDismissed; d[issue.code] = issue.key; healthDismissed = d; health.removeAll { $0.code == issue.code } }
    @Published var rebootNeeded = false
    @Published var efiNotice: EFINotice?
    @Published var efi = EFIStatus()
    @Published var efiTarget = EFITarget.recorded
    @Published var efiPlans: [EFIAction: EFIPlan] = [:]
    private var efiStamp: String?   // efiFingerprint when efi and efiPlans were read
    @Published var hidpi = HiDPIStatus()
    /// the system-volume copy of the Metal driver (SystemVolume.swift)
    @Published var ssv = SSVStatus()
    /// Adaptive-Sync (Variable refresh) modes: wanted (NVRAM intelxefb-vrr-modes, unset = on) and offered this boot
    @Published var vrrWanted = true
    @Published var vrrActive = false
    @Published var efiChoice: String? = UserDefaults.standard.string(forKey: "efiPartition")
    /// A folder holding EFI/OC that the user chose instead of the partition (for this session only).
    @Published var efiFolder: String?
    @Published var requestedSection: Section?
    /// the last install.sh run from the Driver page or the setup assistant, for its step list (InstallProgress.swift)
    @Published var driverRun: DriverRun?
    /// where the output of the run that started last begins in `log`
    private(set) var runLogStart = 0
    private(set) var runStartedAt = Date()   // when the running command started (the report's elapsed time)
    private var logTimer: Timer?

    init() {
        package = Self.savedPackage(key: "package")
        zinkPackage = Self.savedPackage(key: "zinkPackage")
        vulkanPackage = Self.savedPackage(key: "vulkanPackage")
    }

    private static func savedPackage(key: String) -> Package? {
        guard let path = UserDefaults.standard.string(forKey: key) else { return nil }
        let package = Package(directory: URL(fileURLWithPath: path))
        return package.isValid ? package : nil
    }

    func refresh() async {
        probing = true
        facts = await Task.detached { SystemFacts.probe() }.value
        let f = facts, dismissed = healthDismissed
        health = await Task.detached { healthIssues(f).filter { dismissed[$0.code] != $0.key } }.value
        await refreshPackageStatus()
        await refreshEFI()
        await refreshHiDPI()
        probing = false
    }

    /// efi.sh --status, and its plans when the EFI can be read. The EFI is a folder the user chose, else the one OpenCore
    /// recorded; when it recorded none, the partition chosen on the EFI page (by UUID).
    func refreshEFI() async {
        guard let package, let script = efiScript(for: package) else { efi = EFIStatus(); efiPlans = [:]; return }
        let choice = efiChoice
        let test = ProcessInfo.processInfo.environment["INTELXE_MANAGER_TEST_EFI"] ?? efiFolder
        let (status, target, plans, stamp) = await Task.detached { () -> (EFIStatus, EFITarget, [EFIAction: EFIPlan], String?) in
            var target = test.map { EFITarget.folder($0) } ?? .recorded
            var status = efiStatus(script: script, package: package, target: target)
            if !status.found, let choice, status.candidates.contains(where: { $0.uuid == choice }) {
                target = .partition(choice)
                status = efiStatus(script: script, package: package, target: target)
            }
            var plans: [EFIAction: EFIPlan] = [:]
            if status.readable && status.isOpenCore {
                for action in [EFIAction.apply, .settings, .safari, .settingsSafari, .undo] {
                    if let argument = action.planArgument, let plan = efiPlan(script: script, package: package, target: target, argument) {
                        plans[action] = plan
                    }
                }
            }
            return (status, target, plans, efiFingerprint(root: status["efi_mount_point"]))
        }.value
        efi = status
        efiTarget = target
        efiPlans = plans
        efiStamp = stamp
    }

    /// Reads the EFI again when its files changed since the last read (a change made outside the app), so that the EFI
    /// page and its plans are not stale. efi.sh refuses a plan whose EFI changed anyway; this keeps what is shown right.
    func refreshEFIIfChanged() async {
        guard busy == nil, efi.known, let before = efiStamp else { return }
        let root = efi["efi_mount_point"]
        let now = await Task.detached { efiFingerprint(root: root) }.value
        if now != before { await refreshEFI() }
    }

    func chooseEFIPartition(_ uuid: String) {
        efiChoice = uuid
        efiFolder = nil
        UserDefaults.standard.set(uuid, forKey: "efiPartition")
        Task { await refreshEFI() }
    }

    /// An EFI folder instead of the partition, such as a USB stick's: efi.sh --efi with the folder that holds EFI/OC.
    func pickEFIFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = String(localized: "Choose the EFI folder of an OpenCore EFI (the one that holds OC and BOOT), for example on a USB stick.")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let root = efiFolderRoot(for: url) else {
            lastResult = String(localized: "That folder is not an OpenCore EFI: choose the EFI folder, the one with OC inside.")
            return
        }
        efiFolder = root
        Task { await refreshEFI() }
    }

    func useEFIPartition() {
        efiFolder = nil
        Task { await refreshEFI() }
    }

    func refreshPackageStatus() async {
        let chosen = package
        ssv = await Task.detached { SSVStatus.read(package: chosen) }.value
        guard let package else { packageStatus = [:]; return }
        packageStatus = await Task.detached { package.status() }.value
    }

    func checkForUpdate() async {
        checkingRelease = true
        releaseError = nil
        do { release = try await latestRelease() } catch { releaseError = error.localizedDescription }
        checkingRelease = false
    }

    var installedVersion: Version? { facts.installedVersion.flatMap(Version.init) }

    var updateAvailable: Bool {
        guard let latest = release?.version, facts.driverFilesPresent else { return false }
        guard let installed = installedVersion else { return true }
        return latest > installed
    }

    func choose(_ package: Package, kind: PackageKind) {
        switch kind {
        case .zink:
            zinkPackage = package
            UserDefaults.standard.set(package.directory.path, forKey: "zinkPackage")
        case .vulkan:
            vulkanPackage = package
            UserDefaults.standard.set(package.directory.path, forKey: "vulkanPackage")
        case .driver:
            self.package = package
            UserDefaults.standard.set(package.directory.path, forKey: "package")
            Task { await refreshPackageStatus(); await refreshEFI() }
        }
    }

    func pickFolder(kind: PackageKind) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = String(localized: "Choose an unpacked package: the folder with install.sh in it.")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let picked = Package(directory: url)
        guard picked.isValid else {
            lastResult = String(localized: "That folder is not a package: it has no install.sh and SHA256SUMS.")
            return
        }
        choose(picked, kind: kind)
    }

    func downloadLatest(kind: PackageKind) async {
        guard let release else { return }
        guard let suffix = kind == .zink ? "-zink-gl.zip" : kind == .vulkan ? "-vulkan.zip" : packageSuffix(macOSMajor: facts.macOSMajor) else {
            lastResult = String(localized: "There is no package for this version of macOS.")
            return
        }
        busy = String(localized: "Downloading")
        defer { busy = nil }
        do {
            let downloaded = try await download(release, suffix: suffix) { text in
                Task { @MainActor in self.appendLog(text + "\n") }
            }
            choose(downloaded, kind: kind)
        } catch { lastResult = error.localizedDescription }
    }

    private func appendLog(_ text: String) { log += text }

    func perform(title: String, command: String, needsRoot: Bool, then done: @escaping @MainActor (RunResult) -> Void,
                         afterRefresh: (@MainActor () -> Void)? = nil) {
        let logs = dataDirectory.appendingPathComponent("logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let file = logs.appendingPathComponent("\(formatter.string(from: Date())).log")
        busy = title
        lastResult = nil
        log += "\n== \(title)\n"
        let start = log.count
        runLogStart = start
        runStartedAt = Date()
        // install.sh writes to the log file as it works; rebuilding the kernel collection takes minutes.
        logTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, let text = try? String(contentsOf: file, encoding: .utf8) else { return }
                self.log = String(self.log.prefix(start)) + text
            }
        }
        let prompt = String(localized: "IntelXe Manager needs an administrator password for: \(title)")
        Task.detached {
            let result = run(command: command, needsRoot: needsRoot, log: file, prompt: prompt)
            await MainActor.run {
                self.logTimer?.invalidate()
                if let text = try? String(contentsOf: file, encoding: .utf8) { self.log = String(self.log.prefix(start)) + text }
                self.busy = nil
                if !result.cancelled && !result.succeeded && !result.message.isEmpty { self.log += result.message + "\n" }
                done(result)
                Task {
                    await self.refresh()
                    afterRefresh?()
                }
            }
        }
    }

    func perform(_ action: DriverAction) {
        guard let package else { return }
        let command = "\(shellQuoted(package.script.path)) \(action.argument)"
        var succeeded = false
        perform(title: action.title, command: command, needsRoot: action.needsRoot, then: { result in
            if var run = self.driverRun, run.action == action, !run.finished {
                run.finished = true; run.succeeded = result.succeeded; run.cancelled = result.cancelled; run.end = Date()
                self.driverRun = run
            }
            if result.cancelled {
                self.lastResult = String(localized: "Cancelled. Nothing changed.")
            } else if result.succeeded {
                self.lastResult = String(localized: "Finished.")
                if action.needsReboot { self.rebootNeeded = true }
                succeeded = true
            } else {
                self.lastResult = String(localized: "It did not finish. See the log.")
            }
        }, afterRefresh: {
            // the EFI as it is after the refresh: a notice only when its IntelXe.kext is not this package's or could not be read
            if succeeded { self.efiNotice = EFINotice.after(action, status: self.efi) }
        })
        // perform(title:) has set runLogStart for this run: the step list reads the output from there
        driverRun = action.steps.isEmpty ? nil : DriverRun(action: action, logStart: runLogStart)
    }

    /// An EFI change through efi.sh, with the hash of the plan that was shown: efi.sh refuses if the EFI changed since.
    func performEFI(_ action: EFIAction) {
        guard let package, let script = efiScript(for: package) else { return }
        var words = efiWords(script: script, package: package, target: efiTarget, action.argument)
        if action.planArgument != nil {
            guard let plan = efiPlans[action] else { return }
            words += ["--expect", plan.hash]
        }
        perform(title: action.title, command: words.map(shellQuoted).joined(separator: " "), needsRoot: true, then: { result in
            if result.cancelled {
                self.lastResult = String(localized: "Cancelled. Nothing changed.")
            } else if result.succeeded {
                self.lastResult = action == .mount ? String(localized: "Mounted.") : String(localized: "Finished. Restart to use the change.")
                if action != .mount { self.rebootNeeded = true }
            } else {
                self.lastResult = String(localized: "It did not finish. See the log.")
            }
        }, afterRefresh: {
            if action != .mount, let notice = self.efiNotice, notice != .removeKext {
                self.efiNotice = EFINotice.after(.install, status: self.efi)
            }
        })
    }

    func performZink(_ action: ZinkAction) {
        let command: String
        if let zinkPackage {
            command = shellQuoted(zinkPackage.directory.appendingPathComponent("install.sh").path) + " " + action.arguments
        } else if action == .globalOn || action == .globalOff {
            command = zinkGlobalCommand(on: action == .globalOn)
        } else if action == .glOff {
            command = cglzinkGlobalOffCommand()
        } else { return }
        perform(title: action.title, command: command, needsRoot: action.needsRoot, then: { result in
            self.lastResult = result.cancelled ? String(localized: "Cancelled. Nothing changed.")
                : result.succeeded ? String(localized: "Finished.") : String(localized: "It did not finish. See the log.")
        })
    }

    /// The optional Vulkan provider for games (vulkan/install.sh in the -vulkan package).
    func performVulkan(_ action: VulkanAction) {
        guard let vulkanPackage else { return }
        let command = shellQuoted(vulkanPackage.directory.appendingPathComponent("install.sh").path) + " " + action.arguments
        perform(title: action.title, command: command, needsRoot: action.needsRoot, then: { result in
            self.lastResult = result.cancelled ? String(localized: "Cancelled. Nothing changed.")
                : result.succeeded ? String(localized: "Finished.") : String(localized: "It did not finish. See the log.")
        })
    }

    /// Writes cglzink-global.txt (root owns /Library/IntelXe/GL): the next application started reads it.
    func saveCGLZinkRules(_ rules: [CGLZinkRule]) {
        let text = cglzinkRulesText(rules)
        let file = shellQuoted(cglzinkRulesFile)
        let command = "set -e; [ -d /Library/IntelXe/GL ] || { echo 'Zink is not installed'; exit 1; }; "
            + "printf '%s' \(shellQuoted(text)) > \(file).new; chown root:wheel \(file).new; chmod 644 \(file).new; mv \(file).new \(file); echo saved"
        perform(title: String(localized: "Save the application list"), command: command, needsRoot: true, then: { result in
            self.lastResult = result.cancelled ? String(localized: "Cancelled. Nothing changed.")
                : result.succeeded ? String(localized: "Saved. Applications started from now on follow the list.") : String(localized: "It did not finish. See the log.")
        })
    }

    func clearBootGuard() {
        perform(title: String(localized: "Clear the boot guard"), command: "/usr/sbin/nvram intelxe-accel-off=ok", needsRoot: true, then: { result in
            self.lastResult = result.succeeded ? String(localized: "Cleared. It takes effect at the next boot.") : String(localized: "It did not finish. See the log.")
            if result.succeeded { self.rebootNeeded = true }
        })
    }
}

extension DriverAction {
    var title: String {
        switch self {
        case .check: return String(localized: "Check")
        case .install: return String(localized: "Install this package")
        case .repair: return String(localized: "Repair")
        case .undo: return String(localized: "Undo the last install")
        case .uninstall: return String(localized: "Uninstall")
        }
    }
    var explanation: String {
        switch self {
        case .check: return String(localized: "Checks the system and the package. Changes nothing.")
        case .install: return String(localized: "Backs up what it replaces, copies the kexts and the Metal driver into /Library, and rebuilds the Auxiliary Kernel Collection. This takes a few minutes. Then restart.")
        case .repair: return String(localized: "Puts back installed files that differ from this package. The last backup stays as it is.")
        case .undo: return String(localized: "Puts the last backup back and rebuilds the Auxiliary Kernel Collection. Then restart.")
        case .uninstall: return String(localized: "Removes the kexts and the Metal driver from /Library. Backups stay. Remove IntelXe.kext from your EFI before you restart, or macOS comes up without the driver.")
        }
    }
    /// one short line for the Driver page; the explanation is behind its (i) button
    var caption: String {
        switch self {
        case .check: return String(localized: "Checks the system and the package; changes nothing.")
        case .install: return String(localized: "Backs up, copies into /Library and rebuilds the kernel collection: a few minutes.")
        case .repair: return String(localized: "Puts back installed files that differ from this package.")
        case .undo: return String(localized: "Puts the last backup back.")
        case .uninstall: return String(localized: "Removes the driver from /Library; backups stay.")
        }
    }
    var symbol: String {
        switch self {
        case .check: return "checkmark.shield"
        case .install: return "arrow.down.circle"
        case .repair: return "wrench.and.screwdriver"
        case .undo: return "arrow.uturn.backward"
        case .uninstall: return "trash"
        }
    }
    var confirms: Bool { self != .check }
    var destructive: Bool { self == .undo || self == .uninstall }
}

extension ZinkAction {
    var title: String {
        switch self {
        case .install: return String(localized: "Install Zink")
        case .undo: return String(localized: "Remove Zink")
        case .globalOn: return String(localized: "Use Zink for every LWJGL application")
        case .globalOff: return String(localized: "Back to Apple's OpenGL")
        case .glOn: return String(localized: "Turn on for every OpenGL application")
        case .glOff: return String(localized: "Turn off")
        case .test: return String(localized: "Run the self-test")
        }
    }
}

extension VulkanAction {
    var title: String {
        switch self {
        case .install: return String(localized: "Install the Vulkan provider")
        case .undo: return String(localized: "Remove the Vulkan provider")
        case .test: return String(localized: "Run the swapchain test")
        }
    }
}

enum Section: String, CaseIterable, Identifiable {
    case system, monitor, driver, efi, zink, display, settings, tests, report, log
    var id: String { rawValue }
    var title: LocalizedStringKey {
        switch self {
        case .system: return "Overview"
        case .monitor: return "Monitor"
        case .driver: return "Driver"
        case .efi: return "EFI (OpenCore)"
        case .zink: return "OpenGL (Zink)"
        case .display: return "Display"
        case .settings: return "Power and app"
        case .tests: return "Tests"
        case .report: return "Report"
        case .log: return "Log"
        }
    }
    var icon: String {
        switch self {
        case .system: return "square.grid.2x2"
        case .monitor: return "gauge.medium"
        case .driver: return "shippingbox"
        case .efi: return "internaldrive"
        case .zink: return "gamecontroller"
        case .display: return "display"
        case .settings: return "gearshape"
        case .tests: return "checkmark.seal"
        case .report: return "doc.text.magnifyingglass"
        case .log: return "text.alignleft"
        }
    }
}

/// The sidebar's groups, in order.
enum SectionGroup: CaseIterable, Identifiable {
    case status, install, configure, help
    var id: Self { self }
    var title: LocalizedStringKey {
        switch self { case .status: return "Status"; case .install: return "Install"; case .configure: return "Configure"; case .help: return "Help" }
    }
    var sections: [Section] {
        switch self {
        case .status: return [.system, .monitor]
        case .install: return [.driver, .efi, .zink]
        case .configure: return [.display, .settings]
        case .help: return [.tests, .report, .log]
        }
    }
}

// Plain observable state: this builds with the command line tools alone, whose toolchain has no SwiftUI macro plugin.
final class PageState: ObservableObject {
    @Published var section: Section? = .system
    @Published var confirming: DriverAction?
}

struct RootView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = PageState()

    var body: some View {
        NavigationSplitView {
            List(selection: $page.section) {
                ForEach(SectionGroup.allCases) { group in
                    SwiftUI.Section(group.title) {
                        ForEach(group.sections) { item in Label(item.title, systemImage: item.icon).tag(item) }
                    }
                }
            }
            .navigationSplitViewColumnWidth(185)
        } detail: {
            VStack(spacing: 0) {
                Banners()
                switch page.section ?? .system {
                case .system: SystemView()
                case .monitor: MonitorView()
                case .settings: SettingsView()
                case .driver: DriverView()
                case .efi: EFIView()
                case .display: DisplayView()
                case .zink: ZinkView()
                case .tests: TestsView()
                case .report: ReportView()
                case .log: LogView()
                }
            }
            .frame(minWidth: 560, minHeight: 460)
        }
        .disabled(manager.busy != nil)
        .overlay(alignment: .bottom) { BusyBar().animation(.easeInOut(duration: 0.3), value: manager.driverRun?.dismissed) }
        .onChange(of: manager.requestedSection) { _, section in
            if let section { page.section = section; manager.requestedSection = nil }
        }
        .task {
            // --section <name> (e.g. tests): open on that page (for screenshots and support instructions)
            if let i = CommandLine.arguments.firstIndex(of: "--section"), i + 1 < CommandLine.arguments.count,
               let sec = Section(rawValue: CommandLine.arguments[i + 1]) { page.section = sec }
            // --window-height <points>: a taller window, so that a screenshot shows the whole page
            if let i = CommandLine.arguments.firstIndex(of: "--window-height"), i + 1 < CommandLine.arguments.count, let h = Double(CommandLine.arguments[i + 1]) {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {   // after SwiftUI has placed the window
                    guard let w = NSApp.windows.first(where: { $0.isVisible && $0.canBecomeMain }), let screen = w.screen ?? NSScreen.main else { return }
                    let height = min(h, screen.visibleFrame.height)
                    w.setFrame(NSRect(x: w.frame.minX, y: screen.visibleFrame.maxY - height, width: max(w.frame.width, 980), height: height), display: true)
                }
            }
            refreshLoginCheckPath()
            await manager.refresh()
            if UserDefaults.standard.object(forKey: "autoUpdateCheck") as? Bool ?? true { await manager.checkForUpdate() }
            // first runs, until the setup assistant has been finished once: open it where the machine stands
            // (not for a machine where the driver already runs: an update needs no assistant)
            let step = manager.setupStep
            if !UserDefaults.standard.bool(forKey: "setupDone") && step != .done && step != .verify { manager.showSetup = true }
        }
        .sheet(isPresented: $manager.showSetup) { SetupView { manager.showSetup = false }.environmentObject(manager) }
    }
}

struct BusyBar: View {
    @EnvironmentObject var manager: Manager
    var body: some View {
        if let run = manager.driverRun, !run.dismissed {
            // an install, repair, undo or uninstall: its steps, while it runs and after, until closed
            DriverRunView(run: run, closable: true)
                .frame(width: 470)
                .padding(16)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.08)))
                .shadow(color: .black.opacity(0.18), radius: 14, y: 4)
                .padding(14)
                .transition(.move(edge: .bottom).combined(with: .opacity))
        } else if let busy = manager.busy {
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text(busy)
            }
            .padding(.horizontal, 14).padding(.vertical, 8)
            .background(.regularMaterial, in: Capsule())
            .padding(14)
        }
    }
}

struct Banners: View {
    @EnvironmentObject var manager: Manager
    var body: some View {
        VStack(spacing: 1) {
            if let notice = manager.efiNotice {
                Banner(symbol: "externaldrive.badge.exclamationmark", tint: .orange, text: notice.text,
                       fixTitle: "Open the EFI page", fix: { manager.requestedSection = .efi },
                       dismissHelp: "Done", dismiss: { manager.efiNotice = nil })
            }
            ForEach(manager.health) { issue in
                let fix = fix(for: issue)
                Banner(symbol: issue.code == "panics" ? "exclamationmark.octagon.fill" : "exclamationmark.triangle.fill",
                       tint: issue.code == "panics" ? .red : .orange, text: LocalizedStringKey(issue.text),
                       fixTitle: fix?.0 ?? "Create a report", fix: fix?.1 ?? { manager.requestedSection = .report },
                       dismissHelp: "Dismiss", dismiss: { manager.dismiss(issue) },
                       report: fix == nil ? nil : { manager.requestedSection = .report })
            }
            if manager.rebootNeeded {
                Banner(symbol: "arrow.clockwise.circle.fill", tint: .accentColor, text: "Restart to use the change.",
                       fixTitle: "Restart now", fix: { manager.restartNow() },
                       dismissHelp: "Later", dismiss: { manager.rebootNeeded = false })
            }
        }
        .animation(.easeInOut(duration: 0.3), value: manager.health)
        .animation(.easeInOut(duration: 0.3), value: manager.rebootNeeded)
    }

    /// the one thing the app can do about an issue itself; the others say what to do in their text
    func fix(for issue: HealthIssue) -> (LocalizedStringKey, () -> Void)? {
        switch issue.code {
        case "accel-off": return ("Clear the boot guard", { manager.clearBootGuard() })
        case "hidpi-half", "hidpi-stale": return ("Open the Display page", { manager.requestedSection = .display })
        case "ssv-mesa", "ssv-gone", "kext-version": return ("Open the Driver page", { manager.requestedSection = .driver })
        case "cpu-hybridpm": return ("Open the EFI page", { manager.requestedSection = .efi })
        default: return nil
        }
    }
}

/// A warning or notice across the top of the page: a coloured strip and icon on the left, the text, one fix button and a
/// close button on the right. Creating a report, when that is not the fix, is in the banner's context menu.
struct Banner: View {
    let symbol: String
    let tint: Color
    let text: LocalizedStringKey
    var fixTitle: LocalizedStringKey?
    var fix: (() -> Void)?
    var dismissHelp: LocalizedStringKey = "Dismiss"
    var dismiss: (() -> Void)?
    var report: (() -> Void)?
    var body: some View {
        HStack(spacing: 0) {
            Rectangle().fill(tint).frame(width: 4)
            HStack(spacing: 12) {
                Image(systemName: symbol).font(.title3).foregroundStyle(tint).frame(width: 22)
                Text(text).fixedSize(horizontal: false, vertical: true).frame(maxWidth: .infinity, alignment: .leading)
                if let fixTitle, let fix {
                    Button(fixTitle, action: fix).fixedSize()
                }
                if let dismiss {
                    Button(action: dismiss) { Image(systemName: "xmark").font(.caption.weight(.semibold)).foregroundStyle(.secondary) }
                        .buttonStyle(.borderless)
                        .help(Text(dismissHelp))
                        .accessibilityLabel(Text(dismissHelp))
                }
            }
            .padding(.horizontal, 14).padding(.vertical, 10)
        }
        .background(tint.opacity(0.12))
        .contextMenu {
            if let report { Button("Create a report", action: report) }
            if let dismiss { Button(dismissHelp, action: dismiss) }
        }
        .transition(.move(edge: .top).combined(with: .opacity))
    }
}

/// One figure on the Overview page: a title, a value, a line under it and an optional action, on a card tinted by its state.
struct OverviewCard<Action: View>: View {
    let title: LocalizedStringKey
    let symbol: String
    let tint: Color
    let value: String
    let detail: String
    @ViewBuilder var action: Action
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: symbol).foregroundStyle(tint).frame(width: 18)
                Text(title).font(.subheadline.weight(.medium)).foregroundStyle(.secondary)
            }
            Text(value).font(.title3.weight(.semibold)).lineLimit(1).minimumScaleFactor(0.7)
                .contentTransition(.numericText())
            Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(2)
            action
        }
        .frame(maxWidth: .infinity, minHeight: 92, alignment: .topLeading)
        .tintedCard(tint)
        .animation(.easeInOut(duration: 0.35), value: value)
        .animation(.easeInOut(duration: 0.35), value: tint)
    }
}

/// Where the driver stands, for the Overview page's header.
enum OverviewState: Equatable {
    case working, update, problem, notInstalled
    var color: Color {
        switch self { case .working: return .green; case .update: return .orange; case .problem: return .red; case .notInstalled: return neutralTint }
    }
}

extension Manager {
    var driverInstalled: Bool { facts.installedVersion != nil || facts.driverFilesPresent }
    var overviewState: OverviewState {
        if !driverInstalled { return .notInstalled }
        if !health.isEmpty || testResults.contains(where: { $0.state == .bad }) || !facts.kextLoaded || facts.bootGuardOff { return .problem }
        return updateAvailable ? .update : .working
    }
}

/// The top of the Overview page: the card, one line on its state, a coloured badge and the action that matters now.
struct StatusHeader: View {
    @EnvironmentObject var manager: Manager
    var body: some View {
        let state = manager.overviewState, f = manager.facts
        HStack(alignment: .center, spacing: 14) {
            Image(systemName: "cpu.fill")
                .font(.system(size: 30, weight: .medium))
                .foregroundStyle(state.color)
                .frame(width: 58, height: 58)
                .background(state.color.opacity(0.14), in: RoundedRectangle(cornerRadius: 13, style: .continuous))
            VStack(alignment: .leading, spacing: 3) {
                Text(f.hasCard ? f.cardName.components(separatedBy: " (").first ?? f.cardName : f.metalDevice.isEmpty ? String(localized: "No Arc B580 or B570 found") : f.metalDevice)
                    .font(.title2.weight(.semibold)).lineLimit(1)
                Text(line(state)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail).help(line(state))
            }
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 8) {
                StatusBadge(text: badge(state), color: state.color)
                if state == .update {
                    Button("Update on the Driver page") { manager.requestedSection = .driver }.buttonStyle(.borderedProminent)
                } else if state == .problem && manager.health.isEmpty && !f.kextLoaded {
                    Button("Open the EFI page") { manager.requestedSection = .efi }.buttonStyle(.borderedProminent)
                }
            }
        }
        .padding(.vertical, 4)
        .animation(.easeInOut(duration: 0.35), value: state)
    }

    func badge(_ state: OverviewState) -> String {
        switch state {
        case .working: return String(localized: "Driver working · \(manager.facts.installedVersion ?? "?")")
        case .update: return String(localized: "Update available")
        case .problem: return String(localized: "Problem")
        case .notInstalled: return String(localized: "Not installed")
        }
    }

    func line(_ state: OverviewState) -> String {
        let f = manager.facts
        switch state {
        case .notInstalled:
            return String(localized: "The driver is not installed on this Mac.")
        case .problem:
            if let issue = manager.health.first { return issue.text }
            let bad = manager.testResults.filter { $0.state == .bad }.count
            if bad > 0 { return String(localized: "\(bad) test(s) failed: see the Tests page.") }
            return String(localized: "IntelXe.kext is not loaded: it comes from your EFI.")
        case .update:
            let latest = manager.release?.version?.description ?? "?"
            if let installed = f.installedVersion { return String(localized: "Release \(latest) is out; this Mac has \(installed).") }
            return String(localized: "Release \(latest) is out.")
        case .working:
            return f.metalDevice.isEmpty ? String(localized: "Installed and loaded.") : String(localized: "Metal runs on \(f.metalDevice).")
        }
    }
}

final class OptionalFlag: ObservableObject {
    @Published var value: Bool?
}

struct SystemView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var live = MonitorModel()
    /// This Mac: open by itself while a requirement is not met, until the user opens or closes it
    @StateObject var thisMacOpen = OptionalFlag()
    var body: some View {
        let met = manager.facts.checks.allSatisfy { $0.state != .bad }
        Form {
            SwiftUI.Section {
                StatusHeader()
            }
            Section2(title: "At a glance", symbol: "square.grid.2x2") {
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)], spacing: 12) {
                    OverviewCard(title: "Driver", symbol: "shippingbox", tint: driverTint,
                                 value: installedText, detail: String(localized: "Latest release: \(releaseText)")) {
                        // the header carries the update button when the update is what matters now
                        if manager.updateAvailable && manager.overviewState != .update {
                            Button("Update on the Driver page") { manager.requestedSection = .driver }.controlSize(.small)
                        }
                    }
                    OverviewCard(title: "GPU", symbol: "cpu", tint: manager.facts.kextLoaded ? .green : neutralTint,
                                 value: live.now.actMHz.map { $0 == 0 ? String(localized: "Idle") : "\($0) MHz" } ?? (manager.facts.metalDevice.isEmpty ? String(localized: "No Metal device") : manager.facts.metalDevice),
                                 detail: [live.now.packageC.map { String(localized: "GPU \($0) °C") }, live.now.fps.map { String(format: "%.0f ", $0) + String(localized: "frames/s") }].compactMap { $0 }.joined(separator: " · ")) {
                        EmptyView()
                    }
                    OverviewCard(title: "Problems", symbol: manager.health.isEmpty ? "checkmark.circle" : "exclamationmark.triangle",
                                 tint: manager.health.isEmpty ? .green : .orange,
                                 value: manager.health.isEmpty ? String(localized: "None found") : String(localized: "\(manager.health.count) to look at"),
                                 detail: manager.health.isEmpty ? String(localized: "Checked at start: panics, boot guard, versions, HiDPI, CPU.") : String(localized: "Shown at the top of the window, each with what to do.")) {
                        EmptyView()
                    }
                    let bad = manager.testResults.filter { $0.state == .bad }.count, warn = manager.testResults.filter { $0.state == .warning }.count
                    OverviewCard(title: "Last test", symbol: "checkmark.seal", tint: manager.testResults.isEmpty ? neutralTint : bad > 0 ? .red : warn > 0 ? .orange : .green,
                                 value: manager.testResults.isEmpty ? String(localized: "Not run yet") : bad > 0 ? String(localized: "\(bad) failed") : warn > 0 ? String(localized: "\(warn) below expected") : String(localized: "All passed"),
                                 detail: manager.testResults.isEmpty ? String(localized: "About 25 seconds, on the Tests page.") : (manager.testResults.count == 1 ? String(localized: "1 check this session.") : String(localized: "\(manager.testResults.count) checks this session."))) {
                        Button(manager.testResults.isEmpty ? "Run the GPU test" : "Open the Tests page") { manager.requestedSection = .tests }.controlSize(.small)
                    }
                }
            }
            Section2(title: "Setup", symbol: "list.number") {
                HStack {
                    Text("Step by step from a fresh macOS install to a working driver.").font(.callout).foregroundStyle(.secondary)
                    Spacer()
                    if manager.overviewState == .notInstalled {
                        Button("Open the setup assistant") { manager.showSetup = true }.buttonStyle(.borderedProminent)
                    } else {
                        Button("Open the setup assistant") { manager.showSetup = true }
                    }
                }
            }
            Section2(title: "This Mac", symbol: "desktopcomputer") {
                DisclosureGroup(isExpanded: Binding(get: { thisMacOpen.value ?? !met }, set: { thisMacOpen.value = $0 })) {
                    ForEach(manager.facts.checks) { check in
                        CheckRow(check: check)
                    }
                    if manager.facts.bootGuardOff {
                        Button("Clear the boot guard") { manager.clearBootGuard() }
                    }
                } label: {
                    Label(met ? String(localized: "Requirements met") : String(localized: "Some requirements are not met"),
                          systemImage: met ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(met ? Color.green : Color.red)
                }
            }
        }
        .onAppear { live.start() }
        .onDisappear { live.stop() }
        .formStyle(.grouped)
        .toolbar {
            ToolbarItem {
                Button { Task { await manager.refresh(); await manager.checkForUpdate() } } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    .disabled(manager.probing)
            }
        }
    }

    var driverTint: Color {
        if !manager.driverInstalled { return neutralTint }
        if !manager.facts.kextLoaded { return .red }
        return manager.updateAvailable ? .orange : .green
    }

    var installedText: String {
        if let version = manager.facts.installedVersion { return version }
        return manager.facts.driverFilesPresent ? String(localized: "Installed, version not recorded (before 0.1.2)") : String(localized: "Not installed")
    }

    var releaseText: String {
        if manager.checkingRelease { return String(localized: "Checking…") }
        if let error = manager.releaseError { return error }
        return manager.release?.version?.description ?? "–"
    }
}

struct Section2<Content: View>: View {
    let title: LocalizedStringKey
    var symbol: String?
    @ViewBuilder let content: Content
    var body: some View {
        SwiftUI.Section { content } header: {
            if let symbol {
                HStack(spacing: 7) {
                    Image(systemName: symbol).foregroundStyle(Color.accentColor).frame(width: 20)
                    Text(title)
                }
            } else { Text(title) }
        }
    }
}

struct CheckRow: View {
    let check: Check
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Image(systemName: symbol).foregroundStyle(color).accessibilityLabel(stateLabel)
                Text(LocalizedStringKey(check.title))
                Spacer()
                Text(LocalizedStringKey(check.value)).foregroundStyle(.secondary).textSelection(.enabled).multilineTextAlignment(.trailing)
            }
            if !check.help.isEmpty {
                Text(LocalizedStringKey(check.help)).font(.callout).foregroundStyle(.secondary).padding(.leading, 26)
            }
        }
    }
    var symbol: String {
        switch check.state {
        case .good: return "checkmark.circle.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .bad: return "xmark.octagon.fill"
        case .neutral: return "circle"
        }
    }
    var color: Color {
        switch check.state {
        case .good: return .green
        case .warning: return .orange
        case .bad: return .red
        case .neutral: return .secondary
        }
    }
    var stateLabel: LocalizedStringKey {
        switch check.state {
        case .good: return "OK"
        case .warning: return "Warning"
        case .bad: return "Problem"
        case .neutral: return ""
        }
    }
}

struct DriverView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = PageState()

    var body: some View {
        Form {
            Section2(title: "Package", symbol: "shippingbox") {
                if let package = manager.package {
                    PathRow(title: "Folder", path: package.directory.path)
                    LabeledContent("Version") { Text(manager.packageStatus["package_version"] ?? "–") }
                    LabeledContent("Compared with what is installed") { Text(stateText) }
                    if !changedItems.isEmpty {
                        DisclosureGroup(String(localized: "Which items")) {
                            ForEach(changedItems, id: \.self) { item in
                                Text(item).font(.callout.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                            }
                        }
                    }
                } else {
                    Text("Choose a package before you install or repair. Download the latest release, or use a folder you unpacked.")
                        .foregroundStyle(.secondary)
                }
                HStack {
                    Caption("A download is installed only when the release's signature and hashes check out.",
                            detail: "The release's list of hashes must be signed with the project's key, which is built into this app, and the download must match it. A damaged file, or a release that only came from the same GitHub page, is refused.")
                    Spacer(minLength: 12)
                    if let error = manager.releaseError, manager.release == nil {
                        Text(error).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Button("Use a folder…") { manager.pickFolder(kind: .driver) }.fixedSize()
                    if let release = manager.release {
                        // the first thing to do on a machine without a package
                        if manager.package == nil {
                            Button("Download \(release.tag_name)") { Task { await manager.downloadLatest(kind: .driver) } }.buttonStyle(.borderedProminent).fixedSize()
                        } else {
                            Button("Download \(release.tag_name)") { Task { await manager.downloadLatest(kind: .driver) } }.fixedSize()
                        }
                    }
                }
            }
            if manager.package != nil {
                Section2(title: "Install", symbol: "arrow.down.circle") {
                    ActionRow(action: .install, enabled: enabled(.install), prominent: manager.installRecommended) { page.confirming = .install }
                    ActionRow(action: .repair, enabled: enabled(.repair)) { page.confirming = .repair }
                    ActionRow(action: .check, enabled: enabled(.check)) { manager.perform(.check) }
                }
                Section2(title: "Go back", symbol: "arrow.uturn.backward") {
                    ActionRow(action: .undo, enabled: enabled(.undo)) { page.confirming = .undo }
                    ActionRow(action: .uninstall, enabled: enabled(.uninstall)) { page.confirming = .uninstall }
                }
            }
            SSVSection()
            if let result = manager.lastResult {
                Text(result).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .alert(page.confirming?.title ?? "", isPresented: Binding(get: { page.confirming != nil }, set: { if !$0 { page.confirming = nil } }), presenting: page.confirming) { action in
            Button(action.title, role: action.destructive ? .destructive : nil) { manager.perform(action) }
            Button("Cancel", role: .cancel) {}
        } message: { action in
            Text(action.explanation)
        }
    }

    func enabled(_ action: DriverAction) -> Bool {
        switch action {
        case .check, .install, .repair: return manager.package != nil
        case .undo: return manager.package != nil && manager.facts.hasBackup
        case .uninstall: return manager.package != nil && manager.facts.driverFilesPresent
        }
    }

    /// what install.sh --status (0.1.4 and later) lists as differing or missing
    var changedItems: [String] {
        let st = manager.packageStatus
        let differ = (st["items_differing"] ?? "").split(separator: ",").map { String($0) + "  " + String(localized: "(differs)") }
        let missing = (st["items_missing"] ?? "").split(separator: ",").map { String($0) + "  " + String(localized: "(missing)") }
        return st["installed_state"] == "absent" ? [] : differ + missing
    }
    var stateText: String {
        switch manager.packageStatus["installed_state"] {
        case "same": return String(localized: "The same")
        case "different": return String(localized: "Different: \(manager.packageStatus["items_different"] ?? "?") item(s)")
        case "partial": return String(localized: "Partly installed")
        case "absent": return String(localized: "Not installed")
        default: return "–"
        }
    }
}

extension Manager {
    /// the chosen package differs from what is installed: installing it is what the Driver page is for now
    var installRecommended: Bool { package != nil && ["different", "partial", "absent"].contains(packageStatus["installed_state"] ?? "") }
}

/// A Driver page action: its symbol, one short line with the explanation behind (i), and its button (accent when it is
/// the one that matters now, red when it takes something away).
struct ActionRow: View {
    let action: DriverAction
    let enabled: Bool
    var prominent = false
    let perform: () -> Void
    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: action.symbol).foregroundStyle(action.destructive ? Color.red : Color.accentColor).frame(width: 20)
            Caption(text: Text(action.caption), detail: Text(action.explanation))
            Spacer(minLength: 12)
            if prominent {
                Button(action.title, action: perform).disabled(!enabled).fixedSize().buttonStyle(.borderedProminent)
            } else if action.destructive {
                Button(action.title, role: .destructive, action: perform).disabled(!enabled).fixedSize().foregroundStyle(.red)
            } else {
                Button(action.title, action: perform).disabled(!enabled).fixedSize()
            }
        }
    }
}

/// The Zink page's application list: what is shown and what the file holds.
final class CGLZinkRulesState: ObservableObject {
    @Published var rules: [CGLZinkRule] = []
    @Published var saved: [CGLZinkRule] = []
    func reload() { rules = readCGLZinkRules(); saved = rules }
}

struct ZinkView: View {
    @EnvironmentObject var manager: Manager
    @StateObject private var list = CGLZinkRulesState()
    var body: some View {
        let f = manager.facts, ready = manager.zinkPackage != nil
        Form {
            Section2(title: "Zink", symbol: "gamecontroller") {
                Caption("Optional: Minecraft and other LWJGL 3 Java games through Mesa's Zink, straight to Vulkan.",
                        detail: "Optional. LWJGL 3 Java games such as Minecraft render through Mesa's Zink straight to Vulkan, instead of Apple's OpenGL. Native applications stay on Apple's OpenGL.")
                LabeledContent("Installed") {
                    Label(f.zinkInstalled ? String(localized: "Yes") : String(localized: "No"), systemImage: f.zinkInstalled ? "checkmark.circle.fill" : "circle.dashed")
                        .foregroundStyle(f.zinkInstalled ? .green : .secondary)
                }
            }
            Section2(title: "Every LWJGL application", symbol: "square.stack.3d.up") {
                HStack(alignment: .firstTextBaseline, spacing: 14) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(f.zinkGlobal ? "On: LWJGL 3 applications use Zink." : "Off: they use Apple's OpenGL.")
                        Caption("It applies to applications started afterwards. Running ones must be restarted.",
                                detail: "It applies to applications started afterwards. Running ones must be restarted. A single game can use Zink without this, through its launcher's Java arguments.")
                    }
                    Spacer()
                    if f.zinkGlobal {
                        Button(ZinkAction.globalOff.title) { manager.performZink(.globalOff) }.fixedSize()
                    } else if f.zinkInstalled {
                        // installed and not used yet: turning it on is what this page is for now
                        Button(ZinkAction.globalOn.title) { manager.performZink(.globalOn) }.fixedSize().buttonStyle(.borderedProminent)
                    } else {
                        Button(ZinkAction.globalOn.title) { manager.performZink(.globalOn) }.fixedSize().disabled(true)
                    }
                }
            }
            if f.zinkInstalled { everyOpenGLApplication }
            Section2(title: "Install", symbol: "arrow.down.circle") {
                if let package = manager.zinkPackage {
                    PathRow(title: "Package", path: package.directory.path)
                } else {
                    HStack(alignment: .firstTextBaseline, spacing: 14) {
                        Text("Installing, the self-test and removing need the Zink package.").foregroundStyle(.secondary)
                        Spacer()
                        if let release = manager.release, release.asset("-zink-gl.zip") != nil {
                            Button("Download \(release.tag_name)") { Task { await manager.downloadLatest(kind: .zink) } }.fixedSize().buttonStyle(.borderedProminent)
                        } else if manager.release != nil {
                            Text("The latest release has no Zink package.").foregroundStyle(.secondary)
                        }
                        Button("Use a folder…") { manager.pickFolder(kind: .zink) }.fixedSize()
                    }
                }
                if ready {
                    HStack(spacing: 10) {
                        if f.zinkInstalled {
                            Button(ZinkAction.test.title) { manager.performZink(.test) }
                        }
                        Spacer()
                        if f.zinkInstalled {
                            Button(ZinkAction.undo.title, role: .destructive) { manager.performZink(.undo) }.foregroundStyle(.red)
                            Button(String(localized: "Install again")) { manager.performZink(.install) }
                        } else {
                            Button(ZinkAction.install.title) { manager.performZink(.install) }.buttonStyle(.borderedProminent)
                        }
                    }
                    Button("Use another folder…") { manager.pickFolder(kind: .zink) }.buttonStyle(.link).font(.callout)
                }
            }
            vulkanForGames
            if let result = manager.lastResult {
                Text(result).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear { list.reload() }
        .onChange(of: manager.busy) { _, busy in if busy == nil { list.reload() } }
    }

    /// The optional Vulkan provider for games (Wine with DXVK / vkd3d-proton on the GPU's own Vulkan driver).
    @ViewBuilder var vulkanForGames: some View {
        let f = manager.facts
        Section2(title: "Vulkan for games (experimental)", symbol: "cube.transparent") {
            Caption("Optional: Windows games in Wine (DXVK, vkd3d-proton) and other Vulkan programs on the GPU's own Vulkan driver, without MoltenVK.",
                    detail: "Experimental. Programs started with intelxe-vk run (in /Library/IntelXe/Vulkan/bin) get the GPU's own Vulkan driver instead of MoltenVK. Nothing changes for any other program. Wine itself is not included.")
            LabeledContent("Installed") {
                Label(f.vulkanInstalled ? String(localized: "Yes") : String(localized: "No"), systemImage: f.vulkanInstalled ? "checkmark.circle.fill" : "circle.dashed")
                    .foregroundStyle(f.vulkanInstalled ? .green : .secondary)
            }
            if let package = manager.vulkanPackage {
                PathRow(title: "Package", path: package.directory.path)
                HStack(spacing: 10) {
                    if f.vulkanInstalled { Button(VulkanAction.test.title) { manager.performVulkan(.test) } }
                    Spacer()
                    if f.vulkanInstalled {
                        Button(VulkanAction.undo.title, role: .destructive) { manager.performVulkan(.undo) }.foregroundStyle(.red)
                        Button(String(localized: "Install again")) { manager.performVulkan(.install) }
                    } else {
                        Button(VulkanAction.install.title) { manager.performVulkan(.install) }.buttonStyle(.borderedProminent)
                    }
                }
                Button("Use another folder…") { manager.pickFolder(kind: .vulkan) }.buttonStyle(.link).font(.callout)
            } else {
                HStack(alignment: .firstTextBaseline, spacing: 14) {
                    Text("Installing, the test and removing need the Vulkan package.").foregroundStyle(.secondary)
                    Spacer()
                    if let release = manager.release, release.asset("-vulkan.zip") != nil {
                        Button("Download \(release.tag_name)") { Task { await manager.downloadLatest(kind: .vulkan) } }.fixedSize()
                    } else if manager.release != nil {
                        Text("The latest release has no Vulkan package.").foregroundStyle(.secondary)
                    }
                    Button("Use a folder…") { manager.pickFolder(kind: .vulkan) }.fixedSize()
                }
            }
        }
    }

    /// The experimental switch for every native OpenGL application (cglzink) and its per-application list.
    @ViewBuilder var everyOpenGLApplication: some View {
        let f = manager.facts
        Section2(title: "Every OpenGL application (experimental)", symbol: "macwindow.on.rectangle") {
            HStack(alignment: .firstTextBaseline, spacing: 14) {
                VStack(alignment: .leading, spacing: 3) {
                    Text(f.cglzinkGlobal ? "On: native OpenGL applications draw through Zink, as the list below allows." : "Off: native OpenGL applications use Apple's OpenGL.")
                    Caption("Applications started afterwards follow it. Apple's applications (Chess excepted), browsers, Electron applications and video players stay on Apple's OpenGL unless you list them.",
                            detail: "Experimental. A small library in every application you start decides whether it takes part: application bundles only, not Apple's (Chess excepted), not browsers, Electron or Chromium applications, video players or this app. The list below adds an application or keeps one out. Turn it off before you delete /Library/IntelXe/GL by hand.")
                }
                Spacer()
                if f.cglzinkGlobal {
                    Button(ZinkAction.glOff.title) { manager.performZink(.glOff) }.fixedSize()
                } else {
                    Button(ZinkAction.glOn.title) { manager.performZink(.glOn) }.fixedSize()
                        .disabled(!f.cglzinkAvailable || manager.zinkPackage == nil)
                }
            }
            if !f.cglzinkAvailable {
                Text("The installed Zink is older than this switch: install the Zink package of 0.1.4 or later.").font(.callout).foregroundStyle(.secondary)
            } else if !f.cglzinkGlobal && manager.zinkPackage == nil {
                Text("Turning it on needs the Zink package: download it or choose its folder under Install below.").font(.callout).foregroundStyle(.secondary)
            }
            ForEach($list.rules) { $rule in
                HStack(spacing: 10) {
                    appIcon(rule.bundleID)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(appName(rule.bundleID))
                        Text(rule.bundleID).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                    Spacer()
                    Picker("", selection: $rule.useZink) {
                        Text("Zink").tag(true)
                        Text("Apple's OpenGL").tag(false)
                    }
                    .pickerStyle(.segmented).labelsHidden().fixedSize()
                    Button { list.rules.removeAll { $0.bundleID == rule.bundleID } } label: { Image(systemName: "minus.circle") }
                        .buttonStyle(.borderless).help(Text("Remove from the list"))
                }
            }
            HStack(spacing: 10) {
                Button("Add an application…") { addApplication() }
                Spacer()
                if list.rules != list.saved {
                    Button("Revert") { list.rules = list.saved }
                    Button("Save the list") { manager.saveCGLZinkRules(list.rules) }.buttonStyle(.borderedProminent)
                }
            }
        }
    }

    private func appURL(_ id: String) -> URL? { NSWorkspace.shared.urlForApplication(withBundleIdentifier: id) }
    private func appName(_ id: String) -> String {
        appURL(id).map { FileManager.default.displayName(atPath: $0.path).replacingOccurrences(of: ".app", with: "") } ?? id
    }
    @ViewBuilder private func appIcon(_ id: String) -> some View {
        if let url = appURL(id) {
            Image(nsImage: NSWorkspace.shared.icon(forFile: url.path)).resizable().frame(width: 22, height: 22)
        } else {
            Image(systemName: "app.dashed").frame(width: 22, height: 22).foregroundStyle(.secondary)
        }
    }
    private func addApplication() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.applicationBundle]
        panel.allowsMultipleSelection = true
        panel.directoryURL = URL(fileURLWithPath: "/Applications")
        panel.message = String(localized: "Choose applications for the list. They are added as \"Zink\"; switch one to Apple's OpenGL to keep it out.")
        guard panel.runModal() == .OK else { return }
        for url in panel.urls {
            guard let id = Bundle(url: url)?.bundleIdentifier, !list.rules.contains(where: { $0.bundleID == id }) else { continue }
            list.rules.append(CGLZinkRule(bundleID: id, useZink: true))
        }
    }
}

struct LogView: View {
    @EnvironmentObject var manager: Manager
    var body: some View {
        if manager.log.isEmpty {
            VStack(spacing: 8) {
                Image(systemName: "text.alignleft").font(.largeTitle).foregroundStyle(.tertiary)
                Text("Nothing has run yet.").font(.headline)
                Text("Installs, EFI changes, reports and tests write here as they run.").foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
        ScrollView {
            Text(manager.log)
                .font(.system(.callout, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(14)
        }
        .defaultScrollAnchor(.bottom)
        }
    }
}
