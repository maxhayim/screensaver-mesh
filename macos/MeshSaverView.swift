import AppKit
import MeshCore
import ScreenSaver

private func cgColor(_ c: mesh_color) -> CGColor {
    CGColor(srgbRed: CGFloat(c.r), green: CGFloat(c.g), blue: CGFloat(c.b), alpha: CGFloat(c.a))
}

private func parseColor(_ hex: String, fallback: String) -> mesh_color {
    var c = mesh_color()
    if mesh_parse_color(hex, &c) == 0 { mesh_parse_color(fallback, &c) }
    return c
}

private let drawLine: @convention(c) (UnsafeMutableRawPointer?, Float, Float, Float, Float, Float, mesh_color) -> Void = {
    ctx, x0, y0, x1, y1, width, color in
    guard let ctx else { return }
    let cg = Unmanaged<CGContext>.fromOpaque(ctx).takeUnretainedValue()
    cg.setStrokeColor(cgColor(color))
    cg.setLineWidth(CGFloat(width))
    cg.beginPath()
    cg.move(to: CGPoint(x: CGFloat(x0), y: CGFloat(y0)))
    cg.addLine(to: CGPoint(x: CGFloat(x1), y: CGFloat(y1)))
    cg.strokePath()
}

private let drawCircle: @convention(c) (UnsafeMutableRawPointer?, Float, Float, Float, Int32, Float, mesh_color) -> Void = {
    ctx, x, y, radius, filled, width, color in
    guard let ctx else { return }
    let cg = Unmanaged<CGContext>.fromOpaque(ctx).takeUnretainedValue()
    let r = CGFloat(radius)
    let rect = CGRect(x: CGFloat(x) - r, y: CGFloat(y) - r, width: r * 2, height: r * 2)
    if filled != 0 {
        cg.setFillColor(cgColor(color))
        cg.fillEllipse(in: rect)
    } else {
        cg.setStrokeColor(cgColor(color))
        cg.setLineWidth(CGFloat(width))
        cg.strokeEllipse(in: rect)
    }
}

@objc(MeshSaverView)
final class MeshSaverView: ScreenSaverView {
    private let core: OpaquePointer
    private var settings = MeshSettings.load()
    private var client: MeshMonitorClient?
    private var lastTick: CFTimeInterval = 0
    private var configureController: ConfigureSheetController?
    private lazy var clockFormatter = DateFormatter()

    override init?(frame: NSRect, isPreview: Bool) {
        core = mesh_create(UInt32.random(in: 1...UInt32.max))
        super.init(frame: frame, isPreview: isPreview)
        animationTimeInterval = 1.0 / 60.0
        applySettings()
        mesh_resize(core, Float(frame.width), Float(frame.height))

        if !isPreview {
            // Since macOS 14 the legacyScreenSaver host can keep a saver running after
            // the screen saver ends; quit with it so we don't keep polling in the background.
            DistributedNotificationCenter.default().addObserver(
                self, selector: #selector(screenSaverWillStop),
                name: NSNotification.Name("com.apple.screensaver.willstop"), object: nil)
        }
    }

    required init?(coder: NSCoder) {
        core = mesh_create(UInt32.random(in: 1...UInt32.max))
        super.init(coder: coder)
    }

    deinit {
        client?.stop()
        DistributedNotificationCenter.default().removeObserver(self)
        mesh_destroy(core)
    }

    override var isFlipped: Bool { true }
    override var hasConfigureSheet: Bool { true }

    override var configureSheet: NSWindow? {
        let controller = ConfigureSheetController(settings: MeshSettings.load()) { [weak self] saved in
            self?.settings = saved
            self?.applySettings()
            self?.connect()
        }
        configureController = controller
        return controller.window
    }

    @objc private func screenSaverWillStop() {
        exit(0)
    }

    // MARK: - Lifecycle

    override func startAnimation() {
        super.startAnimation()
        settings = MeshSettings.load()
        applySettings()
        lastTick = 0
        connect()
    }

    override func stopAnimation() {
        super.stopAnimation()
        client?.stop()
        client = nil
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        mesh_resize(core, Float(newSize.width), Float(newSize.height))
    }

