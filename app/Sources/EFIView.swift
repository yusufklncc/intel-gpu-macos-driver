// SPDX-License-Identifier: MIT
// The EFI page: stages 2 and 4 of the install guide on the EFI that OpenCore booted from, through efi.sh; the Safari
// setting, opt-in boot logging and undo.
// Every write shows the disk and efi.sh's plan first, and goes through the administrator password dialog.
import AppKit
import SwiftUI

extension EFINotice {
    var text: LocalizedStringKey {
        switch self {
        case .kextDiffers: return "The IntelXe.kext in your EFI differs from this package's. Update it on the EFI page before you restart."
        case .kextMissing: return "Your EFI has no IntelXe.kext, or its config.plist entry is off. Add it on the EFI page before you restart."
        case .unchecked: return "The IntelXe.kext in your EFI could not be checked: the EFI partition is not mounted or was not found. Check it on the EFI page before you restart."
        case .removeKext: return "Remove IntelXe.kext from your EFI before you restart, or macOS comes up without the driver. The EFI page can undo its last change."
        }
    }
}

extension EFIAction {
    var title: String {
        switch self {
        case .mount: return String(localized: "Mount the EFI partition")
        case .apply: return String(localized: "Make these changes")
        case .settings: return String(localized: "Change these settings")
        case .safari: return String(localized: "Turn Apple Secure Boot off")
        case .settingsSafari: return String(localized: "Change these settings")
        case .bootLog: return String(localized: "Turn boot logging on")
        case .bootLogOff: return String(localized: "Turn boot logging off")
        case .undo: return String(localized: "Undo the last EFI change")
        }
    }
    var explanation: String {
        switch self {
        case .mount: return String(localized: "Mounts the partition so that its files can be read and changed. Nothing is written to it.")
        case .apply, .settings, .safari, .settingsSafari, .bootLog, .bootLogOff: return String(localized: "Backs up the whole EFI folder first, then makes exactly these changes. If the EFI changed since this list was made, nothing is written. Restart afterwards.")
        case .undo: return String(localized: "Puts back, from the last backup, the files that change replaced. The backup stays. Restart afterwards.")
        }
    }
    var destructive: Bool { self == .undo }
}

final class EFIPageState: ObservableObject {
    @Published var confirming: EFIAction?
}

struct EFIView: View {
    @EnvironmentObject var manager: Manager
    @StateObject var page = EFIPageState()
    var efi: EFIStatus { manager.efi }

