# Security Policy

The Mesh screen saver keeps a MeshMonitor API token on your computer, so treat it carefully.

## Do not publish

Do not include any of the following in GitHub issues, pull requests, screenshots, or example logs:

- MeshMonitor API tokens
- MeshMonitor addresses that aren't meant to be public
- Private mesh node names, IDs, locations, or messages

## Where things are kept

- **macOS:** the screen saver's preferences, in your user Library (under the `legacyScreenSaver` container).
- **Windows:** the registry, under `HKEY_CURRENT_USER\Software\maxhayim\screensaver-mesh`.
- **Linux:** `~/.config/screensaver-mesh/config`. The installer creates it readable only by you.

None of these are encrypted or in a system keychain. Use a **read-only** MeshMonitor token: Mesh only reads nodes and messages.

On Linux, keep the token in the config file rather than the XScreenSaver settings page. Options set there are passed on the command line, where other users of the computer can see them.

## What Mesh connects to

Only the MeshMonitor server you set, with your token, over `https://` or `http://`. Use `https://` for a server reached over the internet: with `http://`, the token travels unencrypted.

## Reporting a vulnerability

For security-sensitive reports, contact the repository owner privately through an appropriate GitHub contact method instead of opening a public issue.
