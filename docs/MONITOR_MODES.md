# MISRC-GUI: Unified Server and Monitor & Remote

The existing **Server** now hosts both the original full-RF transport and the
MISRC-M/2 monitor transport **on the same TCP port**. There is no separate
M-Server mode or monitor listener. The Server owns capture, processing,
recording, and filesystem access. The Monitor & Remote client renders a low-bandwidth preview
and sends recording/control commands. The legacy Client is unchanged.

## Modes and compatibility

| `net_mode` | Mode | Behaviour |
|---:|---|---|
| 0 | Local | Existing local capture mode |
| 1 | Server | Unified full-RF and MISRC-M/2 monitor server |
| 2 | Client | Existing full-RF network client |
| 3 | old M-Server config ID | Automatically mapped to Server (1); not selectable in the UI |
| 4 | Monitor & Remote | Remote monitoring and control; no local capture hardware required |

The unified Server broadcasts both `MISRC` (legacy) and `MISRC-M` (monitor)
UDP discovery markers pointing to the **same TCP port**. The received HTTP path
selects the protocol: `/monitor/...` is handled by the monitor backend,
while `/rf`, `/baseband`, `/stats`, etc. retain the legacy implementation.
Monitor & Remote insists on a **`MISRC-M/2`** handshake and never receives raw RF.

## Preview and status

- `/monitor/preview` sends bounded Zstd-1-compressed `MRD5` frames containing
  4096 waveform samples and 2049 FFT bins per channel at up to 10 Hz.
  The existing phosphor waveform/FFT GUI renders these data in Monitor & Remote mode.
- `/monitor/status` reports capture state, synchronization, recording progress,
  per-channel raw/encoded bytes, drop/error counts, clip values, REC A/B buffer
  occupancy, Local Save network queue fill and available recording disk space.
- `GET /monitor/start`, `/monitor/stop`, `/monitor/record?on=1|0`, and
  `/monitor/reset-clips?channel=0|1` control the Server.
- Monitor & Remote receives the Server device catalogue; pseudo-devices such as
  Simulated and Playback legitimately use `index=-1`.

- **Monitor Audio Mon:** `GET /monitor/audio?pair=0|1` sends a separate
  low-bandwidth HTTP stream of 48 kHz mono Opus, encoded in 10 ms frames
  (about 80 kbit/s) from the server's existing captured CH1/2 or CH3/4
  baseband audio. Frames use a bounded 12-byte `MAO1` header + Opus payload,
  not Ogg and not the legacy `/baseband` endpoint. The stream exists only
  while the remote Audio Mon is enabled, and stops on disconnect/idle.
  Monitor-only audio controls are **client-local**; they do not change the
  server's local Audio Mon settings. Local, Server and legacy Client playback
  pipelines are unchanged.

**Preview is not a raw-RF transport.** Encoded recordings are transferred only
when Monitor & Remote Local Save is explicitly enabled.

## Recording destinations

In Monitor & Remote Settings, only the active destination is displayed:

- **Local Save OFF:** Record writes to the server's Output folder. The
  Monitor & Remote folder browser navigates the *server* filesystem, including
  Home/Up/root or Windows drive selection. The Server validates and persists
  the chosen directory.
- **Local Save ON:** Record writes directly to the client's selected local
  folder using a separate persistent transfer connection (no SMB mount).
  The server performs encoding; the client writes `.partial` files, acknowledges
  CRC-checked frames, and renames files only on successful COMMIT. Transfer
  failures do not silently fall back to server recording.

The two paths are stored independently. Local Save controls appear **only**
in Monitor & Remote mode. The 64 MiB encoded-send queue is bounded; insufficient
network/disk throughput can cause backpressure or a partial/aborted recording.

Monitor & Remote mirrors an allowlisted set of 68 recording settings from the Server
and can update them over dedicated `/monitor/settings` controls. Client-local
window/layout settings, network mode and Local Save destination are not sent
to the server. Server device selection uses `/monitor/devices` and
`/monitor/device`. Device changes and recording-setting changes are refused
during active recording. On leaving Monitor & Remote, the local configuration profile
is restored.

## Monitor endpoints on the shared server

- `/monitor/settings` and `/monitor/settings/set`: recording configuration
- `/monitor/devices` and `/monitor/device`: server device catalogue/selection
- `/monitor/fs/browse`, `/monitor/fs/output`, `/monitor/fs/select`: server
  directory listing and output directory selection
- `/monitor/local-save/attach`: persistent file receiver handshake

Monitor endpoint handlers and Local Save are separate from the legacy RF
handlers, but use the same TCP listener. The attached Local Save socket
transfers ownership to the dedicated recording worker after the HTTP request.

## Build and usage

Linux prerequisites include `libzstd-dev` (Debian/Ubuntu). Windows MSYS2
UCRT64 prerequisites include `mingw-w64-ucrt-x86_64-zstd`. Android ARM64 builds
must cross-compile the bundled/static Zstandard dependency using
`android/build-deps-android.sh` before configuring Meson.

```bash
# Linux build
bash scripts/build-local.sh

# Start the unified Server without a desktop
./build-local/misrc_gui --headless-server

# Inspect its status from a second terminal
curl -fsS http://127.0.0.1:8080/stats
curl -fsS http://127.0.0.1:8080/monitor/hello
curl -fsS http://127.0.0.1:8080/monitor/status
```

On Windows, run `scripts\build-local.ps1` in PowerShell; select **Monitor & Remote**
and connect to the Server in the discovered server list.

**Security:** monitor HTTP and file transfer endpoints have no built-in
TLS/authentication. Expose these ports only on a trusted LAN or authenticated
VPN, never to the public Internet.

**Compatibility and review scope:** The UI cycles Local / Server / Client /
Monitor & Remote. Existing JSON `net_mode: 3` loads as Server for backward compatibility.
`--headless-server` is the sole headless network-server flag. Local/legacy
client, RF streaming and normal recording should be regression-tested alongside
monitor preview, controls, remote settings and Local Save.
