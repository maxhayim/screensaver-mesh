// Loads build/Mesh.saver like the screen saver host does and writes frames to PNG.
//   swiftc tools/preview.swift -o build/preview -framework ScreenSaver
//   build/preview build/Mesh.saver out.png [seconds] [width] [height] [--preview]
//   build/preview build/Mesh.saver out.png --sheet      (the Options sheet instead)
//   build/preview build/Mesh.saver - --paste-test       (checks pasting a token into the sheet)
import AppKit
import ScreenSaver

let args = CommandLine.arguments
guard args.count >= 3, let bundle = Bundle(path: args[1]), bundle.load(),
      let saverClass = bundle.principalClass as? ScreenSaverView.Type else {
    FileHandle.standardError.write("usage: preview <Mesh.saver> <out.png> [seconds] [width] [height] [--preview]\n".data(using: .utf8)!)
    exit(1)
}
let seconds = args.count > 3 ? Double(args[3]) ?? 3 : 3
let width = args.count > 4 ? Double(args[4]) ?? 1440 : 1440
let height = args.count > 5 ? Double(args[5]) ?? 900 : 900
let isPreview = args.contains("--preview")

_ = NSApplication.shared
let frame = NSRect(x: 0, y: 0, width: width, height: height)
guard let view = saverClass.init(frame: frame, isPreview: isPreview) else { exit(2) }
func allViews(_ v: NSView) -> [NSView] { [v] + v.subviews.flatMap(allViews) }

if args.contains("--paste-test") {
    guard let sheet = view.configureSheet, let content = sheet.contentView else { exit(4) }
    let views = allViews(content)
    let secure = views.compactMap { $0 as? NSSecureTextField }.first!
    let fields = views.compactMap { $0 as? NSTextField }.filter { $0.isEditable && !($0 is NSSecureTextField) }
    let plain = fields.first { $0.placeholderString == "mm_v1_…" }!
    let paste = views.compactMap { $0 as? NSButton }.first { $0.title == "Paste" }!
    let show = views.compactMap { $0 as? NSButton }.first { $0.title == "Show" }!

    // Borrow the clipboard and always put back what was there (exit() skips defer).
    let board = NSPasteboard.general
    let saved = board.pasteboardItems?.map { item -> NSPasteboardItem in
        let copy = NSPasteboardItem()
        for type in item.types { if let data = item.data(forType: type) { copy.setData(data, forType: type) } }
        return copy
    } ?? []
    func restoreClipboard() {
        board.clearContents()
        if !saved.isEmpty { board.writeObjects(saved) }
    }
    let token = "mm_v1_" + (0..<48).map { _ in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-".randomElement()! }.map(String.init).joined()
    var failures = 0
    func check(_ ok: Bool, _ what: String) { print((ok ? "ok   " : "FAIL ") + what); if !ok { failures += 1 } }

    board.clearContents()
    board.setString(token + "\n", forType: .string)
    paste.performClick(nil)
    check(secure.stringValue == token, "Paste button fills the whole token (\(secure.stringValue.count) of \(token.count) chars)")

    secure.stringValue = ""
    sheet.makeKeyAndOrderFront(nil)
    sheet.makeFirstResponder(secure)
    let cmdV = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command, timestamp: 0,
                                windowNumber: sheet.windowNumber, context: nil, characters: "v",
                                charactersIgnoringModifiers: "v", isARepeat: false, keyCode: 9)!
    let handled = sheet.performKeyEquivalent(with: cmdV)
    sheet.makeFirstResponder(nil) // ends editing so the field takes the text
    check(handled && secure.stringValue.trimmingCharacters(in: .whitespacesAndNewlines) == token,
          "⌘V pastes the whole token (\(secure.stringValue.count) chars)")

    show.performClick(nil)
    check(!plain.isHidden && secure.isHidden && plain.stringValue.trimmingCharacters(in: .whitespacesAndNewlines) == token,
          "Show reveals the same token")
    sheet.orderOut(nil)
    restoreClipboard()
    exit(failures == 0 ? 0 : 1)
}

if args.contains("--sheet") {
    guard let sheet = view.configureSheet, let content = sheet.contentView else { exit(4) }
    content.layoutSubtreeIfNeeded()
    print("sheet \(Int(sheet.frame.width))x\(Int(sheet.frame.height)), content \(content.bounds.size)")
    content.appearance = NSAppearance(named: .aqua)
    guard let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds) else { exit(3) }
    content.cacheDisplay(in: content.bounds, to: rep)
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
    print("wrote \(args[2])")
    exit(0)
}
let window = NSWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
window.contentView = view
view.startAnimation()

let end = Date().addingTimeInterval(seconds)
while Date() < end {
    view.animateOneFrame()
    RunLoop.main.run(until: Date().addingTimeInterval(1.0 / 60.0))
}

guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { exit(3) }
view.cacheDisplay(in: view.bounds, to: rep)
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
view.stopAnimation()
print("wrote \(args[2])")
