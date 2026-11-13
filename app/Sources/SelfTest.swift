// SPDX-License-Identifier: MIT
// IntelXeManager --selftest [package folder]: the parts that need no window and no password, for checking a build.
import AppKit
import Metal
import SwiftUI

/// Each page without a window, to look at the layout.
@MainActor
func renderSnapshots(into folder: String) {
    _ = NSApplication.shared
    let manager = Manager()
    manager.facts = SystemFacts.probe()
    if CommandLine.arguments.count > 2 {
        let package = Package(directory: URL(fileURLWithPath: CommandLine.arguments[2]))
        manager.package = package
        manager.packageStatus = package.status()
    }
    if let folder = selfTestEFIFolder, let package = manager.package, let script = efiScript(for: package) {
        manager.efiTarget = .folder(folder)
        manager.efi = efiStatus(script: script, package: package, target: manager.efiTarget)
        for action in [EFIAction.apply, .settings, .safari, .settingsSafari, .bootLog, .bootLogOff, .undo] {
            manager.efiPlans[action] = efiPlan(script: script, package: package, target: manager.efiTarget, action.planArgument!)
        }
        manager.efiNotice = EFINotice.after(.install, status: manager.efi)
    }
    // the EFI page as it is when OpenCore recorded no boot path: the partitions to choose from
    let choosing = Manager()
    choosing.package = manager.package
    choosing.efi = EFIStatus(text: "efi_source=none\nefi_description=no EFI partition found\nkext_file=unknown\nkext_version_package=0.1.0\n"
        + "candidate=00000000-0000-4000-8000-000000000001|disk0s1|WD Blue SN550 1TB|1000 GB||unknown\n"
        + "candidate=00000000-0000-4000-8000-000000000002|disk5s1|SanDisk Ultra|31 GB|/Volumes/EFI|yes\n")
    choosing.efiNotice = .unchecked
    choosing.health = [HealthIssue(code: "accel-off", text: String(localized: "The boot guard holds acceleration off: earlier boots did not reach a healthy desktop. Clear it once you know why."), key: "on"),
                       HealthIssue(code: "panics", text: String(localized: "macOS restarted after a kernel panic (\("9 Nov 2026 10:15")). A diagnostics report holds its text."), key: "x")]
    choosing.rebootNeeded = true
    // the Display page as on the B580 test machine: supported and off, and on after --on before the restart
    let hidpiOff = Manager(), hidpiPending = Manager()
    hidpiOff.hidpi = HiDPIStatus(text: "hidpi_supported=1\nhidpi_nvram=0\nhidpi_override=none\nhidpi_windowserver=0\nhidpi_active=0\ndisplay=4c2d:750c\nnative=2560x1440\n")
    hidpiPending.hidpi = HiDPIStatus(text: "hidpi_supported=1\nhidpi_nvram=1\nhidpi_override=ours\nhidpi_windowserver=1\nhidpi_active=0\ndisplay=4c2d:750c\nnative=2560x1440\n")
    // the setup assistant on its driver page and its last page, as on a working B580 machine
    let setupDone = Manager(), setupDriver = Manager()
    for m in [setupDone, setupDriver] {
        m.facts.architecture = "x86_64"; m.facts.macOSMajor = 26; m.facts.hasCard = true; m.facts.sipRelaxed = true; m.facts.amfiRelaxed = true
        m.package = manager.package ?? Package(directory: URL(fileURLWithPath: "/tmp/intel-gpu-macos-driver-0.1.4-tahoe"))
    }
    setupDone.facts.installedVersion = "0.1.4"; setupDone.facts.kextLoaded = true
    // its first page: with the EFI found (the self-test's fake EFI and its plans), and when OpenCore recorded none
    let setupEFI = Manager(), setupChoose = Manager()
    for m in [setupEFI, setupChoose] {
        m.facts.architecture = "x86_64"; m.facts.macOSMajor = 26; m.facts.hasCard = true; m.package = setupDone.package
    }
    setupEFI.efi = manager.efi; setupEFI.efiPlans = manager.efiPlans; setupEFI.efiTarget = manager.efiTarget
    setupChoose.efi = choosing.efi
    setupDone.testResults = [TestResult(title: "Metal", value: "ok", state: .good)]
    // the Overview page on a working B580 machine, and on one with a newer release out
    let working = Manager(), outdated = Manager()
    for m in [working, outdated] {
        m.facts = setupDone.facts; m.facts.driverFilesPresent = true; m.facts.metalDevice = "Intel Arc B580"; m.facts.cardName = "Intel Arc B580 (8086:e20b)"
        m.testResults = setupDone.testResults
    }
    // the Overview page with macOS updates on offer (one this release supports, one it does not) and the guard's result
    let updating = Manager()
    updating.facts = working.facts; updating.facts.installedVersion = "0.1.5"; updating.testResults = working.testResults
    updating.osUpdate.offered = [OfferedUpdate(version: "27.0", build: "26A428", title: "macOS 27"), OfferedUpdate(version: "26.7.2", build: "25H123", title: "macOS Tahoe 26.7.2")]
    updating.osUpdate.support = ["27.0": .no, "26.7.2": .ok]
    updating.osUpdate.guardStatus = ["prepared": "no", "last_result": "ok", "last_when": "1", "last_from": "26.7 (25G1)", "last_to": "26.7.1 (25H1)", "last_ssv": "gone"]
    outdated.release = try? JSONDecoder().decode(Release.self, from: Data(#"{"tag_name":"v0.1.5","html_url":"https://example.invalid/r","body":"","prerelease":false,"assets":[]}"#.utf8))
    // an install half way (copying the Metal driver, the Kernel Collection next) and one that has finished
    let installing = Manager(), installed = Manager()
    let output = "== intel-gpu-macos-driver 0.1.4: checks\n  ok    macOS 26.7.1\n== 0 problem(s)\n== backup: /Library/Application Support/intel-gpu-macos-driver/backup-20261109-101500\n"
        + "  ok    /Library/Extensions/IntelXeFB.kext\n  ok    /Library/Extensions/NMXAccel.kext\n  ok    /Library/GPUBundles/NVMTLDriverLib.bundle\n"
    installing.log = output; installing.busy = DriverAction.install.title
    installing.driverRun = DriverRun(action: .install, logStart: 0)
    installed.log = output + "== rebuilding the Auxiliary Kernel Collection (log: /tmp/auxkc)\n  ok    /Library/KernelCollections/AuxiliaryKernelExtensions.kc\n== done. Reboot.\n"
    var finishedRun = DriverRun(action: .install, logStart: 0); finishedRun.finished = true; finishedRun.succeeded = true; finishedRun.end = finishedRun.start.addingTimeInterval(207)
    installed.driverRun = finishedRun
    // the Monitor page with five minutes of made-up readings: a game started half way
    let monitor = MonitorModel()
    monitor.live = false
    let now = Date()
    for i in 0..<300 {
        let t = now.addingTimeInterval(Double(i - 299)), busy = i > 150
        let wave = sin(Double(i) / 9)
        monitor.clockHistory.append(MonitorPoint(t: t, v: busy ? 2650 + 150 * wave : (i % 40 < 6 ? 1900 : 0)))
        monitor.tempHistory.append(MonitorPoint(t: t, v: busy ? 58 + Double(i - 150) / 15 + 1.5 * wave : 44 + 0.8 * wave))
        monitor.fpsHistory.append(MonitorPoint(t: t, v: busy ? 160 + 18 * wave : (i % 40 < 6 ? 60 : 2)))
        monitor.powerHistory.append(MonitorPoint(t: t, v: busy ? 148 + 14 * wave : 36 + 0.6 * wave))
    }
    monitor.now = MonitorSample(fps: 171, actMHz: 2780, reqMHz: 2850, maxMHz: 2850, packageC: 67, vramC: 72, vramUsedMB: 3420,
                                cardW: 156.4, packageW: 118.9, cardLimitW: 190, mode: "2560x1440, 180 Hz")
    // the Report page while a report runs: the bar at its fourth section
    let reporting = Manager()
    reporting.log = "== step 1/9: system\n== step 2/9: pci\n== step 3/9: display\n== step 4/9: driver\n"; reporting.busy = reportTitle
    // the Tests page after a quick test with one result below expectations
    let tested = Manager()
    tested.testResults = [TestResult(title: "Compute (vector add)", value: "correct", state: .good),
                          TestResult(title: "Drawing", value: "correct", state: .good),
                          TestResult(title: "Memory bandwidth", value: "391 GB/s", state: .good),
                          TestResult(title: "FP32 compute", value: "9.1 TFLOPS", state: .warning, help: "Below what the card should reach: close other GPU work and run it again."),
                          TestResult(title: "HEVC decode", value: "correct", state: .good)]
    print("setup steps: driver page \(setupDriver.setupStep.number), last page \(setupDone.setupStep.number)")
    let pages: [(String, AnyView, CGFloat)] = [
        ("setup-driver", AnyView(SetupView(close: {}).environmentObject(setupDriver).background(Color(nsColor: .windowBackgroundColor))), 520), ("setup-done", AnyView(SetupView(close: {}).environmentObject(setupDone).background(Color(nsColor: .windowBackgroundColor))), 520),
        ("setup-opencore", AnyView(SetupView(close: {}).environmentObject(setupEFI).background(Color(nsColor: .windowBackgroundColor))), 520),
        ("setup-choose", AnyView(SetupView(close: {}).environmentObject(setupChoose).background(Color(nsColor: .windowBackgroundColor))), 520),
        ("system", AnyView(SystemView().environmentObject(manager)), 640), ("system-working", AnyView(SystemView().environmentObject(working)), 640),
        ("system-update", AnyView(SystemView().environmentObject(outdated)), 640),
        ("system-osupdate", AnyView(SystemView().environmentObject(updating)), 900),
        ("monitor", AnyView(MonitorView(model: monitor).environmentObject(working)), 640),
        ("tests", AnyView(TestsView().environmentObject(tested)), 520), ("report", AnyView(ReportView().environmentObject(tested)), 560),
        ("report-running", AnyView(ReportView().environmentObject(reporting)), 360),
        ("settings", AnyView(SettingsView().environmentObject(tested)), 760),
        ("about", AnyView(AboutView().frame(maxWidth: .infinity, maxHeight: .infinity).background(Color(nsColor: .windowBackgroundColor))), 760),
        ("install-running", AnyView(BusyBar().environmentObject(installing).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom).background(Color(nsColor: .windowBackgroundColor))), 330),
        ("install-done", AnyView(BusyBar().environmentObject(installed).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom).background(Color(nsColor: .windowBackgroundColor))), 360),
        ("setup-installing", AnyView(SetupView(close: {}).environmentObject(installing.copySetup(from: setupDriver)).background(Color(nsColor: .windowBackgroundColor))), 520), ("driver", AnyView(DriverView().environmentObject(manager)), 640),
        ("efi", AnyView(EFIView().environmentObject(manager)), 1500), ("efi-choose", AnyView(EFIView().environmentObject(choosing)), 640),
        ("zink", AnyView(ZinkView().environmentObject(manager)), 640),
        ("display-off", AnyView(DisplayView().environmentObject(hidpiOff)), 480), ("display-pending", AnyView(DisplayView().environmentObject(hidpiPending)), 480), ("banners", AnyView(Banners().environmentObject(choosing).frame(maxHeight: .infinity, alignment: .top).background(Color(nsColor: .windowBackgroundColor))), 260),
    ]
    // each page in the light appearance (name.png) and the dark one (name-dark.png), in a key window so that the
    // default and prominent buttons show their accent colour as they do in the app
    for (name, page, height) in pages {
        for (suffix, appearance) in [("", NSAppearance.Name.aqua), ("-dark", .darkAqua)] {
            let host = NSHostingView(rootView: page.frame(width: 640, height: height).environment(\.controlActiveState, .key))
            host.frame = NSRect(x: 0, y: 0, width: 640, height: height)
            let window = SnapshotWindow(contentRect: NSRect(x: -20000, y: -20000, width: 640, height: height), styleMask: [.borderless], backing: .buffered, defer: false)
            window.appearance = NSAppearance(named: appearance)
            window.contentView = host
            window.orderFrontRegardless()
            window.makeKey()
            host.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.5))
            if let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                host.cacheDisplay(in: host.bounds, to: bitmap)
                try? bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "\(folder)/\(name)\(suffix).png"))
            }
            window.orderOut(nil)
        }
    }
}

