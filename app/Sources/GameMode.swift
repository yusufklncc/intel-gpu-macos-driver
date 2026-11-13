// SPDX-License-Identifier: MIT
// Game mode (docs/design/DESIGN-game-mode.md): opt-in, off at every start of the app, for this session only. macOS's own
// Game Mode (macOS 14 and later) already gives a full-screen game priority on the CPU and lowers background work; it
// knows nothing of this driver's GPU clock. While game mode is on and a game is the frontmost app, the GPU's lowest
// clock goes up (debug.intelxe.slpc_min, sent to the GuC by debug.intelxe.step=17, a knob the release kext has), and
// optionally Time Machine backups that run then are stopped. A root helper (Resources/gamemode-helper.sh) does both: the
// app starts it once through the password dialog and tells it "game" or "desktop" through a pipe only this user can
// write. It puts everything back when told, when the app ends (it watches the app's process) or on a signal; the clock
// it sets lives in the kernel only and is gone at the next start of macOS anyway.
// A kext that offers debug.intelxe.slpc_boost (any user may write it: a floor lease of ten seconds, renewed by the next
// write, clamped by the kernel to the administrator's ceiling) needs no helper: the app writes the floor itself every
// 5 s while a game is in front and 0 when it leaves; if the app dies the kernel drops the floor within 10 s. The helper
// stays for an older kext, and for the Time Machine option (tmutil needs root).
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

/// The kext's floor lease (debug.intelxe.slpc_boost, DESIGN-game-mode.md): readable when the kext has it.
func slpcBoostAvailable() -> Bool {
    var v: Int32 = 0
    var size = MemoryLayout<Int32>.size
    return sysctlbyname("debug.intelxe.slpc_boost", &v, &size, nil, 0) == 0
}

/// Writes the lease (MHz, 0 = release); 0 or the errno. EAGAIN (the GPU sleeps or resets) and EBUSY (rate limit) pass:
/// the next renewal tries again.
func slpcBoostWrite(_ mhz: Int) -> Int32 {
    var v = Int32(clamping: mhz)
    return sysctlbyname("debug.intelxe.slpc_boost", nil, nil, &v, MemoryLayout<Int32>.size) == 0 ? 0 : errno
}

func slpcBoostErrorIsTransient(_ err: Int32) -> Bool { err == 0 || err == EAGAIN || err == EBUSY }

/// Game mode without the root helper: the kext has the lease and nothing else needs root (Time Machine does).
func gameModeUsesKernelLease(available: Bool, stopBackups: Bool) -> Bool { available && !stopBackups }

