### In-app updates

The Info window checks GitHub once a week (or on demand via "Check for Update"):

- "Releases Page" always opens the [releases page](https://github.com/harrypm/MISRC-GUI/releases) in your browser.
- When a newer release is found, "Install" (two-click confirm) downloads the update for your platform and installs it in place. Your current version is archived next to the new one (renamed, never deleted); downloads are kept in `~/.local/share/misrc/updates` (Linux), `%LOCALAPPDATA%\misrc\updates` (Windows) or `~/Library/Application Support/misrc/updates` (macOS).
- Android downloads the APK via the browser and Android itself asks to confirm the install.
- Installing is blocked while a capture/record session is running.

Download naming is delivered by a remote manifest (`update_manifest.json` on `main`), so if a download link ever breaks, a fix reaches every installed copy without shipping a new release.