    private func applySettings() {
        var o = mesh_default_options()
        let base = ColorPreset.all[0]
        o.background = parseColor(settings.background, fallback: base.background)
        o.node = parseColor(settings.dots, fallback: base.dots)
        o.link = parseColor(settings.lines, fallback: base.lines)
        o.packet = parseColor(settings.packets, fallback: base.packets)
        o.reduced_motion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion ? 1 : 0
        o.compact = isPreview ? 1 : 0
        mesh_set_options(core, &o)
        clockFormatter.dateFormat = settings.use24Hour ? "HH:mm" : "h:mm a"
        needsDisplay = true
    }

    /// Live mode when a MeshMonitor server is set up; the simulated mesh otherwise
    /// (and whenever the server can't be reached).
    private func connect() {
        client?.stop()
        client = nil
        mesh_set_live(core, 0)
        guard settings.isLiveConfigured, let url = MeshMonitorClient.baseURL(from: settings.server) else { return }

        let c = MeshMonitorClient(config: .init(baseURL: url, token: settings.token, source: settings.source))
        c.onNodes = { [weak self] ids in
            guard let self, !ids.isEmpty else { return }
            mesh_set_live(self.core, 1)
            Self.withCStrings(ids) { mesh_set_nodes(self.core, $0, Int32(ids.count)) }
        }
        c.onMessage = { [weak self] from, to in
            guard let self, mesh_is_live(self.core) != 0 else { return }
            if let to { mesh_message(self.core, from, to) } else { mesh_message(self.core, from, nil) }
        }
        c.onOffline = { [weak self] in
            guard let self else { return }
            mesh_set_live(self.core, 0)
        }
        client = c
        c.start()
    }

    private static func withCStrings(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> Void) {
        let copies = strings.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers = copies.map { UnsafePointer<CChar>($0) }
        pointers.withUnsafeBufferPointer { body($0.baseAddress!) }
    }

    // MARK: - Drawing

    override func animateOneFrame() {
        let now = CACurrentMediaTime()
        let dt = lastTick == 0 ? 0 : now - lastTick
        lastTick = now
        mesh_step(core, Float(dt))
        needsDisplay = true
    }

    override func draw(_ rect: NSRect) {
        guard let cg = NSGraphicsContext.current?.cgContext else { return }
        let options = parseColor(settings.background, fallback: ColorPreset.all[0].background)
        cg.setFillColor(cgColor(options))
        cg.fill(bounds)
        cg.setLineCap(.round)

        var renderer = mesh_renderer(
            ctx: Unmanaged.passUnretained(cg).toOpaque(),
            line: drawLine,
            circle: drawCircle)
        mesh_render(core, &renderer)

        if settings.showClock { drawClock() }
    }

    /// Bottom-left: the time in large type with "Mesh" under it.
    private func drawClock() {
        let ink = NSColor(cgColor: cgColor(parseColor(settings.dots, fallback: ColorPreset.all[0].dots))) ?? .white
        let scale = isPreview ? max(0.25, bounds.height / 900) : 1
        let margin = 32 * scale

        let timeFont = NSFont.monospacedDigitSystemFont(ofSize: 48 * scale, weight: .semibold)
        let time = NSAttributedString(string: clockFormatter.string(from: Date()), attributes: [
            .font: timeFont,
            .foregroundColor: ink.withAlphaComponent(0.8),
            .kern: -1.0 * scale,
        ])
        let live = mesh_is_live(core) != 0
        let label = NSAttributedString(string: live ? "Mesh · live" : "Mesh", attributes: [
            .font: NSFont.systemFont(ofSize: 12 * scale, weight: .regular),
            .foregroundColor: ink.withAlphaComponent(0.5),
        ])

        let labelSize = label.size()
        let timeSize = time.size()
        let labelY = bounds.height - margin - labelSize.height
        label.draw(at: NSPoint(x: margin, y: labelY))
        time.draw(at: NSPoint(x: margin, y: labelY - timeSize.height + 6 * scale))
    }
}