/// A borderless window that can be the key window, for the snapshots.
final class SnapshotWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

/// The fake EFI the EFI test leaves behind, as stage 1 leaves it, for the EFI page's snapshot.
var selfTestEFIFolder: String?

/// An EFI folder with OpenCore's layout and a config.plist with the parts efi.sh reads, as after stage 1.
func makeFakeEFI(in folder: URL) throws {
    let oc = folder.appendingPathComponent("EFI/OC")
    try FileManager.default.createDirectory(at: oc.appendingPathComponent("Kexts/Lilu.kext/Contents/MacOS"), withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: oc.appendingPathComponent("ACPI"), withIntermediateDirectories: true)
    try Data().write(to: oc.appendingPathComponent("OpenCore.efi"))
    let apple = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
    let config: [String: Any] = [
        "ACPI": ["Add": [["Comment": "SSDT-EC.aml", "Enabled": true, "Path": "SSDT-EC.aml"]]],
        "Booter": ["Quirks": ["ResizeAppleGpuBars": -1]],
        "Kernel": ["Add": [["Arch": "Any", "BundlePath": "Lilu.kext", "Comment": "", "Enabled": true, "ExecutablePath": "Contents/MacOS/Lilu",
                            "MaxKernel": "", "MinKernel": "", "PlistPath": "Contents/Info.plist"]],
                   "Quirks": ["DisableIoMapper": false]],
        "Misc": ["Security": ["ExposeSensitiveData": 6], "Debug": ["Target": 3, "DisplayLevel": 2147483650, "AppleDebug": false, "ApplePanic": false]],
        "NVRAM": ["Add": [apple: ["boot-args": "keepsyms=1", "csr-active-config": Data([0, 0, 0, 0])]], "Delete": [apple: ["boot-args"]]],
        "UEFI": ["Output": ["ReconnectGraphicsOnConnect": false]],
    ]
    let data = try PropertyListSerialization.data(fromPropertyList: config, format: .xml, options: 0)
    try data.write(to: oc.appendingPathComponent("config.plist"))
}

