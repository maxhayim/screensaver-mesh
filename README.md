# Mesh — a Meshtastic screen saver

Your mesh network as a screen saver, for macOS, Windows, and Linux. Nodes drift across a dark screen, links join the ones in reach, and every message sent on your mesh travels across it as an orange packet.

It reads live data from [MeshMonitor](https://github.com/Yeraze/meshmonitor) for Meshtastic:

- **The dots are your nodes:** the active nodes MeshMonitor has heard in the last 7 days, up to 160.
- **Every message moves the mesh:** the packet hops along the links from sender to recipient. A broadcast ripples out three hops. The sender pulses and nearby nodes get a nudge, then drift back.
- **No MeshMonitor?** Leave the server blank and you get a simulated mesh. If the server stops answering, the saver falls back to it too.

Message text is never shown on screen.

## Download

Get the file for your computer from [Releases](../../releases):

| OS | File |
| --- | --- |
| macOS 11 and later (Apple silicon and Intel) | `Mesh-<version>-macos.zip` |
| Windows 10 and 11 (64-bit) | `Mesh-<version>-windows.zip` |
| Linux with XScreenSaver (x86-64) | `screensaver-mesh-<version>-linux-x86_64.tar.gz` |

Mesh isn't code-signed or notarized (that costs money every year), so macOS and Windows warn you the first time. The steps below get you past that once.

### macOS

1. Unzip and double-click `Mesh.saver`, then choose to install it for this user.
2. macOS blocks it the first time. Open **System Settings → Privacy & Security**, scroll down, and click **Open Anyway**. Or run this in Terminal:
   ```sh
   xattr -d com.apple.quarantine ~/Library/Screen\ Savers/Mesh.saver
   ```
3. Choose **Mesh** in **System Settings → Screen Saver**, and click **Options…** to set it up.

### Windows

1. Unzip, and keep `Mesh.scr` somewhere permanent, like `Documents\Mesh`.
2. Right-click `Mesh.scr` and choose **Install**. If Windows says "Windows protected your PC", click **More info**, then **Run anyway**.
3. In **Screen Saver Settings**, choose **Mesh** and click **Settings** to set it up.

### Linux

1. Install XScreenSaver if you don't have it (for example `sudo apt install xscreensaver`).
2. Unpack the tarball and run `./install.sh`.
3. Add your server and token to `~/.config/screensaver-mesh/config` (the installer creates it from `config.example`).
4. Choose **Mesh** in `xscreensaver-settings`. Try it in a window first with `./screensaver-mesh`.

GNOME and KDE don't support third-party screen savers, so XScreenSaver is the way to run it on Linux.

## Settings

- **Server:** your MeshMonitor address, like `https://meshmonitor.example.com` or `http://192.168.1.20:8080`. With no `http://` or `https://`, it uses `https://`.
- **API token:** create one in MeshMonitor under **User Settings** (it starts with `mm_v1_`). Read access to nodes and messages is enough.
- **Source:** leave blank to use your first source, or enter a source ID from MeshMonitor.
- **Colors:** dots, lines, packets, and background, with presets: Default, Meshtastic, Green terminal, Amber terminal, and Paper.
- **Clock:** show it or hide it, in 12- or 24-hour time.
- **Label:** the line under the clock: **Mesh**, **your name** (the full name on your computer account), or **custom text**, like your call sign. In live mode " · live" is added after it.

On macOS and Windows these are in the saver's Options/Settings window, which also has **Test connection**. On Linux they go in the config file; the XScreenSaver settings page also has the server, color presets, clock, and label. Keep the token in the config file: anything set on that page ends up on the command line, where other users of the computer can see it.

Where the token is stored: macOS keeps it in the screen saver's preferences in your user Library, Windows in the registry under `HKEY_CURRENT_USER\Software\maxhayim\screensaver-mesh`, Linux in your config file. None of them use a keychain.

## How it works

- `core/`: plain C99 shared by all three. `mesh.c` runs the simulation and draws through two callbacks (`line` and `circle`), so each OS only supplies its own 2D drawing. `meshmonitor.c` and `json.c` handle the API for Windows and Linux.
- `macos/`: Swift `ScreenSaverView`, a URLSession client, and the Options sheet.
- `windows/`: Win32 + GDI+, WinHTTP on a worker thread, settings dialog.
- `linux/`: an XScreenSaver hack with Xlib + cairo, libcurl on a worker thread.
- MeshMonitor has no push feed, so the saver asks `GET /api/v1/sources/{source}/messages?limit=50` every 4 seconds and animates any messages it hasn't seen. Nodes refresh every 5 minutes from `GET /api/v1/sources/{source}/nodes?active=true&sinceDays=7`.

## Build

```sh
macos/build.sh      # build/Mesh.saver; needs the Xcode command-line tools
windows/build.sh    # build/Mesh.scr; cross-compiles with mingw-w64
make -C linux       # build/screensaver-mesh; needs libx11, cairo, libcurl dev packages
```

Tests and tools:

```sh
cc -std=c99 -Icore core/mesh.c tests/test_mesh.c -lm -o build/test_mesh && build/test_mesh
cc -std=c99 -Icore core/json.c core/meshmonitor.c tests/test_meshmonitor.c -o build/test_mm && build/test_mm
python3 tools/mock_meshmonitor.py                         # fake MeshMonitor on :8787, token mm_v1_test
swiftc tools/preview.swift -o build/preview -framework ScreenSaver
build/preview build/Mesh.saver shot.png 5                 # macOS: render 5 seconds to a PNG
build/screensaver-mesh -render 5 shot.png                 # Linux: same, no X needed
Mesh.exe /x 5 shot.bmp                                    # Windows: same (a copy of Mesh.scr)
```

GitHub Actions builds and tests all three on every push, and publishes a release for every `v*` tag.

## License

MIT. See [LICENSE](LICENSE).