    var body: some View {
        Form {
            if manager.package == nil {
                Section2(title: "Package", symbol: "shippingbox") {
                    Text("Choose a package on the Driver page first: IntelXe.kext and SSDT-ARCOFF.aml come from it.").foregroundStyle(.secondary)
                    Button("Open the Driver page") { manager.requestedSection = .driver }
                }
            } else {
                partition
                // nothing below can be read until the partition is mounted: the one hint for that is in the section above
                if efi.found && (efi.mounted || efi["efi_source"] == "folder") {
                    settings
                    files
                    safari
                    bootLogging
                    undo
                }
            }
            if let result = manager.lastResult {
                Text(result).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        // changes made outside the app (another tool, an editor): look when the page shows, when the app comes back to
        // the front, and every few seconds while the page is open
        .task { await manager.refreshEFIIfChanged() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            Task { await manager.refreshEFIIfChanged() }
        }
        .onReceive(Timer.publish(every: 5, on: .main, in: .common).autoconnect()) { _ in
            if page.confirming == nil { Task { await manager.refreshEFIIfChanged() } }
        }
        .sheet(item: $page.confirming) { action in
            EFIConfirmSheet(action: action, plan: manager.efiPlans[action], disk: efi["efi_description"]) { manager.performEFI(action) }
        }
    }

    var partition: some View {
        Section2(title: "EFI partition", symbol: "internaldrive") {
            Caption("Stages 2 and 4 of the install guide, on your OpenCore EFI.",
                    detail: "For a Mac installed with a plain OpenCore EFI (stage 1 of the install guide): this page does stages 2 and 4 on the EFI that OpenCore booted from, or on an EFI folder you choose, such as a USB stick's.")
            if !efi.known {
                Text(manager.probing ? String(localized: "Checking…") : "–").foregroundStyle(.secondary)
            } else {
                if efi.found {
                    if efi["efi_source"] == "folder" {
                        PathRow(title: "Folder", path: diskText)
                    } else {
                        LabeledContent("Disk") { Text(diskText).textSelection(.enabled).multilineTextAlignment(.trailing) }
                    }
                    if !efi["efi_device"].isEmpty {
                        LabeledContent("Partition") { Text("\(efi["efi_device"]) · \(efi["efi_partition"])").textSelection(.enabled) }
                    }
                    LabeledContent("Found through") { Text(sourceText) }
                    if efi["efi_source"] != "folder" {
                        LabeledContent("Mounted") { Text(efi.mounted ? efi["efi_mount_point"] : String(localized: "No")).textSelection(.enabled) }
                    }
                    LabeledContent("OpenCore") { Text(openCoreText) }
                    if !efi.mounted && efi["efi_source"] != "folder" {
                        HStack(alignment: .firstTextBaseline, spacing: 14) {
                            Text("Mount the EFI partition to see what stages 2 and 4 would change. Mounting changes nothing.")
                                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                            Spacer()
                            Button(EFIAction.mount.title + "…") { page.confirming = .mount }.buttonStyle(.borderedProminent).fixedSize()
                        }
                    }
                }
                if efi["efi_source"] == "none" || efi["efi_source"] == "chosen" {
                    Caption("OpenCore did not record which EFI it booted from: choose it by its disk.",
                            detail: "OpenCore did not record which EFI it booted from (Misc > Security > ExposeSensitiveData without 1). Choose the partition by its disk. The changes below turn that record on for the next boot.")
                    if efi.candidates.isEmpty {
                        Text("No EFI partition found.").foregroundStyle(.secondary)
                    } else {
                        Picker("EFI partition", selection: Binding(get: { manager.efiChoice ?? "" }, set: { manager.chooseEFIPartition($0) })) {
                            Text("Choose…").tag("")
                            ForEach(efi.candidates) { candidate in Text(candidate.label).tag(candidate.uuid) }
                        }
                    }
                }
            }
            HStack(alignment: .firstTextBaseline, spacing: 14) {
                Text(manager.efiFolder != nil ? String(localized: "This page works on a folder you chose, not on a partition.")
                                              : String(localized: "Another EFI, such as a USB stick's or a copy:"))
                    .font(.callout).foregroundStyle(.secondary)
                Spacer()
                if manager.efiFolder != nil {
                    Button("Use the EFI partition instead") { manager.useEFIPartition() }.fixedSize()
                }
                Button("Use an EFI folder…") { manager.pickEFIFolder() }.fixedSize()
            }
        }
    }

    var files: some View {
        Section2(title: "IntelXe.kext and SSDT-ARCOFF (stage 4)", symbol: "puzzlepiece.extension") {
            LabeledContent("IntelXe.kext in the EFI") { Text(kextText).multilineTextAlignment(.trailing) }
            LabeledContent("Kernel > Add entry") { Text(entryText(efi["kext_entry"])) }
            LabeledContent("SSDT-ARCOFF.aml in the EFI") { Text(ssdtText).multilineTextAlignment(.trailing) }
            LabeledContent("ACPI > Add entry") { Text(entryText(efi["ssdt_entry"])) }
            LabeledContent("The card's ACPI path") { Text(cardText).textSelection(.enabled).multilineTextAlignment(.trailing) }
            if efi.showsHybridPM {
                LabeledContent("HybridPM.kext needed") { Text(hpmNeededText).multilineTextAlignment(.trailing) }
                LabeledContent("HybridPM.kext in the EFI") { Text(hpmText).multilineTextAlignment(.trailing) }
                LabeledContent("HybridPM Kernel > Add entry") { Text(entryText(efi["hybridpm_entry"])) }
            }
            planOrHint(.apply)
            applyButton(.apply)
        }
    }

    var settings: some View {
        Section2(title: "Settings (stage 2)", symbol: "slider.horizontal.3") {
            Caption("SIP, the AMFI boot-arg and the GPU quirks, as stage 2 sets them.",
                    detail: "SIP (csr-active-config), the boot-args amfi_get_out_of_my_way=0x1 and intelxe_surfvram=2, ResizeAppleGpuBars, DisableIoMapper and ReconnectGraphicsOnConnect, as stage 2 of the install guide sets them. Separate from the files above; your other settings stay as they are.")
            planOrHint(.settings)
            applyButton(.settings)
        }
    }

    /// only for the system-volume copy (Driver page, Safari and WebKit)
    var safari: some View {
        Section2(title: "Safari on the GPU (optional)", symbol: "safari") {
            Caption("Only for the system-volume copy of the Metal driver (Driver page).",
                    detail: "The system-volume copy of the Metal driver (Driver page) changes the system volume's seal. OpenCore's Apple Secure Boot (Misc > Security > SecureBootModel) refuses to start macOS from such a volume, so it must be Disabled first. Only for the system-volume copy: without it, leave it as it is.")
            LabeledContent("SecureBootModel") { Text(efi["secure_boot_model"].isEmpty ? "–" : efi["secure_boot_model"]) }
            planOrHint(.safari)
            applyButton(.safari)
        }
    }

    /// opt-in, for when macOS does not start (docs/design/DESIGN-efi-rules.md section 7). Off is its own change, not undo:
    /// undo puts back the newest backup's whole config.plist, which is boot logging's only while nothing changed after it.
    var bootLogging: some View {
        Section2(title: "Boot logging (for troubleshooting)", symbol: "doc.text") {
            Caption("When macOS does not start: OpenCore keeps its log, and a kernel panic, on the EFI for a diagnostics report.",
                    detail: "On: OpenCore writes its log to opencore-<date>.txt in the root of the EFI volume it starts from (Misc > Debug > Target 0x41), with the macOS boot loader's log in it (AppleDebug), and the next boot after a kernel panic saves the panic there as panic-<date>.txt (ApplePanic). Off is a change of its own: it turns off the log file, AppleDebug and ApplePanic and keeps the rest of Misc > Debug and every other change. Undo is not used for it: it puts back the whole config.plist of the newest backup, which may be a later change.")
            LabeledContent("Boot logging") { Text(bootLogText).multilineTextAlignment(.trailing) }
            Label("It slows every boot: OpenCore rewrites its whole log file after every line, which adds seconds from an internal SSD and can add tens of seconds or more from a USB stick. Turn it off again once the logs are collected.",
                  systemImage: "exclamationmark.triangle")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Collect the logs on the Report page") {
                    UserDefaults.standard.set(true, forKey: "reportBootLogs")   // the Report page's Boot logs option
                    manager.requestedSection = .report
                }
                .buttonStyle(.link)
                Spacer()
                Button(String(localized: "Turn on") + "…") { page.confirming = .bootLog }
                    .disabled(!(manager.efiPlans[.bootLog]?.changes ?? false))
                Button(String(localized: "Turn off") + "…") { page.confirming = .bootLogOff }
                    .disabled(!(manager.efiPlans[.bootLogOff]?.changes ?? false))
            }
        }
    }

