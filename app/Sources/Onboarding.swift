// SPDX-License-Identifier: MIT
// The setup assistant: from a fresh macOS install (OpenCore with the B580 on its firmware's framebuffer, the EFI on a
// USB stick or a disk) to a verified driver, one page at a time. The page is worked out from the machine each time the
// assistant opens, so it picks up where the last restart left it: the EFI settings (SIP, AMFI, quirks: efi.sh stage 2),
// the driver package (install.sh), IntelXe.kext in the EFI (stage 4), then a GPU test. Every change goes through the
// same scripts and password dialogs as the other pages; nothing here writes on its own.
import AppKit
import SwiftUI

enum SetupStep: Equatable {
    case blocked(String)
    case efiSettings, driver, efiKext, verify, done

    var number: Int {
        switch self { case .blocked: return 0; case .efiSettings: return 1; case .driver: return 2; case .efiKext: return 3; case .verify: return 4; case .done: return 5 }
    }
}

extension Manager {
    /// Where the machine stands, from what refresh() found.
    var setupStep: SetupStep {
        let f = facts
        if f.architecture != "x86_64" { return .blocked(String(localized: "This Mac is not an x86_64 machine: the driver is for Intel-based hackintoshes.")) }
        if f.macOSMajor != 26 && f.macOSMajor != 15 { return .blocked(String(localized: "The driver has packages for macOS 26 Tahoe and macOS 15 Sequoia; this is macOS \(f.macOS).")) }
        if !f.hasCard { return .blocked(String(localized: "No Arc B580 or B570 was found on the PCI bus. Check that the card is seated, has its power cables, and that the BIOS has Above 4G decoding and Resizable BAR on.")) }
        if !(f.sipRelaxed && f.amfiRelaxed) { return .efiSettings }
        if f.installedVersion == nil && !f.driverFilesPresent { return .driver }
        if !f.kextLoaded { return .efiKext }
        if testResults.isEmpty || testing != nil { return .verify }
        return .done
    }

    func restartNow() {
        NSAppleScript(source: "tell application \"System Events\" to restart")?.executeAndReturnError(nil)
    }
}

final class SetupState: ObservableObject {
    @Published var downloading = false
    @Published var confirmInstall = false
    @Published var loginCheck = loginCheckEnabled()
    /// step 1 with the Safari setting (SecureBootModel) in the same EFI change: one backup, one restart
    @Published var withSafari = false
    @Published var confirmSafariEFI = false
    @Published var confirmSSV: SSVAction?
    @Published var downloadingZink = false
}

