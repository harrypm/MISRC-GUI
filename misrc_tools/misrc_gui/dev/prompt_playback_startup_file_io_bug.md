# Playback startup waits on a saved input path

## Finding

The first layout used the selected-file metadata accessor to measure the
saved RF filename. A cold metadata cache could open the file synchronously
on the UI thread before the first frame. A slow or unavailable filesystem
could therefore stall startup. The original reported freeze has no
confirmed cause.

## Change

Startup derives the displayed filename from the saved path string. Playback
readouts use available metadata from a loaded file. An explicit playback
request opens the file on a worker. Drag-and-drop and positional file
arguments copy the paths and classify them on the same background path.
Completed drops use the normal capture start entry point, which retains
the existing dispatch for Client and Monitor Client.

Cancel abandons a pending request in the UI. The worker owns its inputs
and readers, so a late result cannot replace a newer request. On Windows
and desktop Linux, shutdown can bypass CRT cleanup after closing the
window while a discarded loader remains in a blocked read.
See [Playback drag-and-drop](playback_drag_drop_audio.md) for the existing
channel-zone and audio-source contracts.

## Verification and limits

- A Windows x64 GUI run covered a saved file that had been deleted, a
  missing command-line file, and a drop with a missing parent directory.
- A Windows named pipe held the read pending. The saved path was untouched
  on ordinary startup, and both command-line and drop loads left the
  window responsive and able to close. A local synthetic RF file
  exercised successful command-line playback.
- An Ubuntu x64 WSL GUI build exited with status 0 while a pipe kept the
  background read blocked, both after cancelling and when closing directly.
- The startup regression held file open and read calls, exercised
  cancellation and immediate retry, and checked that an abandoned request
  could not commit. Playback seek, timeline, file resolution and length
  cache regressions also passed.
- The drop regression exercised the actual A/B zone selection, confirmed
  that a B-only drop retains A and the monitor file, and observed decoded
  frames from a 48 kHz FLAC sidecar at the monitor queue. It also checked
  that Client and Monitor Client drops call the existing capture start
  entry point. These checks used controlled panel bounds, an audio queue
  stub, and a simulated remote dispatch. The Windows window smoke test
  exercised drop delivery and opening a 48 kHz WAV sidecar. OS pointer
  placement for the B zone and a live server handshake were not verified.
- After the guard fixes, the static CI guards passed. Six MinGW translation
  units passed a syntax check with `LIBFLAC_ENABLED=0`.

The tests simulate an unresponsive input. A real failed mount and a
disconnect after playback has started remain unverified. Harry's sign-off
on the user-visible playback changes is pending.
