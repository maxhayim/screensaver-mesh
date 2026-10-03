// Loads build/Mesh.saver like the screen saver host does and writes frames to PNG.
//   swiftc tools/preview.swift -o build/preview -framework ScreenSaver
//   build/preview build/Mesh.saver out.png [seconds] [width] [height] [--preview]
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
