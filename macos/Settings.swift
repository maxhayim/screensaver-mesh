import Foundation
import ScreenSaver

/// A color scheme for the saver. Colors are "#rrggbb" strings, the same format the core parses.
struct ColorPreset {
    let name: String
    let background: String
    let dots: String
    let lines: String
    let packets: String

    static let all: [ColorPreset] = [
        ColorPreset(name: "Default", background: "#0b0b0a", dots: "#eeebe4", lines: "#eeebe4", packets: "#f06a2a"),
        ColorPreset(name: "Meshtastic", background: "#0a0f0b", dots: "#e8f5ec", lines: "#67ea94", packets: "#67ea94"),
        ColorPreset(name: "Green terminal", background: "#000000", dots: "#33ff66", lines: "#33ff66", packets: "#ccffcc"),
        ColorPreset(name: "Amber terminal", background: "#0a0700", dots: "#ffb000", lines: "#ffb000", packets: "#fff1c4"),
        ColorPreset(name: "Paper", background: "#f4f1ea", dots: "#1d1c1a", lines: "#1d1c1a", packets: "#e8591a"),
    ]
}

/// Everything the user can set, stored in the screen saver's own defaults domain.
struct MeshSettings {
    static let moduleName = "com.maxhayim.screensaver-mesh"

    var server = ""
    var token = ""
    var source = "default"
    var background = ColorPreset.all[0].background
    var dots = ColorPreset.all[0].dots
    var lines = ColorPreset.all[0].lines
    var packets = ColorPreset.all[0].packets
    var showClock = true
    var use24Hour = false

    var isLiveConfigured: Bool {
        !server.trimmingCharacters(in: .whitespaces).isEmpty && !token.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private static var defaults: UserDefaults? { ScreenSaverDefaults(forModuleWithName: moduleName) }

    static func load() -> MeshSettings {
        var s = MeshSettings()
        guard let d = defaults else { return s }
        s.server = d.string(forKey: "server") ?? s.server
        s.token = d.string(forKey: "token") ?? s.token
        s.source = d.string(forKey: "source") ?? s.source
        s.background = d.string(forKey: "background") ?? s.background
        s.dots = d.string(forKey: "dots") ?? s.dots
        s.lines = d.string(forKey: "lines") ?? s.lines
        s.packets = d.string(forKey: "packets") ?? s.packets
        if d.object(forKey: "showClock") != nil { s.showClock = d.bool(forKey: "showClock") }
        if d.object(forKey: "use24Hour") != nil { s.use24Hour = d.bool(forKey: "use24Hour") }
        return s
    }

    func save() {
        guard let d = MeshSettings.defaults else { return }
        d.set(server, forKey: "server")
        d.set(token, forKey: "token")
        d.set(source, forKey: "source")
        d.set(background, forKey: "background")
        d.set(dots, forKey: "dots")
        d.set(lines, forKey: "lines")
        d.set(packets, forKey: "packets")
        d.set(showClock, forKey: "showClock")
        d.set(use24Hour, forKey: "use24Hour")
        d.synchronize()
    }
}
