// SPDX-License-Identifier: MIT
// Game mode (docs/design/DESIGN-game-mode.md): opt-in, off at every start of the app, for this session only. macOS's own
// Game Mode (macOS 14 and later) already gives a full-screen game priority on the CPU and lowers background work; it
// knows nothing of this driver's GPU clock. While game mode is on and a game is the frontmost app, the GPU's lowest
// clock goes up (debug.intelxe.slpc_min, sent to the GuC by debug.intelxe.step=17, a knob the release kext has), and
// optionally Time Machine backups that run then are stopped. A root helper (Resources/gamemode-helper.sh) does both: the
// app starts it once through the password dialog and tells it "game" or "desktop" through a pipe only this user can
// write. It puts everything back when told, when the app ends (it watches the app's process) or on a signal; the clock
// it sets lives in the kernel only and is gone at the next start of macOS anyway.
import AppKit
import Foundation
import SwiftUI

let gameModeStateFile = "/var/run/intelxe-gamemode/state"
let gameModePipe = "/var/run/intelxe-gamemode/ctl"

/// What a helper left in its state file (gamemode-helper.sh write_state).
struct GameModeHelperState: Equatable {
    var pid: Int32 = 0
    var origMin = 0
    var floor = 0
    var phase = ""
    init(text: String) {
        let v = keyValues(text)
        pid = Int32(v["pid"] ?? "") ?? 0
        origMin = Int(v["orig_min"] ?? "") ?? 0
        floor = Int(v["floor"] ?? "") ?? 0
        phase = v["phase"] ?? ""
    }
    static func read() -> GameModeHelperState? {
        (try? String(contentsOfFile: gameModeStateFile, encoding: .utf8)).map(GameModeHelperState.init)
    }
    /// the process still runs (a root process answers kill(0) with EPERM to a user: it exists)
    var alive: Bool { pid > 0 && (kill(pid, 0) == 0 || errno == EPERM) }
}

/// An app counts as a game when its Info.plist puts it in the App Store's games category (public.app-category.games or
/// one of the public.app-category.*-games), as macOS's Game Mode reads it, or when the user added it.
func isGameCategory(_ category: String?) -> Bool {
    guard let c = category?.lowercased() else { return false }
    return c == "public.app-category.games" || (c.hasPrefix("public.app-category.") && c.hasSuffix("-games"))
}

/// How the app names an application in its list: the bundle ID, or the executable's name for a program without one
/// (java, wine64-preloader).
func gameModeKey(bundleID: String?, executable: String?) -> String? {
    if let bundleID, !bundleID.isEmpty { return bundleID }
    return executable.flatMap { $0.isEmpty ? nil : $0 }
}

/// The root command that starts the helper in the background (run() wraps it in the password dialog).
func gameModeHelperCommand(script: String, appPID: Int32, uid: uid_t, floor: Int, stopBackups: Bool) -> String {
    "/bin/bash \(shellQuoted(script)) --run \(appPID) \(uid) \(floor) \(stopBackups ? 1 : 0) </dev/null &"
}

@MainActor
final class GameMode: ObservableObject {
    static let shared = GameMode()

    @Published private(set) var on = false
    @Published private(set) var starting = false
    /// the game in front, while game mode is on
    @Published private(set) var game: String?
    /// the last app in front that is not this one (the menu bar's "Count ... as a game")
    @Published private(set) var lastFront: (key: String, name: String)?
    @Published var message: String?
    /// a helper that was stopped while it held the clock up (kill -9, a crash): its clock stays until put back or a restart
    @Published private(set) var stale: GameModeHelperState?
    @Published var floorMHz: Int { didSet { UserDefaults.standard.set(floorMHz, forKey: "gameModeFloor") } }
    @Published var stopBackups: Bool { didSet { UserDefaults.standard.set(stopBackups, forKey: "gameModeStopBackups") } }
    @Published var extraGames: [String] { didSet { UserDefaults.standard.set(extraGames, forKey: "gameModeApps") } }

    private var observers: [NSObjectProtocol] = []
    private var watchdog: Timer?

