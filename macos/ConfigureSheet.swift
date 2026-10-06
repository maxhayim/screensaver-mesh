import AppKit

/// The screen saver host has no Edit menu, and ⌘V, ⌘C, ⌘X, ⌘A, and ⌘Z normally
/// reach text fields through that menu. Without this, pasting an API token
/// silently fails. The window handles those shortcuts itself.
final class EditingWindow: NSWindow {
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if flags == .command || flags == [.command, .shift], let key = event.charactersIgnoringModifiers?.lowercased() {
            let action: Selector?
            switch (key, flags.contains(.shift)) {
            case ("v", false): action = #selector(NSText.paste(_:))
            case ("c", false): action = #selector(NSText.copy(_:))
            case ("x", false): action = #selector(NSText.cut(_:))
            case ("a", false): action = #selector(NSText.selectAll(_:))
            case ("z", false): action = Selector(("undo:"))
            case ("z", true): action = Selector(("redo:"))
            default: action = nil
            }
            // Straight to this window's focused field: inside the host, this window
            // usually isn't the app's key window, so NSApp.sendAction(to: nil) misses it.
            if let action, let responder = firstResponder, responder.tryToPerform(action, with: self) { return true }
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// The "Options…" sheet in System Settings › Screen Saver.
final class ConfigureSheetController: NSObject {
    let window: NSWindow
    private var settings: MeshSettings
    private let onSave: (MeshSettings) -> Void

    private let serverField = NSTextField()
    private let tokenField = NSSecureTextField()
    private let tokenPlainField = NSTextField() // shown instead of tokenField with "Show"
    private let showTokenCheck = NSButton(checkboxWithTitle: "Show", target: nil, action: nil)
    private let sourceField = NSTextField()
    private let statusLabel = NSTextField(labelWithString: "")
    private let presetPopup = NSPopUpButton()
    private let backgroundWell = NSColorWell()
    private let dotsWell = NSColorWell()
    private let linesWell = NSColorWell()
    private let packetsWell = NSColorWell()
    private let clockCheck = NSButton(checkboxWithTitle: "Show the clock", target: nil, action: nil)
    private let hourCheck = NSButton(checkboxWithTitle: "24-hour time", target: nil, action: nil)
    private let labelPopup = NSPopUpButton()
    private let labelField = NSTextField()
    private static let labelModes = [("mesh", "Mesh"), ("user", "Your name"), ("custom", "Custom text")]
    private var testClient: MeshMonitorClient?
    private static let labelWidth: CGFloat = 80
    private static let fieldWidth: CGFloat = 300

    init(settings: MeshSettings, onSave: @escaping (MeshSettings) -> Void) {
        self.settings = settings
        self.onSave = onSave
        window = EditingWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 420),
                          styleMask: [.titled], backing: .buffered, defer: false)
        super.init()
        build()
        load()
    }

    // MARK: - Layout

