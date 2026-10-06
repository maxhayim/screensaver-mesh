# Changelog

All notable changes to the Mesh screen saver are documented here.

## [Unreleased]

### Added
- **Community files:** `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, and `SECURITY.md`.
- **Developer guide** in `docs/GUIDE.md`, and `docs/assets/` for the logo and screenshot.

### Changed
- README rewritten: what's on screen, install and update steps, settings, privacy notes, repository layout, and versioning.
- **Release downloads** are named after the repository: `screensaver-mesh-<version>-macos.zip`, `screensaver-mesh-<version>-windows.zip`, and `screensaver-mesh-<version>-linux-x86_64.tar.gz`.
- Release notes follow the changelog, with a Compatibility section.

## [0.1.2] - 2026-10-06

### Fixed
- **macOS: pasting the API token.** In the screen saver's Options window, ⌘V pasted only part of the token, or nothing: the screen saver host has no Edit menu, which is how ⌘V normally reaches a text field. Copy, paste, cut, select all, and undo now work in every field.

### Added
- **macOS: Paste and Show** under the API token. **Paste** fills it straight from the clipboard; **Show** lets you check the whole token went in.

## [0.1.1] - 2026-10-05

### Added
- **Your own label under the clock:** Mesh, your name (the full name on your computer account), or your own text, like a call sign. In the settings on macOS and Windows, and in the config file or XScreenSaver settings on Linux.
- **macOS: a picture in the screen saver list** in System Settings, instead of a blank icon.

### Fixed
- **macOS: plain `http://` servers.** Inside the screen saver, macOS blocks ordinary web requests to `http://` addresses, which is how most MeshMonitor servers on a home network are set up. Mesh now connects to them another way, so live mode works.
- **macOS: the Options window** lines up labels with their fields and fits its contents.

### Changed
- Windows keeps its settings as Unicode, so custom text works in any language.

## [0.1.0] - 2026-10-02

### Added
- **The Mesh screen saver for macOS, Windows, and Linux:** a `.saver` for macOS 11 and later (Apple silicon and Intel), a `.scr` for Windows 10 and 11, and an XScreenSaver hack for Linux.
- **Your mesh, live from MeshMonitor:** the dots are your active nodes, and every message travels across the mesh as a packet, hopping along the links from sender to recipient. Broadcasts ripple out three hops. The sender pulses and nearby nodes get a nudge.
- **A simulated mesh** when no server is set, or when MeshMonitor stops answering.
- **Colors** for dots, lines, packets, and background, with presets: Default, Meshtastic, Green terminal, Amber terminal, and Paper.
- **Clock** in the bottom-left corner, in 12- or 24-hour time, or turned off.
- **Settings:** the Options sheet on macOS and the Settings dialog on Windows, with Test connection; a config file and XScreenSaver settings page on Linux.
