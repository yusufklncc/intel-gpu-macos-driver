// SPDX-License-Identifier: MIT
import AppKit
import SwiftUI

struct ManagerApp: App {
    @StateObject var manager = Manager()
    @StateObject var menuModel = MonitorModel()
    @AppStorage("menuBarItem") var menuBarItem = false
    @ObservedObject var gameMode = GameMode.shared   // made at start: it follows the app in front from then on
    var body: some Scene {
        WindowGroup("IntelXe Manager", id: "main") {
            RootView().environmentObject(manager)
        }
        .windowToolbarStyle(.unified)
        .defaultSize(width: 980, height: 720)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .appInfo) { AboutMenuItem() }
        }
        Window("About IntelXe Manager", id: "about") { AboutView() }
            .windowResizability(.contentSize)
            .windowStyle(.hiddenTitleBar)
        MenuBarExtra(isInserted: $menuBarItem) {
            MenuBarContent(model: menuModel).onAppear { menuModel.start() }
        } label: {
            Image(systemName: gameMode.game != nil ? "gamecontroller.fill" : "gauge.with.dots.needle.33percent")
        }
    }
}

if CommandLine.arguments.contains("--selftest") {
    selfTest()
} else if CommandLine.arguments.contains("--login-check") {   // LoginCheck.swift: the LaunchAgent's run at login
    runLoginCheck()
} else if CommandLine.arguments.contains("--gputest") || CommandLine.arguments.contains("--longtest") {   // the Report page's tests, in Terminal
    let long = CommandLine.arguments.contains("--longtest")
    for r in (long ? runLongTest(progress: { _ in }) : runGPUTest(progress: { _ in })) {
        let mark = r.state == .good ? "ok  " : r.state == .warning ? "warn" : r.state == .bad ? "BAD " : "    "
        print("\(mark) \(r.title.padding(toLength: 26, withPad: " ", startingAt: 0)) \(r.value)\(r.help.isEmpty ? "" : "\n       " + r.help)")
    }
} else {
    ManagerApp.main()
}