/// efi.sh through the app's own functions against a fake EFI: status, the plan and its hash, apply, the notice after an
/// install (the bug where the app asked to update an EFI kext that was already the package's), undo.
func efiSelfTest(package: Package, expect: (Bool, String) -> Void) {
    guard let script = efiScript(for: package) else { expect(false, "an efi.sh to run"); return }
    print("efi.sh: \(script.path)")
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("intelxe-manager-efi-\(getpid())")
    let folder = root.appendingPathComponent("efi")
    do { try makeFakeEFI(in: folder) } catch { expect(false, "fake EFI: \(error)"); return }
    let want = folder.standardizedFileURL.path
    expect(efiFolderRoot(for: folder) == want && efiFolderRoot(for: folder.appendingPathComponent("EFI")) == want
           && efiFolderRoot(for: folder.appendingPathComponent("EFI/OC")) == nil && efiFolderRoot(for: root) == nil,
           "a picked EFI folder, or the folder holding it, is the folder for efi.sh --efi; other folders are refused")
    setenv("INTELXE_STATE_DIR", root.appendingPathComponent("state").path, 1)
    setenv("INTELXE_BOOT_PATH", "", 1)
    setenv("INTELXE_CARD_ACPI_PATH", "\\_SB.PC00.PEG1.PEGP", 1)
    setenv("INTELXE_CPU_BRAND", "13th Gen Intel(R) Core(TM) i5-13400F", 1)   // a hybrid CPU: HybridPM.kext in the plan
    let target = EFITarget.folder(folder.path)
    var status = efiStatus(script: script, package: package, target: target)
    expect(status["efi_source"] == "folder" && status.isOpenCore && status["kext_file"] == "absent" && status["kext_entry"] == "absent",
           "efi.sh --status of a fake EFI after stage 1")
    expect(EFINotice.after(.install, status: status) == .kextMissing, "after an install, an EFI without IntelXe.kext is named")
    guard let plan = efiPlan(script: script, package: package, target: target, "--plan") else { expect(false, "efi.sh --plan"); return }
    print(plan.text, terminator: "")
    expect(plan.changes && plan.text.contains("Kernel > Add: add IntelXe.kext"), "the plan adds IntelXe.kext")
    expect(status["hybridpm_needed"] == "yes" && status.showsHybridPM && status["lilu_entry"] == "enabled", "efi.sh --status: a hybrid CPU needs HybridPM.kext, Lilu is on")
    if status["hybridpm_package"] == "yes" {
        expect(plan.text.contains("copy     EFI/OC/Kexts/HybridPM.kext") && plan.text.contains("Kernel > Add: add HybridPM.kext below Lilu.kext, as item 1"),
               "the plan adds HybridPM.kext below Lilu.kext")
    } else {
        expect(plan.text.contains("this package has no HybridPM.kext") && !plan.text.contains("EFI/OC/Kexts/HybridPM.kext"),
               "a package without HybridPM.kext: the plan says so and adds none")
    }
    let log = root.appendingPathComponent("efi.log")
    func runEFI(_ argument: String, _ hash: String) -> Bool {
        let words = efiWords(script: script, package: package, target: target, argument) + ["--expect", hash]
        return run(command: words.map(shellQuoted).joined(separator: " "), needsRoot: false, log: log, prompt: "").succeeded
    }
    expect(!runEFI("--apply", String(repeating: "0", count: 64)), "efi.sh refuses a plan that was not the one shown")
    expect(runEFI("--apply", plan.hash), "efi.sh --apply with the hash of the plan the app computed")
    status = efiStatus(script: script, package: package, target: target)
    expect(status["kext_file"] == "same" && status["kext_entry"] == "enabled", "after --apply the EFI's IntelXe.kext is the package's and on")
    if status["hybridpm_package"] == "yes" {
        expect(status["hybridpm_file"] == "same" && status["hybridpm_entry"] == "enabled", "after --apply the EFI's HybridPM.kext is the package's and on")
    }
    expect(EFINotice.after(.install, status: status) == nil, "after an install, no EFI notice when the EFI's IntelXe.kext is the package's")
    let executable = folder.appendingPathComponent("EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe")
    if let handle = try? FileHandle(forWritingTo: executable) { handle.seekToEndOfFile(); handle.write(Data("x".utf8)); try? handle.close() }
    status = efiStatus(script: script, package: package, target: target)
    expect(EFINotice.after(.install, status: status) == .kextDiffers, "after an install, a different EFI IntelXe.kext is named")
    if let undo = efiPlan(script: script, package: package, target: target, "--undo-plan") {
        expect(runEFI("--undo", undo.hash), "efi.sh --undo with the hash of its plan")
    } else { expect(false, "efi.sh --undo-plan") }
    status = efiStatus(script: script, package: package, target: target)
    expect(status["kext_file"] == "absent" && status["kext_entry"] == "absent" && status["hybridpm_file"] == "absent" && status["hybridpm_entry"] == "absent",
           "after --undo the EFI is as before")
    // boot logging: on, then off with its own change (not undo), each through its plan's hash
    expect(status.bootLogging == "off", "efi.sh --status: boot logging off on the fake EFI")
    for (action, want) in [(EFIAction.bootLog, "on"), (.bootLogOff, "off")] {
        guard let plan = efiPlan(script: script, package: package, target: target, action.planArgument!), plan.changes else {
            expect(false, "efi.sh \(action.planArgument!) has changes"); continue
        }
        expect(action != .bootLog || plan.text.contains("boot logging slows every boot"), "the boot logging plan says it slows every boot")
        expect(runEFI(action.argument, plan.hash), "efi.sh \(action.argument) with the hash of its plan")
        status = efiStatus(script: script, package: package, target: target)
        expect(status.bootLogging == want && efiPlan(script: script, package: package, target: target, action.planArgument!)?.changes == false,
               "after \(action.argument) boot logging is \(want), and its plan has nothing left")
    }
    selfTestEFIFolder = folder.path
}