    private init() {
        let d = UserDefaults.standard
        floorMHz = d.object(forKey: "gameModeFloor") as? Int ?? 2400
        stopBackups = d.bool(forKey: "gameModeStopBackups")
        extraGames = d.stringArray(forKey: "gameModeApps") ?? []
        if let s = GameModeHelperState.read(), !s.alive, s.phase == "game" { stale = s }
        observers.append(NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main) { note in
            let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            MainActor.assumeIsolated { GameMode.shared.frontChanged(app) }
        })
        observers.append(NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { _ = GameMode.shared.send("quit") }
        })
    }

    var statusLine: String {
        if starting { return String(localized: "Game mode: starting…") }
        if !on { return String(localized: "Game mode: off") }
        if let game { return String(localized: "Game mode: \(game) in front") }
        return String(localized: "Game mode: on, no game in front")
    }

    func isGame(_ app: NSRunningApplication) -> Bool {
        if let key = gameModeKey(bundleID: app.bundleIdentifier, executable: app.executableURL?.lastPathComponent), extraGames.contains(key) { return true }
        guard let url = app.bundleURL, let info = Bundle(url: url)?.infoDictionary else { return false }
        return isGameCategory(info["LSApplicationCategoryType"] as? String)
    }

    func setOn(_ want: Bool) { want ? start() : stop() }

    func start() {
        guard !on, !starting, let script = Bundle.main.url(forResource: "gamemode-helper", withExtension: "sh") else { return }
        starting = true; message = nil
        let command = gameModeHelperCommand(script: script.path, appPID: ProcessInfo.processInfo.processIdentifier, uid: getuid(),
                                            floor: floorMHz, stopBackups: stopBackups)
        let logs = dataDirectory.appendingPathComponent("logs")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        let log = logs.appendingPathComponent("gamemode.log")
        let prompt = String(localized: "IntelXe Manager needs an administrator password for: \(String(localized: "Game mode"))")
        Task.detached {
            let result = run(command: command, needsRoot: true, log: log, prompt: prompt)
            // the helper makes its pipe when it is ready
            var waited = 0
            while result.succeeded && waited < 30 && !(FileManager.default.fileExists(atPath: gameModePipe) && GameModeHelperState.read()?.alive == true) {
                try? await Task.sleep(nanoseconds: 100_000_000); waited += 1
            }
            let ready = result.succeeded && waited < 30
            let tail = (try? String(contentsOf: log, encoding: .utf8))?.split(separator: "\n").last.map(String.init)
            await MainActor.run {
                let gm = GameMode.shared
                gm.starting = false
                if result.cancelled { gm.message = String(localized: "Cancelled. Nothing changed."); return }
                guard ready else { gm.message = tail ?? String(localized: "Game mode did not start."); return }
                gm.on = true; gm.stale = nil
                gm.watchdog = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                    MainActor.assumeIsolated { GameMode.shared.checkHelper() }
                }
                gm.frontChanged(NSWorkspace.shared.frontmostApplication)
            }
        }
    }

    func stop() {
        guard on else { return }
        _ = send("quit")
        watchdog?.invalidate(); watchdog = nil
        on = false; game = nil
    }

    /// One word to the helper; false when no helper reads the pipe.
    @discardableResult
    func send(_ word: String) -> Bool {
        let fd = open(gameModePipe, O_WRONLY | O_NONBLOCK)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        let line = Array((word + "\n").utf8)
        return write(fd, line, line.count) == line.count
    }

    private func checkHelper() {
        guard on, GameModeHelperState.read()?.alive != true else { return }
        watchdog?.invalidate(); watchdog = nil
        on = false; game = nil
        message = String(localized: "Game mode stopped: its helper ended.")
        if let s = GameModeHelperState.read(), s.phase == "game" { stale = s }
    }

    func frontChanged(_ app: NSRunningApplication?) {
        guard let app, app.processIdentifier != ProcessInfo.processInfo.processIdentifier else { return }
        let name = app.localizedName ?? app.bundleIdentifier ?? "?"
        if let key = gameModeKey(bundleID: app.bundleIdentifier, executable: app.executableURL?.lastPathComponent) { lastFront = (key, name) }
        guard on else { return }
        let nowGame = isGame(app) ? name : nil
        guard nowGame != game else { return }
        if send(nowGame != nil ? "game" : "desktop") { game = nowGame } else { checkHelper() }
    }

    func addLastFront() {
        guard let f = lastFront, !extraGames.contains(f.key) else { return }
        extraGames.append(f.key)
        if on, let app = NSWorkspace.shared.frontmostApplication { frontChanged(app) }
    }

    /// Put back the clock a stopped helper left raised (the helper's --restore, through the password dialog).
    func restoreStale() {
        guard let script = Bundle.main.url(forResource: "gamemode-helper", withExtension: "sh") else { return }
        let log = dataDirectory.appendingPathComponent("logs/gamemode.log")
        let prompt = String(localized: "IntelXe Manager needs an administrator password for: \(String(localized: "Game mode"))")
        Task.detached {
            let result = run(command: "/bin/bash \(shellQuoted(script.path)) --restore", needsRoot: true, log: log, prompt: prompt)
            await MainActor.run { if result.succeeded { GameMode.shared.stale = nil } }
        }
    }
}

