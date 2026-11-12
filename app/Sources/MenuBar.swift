// SPDX-License-Identifier: MIT
// The menu bar item (opt-in, from the Monitor page): frames, GPU clock and temperature at a glance, the main display's
// "looks like" sizes at the current refresh rate (the HiDPI ones included) to switch with one click, game mode
// (GameMode.swift: on or off, and the app in front counted as a game), and the app.
import AppKit
import CoreGraphics
import SwiftUI

/// The main display's sizes at the refresh rate in use, one per "looks like" size (the HiDPI variant when there is one).
struct DisplaySize: Identifiable, Hashable {
    let mode: CGDisplayMode
    var id: String { "\(mode.width)x\(mode.height)-\(mode.pixelWidth)" }
    var title: String { "\(mode.width) × \(mode.height)" + (mode.pixelWidth > mode.width ? "  HiDPI" : "") }
    static func == (a: DisplaySize, b: DisplaySize) -> Bool { a.id == b.id }
    func hash(into h: inout Hasher) { h.combine(id) }
}

func displaySizes() -> (sizes: [DisplaySize], current: String) {
    let id = CGMainDisplayID()
    guard let cur = CGDisplayCopyDisplayMode(id) else { return ([], "") }
    let opts = [kCGDisplayShowDuplicateLowResolutionModes as String: true] as CFDictionary
    let all = (CGDisplayCopyAllDisplayModes(id, opts) as? [CGDisplayMode]) ?? []
    var best: [String: CGDisplayMode] = [:]
    for m in all where m.isUsableForDesktopGUI() && abs(m.refreshRate - cur.refreshRate) < 0.5 {
        let key = "\(m.width)x\(m.height)"
        if let b = best[key], b.pixelWidth >= m.pixelWidth { continue }   // prefer the HiDPI one of a size
        best[key] = m
    }
    let sizes = best.values.map { DisplaySize(mode: $0) }.sorted { $0.mode.width > $1.mode.width }
    return (sizes, DisplaySize(mode: cur).id)
}

struct MenuBarContent: View {
    @ObservedObject var model: MonitorModel
    @Environment(\.openWindow) var openWindow
    var body: some View {
        let s = model.now
        Text(s.fps.map { String(format: "%.0f ", $0) + String(localized: "frames/s") } ?? "–")
        if let c = s.actMHz { Text(c == 0 ? String(localized: "GPU idle") : String(localized: "GPU \(c) MHz")) }
        if let t = s.packageC { Text(String(localized: "GPU \(t) °C")) }
        Divider()
        let (sizes, current) = displaySizes()
        Menu(String(localized: "Display size")) {
            ForEach(sizes) { size in
                Button((size.id == current ? "✓ " : "    ") + size.title) {
                    var cfg: CGDisplayConfigRef?
                    if CGBeginDisplayConfiguration(&cfg) == .success {
                        CGConfigureDisplayWithDisplayMode(cfg, CGMainDisplayID(), size.mode, nil)
                        CGCompleteDisplayConfiguration(cfg, .permanently)
                    }
                }
            }
        }
        Divider()
        GameModeMenu()
        Divider()
        Button(String(localized: "Open IntelXe Manager")) { NSApp.activate(ignoringOtherApps: true); openWindow(id: "main") }
        Button(String(localized: "Quit")) { NSApp.terminate(nil) }
    }
}