struct SetupView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var st = SetupState()
    var close: () -> Void

    var body: some View {
        let step = manager.setupStep
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Setup assistant").font(.title2.bold())
                Spacer()
                // the page headings carry the same numbers (1. Prepare OpenCore ... 5. Done)
                Text(step.number == 0 ? String(localized: "Before you start") : String(localized: "Step \(step.number) of 5")).foregroundStyle(.secondary)
            }
            StepIndicator(current: step.number)
            Divider()
            ScrollView { VStack(alignment: .leading, spacing: 12) { page(step) }.frame(maxWidth: .infinity, alignment: .leading) }
                .disabled(manager.busy != nil)
            // the install's steps stay usable (its log can be opened) while the rest waits for it
            if let run = manager.driverRun, !run.dismissed {
                DriverRunView(run: run, closable: true)
                    .padding(12)
                    .background(Color.primary.opacity(0.04), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            } else if let busy = manager.busy { HStack { ProgressView().controlSize(.small); Text(busy).foregroundStyle(.secondary) } }
            if let r = manager.lastResult { Text(r).font(.callout) }
            Divider()
            HStack {
                Button("Check again") { Task { await manager.refresh() } }.disabled(manager.probing || manager.busy != nil)
                Spacer()
                if step == .done {
                    Button("Finish") { UserDefaults.standard.set(true, forKey: "setupDone"); close() }
                        .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                        .disabled(manager.busy != nil)
                } else {
                    Button("Close, continue later") { close() }.disabled(manager.busy != nil)
                }
            }
        }
        .padding(22)
        .frame(width: 620, height: 520)
    }

    @ViewBuilder func page(_ step: SetupStep) -> some View {
        switch step {
        case .blocked(let why):
            Label(why, systemImage: "xmark.octagon").foregroundStyle(.red)
            Text("The assistant continues once this is solved. The install guide covers the BIOS and OpenCore preparation.")
            Link("Open the install guide", destination: URL(string: "https://github.com/\(repository)/blob/main/release/README-INSTALL.md")!)
        case .efiSettings:
            Text("1. Prepare OpenCore").font(.headline)
            Text("The driver needs System Integrity Protection relaxed and AMFI's boot-arg in your OpenCore config, plus a few quirks. The assistant writes them into the EFI you booted from, after a backup, and shows every change before it makes it.")
            CaptionedToggle(title: "Also prepare Safari on the GPU", caption: "Turns OpenCore's Apple Secure Boot off in the same change: no extra restart later.",
                            detail: "Safari draws on the GPU only with a second copy of the Metal driver on the system volume, which the last page can install. Apple Secure Boot refuses to boot such a system volume, so it has to be off. It is a macOS security feature; leave this off if you do not want Safari on the GPU. The EFI page can change it later.",
                            isOn: $st.withSafari)
            efiStep(st.withSafari ? .settingsSafari : .settings)
        case .driver:
            Text("2. Install the driver").font(.headline)
            Text("The package puts the framebuffer, the accelerator and the Metal driver into /Library and rebuilds the kernel collection (a few minutes). It backs up what it replaces.")
            if let p = manager.package {
                Label(String(localized: "Package \(p.directory.lastPathComponent) is ready."), systemImage: "checkmark.circle").foregroundStyle(.green)
                Button("Install the driver") { st.confirmInstall = true }.buttonStyle(.borderedProminent)
                    .alert(DriverAction.install.title, isPresented: $st.confirmInstall) {
                        Button(DriverAction.install.title) { manager.perform(.install) }
                        Button("Cancel", role: .cancel) {}
                    } message: { Text(DriverAction.install.explanation) }
            } else if let rel = manager.release {
                Button(String(localized: "Download \(rel.tag_name)")) {
                    st.downloading = true
                    Task { await manager.downloadLatest(kind: .driver); st.downloading = false }
                }.buttonStyle(.borderedProminent).disabled(st.downloading)
                if st.downloading { ProgressView().controlSize(.small) }
            } else {
                Text(manager.releaseError ?? String(localized: "Looking for the latest release…")).foregroundStyle(.secondary)
                Button("Look again") { Task { await manager.checkForUpdate() } }
            }
            Text("When it has finished: restart, then open this assistant again.").font(.callout).foregroundStyle(.secondary)
            if manager.rebootNeeded { Button("Restart now") { manager.restartNow() }.buttonStyle(.borderedProminent) }
        case .efiKext:
            Text("3. Add IntelXe.kext to OpenCore").font(.headline)
            Text("The last piece loads before macOS: IntelXe.kext and SSDT-ARCOFF go into the EFI and its config, and on a 12th-14th gen Intel CPU with efficiency cores HybridPM.kext too. The EFI page shows the plan and applies it.")
            efiStep(.apply)
            Text("If the next boot stops at the Apple logo, choose the previous entry or your USB stick in the OpenCore picker: the backup is kept, and the EFI page can put it back.")
                .font(.callout).foregroundStyle(.secondary)
        case .verify:
            Text("4. Check that it works").font(.headline)
            Text("The driver is loaded. A quick test checks that the GPU computes, draws and decodes video correctly and measures it.")
            Button("Run the quick GPU test") { manager.runTest() }.buttonStyle(.borderedProminent).disabled(manager.testing != nil)
            if let s = manager.testing { HStack { ProgressView().controlSize(.small); Text(s).foregroundStyle(.secondary) } }
        case .done:
            let bad = manager.testResults.filter { $0.state == .bad }.count, warn = manager.testResults.filter { $0.state == .warning }.count
            Text("5. Done").font(.headline)
            if bad == 0 {
                Label(warn == 0 ? String(localized: "Everything works.") : String(localized: "It works; \(warn) result(s) are below what the card should reach (see the Report page)."),
                      systemImage: "checkmark.seal").foregroundStyle(warn == 0 ? .green : .orange)
            } else {
                Label(String(localized: "\(bad) test(s) failed. The Report page shows which, and makes a diagnostics report for a bug report."), systemImage: "exclamationmark.triangle").foregroundStyle(.red)
            }
            Text("Optional extras:").padding(.top, 6)
            safariExtra
            zinkExtra
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)], spacing: 10) {
                NextStepCard(symbol: "display", title: "HiDPI sizes and Variable refresh", caption: Text("Display page")) { manager.requestedSection = .display; close() }
                NextStepCard(symbol: "gauge.medium", title: "GPU clock and temperature in the menu bar", caption: Text("Monitor page")) { manager.requestedSection = .monitor; close() }
            }
            if manager.rebootNeeded {
                HStack {
                    Label("Restart to use the change.", systemImage: "arrow.clockwise.circle").foregroundStyle(.orange)
                    Spacer()
                    Button("Restart now") { manager.restartNow() }.buttonStyle(.borderedProminent)
                }
            }
            CaptionedToggle(title: "Check the driver at login", caption: "A dialog after login only when something needs attention, such as a macOS update that removed the system-volume copy.",
                            isOn: Binding(get: { st.loginCheck }, set: { setLoginCheck($0); st.loginCheck = loginCheckEnabled() }))
                .padding(.top, 4)
            if manager.efi["efi_disk_location"].lowercased().contains("external") {
                Label("Your EFI is on a USB stick. Once everything works you can copy it to the EFI partition of the macOS disk (install guide, the last stage), and keep the stick as a fallback.",
                      systemImage: "externaldrive").font(.callout)
            }
        }
    }

    /// The package is needed first (the EFI files and efi.sh come from it): download it here when it is missing.
    @ViewBuilder var packageGate: some View {
        if let rel = manager.release {
            Text("First the driver package is downloaded: the EFI files come from it.").font(.callout)
            Button(String(localized: "Download \(rel.tag_name)")) {
                st.downloading = true
                Task { await manager.downloadLatest(kind: .driver); st.downloading = false; await manager.refresh() }
            }.buttonStyle(.borderedProminent).disabled(st.downloading)
            if st.downloading { ProgressView().controlSize(.small) }
        } else {
            Text(manager.releaseError ?? String(localized: "Looking for the latest release…")).foregroundStyle(.secondary)
            Button("Look again") { Task { await manager.checkForUpdate() } }
        }
    }

    /// An EFI change in place: where, the plan efi.sh made (exactly what will change), Apply, then restart.
    @ViewBuilder func efiStep(_ action: EFIAction) -> some View {
        if manager.package == nil {
            packageGate
        } else {
            efiLocation
            let e = manager.efi
            if e.found && !e.mounted {
                Button("Mount the EFI partition (asks for your password)") { manager.performEFI(.mount) }.buttonStyle(.borderedProminent)
            } else if e.found, let plan = manager.efiPlans[action] {
                Text("What will change (a backup of the whole EFI folder is made first):").font(.callout)
                ScrollView { PlanText(plan: plan).padding(8) }
                    .frame(maxHeight: 170)
                    .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
                if plan.changes {
                    Button(String(localized: "Apply (asks for your password)")) { manager.performEFI(action) }.buttonStyle(.borderedProminent)
                } else {
                    Label("Nothing left to change in the EFI.", systemImage: "checkmark.circle").foregroundStyle(.green)
                }
            } else if e.found {
                Text("Reading the EFI…").foregroundStyle(.secondary)
            } else {
                efiChooser
            }
            Text("Then restart and open this assistant again: it continues from there.").font(.callout).foregroundStyle(.secondary)
            if manager.rebootNeeded { Button("Restart now") { manager.restartNow() }.buttonStyle(.borderedProminent) }
            else { Button("Restart now") { manager.restartNow() } }
        }
    }

    @ViewBuilder var efiLocation: some View {
        let e = manager.efi
        if e.found {
            let usb = e["efi_disk_location"].lowercased().contains("external")
            Label(String(localized: "OpenCore EFI: \(e["efi_description"].isEmpty ? e["efi_partition"] : e["efi_description"])") + (usb ? String(localized: " (USB stick: that is fine)") : ""),
                  systemImage: usb ? "externaldrive" : "internaldrive")
        } else {
            Label("OpenCore did not record which EFI it booted from (it does from step 3 on): choose it below.", systemImage: "questionmark.circle").foregroundStyle(.orange)
        }
    }

    /// The EFI partitions efi.sh found, to choose from here when OpenCore's boot-path is missing (a fresh EFI has
    /// ExposeSensitiveData without it), or a folder such as a USB stick's.
    @ViewBuilder var efiChooser: some View {
        let candidates = manager.efi.candidates
        if candidates.isEmpty {
            Text("No EFI partition was found. Choose the EFI folder instead, for example on a USB stick.").font(.callout)
        } else {
            ForEach(candidates) { c in
                Button { manager.chooseEFIPartition(c.uuid) } label: {
                    Label(c.label + (c.openCore == "yes" ? " · OpenCore" : ""), systemImage: "internaldrive")
                }
            }
        }
        Button("Choose a folder…") { manager.pickEFIFolder() }
    }

    /// Safari on the GPU, from the last page: Apple Secure Boot off in the EFI (a restart), then the system-volume copy
    /// (a restart). The same scripts and confirmations as the EFI and Driver pages.
    @ViewBuilder var safariExtra: some View {
        let s = manager.ssv, e = manager.efi
        let done = s.state == .inUse || s.state == .notPackage
        let secureBootOff = e["secure_boot_model"] == "Disabled"
        ExtraRow(symbol: "safari", title: "Safari on the GPU",
                 caption: done ? Text("Installed.")
                     : !secureBootOff ? Text("First Apple Secure Boot goes off in the EFI, then a restart.")
                     : Text("Then a second copy of the Metal driver goes on the system volume (a few minutes), and a restart. A macOS update removes it again."),
                 detail: Text("Without it Safari works, drawing in software. Apple Secure Boot is a macOS security feature that refuses a system volume with a changed seal.")) {
            if done {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            } else if !secureBootOff {
                if !e.found {
                    Button("Open the EFI page") { manager.requestedSection = .efi; close() }
                } else if !e.mounted && e["efi_source"] != "folder" {
                    Button("Mount the EFI") { manager.performEFI(.mount) }
                } else {
                    Button(EFIAction.safari.title) { st.confirmSafariEFI = true }.disabled(manager.efiPlans[.safari]?.changes != true)
                }
            } else if s.packageScript == nil {
                Button("Driver page") { manager.requestedSection = .driver; close() }
            } else {
                let action: SSVAction = s.state == .notUsed ? .use : .install
                Button(action.title) { st.confirmSSV = action }
            }
        }
        .sheet(isPresented: $st.confirmSafariEFI) {
            EFIConfirmSheet(action: .safari, plan: manager.efiPlans[.safari], disk: e["efi_description"]) { manager.performEFI(.safari) }
        }
        .alert(st.confirmSSV?.title ?? "", isPresented: Binding(get: { st.confirmSSV != nil }, set: { if !$0 { st.confirmSSV = nil } }), presenting: st.confirmSSV) { action in
            Button(action.title) { manager.performSSV(action) }
            Button("Cancel", role: .cancel) {}
        } message: { action in Text(action.explanation) }
    }

    /// Zink for Java games, from the last page: download and install it, then the switch that makes LWJGL 3 games use it.
    @ViewBuilder var zinkExtra: some View {
        let f = manager.facts
        ExtraRow(symbol: "gamecontroller", title: "Zink for Java games",
                 caption: Text("Minecraft and other LWJGL games draw through Vulkan: much faster. Native Mac apps and OpenCL are not affected."),
                 detail: Text("Zink is Mesa's OpenGL on Vulkan. The switch sets it for every LWJGL 3 Java application started afterwards; the OpenGL page has the test and the per-version option.")) {
            if !f.zinkInstalled {
                if manager.zinkPackage == nil {
                    Button("Download Zink") {
                        st.downloadingZink = true
                        Task { await manager.downloadLatest(kind: .zink); st.downloadingZink = false }
                    }.disabled(st.downloadingZink || manager.release == nil)
                } else {
                    Button("Install Zink") { manager.performZink(.install) }
                }
            } else {
                Toggle("Use by default", isOn: Binding(get: { f.zinkGlobal }, set: { manager.performZink($0 ? .globalOn : .globalOff) }))
                    .toggleStyle(.switch).fixedSize()
            }
        }
    }
}

