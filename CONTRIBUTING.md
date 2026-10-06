# Contributing

Contributions are welcome.

## Before opening a pull request

1. Open an issue describing the proposed change or bug.
2. Change the look in one place: drawing lives in `core/mesh.c` and every system draws through it. Keep the defaults (`#0b0b0a` background, `#eeebe4` dots and lines, `#f06a2a` packets) and the presets in step across `core/meshmonitor.c` and `macos/Settings.swift`.
3. Keep all three systems working. A setting added on one belongs on the others: macOS Options, the Windows dialog and registry, and the Linux config file, flags, and XScreenSaver XML.
4. Talk only to the user's own MeshMonitor, through its REST API v1. Don't add other services, analytics, or update checks.
5. Never show message text, node names, or node IDs on screen.
6. Keep it free: no paid code signing, services, or dependencies.
7. Run the tests and build every system you can (see [docs/GUIDE.md](docs/GUIDE.md)). CI builds and tests all three on every push.
8. Test inside the real screen saver host, not only in the preview tool: System Settings on macOS, Screen Saver Settings on Windows, XScreenSaver on Linux. Each one behaves differently (for example, macOS gives the Options window no Edit menu).
9. Do not include MeshMonitor tokens, server addresses, or node data in commits or screenshots. `tools/mock_meshmonitor.py` makes fake ones.

## Pull requests

Include:
- A concise description of the change
- Which systems it affects
- How it was tested, and on which systems (Windows, macOS, Linux)
- Screenshots for visual changes
