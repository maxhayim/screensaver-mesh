# Changelog

## 0.1.1

- **Your own label under the clock.** Choose what the line under the time says: Mesh, your name, or your own text, like a call sign. In the settings on macOS and Windows, and in the config file or XScreenSaver settings on Linux.

- **macOS: plain `http://` servers work.** Inside the screen saver, macOS blocks ordinary web requests to `http://` addresses, which is how most MeshMonitor servers on a home network are set up. Mesh now connects to those another way, so live mode works with them.
- **macOS: a picture in the screen saver list.** Mesh shows a preview of the mesh in System Settings instead of a blank icon.

## 0.1.0

The first release of Mesh, for macOS, Windows, and Linux.

- **Your mesh, live:** connect it to MeshMonitor and the dots are your active nodes. Every message travels across the mesh as a packet, hopping along the links from sender to recipient. Broadcasts ripple out three hops. The sender pulses and nearby nodes get a nudge.
- **Works without a server:** leave the server blank and it shows a simulated mesh. If MeshMonitor stops answering, it falls back to the simulation on its own.
- **Your colors:** pick dots, lines, packets, and background, or start from a preset: Default, Meshtastic, Green terminal, Amber terminal, or Paper.
- **Clock:** the time in the bottom-left corner, in 12- or 24-hour time, or turned off.
- **Downloads:** a `.saver` for macOS 11 and later (Apple silicon and Intel), a `.scr` for Windows 10 and 11, and an XScreenSaver hack for Linux.

The macOS and Windows builds aren't code-signed. The README explains how to get past the one-time warning.