/// An optional extra on the assistant's last page: icon, what it is and where it stands, and its action on the right.
struct ExtraRow<Trailing: View>: View {
    let symbol: String
    let title: LocalizedStringKey
    let caption: Text
    var detail: Text?
    @ViewBuilder let trailing: () -> Trailing
    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: symbol).font(.title2).foregroundStyle(Color.accentColor).frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout.weight(.medium))
                caption.font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            .help(detail ?? caption)
            Spacer(minLength: 8)
            trailing()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .tintedCard(neutralTint)
    }
}

/// The assistant's five steps as numbered dots on a line: done ones filled with a check, the current one ringed, in the
/// accent colour; before the first step (something blocks it) all are grey.
struct StepIndicator: View {
    let current: Int
    static let labels: [LocalizedStringKey] = ["OpenCore", "Driver", "EFI kext", "Test", "Ready"]
    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            ForEach(1...5, id: \.self) { n in
                let done = n < current || (n == 5 && current == 5)
                VStack(spacing: 5) {
                    ZStack {
                        Circle().fill(Color(nsColor: .windowBackgroundColor))
                        Circle().fill(done ? Color.accentColor : n == current ? Color.accentColor.opacity(0.15) : Color.secondary.opacity(0.12))
                        Circle().strokeBorder(n <= current ? Color.accentColor : Color.secondary.opacity(0.4), lineWidth: 1.5)
                        if done {
                            Image(systemName: "checkmark").font(.caption.weight(.bold)).foregroundStyle(.white)
                        } else {
                            Text(verbatim: "\(n)").font(.caption.weight(.semibold)).foregroundStyle(n == current ? Color.accentColor : .secondary)
                        }
                    }
                    .frame(width: 24, height: 24)
                    Text(Self.labels[n - 1]).font(.caption).foregroundStyle(n == current ? .primary : .secondary)
                        .fontWeight(n == current ? .semibold : .regular).lineLimit(1).fixedSize()
                }
                .frame(maxWidth: .infinity)
            }
        }
        .background(alignment: .topLeading) {
            // the line from the first dot's centre to the last one's, accent up to the current step
            GeometryReader { g in
                let column = g.size.width / 5
                Rectangle().fill(Color.secondary.opacity(0.3))
                    .frame(width: column * 4, height: 2).offset(x: column / 2, y: 11)
                Rectangle().fill(Color.accentColor)
                    .frame(width: column * CGFloat(max(0, min(current, 5) - 1)), height: 2).offset(x: column / 2, y: 11)
            }
        }
        .animation(.easeInOut(duration: 0.35), value: current)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(current == 0 ? Text("Before you start") : Text("Step \(current) of 5"))
    }
}

/// An optional next step on the assistant's last page: a small card with an icon, what it is and where, as a button.
struct NextStepCard: View {
    let symbol: String
    let title: LocalizedStringKey
    let caption: Text
    var detail: Text?
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: symbol).font(.title2).foregroundStyle(Color.accentColor).frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.callout.weight(.medium)).foregroundStyle(.primary).fixedSize(horizontal: false, vertical: true)
                    caption.font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, minHeight: 50, alignment: .topLeading)
            .contentShape(Rectangle())
            .tintedCard(neutralTint)
        }
        .buttonStyle(.plain)
        .help(detail ?? caption)
    }
}