    var bootLogText: String {
        switch efi.bootLogging {
        case "on": return String(localized: "On: OpenCore writes its log and saves kernel panics on this EFI")
        case "off": return String(localized: "Off")
        case "partial": return String(localized: "Partly on: some of its settings are set")
        case "missing": return String(localized: "Not available: config.plist has no Misc > Debug")
        default: return "–"
        }
    }

    var undo: some View {
        Section2(title: "Undo", symbol: "arrow.uturn.backward") {
            if let backup = efi.backup {
                PathRow(title: "Last backup", path: backup)
            } else {
                LabeledContent("Last backup") { Text("None") }
            }
            HStack {
                if efi.backup != nil && !efi["backup_kind"].isEmpty {
                    Text(verbatim: "(\(efi["backup_kind"]))").font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
                Button(EFIAction.undo.title + "…", role: .destructive) { page.confirming = .undo }
                    .foregroundStyle(.red)
                    .disabled(efi.backup == nil || !(manager.efiPlans[.undo]?.changes ?? false))
            }
        }
    }

    /// right-aligned; the default button when there is something to change, else a note that there is nothing
    @ViewBuilder func applyButton(_ action: EFIAction) -> some View {
        let changes = manager.efiPlans[action]?.changes ?? false
        HStack {
            if manager.efiPlans[action] != nil && !changes {
                Label("Nothing to change", systemImage: "checkmark.circle").foregroundStyle(.green)
            }
            Spacer()
            if changes {
                // one accent button on the page: the first change in the install guide's order (stage 2, then 4);
                // Apple Secure Boot is optional and never the accent one
                Button(action.title + "…") { page.confirming = action }.buttonStyle(ProminentUnless(plain: action != prominentAction))
            } else {
                Button(action.title + "…") { page.confirming = action }.disabled(true)
            }
        }
    }

    var prominentAction: EFIAction? {
        [EFIAction.settings, .apply].first { manager.efiPlans[$0]?.changes ?? false }
    }

    @ViewBuilder func planOrHint(_ action: EFIAction) -> some View {
        if let plan = manager.efiPlans[action] {
            if plan.changes { PlanText(plan: plan) }   // without changes the button row says so
        } else if !efi.isOpenCore {
            Text("This partition has no EFI/OC/OpenCore.efi and config.plist: it is not an OpenCore EFI.").foregroundStyle(.secondary)
        } else if !efi.readable {
            Text("The EFI's config.plist cannot be read.").foregroundStyle(.secondary)
        }
    }

    var diskText: String {
        if efi["efi_source"] == "folder" { return (efi["efi_mount_point"] as NSString).appendingPathComponent("EFI") }
        var parts = [efi["efi_disk"].isEmpty ? String(localized: "Unknown disk") : efi["efi_disk"]]
        switch efi["efi_disk_location"] {
        case "internal": parts.append(String(localized: "internal"))
        case "external": parts.append(String(localized: "external"))
        default: break
        }
        if let bytes = Int64(efi["efi_disk_size"]) { parts.append(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .decimal)) }
        return parts.joined(separator: ", ")
    }