/// The Power and app page's section.
struct GameModeSection: View {
    @ObservedObject var gm = GameMode.shared
    var body: some View {
        Section2(title: "Game mode", symbol: "gamecontroller") {
            CaptionedToggle(title: "Game mode", caption: "While a game is in front: a higher lowest GPU clock. Off again when this app quits.",
                            detail: "macOS's own Game Mode already gives a full-screen game priority, but it does not know this driver's GPU clock. With this on, the GPU's lowest clock goes up while a game is the frontmost app, so that frames do not wait for the clock to rise, and goes back when you switch away. Asks for the administrator password once; nothing is kept: it is off again when this app quits, and at the next start of macOS in any case.",
                            isOn: Binding(get: { gm.on || gm.starting }, set: { gm.setOn($0) }))
                .disabled(gm.starting)
            Picker("Lowest GPU clock in a game", selection: $gm.floorMHz) {
                Text("2000 MHz").tag(2000); Text("2400 MHz").tag(2400); Text("The card's highest").tag(3000)
            }
            .disabled(gm.on || gm.starting)
            CaptionedToggle(title: "Stop Time Machine backups during a game", caption: "A backup that runs while a game is in front is stopped; the next one runs as scheduled.",
                            isOn: $gm.stopBackups)
                .disabled(gm.on || gm.starting)
            Text(gm.statusLine + (gm.message.map { " – " + $0 } ?? "")).font(.caption).foregroundStyle(.secondary)
            if !gm.extraGames.isEmpty {
                ForEach(gm.extraGames, id: \.self) { key in
                    HStack {
                        Text(key).font(.callout.monospaced())
                        Spacer()
                        Button(role: .destructive) { gm.extraGames.removeAll { $0 == key } } label: { Image(systemName: "minus.circle") }
                            .buttonStyle(.borderless).help(Text("Remove from the list"))
                    }
                }
            }
            Text("Games are the apps macOS lists as games (their category), and the ones you add from the menu bar item while they are in front (Count … as a game).")
                .font(.caption).foregroundStyle(.secondary)
            if let s = gm.stale {
                HStack {
                    Text("A game mode helper stopped while the lowest GPU clock was \(String(s.floor)) MHz; it stays so until it is put back or macOS restarts.")
                        .font(.callout).foregroundStyle(.orange)
                    Spacer()
                    Button("Put it back") { gm.restoreStale() }
                }
            }
        }
    }
}

/// The menu bar item's lines.
struct GameModeMenu: View {
    @ObservedObject var gm = GameMode.shared
    var body: some View {
        Text(gm.statusLine)
        Button(gm.on ? String(localized: "Turn game mode off") : String(localized: "Turn game mode on…")) { gm.setOn(!gm.on) }
            .disabled(gm.starting)
        if let f = gm.lastFront, !gm.extraGames.contains(f.key), gm.game == nil {
            Button(String(localized: "Count \(f.name) as a game")) { gm.addLastFront() }
        }
    }
}