func selfTest() {
    var failures = 0
    func expect(_ condition: Bool, _ what: String) {
        print(condition ? "ok    \(what)" : "FAIL  \(what)")
        if !condition { failures += 1 }
    }
    expect(Version("v0.1.2")! > Version("0.1.1")!, "0.1.2 is newer than 0.1.1")
    expect(Version("0.10.0")! > Version("0.9.9")!, "versions compare as numbers")
    expect(Version("v0.2") == nil && Version("0.1.x") == nil, "a tag that is not x.y.z is not a version")
    expect(listedHash("abc  a.zip\ndef *b.zip\n", name: "b.zip") == "def", "hash list with the binary mark")
    expect(listedHash("abc  a.zip\n", name: "c.zip") == nil, "a file not in the list has no hash")
    expect(shellQuoted("it's") == "'it'\\''s'", "shell quoting")
    let script = privilegedScript(command: "'/a b/in\"st.sh' --apply", log: "/l.log", prompt: "p")
    expect(script.contains("in\\\"st.sh") && script.hasSuffix("with administrator privileges"), "AppleScript quoting")
    expect(packageSuffix(macOSMajor: 26) == "-tahoe.zip" && packageSuffix(macOSMajor: 15) == "-sequoia.zip" && packageSuffix(macOSMajor: 27) == nil, "package per macOS")

    let parsed = EFIStatus(text: "efi_source=none\nkext_file=unknown\ncandidate=AAAA-1|disk0s1|WD Blue SN550|1000 GB||unknown\ncandidate=BBBB-2|disk3s1|USB|16 GB|/Volumes/EFI|yes\n")
    expect(!parsed.found && parsed.candidates.count == 2 && parsed.candidates[1].mountPoint == "/Volumes/EFI" && parsed.candidates[0].disk == "WD Blue SN550",
           "efi.sh --status with EFI partitions to choose from")
    expect(EFINotice.after(.install, status: parsed) == .unchecked, "after an install, an EFI that could not be read is 'not checked', not 'update it'")
    expect(EFINotice.after(.install, status: EFIStatus(text: "kext_file=same\nkext_entry=enabled")) == nil
           && EFINotice.after(.install, status: EFIStatus(text: "kext_file=different\nkext_entry=enabled")) == .kextDiffers
           && EFINotice.after(.install, status: EFIStatus(text: "kext_file=same\nkext_entry=disabled")) == .kextMissing
           && EFINotice.after(.uninstall, status: EFIStatus(text: "kext_file=same\nkext_entry=disabled")) == nil
           && EFINotice.after(.uninstall, status: EFIStatus(text: "kext_file=same\nkext_entry=enabled")) == .removeKext, "EFI notice after each action")
    expect(hybridCPU("13th Gen Intel(R) Core(TM) i5-13400F") && hybridCPU("Intel(R) Core(TM) i7-14700K") && hybridCPU("12th Gen Intel(R) Core(TM) i5-12500H")
           && hybridCPU("12th Gen Intel(R) Core(TM) i5-12600K") && !hybridCPU("12th Gen Intel(R) Core(TM) i5-12400F") && !hybridCPU("12th Gen Intel(R) Core(TM) i5-12600")
           && !hybridCPU("Intel(R) Core(TM) Ultra 7 155H") && !hybridCPU("13th Gen Intel(R) Core(TM) i3-13100F") && !hybridCPU("Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz")
           && !hybridCPU("Apple M2"), "a hybrid CPU by its name: 12th-14th gen with efficiency cores, not Core Ultra or P-core-only models")
    let b580 = gpuTestExpectations["e20b"], b570 = gpuTestExpectations["e20c"]
    expect(b580?.measured == true && b580?.tflops == 13 && b580?.tflopsGood == 8 && b580?.tflopsFair == 4
           && b580?.memoryGBs == 400 && b580?.memoryGood == 250 && b580?.memoryFair == 120
           && b570?.measured == false && gpuTestExpectations["56a0"] == nil && supportedCards.allSatisfy { gpuTestExpectations[$0.id] != nil },
           "the GPU test's expectations by device id: the B580's measured ones, the B570's from its specification, none for another card")
    let b580Clocks = defaultClockRange(rp0: 2850, rpn: 400), highFloor = defaultClockRange(rp0: 2500, rpn: 1300), lowTop = defaultClockRange(rp0: 1000, rpn: 300)
    expect(b580Clocks.min == 1200 && b580Clocks.max == 2850 && highFloor.min == 1300 && highFloor.max == 2500 && lowTop.min == 1000 && lowTop.max == 1000,
           "the default clock range: the 1200 MHz floor up to RP0, kept within RPn...RP0 (B580: 1200-2850 MHz)")
    if let d = MTLCreateSystemDefaultDevice() {
        print("gpu test here: '\(d.name)', PCI \(intelPCIDeviceID(registryID: d.registryID) ?? "-"), expectation \(gpuTestExpectation(for: d)?.name ?? "none")")
    }
    expect(EFIPlan(text: "EFI: x\nnothing to change\nnote     y\n").changes == false && EFIPlan(text: "EFI: x\ncopy     z\n").changes, "a plan with and without changes")
    expect(Bundle.main.url(forResource: "efi", withExtension: "sh") != nil, "the app carries efi.sh for packages made before it")
    expect(Bundle.main.url(forResource: "hidpi", withExtension: "sh") != nil, "the app carries hidpi.sh for packages made before it")
    expect(Bundle.main.url(forResource: "collect-macos", withExtension: "sh") != nil, "the app carries the diagnostics report script")
    print("health on this machine:", healthIssues(SystemFacts.probe()).map { $0.code })
    let hOn = HiDPIStatus(text: "hidpi_supported=1\nhidpi_nvram=1\nhidpi_override=ours\nhidpi_active=1\ndisplay=4c2d:750c\n")
    let hPending = HiDPIStatus(text: "hidpi_supported=1\nhidpi_nvram=1\nhidpi_override=ours\nhidpi_active=0\n")
    let hOther = HiDPIStatus(text: "hidpi_supported=1\nhidpi_nvram=0\nhidpi_override=other\nhidpi_active=0\n")
    let hOld = HiDPIStatus(text: "hidpi_supported=0\nhidpi_nvram=0\nhidpi_override=none\nhidpi_active=0\ndisplay=\n")
    expect(hOn.on && hOn.active && !hOn.pending && hPending.on && hPending.pending && !hOther.on && hOther.foreignOverride
           && !hOld.supported && hOld.known && !HiDPIStatus().known, "hidpi.sh --status: on, waiting for a restart, another tool's override, a driver before 0.1.3")
    if let script = Bundle.main.url(forResource: "hidpi", withExtension: "sh") {
        let here = hidpiStatus(script: script)
        print("hidpi here: \(here.values.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: " "))")
        expect(here.known && here["hidpi_supported"] != "", "hidpi.sh --status runs and answers")
    }

    // the macOS update guard (OSUpdate.swift, Resources/osupdate-guard.sh)
    let record: [String: Any] = ["RecommendedUpdates": [
        ["Display Name": "Command Line Tools for Xcode 27.2", "Display Version": "27.2", "Identifier": "Command Line Tools for Xcode 27.2", "Product Key": "142-50405"],
        ["Display Name": "macOS Tahoe 26.7.2", "Display Version": "26.7.2", "Identifier": "MSU_UPDATE_25H123_patch_26.7.2_minor", "MobileSoftwareUpdate": true],
        ["Display Name": "macOS 27", "Identifier": "MSU_UPDATE_26A428_patch_27.0_major", "MobileSoftwareUpdate": 1]]]
    let offered = offeredMacOSUpdates(record)
    expect(offered.map(\.version) == ["27.0", "26.7.2"] && offered[1].build == "25H123" && offered[0].build == "26A428" && offered[0].major == 27,
           "Software Update's record: the macOS updates with their builds, newest first, not the Command Line Tools")
    let listed = parseSoftwareUpdateList("Software Update Tool\n\nFinding available software\nSoftware Update found the following new or updated software:\n"
        + "* Label: macOS Tahoe 26.7.2-25H123\n\tTitle: macOS Tahoe 26.7.2, Version: 26.7.2, Size: 1234567KiB, Recommended: YES, Action: restart, \n"
        + "* Label: Command Line Tools for Xcode-27.2\n\tTitle: Command Line Tools for Xcode, Version: 27.2, Size: 1KiB, Recommended: YES, \n")
    expect(listed == [OfferedUpdate(version: "26.7.2", build: "25H123", title: "macOS Tahoe 26.7.2")], "softwareupdate --list: the macOS update and its build")
    expect(supportByMajor("26.7.2", packageMajor: 26) == .ok && supportByMajor("27.0", packageMajor: 26) == .no && supportByMajor("26.8", packageMajor: nil) == .no,
           "support by the package's macOS major version (packages before --supports-macos)")
    expect(Version(padded: "27") > Version(padded: "26.7.2") && Version(padded: "26.7") == Version("26.7.0")! && Version(padded: "x") == Version("0.0.0")!, "macOS versions with fewer parts")
    let guardStatus = keyValues("prepared=yes\nstate_from_version=26.7.1\nlast_result=ok\nlast_message=a=b\nlast_result=second\n")
    expect(guardStatus["last_message"] == "a=b" && guardStatus["prepared"] == "yes" && guardStatus["last_result"] == "ok", "osupdate-guard.sh's key=value lines")
    expect(osUpdateResultText("unsupported", ["last_to": "27.0 (26A1)", "last_package": "0.1.5"]).contains("27.0 (26A1)")
           && osUpdateResultText("ok", ["last_from": "26.7.1 (25H1)", "last_to": "26.7.2 (25H2)"]).contains("26.7.1 (25H1)"), "the update guard's results in words")
    expect(osGuardScript() != nil && Bundle.main.url(forResource: "gamemode-helper", withExtension: "sh") != nil, "the app carries osupdate-guard.sh and gamemode-helper.sh")
    if let script = osGuardScript(), let out = commandOutput("/bin/bash", [script.path, "--status"]) {
        expect(keyValues(out)["prepared"] != nil, "osupdate-guard.sh --status runs and answers (read only)")
    }
    if CommandLine.arguments.count > 2 {
        let p = Package(directory: URL(fileURLWithPath: CommandLine.arguments[2]))
        let major = Int(p.status()["package_macos"] ?? "")
        print("package support: 26.7.2 \(macOSSupport("26.7.2", package: p, packageMajor: major)), 27.0 \(macOSSupport("27.0", package: p, packageMajor: major))")
        expect(macOSSupport("27.0", package: p, packageMajor: major) == .no || major == 27, "the package does not support another major version")
    }
    // game mode (GameMode.swift, Resources/gamemode-helper.sh)
    expect(isGameCategory("public.app-category.games") && isGameCategory("public.app-category.action-games") && isGameCategory("PUBLIC.APP-CATEGORY.Strategy-Games")
           && !isGameCategory("public.app-category.utilities") && !isGameCategory(nil), "a game by its Info.plist category")
    expect(gameModeKey(bundleID: "com.example.game", executable: "x") == "com.example.game" && gameModeKey(bundleID: nil, executable: "java") == "java"
           && gameModeKey(bundleID: "", executable: "") == nil, "an app in the game list: bundle ID, else the executable's name")
    expect(gameModeHelperCommand(script: "/A b/it's.sh", appPID: 42, uid: 501, floor: 2400, stopBackups: true) == "/bin/bash '/A b/it'\\''s.sh' --run 42 501 2400 1 </dev/null &",
           "the game mode helper's command")
    let helperState = GameModeHelperState(text: "pid=1\napp=2\norig_min=1200\nfloor=2400\nphase=game\nstop_backups=0\n")
    expect(helperState.pid == 1 && helperState.origMin == 1200 && helperState.floor == 2400 && helperState.phase == "game" && helperState.alive
           && !GameModeHelperState(text: "pid=0\n").alive, "the game mode helper's state file")
    // game mode through the kext's lease (debug.intelxe.slpc_boost)
    expect(gameModeUsesKernelLease(available: true, stopBackups: false) && !gameModeUsesKernelLease(available: true, stopBackups: true)
           && !gameModeUsesKernelLease(available: false, stopBackups: false), "the lease without a helper only when the kext has it and Time Machine is left alone")
    expect(gameModeLeaseMHz(game: "Minecraft", floor: 2400) == 2400 && gameModeLeaseMHz(game: nil, floor: 2400) == 0, "the lease: the floor in a game, released on the desktop")
    expect(slpcBoostErrorIsTransient(0) && slpcBoostErrorIsTransient(EAGAIN) && slpcBoostErrorIsTransient(EBUSY)
           && !slpcBoostErrorIsTransient(EPERM) && !slpcBoostErrorIsTransient(ENOENT), "lease errors: a sleeping GPU and the rate limit pass, a refusal stops")
    if !slpcBoostAvailable() {
        expect(slpcBoostWrite(2400) != 0, "no lease without the kext's sysctl")
    }

    let sample = #"{"tag_name":"v0.1.2","html_url":"https://example.invalid/r","body":"notes","prerelease":false,"unknown":1,"assets":[{"name":"intel-gpu-macos-driver-0.1.2-tahoe.zip","browser_download_url":"https://example.invalid/a.zip","size":5,"state":"uploaded"},{"name":"intel-gpu-macos-driver-0.1.2.sha256","browser_download_url":"https://example.invalid/s","size":1}]}"#
    let decoded = try? JSONDecoder().decode(Release.self, from: Data(sample.utf8))
    expect(decoded?.version == Version("0.1.2") && decoded?.asset("-tahoe.zip") != nil && decoded?.asset(".sha256") != nil && decoded?.asset("-zink-gl.zip") == nil, "a release as GitHub's API sends it")

    // release signatures (Signing.swift): a list signed with the project's key verifies, a changed list or signature does not
    let signedList = Data("intel-gpu-macos-driver selftest list\n".utf8)
    let goodSignature = "sG8t9iThA+B3qjfc6FM68/zDaUgkU52zyozwZ4xBTq5Nw2avuL5YtbEQD1wIDhAXDVedCT9g7qvxI0VfhND6BQ=="
    expect(releaseSignatureValid(list: signedList, signature: goodSignature + "\n"), "a list signed with the project's key verifies")
    expect(!releaseSignatureValid(list: Data("intel-gpu-macos-driver selftest lisT\n".utf8), signature: goodSignature), "a changed list does not verify")
    expect(!releaseSignatureValid(list: signedList, signature: "A" + goodSignature.dropFirst()), "a changed signature does not verify")
    expect(!releaseSignatureValid(list: signedList, signature: "not base64"), "a signature that is not base64 does not verify")
    expect(releaseSignatureValid(list: signedList, signature: "A" + goodSignature.dropFirst() + "\n" + goodSignature + "\n"),
           "a .sigs file verifies when one of its lines is the project's signature")
    expect(!releaseSignatureValid(list: signedList, signature: "A" + goodSignature.dropFirst() + "\nnot base64\n"),
           "a .sigs file with no valid line does not verify")

    let facts = SystemFacts.probe()
    print("facts macOS \(facts.macOS), \(facts.architecture), card \(facts.hasCard), kext \(facts.kextLoaded), metal '\(facts.metalDevice)', sip '\(facts.sipText)', installed \(facts.installedVersion ?? "-"), zink \(facts.zinkInstalled)/\(facts.zinkGlobal)")
    for check in facts.checks { print("check \(check.id): \(check.state) \(check.value)") }

    if CommandLine.arguments.count > 2 {
        let package = Package(directory: URL(fileURLWithPath: CommandLine.arguments[2]))
        expect(package.isValid, "package folder is valid")
        print("status \(package.status().sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: " "))")
        let log = FileManager.default.temporaryDirectory.appendingPathComponent("intelxe-manager-selftest-\(getpid()).log")
        let ran = run(command: "\(shellQuoted(package.script.path)) --check", needsRoot: false, log: log, prompt: "")
        let text = (try? String(contentsOf: log, encoding: .utf8)) ?? ""
        print(text.split(separator: "\n").suffix(3).joined(separator: "\n"))
        expect(text.contains("checks") && !ran.cancelled, "ran the package's check and read its log")
        try? FileManager.default.removeItem(at: log)
        efiSelfTest(package: package, expect: expect)
    }

    if let at = CommandLine.arguments.firstIndex(of: "--snapshot"), CommandLine.arguments.count > at + 1 {
        MainActor.assumeIsolated { renderSnapshots(into: CommandLine.arguments[at + 1]) }
    }

    let semaphore = DispatchSemaphore(value: 0)
    Task.detached {
        do {
            let release = try await latestRelease()
            print("latest \(release.tag_name), assets \(release.assets.map(\.name).joined(separator: " "))")
            expect(release.version != nil, "the latest tag is a version")
        } catch { print("no network answer: \(error.localizedDescription)") }
        semaphore.signal()
    }
    semaphore.wait()
    if let folder = selfTestEFIFolder { try? FileManager.default.removeItem(at: URL(fileURLWithPath: folder).deletingLastPathComponent()) }
    print(failures == 0 ? "selftest passed" : "selftest: \(failures) failure(s)")
    exit(failures == 0 ? 0 : 1)
}

extension Manager {
    /// this manager with another's facts and package, for a snapshot of the setup assistant during a run
    @MainActor func copySetup(from other: Manager) -> Manager { facts = other.facts; package = other.package; return self }
}
