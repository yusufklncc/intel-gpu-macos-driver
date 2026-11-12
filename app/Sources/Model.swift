// SPDX-License-Identifier: MIT
// What the app knows about the machine and the releases. Nothing here changes the system: Installer.swift runs the changes.
import CryptoKit
import Foundation

let repository = "yusufklncc/intel-gpu-macos-driver"
let dataDirectory = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Application Support/IntelXe Manager")
let installerDirectory = "/Library/Application Support/intel-gpu-macos-driver"
let zinkLibrary = "/Library/IntelXe/GL/libEGL.1.dylib"
let zinkAgent = "/Library/LaunchAgents/io.github.yusufklncc.intelxe.zink-gl.plist"
/// the optional Vulkan provider for games (vulkan/install.sh)
let vulkanProvider = "/Library/IntelXe/Vulkan/lib/libintelxe-vk.dylib"
/// the experimental switch for every native OpenGL application (zink-gl install.sh --global-gl): its LaunchAgent, the
/// library it inserts, and the per-application rules it reads ("+bundle.id" use Zink, "-bundle.id" keep Apple's OpenGL)
let cglzinkGlobalAgent = "/Library/LaunchAgents/io.github.yusufklncc.intelxe.cglzink-global.plist"
let cglzinkBootLibrary = "/Library/IntelXe/GL/libcglzink-boot.dylib"
let cglzinkRulesFile = "/Library/IntelXe/GL/cglzink-global.txt"

/// A command's standard output; nil when it could not run, a time limit keeps a hung tool from holding the app.
func commandOutput(_ path: String, _ arguments: [String], seconds: TimeInterval = 15) -> String? {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: path)
    process.arguments = arguments
    let output = Pipe()
    process.standardOutput = output
    process.standardError = Pipe()
    do { try process.run() } catch { return nil }
    let limit = DispatchWorkItem { if process.isRunning { process.terminate() } }
    DispatchQueue.global().asyncAfter(deadline: .now() + seconds, execute: limit)
    let data = output.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    limit.cancel()
    return String(data: data, encoding: .utf8)
}

/// The cards the driver supports: PCI device id (vendor 8086) and name. The same list as data/platforms.json "supported"
/// (tools/parity/devices.py checks every copy in the repository). The B570 is the same BMG-G21 with fewer Xe cores (#2).
let supportedCards: [(id: String, name: String)] = [("e20b", "Intel Arc B580"), ("e20c", "Intel Arc B570")]

/// The first supported card on the PCI bus, in list order (ioreg shows the device id little-endian: e20b = <0be20000>).
func supportedCardOnBus() -> (id: String, name: String)? {
    let pci = commandOutput("/usr/sbin/ioreg", ["-r", "-c", "IOPCIDevice", "-l"]) ?? ""
    guard pci.contains("\"vendor-id\" = <86800000>") else { return nil }
    return supportedCards.first(where: { pci.contains("\"device-id\" = <\($0.id.suffix(2))\($0.id.prefix(2))0000>") })
}

struct Check: Identifiable, Hashable {
    enum State { case good, warning, bad, neutral }
    let id: String
    let title: String
    let value: String
    let state: State
    var help: String = ""
}

struct SystemFacts {
    var macOS = ""
    var macOSMajor = 0
    var architecture = ""
    var hasCard = false
    var cardName = ""
    var kextLoaded = false
    var oldKextLoaded = false
    var sipRelaxed = false
    var sipText = ""
    var amfiRelaxed = false
    var surfaceSharing = false
    var bootGuardOff = false
    var metalDevice = ""
    var installedVersion: String?
    var driverFilesPresent = false
    var hasBackup = false
    var zinkInstalled = false
    var zinkGlobal = false
    var cglzinkAvailable = false   // the installed Zink has the boot library of the switch below
    var cglzinkGlobal = false      // every native OpenGL application through Zink (experimental)
    var vulkanInstalled = false    // the Vulkan provider for games (experimental)

