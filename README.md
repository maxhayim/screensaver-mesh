# Mesh — a Meshtastic screen saver

Your mesh network as a screen saver. Nodes drift across a dark screen, links join the ones in reach, and every message sent on your mesh travels across it as an orange packet.

It reads live data from [MeshMonitor](https://github.com/Yeraze/meshmonitor) for Meshtastic:

- **The dots are your nodes:** the active nodes MeshMonitor has heard in the last 7 days, up to 160.
- **Every message moves the mesh:** the packet hops along the links from sender to recipient. A broadcast ripples out three hops. The sender pulses and nearby nodes get a nudge, then drift back.
- **No MeshMonitor?** Leave the server blank and you get a simulated mesh. If the server stops answering, the saver falls back to it too.

Message text is never shown on screen.

## Platforms

| OS | Status |
| --- | --- |
| macOS 11 and later (Apple silicon and Intel) | ✅ `Mesh.saver` |
| Windows (`.scr`) | Planned |
| Linux (XScreenSaver) | Planned |

## Install on macOS

1. Download `Mesh.saver.zip` from [Releases](../../releases) and unzip it.
2. Double-click `Mesh.saver` and choose to install it for this user.
3. The saver isn't notarized by Apple, so macOS blocks it the first time. Open **System Settings → Privacy & Security**, scroll down, and click **Open Anyway**. Or run this in Terminal:
   ```sh
   xattr -d com.apple.quarantine ~/Library/Screen\ Savers/Mesh.saver
   ```
4. Choose **Mesh** in **System Settings → Screen Saver**.

## Settings

Click **Options…** next to Mesh in Screen Saver settings.

- **Server:** your MeshMonitor address, like `https://meshmonitor.example.com` or `http://192.168.1.20:8080`. With no `http://` or `https://`, it uses `https://`.
- **API token:** create one in MeshMonitor under **User Settings** (it starts with `mm_v1_`). Read access to nodes and messages is enough.
- **Source:** leave blank to use your first source, or enter a source ID from MeshMonitor.
- **Test connection:** checks the server and shows how many active nodes it found.
- **Colors:** dots, lines, packets, and background, with presets (Default, Meshtastic, green terminal, amber terminal, paper).
- **Clock:** show it or hide it, in 12- or 24-hour time.

The token is stored in the screen saver's preferences file in your user Library. It isn't stored in the Keychain.

## How it works

- `core/`: the drawing core in plain C99. It runs the simulation and draws through two callbacks (`line` and `circle`), so each OS only plugs in its own 2D drawing.
- `macos/`: the Swift `ScreenSaverView`, the MeshMonitor client, and the Options sheet.
- MeshMonitor has no push feed, so the saver asks `GET /api/v1/sources/{source}/messages?limit=50` every 4 seconds and animates any messages it hasn't seen. Nodes refresh every 5 minutes from `GET /api/v1/sources/{source}/nodes?active=true&sinceDays=7`.

## Build

You need only the Xcode command-line tools (`xcode-select --install`), not Xcode itself.

```sh
macos/build.sh                         # -> build/Mesh.saver (universal, ad-hoc signed)
```

Test without installing:

```sh
cc -std=c99 -Icore core/mesh.c tests/test_mesh.c -lm -o build/test_mesh && build/test_mesh
swiftc tools/preview.swift -o build/preview -framework ScreenSaver
build/preview build/Mesh.saver shot.png 5           # render 5 seconds to a PNG
python3 tools/mock_meshmonitor.py                   # fake MeshMonitor on :8787, token mm_v1_test
```

## License

MIT. See [LICENSE](LICENSE).