/// What the lease should be: the chosen floor while a game is in front, else 0 (released).
func gameModeLeaseMHz(game: String?, floor: Int) -> Int { game != nil ? floor : 0 }

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

    /// on through the kext's lease (no helper) rather than the helper
    @Published private(set) var kernelLease = false
    /// the kext offers the lease (read when the Power and app page shows, and at each start)
    @Published private(set) var leaseAvailable = slpcBoostAvailable()

    private var observers: [NSObjectProtocol] = []
    private var watchdog: Timer?
    private var renewal: Timer?
    private let leaseQueue = DispatchQueue(label: "IntelXeManager.gamemode.lease")

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
            MainActor.assumeIsolated {
                let gm = GameMode.shared
                if gm.kernelLease { _ = slpcBoostWrite(0) } else { _ = gm.send("quit") }
            }
        })
    }

    /// Whether turning it on now would use the kext's lease (no password).
    var wouldUseKernelLease: Bool { gameModeUsesKernelLease(available: leaseAvailable, stopBackups: stopBackups) }

    func refreshLeaseAvailable() { leaseAvailable = slpcBoostAvailable() }

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
        guard !on, !starting else { return }
        refreshLeaseAvailable()
        if wouldUseKernelLease { startLease(); return }
        guard let script = Bundle.main.url(forResource: "gamemode-helper", withExtension: "sh") else { return }
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

    /// On through the kext's lease: no helper, no password. The renewal every 5 s keeps the 10 s lease while a game stays
    /// in front.
    private func startLease() {
        message = nil
        on = true; kernelLease = true
        renewal = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
            MainActor.assumeIsolated { if GameMode.shared.game != nil { GameMode.shared.writeLease() } }
        }
        frontChanged(NSWorkspace.shared.frontmostApplication)
    }

    /// The lease as it should be now, written off the main thread (the sysctl may wait for the driver's bring-up).
    private func writeLease() {
        guard kernelLease else { return }
        let mhz = gameModeLeaseMHz(game: game, floor: floorMHz)
        leaseQueue.async {
            let err = slpcBoostWrite(mhz)
            guard !slpcBoostErrorIsTransient(err) else { return }
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    let gm = GameMode.shared
                    guard gm.kernelLease else { return }
                    gm.stop()
                    gm.refreshLeaseAvailable()
                    gm.message = String(localized: "Game mode stopped: the driver refused the GPU clock (error \(err)).")
                }
            }
        }
    }

    func stop() {
        guard on else { return }
        if kernelLease {
            renewal?.invalidate(); renewal = nil
            kernelLease = false
            leaseQueue.async { _ = slpcBoostWrite(0) }
        } else {
            _ = send("quit")
        }
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
        if kernelLease {
            game = nowGame
            writeLease()
        } else if send(nowGame != nil ? "game" : "desktop") { game = nowGame } else { checkHelper() }
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
            if gm.on ? gm.kernelLease : gm.wouldUseKernelLease {
                CaptionedToggle(title: "Game mode", caption: "While a game is in front: a higher lowest GPU clock. No password; off again when this app quits.",
                                detail: "macOS's own Game Mode already gives a full-screen game priority, but it does not know this driver's GPU clock. With this on, the GPU's lowest clock goes up while a game is the frontmost app, so that frames do not wait for the clock to rise, and goes back when you switch away. The driver lends the clock for ten seconds at a time and this app renews it while the game stays in front: no password is needed, and if this app stops, the clock goes back within ten seconds. It never goes above the GPU's highest clock as the driver is set.",
                                isOn: Binding(get: { gm.on || gm.starting }, set: { gm.setOn($0) }))
                    .disabled(gm.starting)
            } else {
                CaptionedToggle(title: "Game mode", caption: "While a game is in front: a higher lowest GPU clock. Off again when this app quits.",
                                detail: "macOS's own Game Mode already gives a full-screen game priority, but it does not know this driver's GPU clock. With this on, the GPU's lowest clock goes up while a game is the frontmost app, so that frames do not wait for the clock to rise, and goes back when you switch away. Asks for the administrator password once; nothing is kept: it is off again when this app quits, and at the next start of macOS in any case.",
                                isOn: Binding(get: { gm.on || gm.starting }, set: { gm.setOn($0) }))
                    .disabled(gm.starting)
            }
            Picker("Lowest GPU clock in a game", selection: $gm.floorMHz) {
                Text("2000 MHz").tag(2000); Text("2400 MHz").tag(2400); Text("The card's highest").tag(3000)
            }
            .disabled(gm.on || gm.starting)
            if gm.leaseAvailable {
                CaptionedToggle(title: "Stop Time Machine backups during a game", caption: "A backup that runs while a game is in front is stopped; the next one runs as scheduled. Needs the administrator password.",
                                isOn: $gm.stopBackups)
                    .disabled(gm.on || gm.starting)
            } else {
                CaptionedToggle(title: "Stop Time Machine backups during a game", caption: "A backup that runs while a game is in front is stopped; the next one runs as scheduled.",
                                isOn: $gm.stopBackups)
                    .disabled(gm.on || gm.starting)
            }
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
        .onAppear { gm.refreshLeaseAvailable() }
    }
}

/// The menu bar item's lines.
struct GameModeMenu: View {
    @ObservedObject var gm = GameMode.shared
    var body: some View {
        Text(gm.statusLine)
        Button(gm.on ? String(localized: "Turn game mode off")
               : gm.wouldUseKernelLease ? String(localized: "Turn game mode on") : String(localized: "Turn game mode on…")) { gm.setOn(!gm.on) }
            .disabled(gm.starting)
        if let f = gm.lastFront, !gm.extraGames.contains(f.key), gm.game == nil {
            Button(String(localized: "Count \(f.name) as a game")) { gm.addLastFront() }
        }
    }
}