    static func probe() -> SystemFacts {
        var facts = SystemFacts()
        let version = ProcessInfo.processInfo.operatingSystemVersion
        facts.macOS = "\(version.majorVersion).\(version.minorVersion).\(version.patchVersion)"
        facts.macOSMajor = version.majorVersion
        facts.architecture = commandOutput("/usr/bin/uname", ["-m"])?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""

        if let card = supportedCardOnBus() { facts.cardName = "\(card.name) (8086:\(card.id))" }
        facts.hasCard = !facts.cardName.isEmpty

        let loaded = commandOutput("/usr/bin/kmutil", ["showloaded", "--list-only"], seconds: 30) ?? ""
        facts.kextLoaded = loaded.contains("io.github.yusufklncc.IntelXe ")
        facts.oldKextLoaded = loaded.contains("org.hackintosh.B580 ")

        let sip = commandOutput("/usr/bin/csrutil", ["status"]) ?? ""
        facts.sipText = sip.split(separator: "\n").first.map(String.init) ?? ""
        facts.sipRelaxed = sip.contains("disabled") || sip.contains("Custom Configuration")

        let bootArgs = commandOutput("/usr/sbin/nvram", ["boot-args"]) ?? ""
        facts.amfiRelaxed = ["amfi_get_out_of_my_way=1", "amfi_get_out_of_my_way=0x1", "amfi=0x80", "amfi=128"]
            .contains { bootArgs.contains($0) }
        facts.surfaceSharing = bootArgs.contains("intelxe_surfvram=2")
        let guardValue = commandOutput("/usr/sbin/nvram", ["intelxe-accel-off"]) ?? ""
        facts.bootGuardOff = !guardValue.isEmpty && !guardValue.contains("\tok")

        facts.metalDevice = metalDeviceName()

        let manager = FileManager.default
        if let text = try? String(contentsOfFile: "\(installerDirectory)/installed-version", encoding: .utf8) {
            facts.installedVersion = text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        facts.driverFilesPresent = manager.fileExists(atPath: "/Library/Extensions/IntelXeFB.kext")
            && manager.fileExists(atPath: "/Library/GPUBundles/NVMTLDriverLib.bundle")
        if let last = try? String(contentsOfFile: "\(installerDirectory)/last-backup", encoding: .utf8) {
            facts.hasBackup = manager.fileExists(atPath: last.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        facts.zinkInstalled = manager.fileExists(atPath: zinkLibrary)
        facts.zinkGlobal = manager.fileExists(atPath: zinkAgent)
        facts.cglzinkAvailable = manager.fileExists(atPath: cglzinkBootLibrary)
        facts.cglzinkGlobal = manager.fileExists(atPath: cglzinkGlobalAgent)
        facts.vulkanInstalled = manager.fileExists(atPath: vulkanProvider)
        return facts
    }

    var checks: [Check] {
        var list = [
            Check(id: "macos", title: "macOS", value: macOS,
                  state: macOSMajor == 26 || macOSMajor == 15 ? .good : .bad,
                  help: macOSMajor == 26 || macOSMajor == 15 ? "" : "There are packages for macOS 26 and macOS 15."),
            Check(id: "cpu", title: "Processor", value: architecture, state: architecture == "x86_64" ? .good : .bad,
                  help: architecture == "x86_64" ? "" : "The driver is for x86_64 Macs. This app can run here, but it cannot install on this machine."),
            Check(id: "card", title: "Graphics card", value: hasCard ? cardName : String(localized: "No Arc B580 or B570 found"), state: hasCard ? .good : .bad),
            Check(id: "sip", title: "System Integrity Protection", value: sipText, state: sipRelaxed ? .good : .bad,
                  help: sipRelaxed ? "" : "The driver needs csr-active-config in your EFI: see the install guide."),
            Check(id: "amfi", title: "AMFI boot-arg", value: amfiRelaxed ? "Relaxed" : "Not set", state: amfiRelaxed ? .good : .bad,
                  help: amfiRelaxed ? "" : "Add amfi_get_out_of_my_way=0x1 to the boot-args in your EFI."),
            Check(id: "kext", title: "IntelXe.kext (OpenCore)",
                  value: kextLoaded ? "Loaded" : (oldKextLoaded ? "Earlier name (B580.kext) loaded" : "Not loaded"),
                  state: kextLoaded ? .good : .warning,
                  help: kextLoaded ? "" : "It comes from your EFI. Expected before the first install is finished."),
            Check(id: "metal", title: "Metal", value: metalDevice.isEmpty ? "No device" : metalDevice, state: metalDevice.isEmpty ? .warning : .good),
        ]
        if bootGuardOff {
            list.append(Check(id: "guard", title: "Boot guard", value: "Acceleration is off", state: .bad,
                              help: "A boot did not finish, so the guard kept acceleration off. Clear it after you have checked why."))
        }
        return list
    }
}

import Metal
func metalDeviceName() -> String { MTLCreateSystemDefaultDevice()?.name ?? "" }

// MARK: - Releases

struct ReleaseAsset: Decodable, Hashable {
    let name: String
    let browser_download_url: URL
    let size: Int
}

struct Release: Decodable, Hashable {
    let tag_name: String
    let html_url: URL
    let body: String?
    let prerelease: Bool
    let assets: [ReleaseAsset]
    var version: Version? { Version(tag_name) }
    func asset(_ suffix: String) -> ReleaseAsset? { assets.first { $0.name.hasSuffix(suffix) } }
}

struct Version: Comparable, CustomStringConvertible {
    let parts: [Int]
    init?(_ text: String) {
        let trimmed = text.hasPrefix("v") ? String(text.dropFirst()) : text
        let parts = trimmed.split(separator: ".", omittingEmptySubsequences: false).map { Int($0) }
        guard parts.count == 3, !parts.contains(where: { $0 == nil }) else { return nil }
        self.parts = parts.compactMap { $0 }
    }
    static func < (a: Version, b: Version) -> Bool { a.parts.lexicographicallyPrecedes(b.parts) }
    var description: String { parts.map(String.init).joined(separator: ".") }
}

enum ManagerError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

func latestRelease() async throws -> Release {
    var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!)
    request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    request.setValue("IntelXe-Manager", forHTTPHeaderField: "User-Agent")
    let (data, response) = try await URLSession.shared.data(for: request)
    guard (response as? HTTPURLResponse)?.statusCode == 200 else {
        throw ManagerError.message("GitHub answered \((response as? HTTPURLResponse)?.statusCode ?? 0).")
    }
    return try JSONDecoder().decode(Release.self, from: data)
}

func sha256(of file: URL) throws -> String {
    let handle = try FileHandle(forReadingFrom: file)
    defer { try? handle.close() }
    var hash = SHA256()
    while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty { hash.update(data: chunk) }
    return hash.finalize().map { String(format: "%02x", $0) }.joined()
}

/// The hash a `shasum -a 256` list gives for this file name.
func listedHash(_ list: String, name: String) -> String? {
    for line in list.split(separator: "\n") {
        let fields = line.split(separator: " ", omittingEmptySubsequences: true)
        if fields.count == 2, fields[1].trimmingCharacters(in: CharacterSet(charactersIn: "*")) == name { return String(fields[0]) }
    }
    return nil
}

/// A package on disk: a directory with install.sh and SHA256SUMS.
struct Package: Hashable {
    let directory: URL
    var script: URL { directory.appendingPathComponent("install.sh") }
    var isValid: Bool {
        FileManager.default.isExecutableFile(atPath: script.path)
            && FileManager.default.fileExists(atPath: directory.appendingPathComponent("SHA256SUMS").path)
    }
    /// install.sh --status, which changes nothing.
    func status() -> [String: String] {
        guard let text = commandOutput(script.path, ["--status"], seconds: 60) else { return [:] }
        var values: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let pair = line.split(separator: "=", maxSplits: 1).map(String.init)
            if pair.count == 2 { values[pair[0]] = pair[1] }
        }
        return values
    }
}

func packageSuffix(macOSMajor: Int) -> String? {
    switch macOSMajor {
    case 26: return "-tahoe.zip"
    case 15: return "-sequoia.zip"
    default: return nil
    }
}

/// Downloads a release's zip, checks that the release's list of hashes is signed with the project's key (Signing.swift)
/// and that the zip's hash is in it, and unpacks it. The signature proves who made the release; the hash finds a damaged
/// or replaced download.
func download(_ release: Release, suffix: String, progress: @escaping @Sendable (String) -> Void) async throws -> Package {
    guard let asset = release.asset(suffix) else { throw ManagerError.message("This release has no \(suffix) package.") }
    guard let sums = release.asset(".sha256") else { throw ManagerError.message("This release has no list of hashes.") }
    // .sha256.sigs (one signature per key, during a key change) when the release has it, else .sha256.sig
    guard let signature = release.asset(".sha256.sigs") ?? release.asset(".sha256.sig") else {
        throw ManagerError.message("This release's list of hashes is not signed, so the app cannot tell that the project made it. Download the package from the release page and choose its folder on the Driver page if you trust it.")
    }
    let target = dataDirectory.appendingPathComponent("packages/\(release.tag_name)")
    try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
    let folder = target.appendingPathComponent(String(asset.name.dropLast(4)))
    let existing = Package(directory: folder)
    if existing.isValid { return existing }

    progress("Downloading \(asset.name)")
    var request = URLRequest(url: sums.browser_download_url)
    request.setValue("IntelXe-Manager", forHTTPHeaderField: "User-Agent")
    let listData = try await URLSession.shared.data(for: request).0
    var sigRequest = URLRequest(url: signature.browser_download_url)
    sigRequest.setValue("IntelXe-Manager", forHTTPHeaderField: "User-Agent")
    let sigText = String(data: try await URLSession.shared.data(for: sigRequest).0, encoding: .utf8) ?? ""
    guard releaseSignatureValid(list: listData, signature: sigText) else {
        throw ManagerError.message("This release's signature does not match the project's key: the package was not made by the project, or was changed. Nothing was installed.")
    }
    let list = String(data: listData, encoding: .utf8) ?? ""
    guard let expected = listedHash(list, name: asset.name) else { throw ManagerError.message("\(asset.name) is not in the list of hashes.") }
    let (file, _) = try await URLSession.shared.download(from: asset.browser_download_url)
    defer { try? FileManager.default.removeItem(at: file) }
    progress("Checking the download")
    let actual = try sha256(of: file)
    guard actual == expected else { throw ManagerError.message("The download is damaged: its hash differs from the release's list.") }
    progress("Unpacking")
    guard commandOutput("/usr/bin/ditto", ["-x", "-k", file.path, target.path], seconds: 300) != nil else {
        throw ManagerError.message("The zip could not be unpacked.")
    }
    guard existing.isValid else { throw ManagerError.message("The zip does not hold a package.") }
    return existing
}

/// One line of cglzink-global.txt: an application by bundle ID, through Zink (+) or kept on Apple's OpenGL (-).
struct CGLZinkRule: Identifiable, Hashable {
    var bundleID: String
    var useZink: Bool
    var id: String { bundleID }
}

/// The rules file as the boot library reads it (zink-gl/cgl/boot.c config_says: "+id" / "-id", the last line for an ID
/// wins, # comments). Comments and blank lines are not kept when the app writes the file again.
func readCGLZinkRules(_ path: String = cglzinkRulesFile) -> [CGLZinkRule] {
    guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return [] }
    var rules: [CGLZinkRule] = []
    for raw in text.split(whereSeparator: \.isNewline) {
        let line = raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init)?
            .trimmingCharacters(in: .whitespaces) ?? ""
        guard let sign = line.first, sign == "+" || sign == "-", line.count > 1 else { continue }
        let id = String(line.dropFirst())
        guard !id.contains(where: { $0 == " " || $0 == "\t" }) else { continue }
        rules.removeAll { $0.bundleID == id }
        rules.append(CGLZinkRule(bundleID: id, useZink: sign == "+"))
    }
    return rules
}

func cglzinkRulesText(_ rules: [CGLZinkRule]) -> String {
    "# IntelXe Manager: applications for the switch for every OpenGL application (+ use Zink, - keep Apple's OpenGL)\n"
        + rules.map { ($0.useZink ? "+" : "-") + $0.bundleID }.joined(separator: "\n") + "\n"
}