    var sourceText: String {
        switch efi["efi_source"] {
        case "boot-path": return String(localized: "OpenCore's record of this boot")
        case "chosen": return String(localized: "Your choice")
        case "folder": return String(localized: "A folder you chose")
        default: return "–"
        }
    }

    var openCoreText: String {
        switch efi["opencore"] {
        case "yes": return String(localized: "Yes")
        case "no": return String(localized: "No OpenCore here")
        default: return String(localized: "Not checked")
        }
    }

    var kextText: String {
        let package = efi["kext_version_package"], here = efi["kext_version_efi"]
        switch efi["kext_file"] {
        case "same": return String(localized: "The same as the package's (\(package))")
        case "different": return String(localized: "Differs: \(here.isEmpty ? "?" : here) here, \(package) in the package")
        case "absent": return String(localized: "Not there")
        default: return String(localized: "Not checked: the EFI is not mounted")
        }
    }

    var hpmNeededText: String {
        efi["hybridpm_needed"] == "yes" ? String(localized: "Yes: this CPU has efficiency cores (12th-14th gen Core)")
                                        : String(localized: "No: this CPU has no efficiency cores, or is not a 12th-14th gen Core")
    }

    var hpmText: String {
        let package = efi["hybridpm_version_package"], here = efi["hybridpm_version_efi"]
        switch efi["hybridpm_file"] {
        case "same": return String(localized: "The same as the package's (\(package))")
        case "different" where efi["hybridpm_package"] != "yes": return String(localized: "There (\(here.isEmpty ? "?" : here)); this package has none")
        case "different": return String(localized: "Differs: \(here.isEmpty ? "?" : here) here, \(package) in the package")
        case "absent" where efi["hybridpm_package"] != "yes": return String(localized: "Not there; this package has none (0.1.4 and later have it)")
        case "absent": return String(localized: "Not there")
        default: return String(localized: "Not checked: the EFI is not mounted")
        }
    }

    func fileText(_ state: String) -> String {
        switch state {
        case "same": return String(localized: "The same as the package's")
        case "different": return String(localized: "Differs from the package's")
        case "absent": return String(localized: "Not there")
        default: return String(localized: "Not checked")
        }
    }

    /// efi.sh compares the EFI's table with the one this card needs: the package's, or the package's adapted to the card's
    /// port ("adaptable": the same table with the card's ACPI path written in).
    var ssdtText: String {
        let adapted = efi["ssdt_package_names_card"] == "adaptable"
        switch efi["ssdt_file"] {
        case "same" where adapted: return String(localized: "The package's, adapted to this card's port")
        case "different" where adapted: return String(localized: "Differs from the package's adapted to this card's port")
        case let state: return fileText(state)
        }
    }

    func entryText(_ state: String) -> String {
        switch state {
        case "enabled": return String(localized: "On")
        case "disabled": return String(localized: "Off")
        case "absent": return String(localized: "Missing")
        default: return "–"
        }
    }

    var cardText: String {
        switch efi["card_acpi_path"] {
        case "": return String(localized: "No Arc B580 or B570 found")
        case "none": return String(localized: "The card's port has no ACPI name")
        case let path:
            switch efi["ssdt_package_names_card"] {
            case "yes": return String(localized: "\(path) (the package's table is for it)")
            case "adaptable": return String(localized: "\(path) (adapted to this card's port: the package's table is for \\_SB.PC00.PEG1.PEGP)")
            default: return String(localized: "\(path) (the package's table is for \\_SB.PC00.PEG1.PEGP)")
            }
        }
    }
}

/// efi.sh's plan as it prints it, without its first line (the EFI, shown above it).
struct PlanText: View {
    let plan: EFIPlan
    var all = false
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            ForEach(Array((all ? plan.lines : Array(plan.lines.dropFirst())).enumerated()), id: \.offset) { _, line in
                Text(line)
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(line.hasPrefix("note ") || line == "nothing to change" ? .secondary : .primary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct EFIConfirmSheet: View {
    let action: EFIAction
    let plan: EFIPlan?
    let disk: String
    let confirm: () -> Void
    @Environment(\.dismiss) var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(action.title).font(.title2).bold()
            Label(disk, systemImage: "externaldrive").font(.headline).textSelection(.enabled)
            Text(action.explanation).fixedSize(horizontal: false, vertical: true)
            if let plan {
                ScrollView {
                    PlanText(plan: plan, all: true).padding(8)
                }
                .frame(maxHeight: 280)
                .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
            }
            Text("macOS asks for an administrator password next.").font(.callout).foregroundStyle(.secondary)
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button(action.title, role: action.destructive ? .destructive : nil) {
                    dismiss()
                    confirm()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 600)
    }
}