    private func build() {
        serverField.placeholderString = "https://meshmonitor.example.com"
        tokenField.placeholderString = "mm_v1_…"
        tokenPlainField.placeholderString = "mm_v1_…"
        tokenPlainField.isHidden = true
        sourceField.placeholderString = "default"
        [serverField, tokenField, tokenPlainField, sourceField].forEach { $0.widthAnchor.constraint(equalToConstant: Self.fieldWidth).isActive = true }
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.widthAnchor.constraint(equalToConstant: Self.fieldWidth).isActive = true

        let tokenFields = NSStackView(views: [tokenField, tokenPlainField])
        let pasteButton = NSButton(title: "Paste", target: self, action: #selector(pasteToken))
        pasteButton.controlSize = .small
        showTokenCheck.controlSize = .small
        showTokenCheck.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        showTokenCheck.target = self
        showTokenCheck.action = #selector(toggleShowToken)
        let tokenTools = NSStackView(views: [pasteButton, showTokenCheck])
        tokenTools.spacing = 12

        let testButton = NSButton(title: "Test connection", target: self, action: #selector(testConnection))
        let hint = NSTextField(wrappingLabelWithString: "Leave the server empty to show the simulated mesh. Create an API token in MeshMonitor under User Settings.")
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        hint.textColor = .secondaryLabelColor
        hint.preferredMaxLayoutWidth = Self.fieldWidth

        presetPopup.addItems(withTitles: ColorPreset.all.map(\.name) + ["Custom"])
        presetPopup.target = self
        presetPopup.action = #selector(presetChanged)
        for well in [backgroundWell, dotsWell, linesWell, packetsWell] {
            well.target = self
            well.action = #selector(colorChanged)
            if #available(macOS 13.0, *) { well.colorWellStyle = .minimal }
            well.widthAnchor.constraint(equalToConstant: 44).isActive = true
            well.heightAnchor.constraint(equalToConstant: 24).isActive = true
        }
        let wells = NSStackView(views: [
            labeled("Background", backgroundWell), labeled("Dots", dotsWell),
            labeled("Lines", linesWell), labeled("Packets", packetsWell),
        ])
        wells.spacing = 14

        let clockRow = NSStackView(views: [clockCheck, hourCheck])
        clockRow.spacing = 16

        labelPopup.addItems(withTitles: Self.labelModes.map(\.1))
        labelPopup.target = self
        labelPopup.action = #selector(labelModeChanged)
        labelField.placeholderString = "Text under the clock"
        labelField.widthAnchor.constraint(equalToConstant: 150).isActive = true
        let labelRow = NSStackView(views: [labelPopup, labelField])
        labelRow.spacing = 8

        let grid = NSGridView(views: [
            [header("MeshMonitor"), NSGridCell.emptyContentView],
            [label("Server"), serverField],
            [label("API token"), tokenFields],
            [NSGridCell.emptyContentView, tokenTools],
            [label("Source"), sourceField],
            [NSGridCell.emptyContentView, NSStackView(views: [testButton])],
            [NSGridCell.emptyContentView, statusLabel],
            [NSGridCell.emptyContentView, hint],
            [header("Colors"), NSGridCell.emptyContentView],
            [label("Preset"), presetPopup],
            [NSGridCell.emptyContentView, wells],
            [header("Clock"), NSGridCell.emptyContentView],
            [NSGridCell.emptyContentView, clockRow],
            [label("Label"), labelRow],
        ])
        grid.rowSpacing = 8
        grid.rowAlignment = .firstBaseline
        grid.columnSpacing = 10
        grid.column(at: 0).xPlacement = .trailing
        for row in [0, 8, 11] { // the section headers
            grid.row(at: row).topPadding = row == 0 ? 0 : 10
            grid.row(at: row).mergeCells(in: NSRange(location: 0, length: 2))
            grid.cell(atColumnIndex: 0, rowIndex: row).xPlacement = .leading
        }

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        cancel.keyEquivalent = "\u{1b}"
        let ok = NSButton(title: "OK", target: self, action: #selector(save))
        ok.keyEquivalent = "\r"
        let buttons = NSStackView(views: [cancel, ok])

        // Explicit columns and margins: NSGridView's own sizing leaves the sheet lopsided.
        grid.column(at: 0).width = Self.labelWidth
        grid.column(at: 1).width = Self.fieldWidth
        let root = NSView()
        [grid, buttons].forEach {
            $0.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview($0)
        }
        let margin: CGFloat = 20
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: root.topAnchor, constant: margin),
            grid.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: margin),
            grid.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -margin),
            buttons.topAnchor.constraint(equalTo: grid.bottomAnchor, constant: 18),
            buttons.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -margin),
            buttons.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -margin),
        ])
        window.contentView = root
        root.layoutSubtreeIfNeeded()
        window.setContentSize(root.fittingSize)
    }

    private func label(_ text: String) -> NSTextField { NSTextField(labelWithString: text) }

    private func header(_ text: String) -> NSTextField {
        let f = NSTextField(labelWithString: text)
        f.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        return f
    }

    private func labeled(_ text: String, _ view: NSView) -> NSStackView {
        let caption = NSTextField(labelWithString: text)
        caption.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        caption.textColor = .secondaryLabelColor
        let stack = NSStackView(views: [view, caption])
        stack.orientation = .vertical
        stack.spacing = 4
        return stack
    }

    // MARK: - Values

    private func load() {
        serverField.stringValue = settings.server
        tokenField.stringValue = settings.token
        tokenPlainField.stringValue = settings.token
        sourceField.stringValue = settings.source == "default" ? "" : settings.source
        backgroundWell.color = Self.color(settings.background)
        dotsWell.color = Self.color(settings.dots)
        linesWell.color = Self.color(settings.lines)
        packetsWell.color = Self.color(settings.packets)
        clockCheck.state = settings.showClock ? .on : .off
        hourCheck.state = settings.use24Hour ? .on : .off
        labelPopup.selectItem(at: Self.labelModes.firstIndex { $0.0 == settings.label } ?? 0)
        labelField.stringValue = settings.labelText
        labelModeChanged()
        selectMatchingPreset()
    }

    private func readFields() {
        settings.server = serverField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        settings.token = currentToken
        let source = sourceField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        settings.source = source.isEmpty ? "default" : source
        settings.background = Self.hex(backgroundWell.color)
        settings.dots = Self.hex(dotsWell.color)
        settings.lines = Self.hex(linesWell.color)
        settings.packets = Self.hex(packetsWell.color)
        settings.showClock = clockCheck.state == .on
        settings.use24Hour = hourCheck.state == .on
        settings.label = Self.labelModes[max(0, labelPopup.indexOfSelectedItem)].0
        settings.labelText = String(labelField.stringValue.prefix(60))
    }

    private func selectMatchingPreset() {
        let current = [backgroundWell, dotsWell, linesWell, packetsWell].map { Self.hex($0.color) }
        let index = ColorPreset.all.firstIndex {
            [$0.background, $0.dots, $0.lines, $0.packets].map { $0.lowercased() } == current
        }
        presetPopup.selectItem(at: index ?? ColorPreset.all.count)
    }

    static func color(_ hex: String) -> NSColor {
        var s = hex.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return .white }
        return NSColor(srgbRed: CGFloat((v >> 16) & 0xff) / 255, green: CGFloat((v >> 8) & 0xff) / 255,
                       blue: CGFloat(v & 0xff) / 255, alpha: 1)
    }

    static func hex(_ color: NSColor) -> String {
        guard let c = color.usingColorSpace(.sRGB) else { return "#ffffff" }
        let v = [c.redComponent, c.greenComponent, c.blueComponent].map { Int(($0 * 255).rounded()).clamped(0, 255) }
        return String(format: "#%02x%02x%02x", v[0], v[1], v[2])
    }

    // MARK: - Actions

    @objc private func presetChanged() {
        let i = presetPopup.indexOfSelectedItem
        guard i < ColorPreset.all.count else { return }
        let p = ColorPreset.all[i]
        backgroundWell.color = Self.color(p.background)
        dotsWell.color = Self.color(p.dots)
        linesWell.color = Self.color(p.lines)
        packetsWell.color = Self.color(p.packets)
    }

    @objc private func colorChanged() { selectMatchingPreset() }

    private var currentToken: String {
        (showTokenCheck.state == .on ? tokenPlainField : tokenField).stringValue
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Fills the token straight from the clipboard, whatever the host does with ⌘V.
    @objc private func pasteToken() {
        guard let text = NSPasteboard.general.string(forType: .string)?
            .trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty else {
            statusLabel.stringValue = "The clipboard has no text to paste"
            return
        }
        tokenField.stringValue = text
        tokenPlainField.stringValue = text
        statusLabel.stringValue = "Pasted a token of \(text.count) characters"
    }

    @objc private func toggleShowToken() {
        let show = showTokenCheck.state == .on
        let text = (show ? tokenField : tokenPlainField).stringValue
        tokenField.stringValue = text
        tokenPlainField.stringValue = text
        tokenField.isHidden = show
        tokenPlainField.isHidden = !show
    }

    @objc private func labelModeChanged() {
        let custom = Self.labelModes[max(0, labelPopup.indexOfSelectedItem)].0 == "custom"
        labelField.isEnabled = custom
        labelField.isHidden = !custom
    }

    @objc private func testConnection() {
        readFields()
        guard let url = MeshMonitorClient.baseURL(from: settings.server) else {
            statusLabel.stringValue = "Enter a server address, like https://meshmonitor.example.com"
            return
        }
        guard !settings.token.isEmpty else {
            statusLabel.stringValue = "Enter an API token"
            return
        }
        statusLabel.stringValue = "Connecting…"
        let client = MeshMonitorClient(config: .init(baseURL: url, token: settings.token, source: settings.source))
        testClient = client
        client.test { [weak self] message in
            self?.statusLabel.stringValue = message
            self?.testClient = nil
        }
    }

    @objc private func cancel() { close() }

    @objc private func save() {
        readFields()
        settings.save()
        onSave(settings)
        close()
    }

    private func close() {
        NSColorPanel.shared.close()
        if let parent = window.sheetParent { parent.endSheet(window) } else { window.close() }
    }
}

private extension Int {
    func clamped(_ lo: Int, _ hi: Int) -> Int { Swift.min(Swift.max(self, lo), hi) }
}
