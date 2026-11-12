// SPDX-License-Identifier: MIT
// Runs the package's own scripts. The app decides nothing about what is copied where: install.sh does, so a change made in
// Terminal and one made here are the same change. EFI files are never touched.
import Foundation

/// A shell word: the whole text, quoted for /bin/sh.
func shellQuoted(_ text: String) -> String { "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'" }

/// Text for a double-quoted AppleScript string.
func appleScriptQuoted(_ text: String) -> String {
    text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
}

/// The AppleScript that runs this command as root with its output in a log file: macOS asks for the password.
func privilegedScript(command: String, log: String, prompt: String) -> String {
    let shell = "/bin/bash -c \(shellQuoted(command)) >> \(shellQuoted(log)) 2>&1"
    return "do shell script \"\(appleScriptQuoted(shell))\" with prompt \"\(appleScriptQuoted(prompt))\" with administrator privileges"
}

struct RunResult {
    let succeeded: Bool
    let cancelled: Bool
    let message: String
}

/// `needsRoot`: through the macOS password dialog. Blocks: call it off the main thread.
func run(command: String, needsRoot: Bool, log: URL, prompt: String) -> RunResult {
    FileManager.default.createFile(atPath: log.path, contents: nil)
    let process = Process()
    let errors = Pipe()
    process.standardError = errors
    process.standardOutput = Pipe()
    if needsRoot && ProcessInfo.processInfo.environment["INTELXE_MANAGER_TEST_NO_ROOT"] == nil {
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", privilegedScript(command: command, log: log.path, prompt: prompt)]
    } else {
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = ["-c", "(\(command)) >> \(shellQuoted(log.path)) 2>&1"]
    }
    do { try process.run() } catch { return RunResult(succeeded: false, cancelled: false, message: error.localizedDescription) }
    let data = errors.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    let text = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    return RunResult(succeeded: process.terminationStatus == 0, cancelled: text.contains("-128"), message: text)
}

enum DriverAction: String, CaseIterable, Identifiable {
    case check, install, repair, undo, uninstall
    var id: String { rawValue }
    var argument: String {
        switch self {
        case .check: return "--check"
        case .install: return "--apply"
        case .repair: return "--repair"
        case .undo: return "--undo"
        case .uninstall: return "--uninstall"
        }
    }
    var needsRoot: Bool { self != .check }
    var needsReboot: Bool { self != .check }
}

/// Which package a download or a chosen folder is for.
enum PackageKind { case driver, zink, vulkan }

/// vulkan/install.sh: the optional Vulkan provider for games (experimental).
enum VulkanAction: String, Identifiable {
    case install, undo, test
    var id: String { rawValue }
    var arguments: String {
        switch self {
        case .install: return ""
        case .undo: return "--undo"
        case .test: return "--test"
        }
    }
    var needsRoot: Bool { self != .test }
}

enum ZinkAction: String, Identifiable {
    case install, undo, globalOn, globalOff, glOn, glOff, test
    var id: String { rawValue }
    var arguments: String {
        switch self {
        case .install: return ""
        case .undo: return "--undo"
        case .globalOn: return "--global on"
        case .globalOff: return "--global off"
        case .glOn: return "--global-gl on"
        case .glOff: return "--global-gl off"
        case .test: return "--test"
        }
    }
    var needsRoot: Bool { self != .test }
}

/// zink-gl/install.sh --global-gl off without the package: the LaunchAgent goes, and the logged-in user's session stops
/// inserting the boot library into applications started from now on.
func cglzinkGlobalOffCommand() -> String {
    "rm -f \(shellQuoted(cglzinkGlobalAgent)); u=$(stat -f %Su /dev/console); if [ -n \"$u\" ] && [ \"$u\" != root ]; then "
        + "case \"$(launchctl asuser \"$(id -u \"$u\")\" launchctl getenv DYLD_INSERT_LIBRARIES)\" in *libcglzink-boot*) "
        + "launchctl asuser \"$(id -u \"$u\")\" launchctl unsetenv DYLD_INSERT_LIBRARIES ;; esac; fi; echo off"
}

/// zink-gl/install.sh --global on|off without the package (it is only needed to install): the same LaunchAgent, which
/// sets JAVA_TOOL_OPTIONS at every login, and the same variable for the user logged in now.
func zinkGlobalCommand(on: Bool) -> String {
    let gl = "/Library/IntelXe/GL"
    let options = "-Dorg.lwjgl.glfw.libname=\(gl)/libglfw.3.dylib -Dorg.lwjgl.opengl.libname=\(gl)/libGLzink.dylib"
    let agent = shellQuoted(zinkAgent), domain = shellQuoted(String(zinkAgent.dropLast(6)))
    let user = "u=$(stat -f %Su /dev/console); "
    if !on {
        return "rm -f \(agent); " + user + "if [ -n \"$u\" ] && [ \"$u\" != root ]; then launchctl asuser \"$(id -u \"$u\")\" launchctl unsetenv JAVA_TOOL_OPTIONS; fi; echo off"
    }
    return "set -e; [ -d \(gl) ] || { echo 'Zink is not installed'; exit 1; }; rm -f \(agent); "
        + "defaults write \(domain) Label -string io.github.yusufklncc.intelxe.zink-gl; "
        + "defaults write \(domain) ProgramArguments -array /bin/launchctl setenv JAVA_TOOL_OPTIONS \(shellQuoted(options)); "
        + "defaults write \(domain) RunAtLoad -bool true; chown root:wheel \(agent); chmod 644 \(agent); plutil -convert xml1 \(agent); "
        + user + "if [ -n \"$u\" ] && [ \"$u\" != root ]; then launchctl asuser \"$(id -u \"$u\")\" launchctl setenv JAVA_TOOL_OPTIONS \(shellQuoted(options)); fi; echo on"
}
