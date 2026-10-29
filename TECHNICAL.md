# MISRC GUI — Technical Notes

This file consolidates the code-focused technical notes that previously lived as
standalone files (`GUI_README.md`, `MISRC_GUI_README.md`) in the upstream
`harrypm/MISRC` repo. The user-facing project README is `README.md`; this
document is for developers working on the GUI/tool source.

The packages contain two command-line applications, `misrc_capture` and
`misrc_extract`, plus the `misrc_gui` graphical application. For detailed usage
information see `misrc_tools/README.md`.

## Source build dependency note (FFT)

`misrc_gui` requires FFT support via FFTW single-precision (`fftw3f`). Source
builds fail at configure time if FFTW is missing.

Install FFTW development packages before running Meson:

- Debian/Ubuntu/Linux Mint: `libfftw3-dev`
- macOS (Homebrew): `fftw`
- MSYS2 UCRT64 x86_64: `mingw-w64-ucrt-x86_64-fftw`
- MSYS2 MinGW arm64: `mingw-w64-clang-aarch64-fftw`

If you already configured a Meson build directory before installing FFTW, wipe
and reconfigure so stale dependency paths are removed:

    meson setup --wipe misrc_tools/build misrc_tools

## Local AppImage test build (Ubuntu 22.04 baseline)

For a reproducible local AppImage build that targets a `glibc 2.35` baseline,
run:

    ./scripts/build-appimage-local.sh

The script uses `docker` or `podman` (prefers docker if both are installed),
installs build dependencies in an `ubuntu:22.04` container, and writes the
output AppImage to `.ci-artifacts/linux-appimage/`.

If you want to run directly on your host (with all dependencies already
installed), use:

    ./scripts/build-appimage-local.sh --native

## Linux AppImage desktop integration (taskbar icon / pinning)

An AppImage is never registered with the desktop by itself. The AppDir ships a
real `assets/appimage/AppRun` and `assets/appimage/misrc_gui.desktop` (shared by
the CI workflow and `scripts/build-appimage-local.sh`; not inline heredocs).

What `AppRun` does:

- Exports `RESOURCE_NAME=misrc_gui` (X11 `WM_CLASS` instance).
- On plain GUI launches (no args, or only `--debug-view` / `--auto-connect` /
  `--config <file>`) it installs
  `~/.local/share/applications/misrc_gui.desktop` (`Exec` = the AppImage file
  itself) and the hicolor icon. It rewrites them only when their content changes,
  so the launcher follows whichever AppImage ran last.
- It does nothing for `capture`, `extract`, `--smoke-test`, `--version`, `--help`,
  when `APPIMAGE` is unset, or when the path contains `" ` $ \`.
- It never writes to `~/Desktop`. A Desktop icon is created only on request:
  `./<MISRC AppImage> --create-shortcut`. (Earlier builds copied one to the
  Desktop on every launch, so it came back after being deleted.)
- `MISRC_GUI_NO_INTEGRATION=1` skips the automatic launcher install.

Window identity (`misrc_tools/misrc_gui/core/misrc_gui.c`): GLFW/raylib builds
the X11 `WM_CLASS` from the window title at creation, so the window is created
with the fixed title `MISRC Capture` and the versioned title is applied
afterwards with `SetWindowTitle()`. Result: `WM_CLASS = "misrc_gui", "MISRC Capture"`
on every release, matching `StartupWMClass=MISRC Capture`. Before this, the class
was the versioned title (e.g. `MISRC Capture v1.2.2`) and changed every release.

The desktop id (`misrc_gui.desktop`), `Icon=misrc` / `misrc.png`, the
`RESOURCE_NAME` instance and `StartupWMClass` must stay in agreement;
`misrc_tools/test/ci_guard_tests.py` enforces this (`window identity contract`,
`AppRun static contract`, `AppRun runtime behavior`).

To verify on X11, match the window by PID (never by name, another instance may
be running) and only read the mapped main window:

    xdotool search --onlyvisible --pid <pid>
    xprop -id <window> WM_CLASS _NET_WM_PID WM_STATE

The same recipe is used by FLAC-Chop and tape-decode-rust
(`docs/DEV_NOTE_linux_appimage_taskbar_integration.md` in those repos).

## Direct native RAW recording (CXADC passthrough) and RAW naming

When the selected device is a running CXADC capture, FLAC compression is
off, and a channel's resampler is off, that channel records *direct*: the
output file is the card's native byte stream — unsigned `.u8` bytes in
8-bit mode, unsigned `.u16` words in tenbit mode — byte-for-byte what
`dd if=/dev/cxadcN` would produce. Everything else keeps the converting
writer (signed samples, `.s8`/`.s16`).

### Data flow (direct channels)

    card read()                                gui_cxadc.c: cxadc_capture_thread
      -> gui_record_direct_tap_push(card,..)  gui_record.c; per card, immediately
                                               after each successful read(),
                                               BEFORE the A/B pairing
      -> gui_record_direct_push(ctx,..)        gui_record_direct.c:
           BUF_RECORD_A/B (2 attempts x 1 ms,  same backpressure as extraction,
           then spill, then logged drop)       or the spill file while a
                                               backlog exists
      -> gui_record_direct_writer_thread       gui_record_direct.c: ring first
           (oldest data), then spill, any     length up to 2 MiB per read
      -> fwrite unchanged -> .u8/.u16 file

The tap sits before the A/B pairing deliberately: the pairing truncates both
reads to the shorter one and drops card A's data entirely when card B
returns 0, and tenbit odd trailing bytes are truncated — the direct path
must not inherit any of that. The display chain is unchanged (the waveform
still decodes); only the recording chain is byte-exact. Extraction skips
`BUF_RECORD_*` writes for direct channels
(`gui_extract_set_recording(..., direct_a, direct_b)`), so each record ring
has exactly one producer.

### Eligibility (per channel, decided at record start)

`gui_record_direct_channel_eligible(app, channel)` in `gui_record.c` —
all must hold:

- the selected device is a `DEVICE_TYPE_CXADC` entry (single-card or
  2-card Clockgen) and the CXADC capture is running;
- `use_flac` is off (RAW output);
- that channel's resampler is off (`enable_resample_a/b`);
- that channel's capture is on;
- the card behind that channel (card index == channel) delivers samples of
  the requested RAW width: `gui_cxadc_direct_record_available()` checks the
  live tenbit mode against `rf_bits` (8-bit mode -> u8 bytes, tenbit ->
  u16 words).

The same predicate drives file naming (`gui_record_apply_auto_names`), the
capture log and writer/tap selection, so filename, log and on-disk content
always agree. Mixed mode works: channel A direct while channel B resamples
(B converts to `.s8/.s16`).

### Naming by content

`raw_ext_for_bits(bits, direct)` in `gui_record.c`: direct -> `.u8`/`.u16`
(truly unsigned, byte-exact), converted -> `.s8`/`.s16`. The load-time
preview copy in `gui_settings.c` maps converted only (it has no device
knowledge at load; record start recomputes the authoritative name). (The
`.u8/.u16` naming introduced in v1.2.3 previously covered converted files
too; those are `.s8/.s16` from this change on.)

### Ordering contract (why the two rules)

Two rules keep the byte stream in order under backpressure:

- Producer: while the spill backlog is non-zero, new bytes go to the spill
  (queuing behind the not-yet-drained spill data).
- Writer: drain the ringbuffer first, then the spill.

Together they hold the invariant "ring data is always older than spill
data": the ring only accumulates while the backlog is zero; once a push
spills, every later push appends to the spill until it is fully drained.
The legacy path violates this (it retries the ring as soon as any space
appears while older data still sits in the spill), which can reorder
blocks. Empirical oracle: the unit harness pushes a deterministic pattern
larger than the ring while the writer drains concurrently — any reordering
shows up as a byte mismatch.

Other semantics:

- Backpressure: identical to the extraction record path (2 attempts x 1 ms,
  then the spill temp file, then a logged drop; a drop mirrors the
  extraction drop handling including optional stop_on_dropout).
- Variable-length reads: the writer reads whatever is available (capped at
  2 MiB), so the final partial block at stop is written too — the legacy
  fixed-block RAW writer lost up to one block (~52 ms of 16-bit/20 MSPS
  data) at every stop.
- Write errors (locked output file): the failed block is held in memory (it
  is the oldest unread data) and retried every ~2 s; nothing is read while
  it is held, so the stream cannot reorder across the outage. A bounded
  final attempt happens at stop (exactly one attempt, never a hang at
  join); if it fails the held block is lost — the same semantics as the
  legacy writer stopping with a locked output.

### Start/stop handshake

- Start (RAW branch of `gui_record_start_confirmed`): writer threads
  started -> 10 ms -> taps enabled (`gui_record_direct_tap_enable`) ->
  `gui_extract_set_recording(true, false, bits, direct_a, direct_b)`.
- Stop (`gui_record_stop`): taps disabled and waited idle
  (`gui_record_direct_tap_disable` + `gui_record_direct_tap_wait_idle`)
  BEFORE `gui_extract_set_recording(false, ...)` and BEFORE `is_recording`
  clears, so the writers' final drain can never race a producer still
  pushing. Tap handshake itself: the producer marks itself in-flight, then
  reads the enabled flag; the stopper clears the flag, then waits for
  in-flight == 0.

### Logging, status and accounting

- Capture log: `capture_format: RAW direct native passthrough` (vs
  `RAW converted`) plus a per-channel `RAW output: A=direct native u16
  B=converted s16` line.
- Status bar: `Recording (RAW direct)...`.
- End-of-recording summary: `Direct RAW channel A/B: bytes_read=...
  bytes_written=...`, with `(MISMATCH: data was dropped or left in the
  spill backlog)` appended when they differ.
- Accounting: `recording_raw_a/b` and `recording_bytes` count bytes actually
  written, so the readout, the stat()-based on-disk readout and the capture
  log all match the real file size.

### Verification

- Unit (`meson test gui_record_direct`; `test/gui_record_direct_harness.c`
  drives the real producer + writer through a real ringbuffer, 56 checks):
  tap handshake; spill fallback once the ring is full; byte-identical output
  over 3x2 MiB + odd tail in mixed chunk sizes 1..999999 bytes (proves
  ordering + tail drain); per-channel accounting; write-error recovery via
  unbuffered `/dev/full` then a swapped-in output (held block first, then
  ordered drain).
- Guards: `ci_guard_tests.py` `raw direct passthrough contract` (static)
  and `direct RAW writer runtime` (compiles + runs the harness).
- Hardware (2026-10-02, 2x CXADC + clockgen rig, 36-core, unattended
  `--auto-record 15` under 24 CPU-load workers): both channels direct,
  599,785,472 bytes each = bytes_read = bytes_written = on-disk size,
  waits=0 drops=0, 39.99 MB/s per channel (true 40 MSPS) on both channels
  concurrently; `.u8` means matched a live `dd if=/dev/cxadc0` read
  (file 126.0 / 126.1 vs card 126.0 — a mislabeled-converted file would
  read ~254 at this DC, i.e. `b-128` wrapped).

## Automated local load + capture testing (CLI -> GUI)

The GUI runs unattended from a terminal for local automated load/capture
testing — no UI clicking and no server/client setup. (The net route stays
available: `--auto-connect` with a Server/Client config, see
`misrc_tools/test/cxadc_remote_capture_ci.sh`; the CLI-side loop suite is
`misrc_tools/test/capture_stability_ci.sh <capture_bin> <extract_bin>
<dir>`.)

### Flags (`misrc_gui --help`)

- `--select-device <index-or-name>` — pick the capture device at launch.
  `<sel>` is either the device-list index (`0`, `1`, ...) or a
  case-insensitive substring of the device name (first match wins, e.g.
  `"CXADC Clockgen"`, `"MS2130"`, `"Simulated"`). Applied after enumeration,
  overriding the hsdaoh > MISRC Clockgen > CXADC auto-selection — required
  for automated runs because that preference otherwise picks the MS2130
  stick on a mixed rig. No match ->
  `[AUTO] --select-device: no device matches '<sel>'` and exit 1.
- `--auto-capture` — arm an unattended capture start ~1 s after launch
  (same reconnect-pending mechanism `--auto-connect` uses in server mode,
  but Local mode and no `--config` required). Use it alone for a
  streaming/load run with no output files.
- `--auto-record <seconds>` — full unattended record cycle; implies the
  auto-capture arming. Phase machine (in the main loop, every transition
  logged with an `[AUTO]` prefix):
  1. wait for capture to start (30 s budget, else failure);
  2. settle 2 s (data flowing) -> start recording. `RECORD_PENDING` (the
     overwrite popup is open) is a failure with an explicit hint: set
     `overwrite_files: true` in the config or use timestamped auto-names;
  3. record until the deadline -> stop recording;
  4. wait for the finalize (writer threads joined, files closed) ->
     stop capture;
  5. clean exit: 0 on a completed cycle, 1 if any phase failed.
- The settings `overwrite_files` toggle ("Overwrite output files") now
  actually skips the overwrite confirmation popup in `gui_record_start` —
  the toggle existed in the UI but was ignored before; without it an
  unattended run stalls on that popup (timestamped auto-names also avoid
  it).

### `[AUTO]` markers (for scripted verification)

    [AUTO] --select-device: <idx>: <name>
    [AUTO] auto-capture armed (device <idx>: <name>)
    [AUTO] capture running; settling 2 s before recording
    [AUTO] recording started (<n> s)
    [AUTO] recording stopped after <n> s
    [AUTO] capture stopped
    [AUTO] record cycle complete[ (with errors)]; exiting
    [AUTO] ERROR: ...   (failure detail; exit code 1)

A backgrounded GUI must be `wait`ed on (or `nohup`/`setsid` detached) or the
launching shell's exit SIGHUPs it — the example's `wait $GUIPID` covers
that.

Copy-paste example — direct native RAW capture on the CXADC Clockgen entry
under CPU load (uses the real X display; wrap the command in `xvfb-run -a`
for a throwaway headless X server):

```bash
# 1) One-off output dir + config (FLAC off, no resample, 8-bit, both cards)
mkdir -p ~/misrc-autotest
cat > ~/misrc-autotest/settings.config <<'EOF'
{
  "output_path": "/home/YOU/misrc-autotest",
  "auto_names_enabled": true,
  "output_base_name": "directtest",
  "append_timestamp_on_capture_start": true,
  "rf_bits_a": 8,
  "rf_bits_b": 8,
  "cxadc_tenbit_mode_a": false,
  "cxadc_tenbit_mode_b": false,
  "capture_a": true,
  "capture_b": true,
  "use_flac": false,
  "enable_resample_a": false,
  "enable_resample_b": false,
  "stop_on_dropout": false,
  "level_autostop_enabled": false,
  "overwrite_files": true
}
EOF

# 2) Unattended 15 s record run on CXADC, with CPU load while it records
GUI=./build-local/misrc_gui
$GUI --config ~/misrc-autotest/settings.config \
     --select-device "CXADC Clockgen" --auto-record 15 \
     >~/misrc-autotest/gui.log 2>&1 &
GUIPID=$!
# keep most cores busy while it records (scale to your core count)
for i in $(seq 1 24); do timeout 25 sha256sum /dev/zero >/dev/null & done
wait $GUIPID; echo "gui exit: $?"   # 0 = full cycle completed

# 3) Verify the direct native RAW output
grep -E "capture_format|RAW output|Direct RAW channel" ~/misrc-autotest/*_misrc_capture.log
ls -l ~/misrc-autotest/*.u8
# bytes_read == bytes_written == file size proves ordering + no loss;
# the file mean must match the card's own live DC (unsigned native bytes)
dd if=/dev/cxadc0 bs=64k count=64 2>/dev/null | \
  python3 -c "import sys; d=sys.stdin.buffer.read(); print('card mean %.1f' % (sum(d)/len(d)))"
python3 - <<'EOF'
import glob
for f in sorted(glob.glob('/home/YOU/misrc-autotest/*.u8')):
    with open(f, 'rb') as fh:
        d = fh.read(4 << 20)
    print(f, 'file mean %.1f' % (sum(d) / len(d)))
EOF
```

Notes:

- A resample-on run of the same config produces `.s8/.s16` (converted) —
  flip `enable_resample_a/b` to `true` and set `resample_rate_a/b` to A/B
  the naming and converted output.
- The same flags drive any device: `--select-device 0` (list index) or a
  substring like `--select-device MS2130`.

### Validated on real hardware (2026-10-02)

The exact example above, on the 2x CXADC + clockgen rig (36-core, 24
sha256sum load workers, 15 s record): `GUI_EXIT=0`, both channels direct,
599,785,472 bytes per channel = bytes_read = bytes_written = on-disk size,
waits=0 drops=0, 39.99 MB/s per channel (true 40 MSPS) on both channels
concurrently under load, `.u8` means matching a live `dd if=/dev/cxadc0`
read (126.0). Tier-snap and rate-check lines:
`[CXADC] rate check: card 0/1 measured 40.000 MHz, sysfs 40.000 MHz
(agree)` at startup and again at capture start.

## CXADC measured feed-rate check (sysfs crystal can lie)

sysfs `crystal`/`tenxfsc` report what the cxadc driver *believes*. A
hardware-modded card (e.g. a 40 MHz oscillator swap) can still carry the
stock 28.636 MHz `crystal` parameter — reported by a user with a single
40 MHz-modded card: resample cycle options capped at 28.6, which is only
correct if the sysfs is right. The GUI therefore *measures* the feed rate
by timing a real blocking read from `/dev/cxadcN`.

### Algorithm (`cxadc_measure_card_rate_hz`, gui_cxadc.c) — drain, then time

1. **Drain the backlog until the live stream is detected.** The card
   free-runs since boot/driver load, so the driver holds a backlog that
   reads return far faster than the live feed — a naive timed window over
   the backlog reports a bogus rate (observed on a real 40 MSPS card:
   50-72 MHz "measurements"). The probe reads 256 KiB chunks until one
   chunk takes as long as a live one — `>= 4 ms` (256 KiB at <= 64 MB/s;
   every real tier feeds at <= 54 MB/s so a live chunk takes >= 4.8 ms,
   a buffered chunk far less) — bounded by 1 GiB / 3 s caps.
2. **Time the all-live window.** 2 MiB at the startup probe, 4 MiB at
   capture start; `rate = bytes / elapsed / bytes-per-sample`, requiring
   >= 20 ms elapsed and sanity `0 < rate <= 100 MSPS`.

The measured rate is snapped to a known tier (2% tolerance) from
`misrc_tools/common/cxadc_rate_tiers.h`:

    14.318 (stock 10-bit)   17.898 (stock 10-bit tenxfsc=1)
    20 (40-mod 10-bit)      27 (54-mod 10-bit)
    28.636 (stock crystal)  40 (40 MHz mod / clockgen)   54 (54 MHz mod)

35.8 MHz (35795454 Hz = 28.636 x 10/8, the upsampled 8-bit tenxfsc=1 feed)
is deliberately NOT a tier: that mode's sysfs presentation is the crystal
rate, and an unsnapped measurement falls back to sysfs.

Failures are conservative: live stream not reached within the caps,
measurement failed, or no tier match -> the measurement is negatively
cached for the session (per card, per tenbit mode) and the sysfs-derived
rate stays — never a wrong override.

### Probe points and consumers

- Startup probe: `gui_cxadc_probe_card_rates()` from device enumeration,
  once per (card, mode) per session. Skipped while a CXADC capture is
  running, and via `MISRC_GUI_NO_CXADC_RATE_PROBE=1` (it briefly reads the
  cards — it must not run while another process is capturing on them).
- Capture-start probe: `gui_cxadc_start`, per card, using the already-open
  fds, after tenbit modes are applied and before the RF thread starts; the
  measured tier overrides `card_sample_rate_hz[i]` and refreshes
  `rf_sample_rate_hz`, so the rate readout, the resample options and the
  direct RAW width check all use the measured truth.
- `gui_cxadc_get_effective_rate_hz()` (measured tier if cached, else
  sysfs) is used by the UI rate getters (`gui_ui_cxadc_base_rate_khz`,
  `gui_ui_cxadc_hw_rate_for_tenbit` — the resample cycle max), the CXADC
  settings-profile sync in `gui_capture.c`, and `gui_cxadc_start` — so the
  resample cycle options follow the measured rate (a 40-mod card gets the
  `5/10/20/40` preset set instead of the stock `5/14.3/17.9/28.6`).

### Log lines

    [CXADC] rate check: card 0 measured 40.000 MHz, sysfs 40.000 MHz (agree)
    [CXADC] rate check: card 0 sysfs says 28.636 MHz but the card feeds 40.000 MHz
                   (measured) - using the measured rate (stale sysfs crystal
                   parameter on a modded card?)          [+ a status-bar line]
    [CXADC] rate check: card 0 measured 50.320 MHz matches no known rate tier;
                   using the sysfs-derived rate
    [CXADC] rate check: card 0 could not reach the live stream while draining
                   the backlog; using the sysfs-derived rate

### Verification

- Unit: `meson test cxadc_rate_tiers` (`test/cxadc_rate_tiers_test.c`, 17
  checks — jittery-but-known feeds snap; unknown/ambiguous rates and the
  excluded 35.8 do not).
- Guard: `ci_guard_tests.py` `CXADC rate probe contract` (probe wiring,
  drain phase, tier exclusion, effective-rate consumers, capture-start
  ordering).
- Hardware (2026-10-02, 2x CXADC + clockgen rig): the first attempt
  measured the driver backlog (72.5/61.5/50.3 MHz -> no tier -> sysfs kept,
  capture unaffected) — which is exactly what motivated the drain phase;
  after the drain fix both cards measured exactly `40.000 MHz, sysfs
  40.000 MHz (agree)` at startup and again at capture start. The mismatch
  path (stale sysfs 28.6 -> measured 40 override) is pinned by the tier
  unit test; a genuinely stale-sysfs card (the reporting user's machine)
  is the final end-to-end confirmation.

## CXADC kernel driver — capture start/stop + stream sync (driver side)

How the stock cxadc kernel driver (cxadc-linux3 tree, source verified at
`/home/harry/cxadc/cxadc.c`) actually starts, paces and stops a capture. This
is the foundation the GUI-side alignment work in the next section builds on;
line numbers are from that tree (2026-10-29).

### Constants that define the sync grid (cxadc.c:60-68)

- 64 MiB DMA ring = `MAX_DMA_PAGE` 8192 individually-coherent 4 KiB pages
  (allocated per page in `cxadc_probe`, not one contiguous block).
- `CLUSTER_BUFFER_SIZE` 2048: each RISC WRITE moves 2 KiB, so a page is
  filled by exactly 2 WRITEs.
- `IRQ_PERIOD_IN_PAGES` = `0x200000 >> PAGE_SHIFT` = 512 pages = 2 MiB —
  must be a power of two (the IRQ handler masks with `& ~(512-1)`).

### There is no userspace start/stop command

No ioctl starts or stops capture. The RISC/DMA engine starts once in
`cxadc_probe()` (cxadc.c:1044-1047: `MO_DEV_CNTRL2` bit 5 "run risc",
`MO_VID_DMACNTRL` bits 7|3 "enable fifo and risc") and free-runs for the
life of the module. `MO_VID_INTMSK` is set to `INTERRUPT_MASK` 0x18888 at
probe (cxadc.c:1155), but PCI-level delivery stays gated off until a user
opens the device. Userspace open()/close() are reader attach/detach:

- open: IRQ-gated handshake (below) that anchors the reader.
- close: `cxadc_char_release()` (cxadc.c:718-728) writes ONLY
  `MO_PCI_INTMSK = 0` — the engine keeps DMA-ing and `MO_VBI_GPCNT` keeps
  counting.

This free-run is why the GUI rate probe must drain a backlog (see the
feed-rate check section above) and why an fd always has fresh pre-open
content available immediately.

### Producer: the RISC program (make_risc_instructions, cxadc.c:560-599)

An infinite loop of WRITE instructions in a coherent buffer:

- `SYNC|CNT_RESET` at entry.
- Per page: 2 x 2 KiB WRITEs; the page-final WRITE carries `RISC_CNT_INC`
  (so the VBI general-purpose counter `MO_VBI_GPCNT` = current page index
  0..8191), `RISC_CNT_RESET` on the last page 8191 (counter wraps to 0), and
  `RISC_IRQ1` on every 512th page (cxadc.c:585-590).
- `JUMP` back to the first WRITE (cxadc.c:593-594).

So the producer position is hardware state (GPCNT), and IRQs land exactly on
the 2 MiB grid — the same grid the GUI sync-start uses.

### Start sync: the open() handshake (cxadc_char_open, cxadc.c:601-716)

1. Exclusive open: `in_use` under `mutex` (one reader per card; v0.5 change).
2. Card config re-applied (vmux, 8/16-bit capture ctrl, AGC/gain, PLL/SCONV
   sample rate) while the engine keeps running — settings take effect
   mid-stream.
3. `atomic_set(&lgpcnt, -1)` sentinel (cxadc.c:699), then
   `MO_PCI_INTMSK = 1` to un-gate PCI IRQ delivery (cxadc.c:700).
4. `wait_event_interruptible(readQ, lgpcnt != -1)` (cxadc.c:702) — open()
   BLOCKS until the IRQ handler publishes a page position. A signal aborts
   cleanly: re-mask the IRQ, `in_use = false`, return the error.
5. `initial_page = lgpcnt` (cxadc.c:713) — the read anchor for this fd.

First-ever open after module load waits up to one IRQ period (~73 ms
stock). A re-open returns near-instantly: `MO_VID_INTMSK` stays 0x18888
while nobody holds the fd, so `MO_VID_INTSTAT` accumulates un-acked IRQ1
bits and the pending status fires as soon as `MO_PCI_INTMSK` is un-gated.

### IRQ handler: the guaranteed-pages rule (cxadc_irq, cxadc.c:829-857)

On RISC IRQ1 (video status bit 0x8):

- read `MO_VBI_GPCNT`, round DOWN to the 512-page grid:
  `gp_cnt &= ~(IRQ_PERIOD_IN_PAGES - 1)` (cxadc.c:850)
- publish `atomic_set(&lgpcnt, gp_cnt)` + `wake_up_interruptible(&readQ)`
  (cxadc.c:851-852)
- acknowledge by writing the masked status back to `MO_VID_INTSTAT`
  (cxadc.c:854)

The in-code comment records the hardware caveat: GPCNT can LEAD the data
(the counter may show page 1 while page 1 is still in flight to RAM), but a
received IRQ guarantees everything up to its boundary has landed. `lgpcnt`
therefore means exactly: pages [anchor .. lgpcnt) are resident and safe to
copy. The power-of-two mask keeps the round-down on the RISC program's own
IRQ grid, so it never rounds past a guaranteed boundary. Other bits of
0x18888 (bits 3/7/11/15/16) are only logged and acked — bit 3 alone drives
the sync.

### Steady-state sync: read() (cxadc_char_read, cxadc.c:730-796)

- Reader page = `((offset % 64 MiB) / 4096 + initial_page) % 8192`
  (cxadc.c:738-740): position is a pure function of the fd offset,
  anchored at open.
- Copy while reader page != lgpcnt (partial-page offsets supported);
  every copied page is memset to 0 (cxadc.c:758) — destructive read.
  Consumed bytes read back as zeros until the writer's next lap rewrites
  them (~2.3 s later); zeros on a forward read are only possible via
  seek-back/re-read of consumed offsets.
- Caught up (reader page == lgpcnt): `O_NONBLOCK` returns a short read;
  blocking sleeps on readQ until the next IRQ moves lgpcnt (cxadc.c:786) —
  wakeups are 2 MiB-granular, ~73 ms stock.
- No flow control, no overrun signalling: a reader slower than the feed is
  silently lapped — unread pages are overwritten with newer data, so loss
  appears as a forward content jump, not as zeros or an error. A reader
  that idles with the fd open accumulates readable backlog bounded by the
  64 MiB ring (beyond that it is lapped).
- Gain registers are re-written on every loop stall (cxadc.c:773-778),
  which is how live `level`/`sixdb`/`center_offset` changes apply
  mid-capture without ioctls.

### Stop semantics: two levels

Level 1 — close() (reader detach): `MO_PCI_INTMSK = 0` only. No drain, no
flush, no final-release wait: whatever the writer has produced past the
reader's position stays in the ring and is abandoned (overwritten on later
laps). GPCNT keeps counting, INTSTAT keeps accumulating, and a subsequent
open re-syncs immediately via the pending status.

Level 2 — `disable_card()` (cxadc.c:480-491), the only true stop:
`MO_PCI_INTMSK = 0`, `MO_VID_INTMSK = 0`, `MO_VID_INTSTAT = ~0` (clear all),
`MO_VID_DMACNTRL = 0` (FIFO + RISC off), `MO_DEV_CNTRL2 = 0` (RISC halted).
Callers: `cxadc_remove()` (rmmod / device removal / system shutdown —
`.shutdown = cxadc_remove`, cxadc.c:1414) and `cxadc_suspend()`.

### Suspend/resume (cxadc.c:1242-1405)

Suspend = `disable_card()` + AGC register reset + PCI power-down. Resume
re-runs the full probe init sequence (the comment: "I have no idea what
state this card is in after resume so re-init the hardware and re-sync our
settings"), including restarting the RISC engine and `request_irq()`.
GPCNT restarts from 0, so any fd held open across suspend has a stale
`initial_page` anchor — readers must re-open to re-sync after a resume.

### Pace and timing accuracy (crystal-locked byte rate, both widths)

The byte rate equals the "MHz" figure in every mode (8-bit @ 8fsc ~=
28.64 MB/s; 16-bit @ 4fsc = 14.32 MS/s x 2 B = the same bytes/s), so grid
timing is identical for both sample widths:

- 28.636 MHz stock: 2 MiB release ~= 73.2 ms; 64 MiB ring ~= 2.34 s
- 35.8 MHz (tenxfsc=1): 58.6 ms; 1.87 s
- 40 MHz (mod/clockgen): 52.4 ms; 1.68 s
- 54 MHz (mod): 38.8 ms; 1.25 s

Accuracy properties of the sync mechanism:

- Sample continuity is exact by construction: the RISC loop writes every
  page in order and never skips, so a stream is gapless unless the reader
  is lapped (a silent full-ring overwrite). The sync mechanism cannot drop
  or duplicate samples — it only decides when data becomes readable.
- Release granularity is the 2 MiB grid: lgpcnt advances only at
  boundaries, so wakeups are quantized to one period. The writer is
  mid-period most of the time, so the newest readable byte sits 0..1
  period (0-73 ms stock) behind the live capture edge.
- Wakeup latency never becomes drift: the IRQ guarantees the pages are
  resident in RAM before the wakeup, so a delayed wakeup (tens of us on
  the 36-core rig; potentially ms under heavy load) only delays
  consumption — backlog grows, the content time of every byte is
  unchanged.
- The stream carries no timestamps: content time = byte offset / nominal
  rate, so absolute timing accuracy = clock accuracy. A stock crystal is
  typically ±20-50 ppm (class spec, not measured here): 20 ppm = 1.2 ms of
  drift per minute (72 ms/h); 50 ppm = 3 ms/min (180 ms/h) vs wall clock.
  The sysfs `crystal` value is only an input to the PLL/SCONV math — the
  driver does no ongoing rate correction (the GUI's measured-rate check is
  the mitigation; see the feed-rate section above).
- Driver arithmetic quantization (custom rates): the PLL fractional field
  is 20 bits — steps of (crystal/40)/2^20 ~= 0.68 Hz at stock
  (cxadc.c:674-684) — and SCONV is integer-truncated: tenxfsc=1 computes
  `131072*4/5` = 104857, truncating 104857.6 (~5.7 ppm ratio error), and
  the generic `131072*crystal/tenxfsc` (cxadc.c:686) truncates by up to
  ~10 ppm at 40 MHz-scale values. Custom tenxfsc rates carry this on top
  of the crystal spec.
- A/B relative drift: two cards on independent crystals walk apart at
  their relative ppm (milliseconds per minute at tens of ppm). The
  clockgen rig feeds both cards from one clock, so relative drift is ~0 —
  the precondition for the byte-exact A/B alignment in the next section
  holding over a whole capture.
- When a boundary's actual instant is needed (not just "somewhere in this
  period"), the GUI's 1-byte blocking read (next section) completes within
  the IRQ/wakeup slop of the crossing itself — measured tens of
  microseconds, near-identical per card. The anchor is byte-exact, so grid
  quantization affects when you learn the position, never what the
  position is.

### How the GUI layers on top of this

- `cxadc_sync_card_starts()` (next section) uses exactly this grid: open()
  latches a 512-page-aligned `initial_page`, and lgpcnt advances only on
  the same grid, so boundary releases recur at exact 2,097,152-byte
  multiples of an fd's consumed bytes — a grid-aligned reader is always a
  whole number of 2 MiB releases behind the writer.
- The rate probe's drain phase exists because of the free-run + backlog
  behavior above (first-attempt hardware measurements of 50-72 MHz were the
  driver backlog, not the card).
- The destructive zeroing is why the card stream is consume-once: the GUI
  never re-reads an offset, and the direct RAW path records exactly what
  read() returned.
- Per-card state only: every `struct cxadc` has its own readQ/lgpcnt/
  initial_page; the driver has zero cross-card sync — that is the GUI +
  clockgen layer's job.

## Multi-stream capture start alignment (CXADC cards + clockgen audio)

Equal sample counts between capture streams do NOT mean aligned content.
Two independent start-skew mechanisms were measured on the 2x CXADC +
clockgen rig (2026-10-06, prompt log `cx-misrc-sync-investigation-2026-10-06`)
and fixed:

1. **RF A/B content offset (~105-155 ms per capture).** The capture-start
   rate probes run sequentially (drain + 4 MiB timed live window per card),
   so card 0's stream position is left ~one probe duration behind card 1's
   in wall-clock content time. The RF thread then reads both cards in
   lockstep (equal chunks per iteration), which preserves that content
   offset for the whole capture — measured 114.076 ms (rfA/rfB were
   byte-exact equal, yet misaligned by that much).
2. **PCM1802 audio vs RF offset (0-350 ms).** The audio thread reads
   BUF_CAPTURE_AUDIO in full 196,608 B blocks (~349.5 ms of 46,875 Hz x 12 B
   clockgen audio) and only observed the record state after a full block
   read — so at record START up to one full block of pre-record audio was
   written (measured: WAV span exceeded the RF span by +275 ms).

### Fix 1 — audio record transitions at the top of the loop (gui_audio.c)

The record-state transitions are now handled at the TOP of every audio
loop iteration. Each iteration is bounded by the 2 ms read timeout (the
loop spins at ~500 Hz), so transitions are observed within ~2-4 ms of the
record button:

- START discards everything buffered before the transition, at the capture
  thread's 576 B write granularity (`AUDIO_SYNC_CHUNK_BYTES` = 48 frames x
  12 B, matching the ALSA readi period), so the WAV files begin at the record
  instant within the ~1 ms USB audio delivery quantum.
- STOP flushes everything buffered up to "now" through the same conversion
  path (peaks + 4ch/2ch/1ch writes), so the WAV files end at the stop
  instant. (The record-stop path stops + restarts the audio thread, whose
  exit drain already flushes the tail; the stop-flush is the safety net for
  any path where the thread survives a record stop.)

Log lines:

    [AUDIO] Record start aligned: discarded 147456 B (~262.1 ms) of pre-record audio
    [AUDIO] Record stop aligned: flushed N B (~X.X ms) of tail audio

Verified on the rig (same 15 s auto-record load test as the direct RAW
section): WAV span excess vs the RF files went from **+275 ms (baseline)
to +13 ms**; rfA/rfB remain byte-exact equal (599,785,472 B each).

### Fix 2 — synchronized card starts, exact boundary grid (gui_cxadc.c)

`cxadc_sync_card_starts()` runs after the sequential capture-start probes
and before the RF thread starts (2+ cards only), using the driver's exact
grid: `open()` latches `initial_page` = a 512-page-aligned boundary index,
so a card's release boundaries recur at exactly 2,097,152-byte multiples of
the fd's total consumed byte count (tracked across the capture-start probe
via `s_cxadc_probe_consumed_bytes`, reset per card at the capture-start
probe), and `lgpcnt` is always on the same grid — so a grid-aligned reader
is always a whole number of 2 MiB releases behind lgpcnt. Per card:

1. Drain to the current release boundary (bounded; efficiency only).
2. Grid-align: consume the exact byte remainder to the next boundary.
3. Measure a boundary crossing EXACTLY: whole 2 MiB grid reads until one
   blocks a full release period (the stream is exactly at lgpcnt), then a
   1-byte read that blocks until the NEXT boundary crossing and completes
   within microseconds of it — that completion IS the content time of the
   stream position (measured directly, no period arithmetic, no mid-release
   ambiguity).
4. Skip to T_ref = max(card crossing times) + 1 ms with EXACT byte counts
   (64 KiB chunks + an exact-remainder final read; blocking reads fill
   exactly the requested count).

Residual: the 1-byte probe's IRQ/wakeup slop (tens of microseconds,
near-identical per card) + the measured-rate error over the skip
(microseconds) — byte-exact A/B content alignment. The clockgen audio path
also runs the ALSA readi at 48-frame (~1 ms) granularity so the ring's
"now" advances at the USB delivery quantum.

Log line:

    [CXADC] sync-start: exact boundary alignment to a common content instant;
            card0: crossing@+157.123ms skip=4398.0KB total=12.34MB card1: ...
            -> all streams at T_ref (byte-exact; residual ~tens of us)

### Fix 3 — lockstep tap gate (gui_record_direct.c/.h, gui_record.c/.h, gui_cxadc.c)

The record start/stop handshake flips the live `tap_enabled` flag
asynchronously to the capture thread. With one live-checked push per
channel, a flip landing between the A and B pushes of one lockstep
iteration split the channels by one 64 KiB chunk (observed once in three
runs before this gate). The CXADC capture thread now latches the record
decision ONCE per iteration: enter both active channels' taps
(`gui_record_tap_iteration_enter` — marks each channel's tap in-flight and
returns the latched enabled state), combine into a single per-iteration
decision, push both chunks with `gui_record_direct_tap_push_latched` (which
honors the latched decision instead of the live flag) or neither, then
leave. The in-flight marking makes the stop-side disable + wait-idle
handshake wait for the whole gated iteration, so no latched push can run
after the writers' final drain. A/B record counts are now chunk-atomic
(verified: two consecutive 15 s runs byte-exact equal, read==written).

Residual audio-vs-RF offset: the PCM1802 WAV begins ~3-7 ms after the
record instant (~1 ms USB delivery quantum + ~2-4 ms observation) and ends
~2-10 ms after the stop (the record-stop entry stops the audio thread,
whose exit drain ends the files at "now"); measured span differences vs
the RF files: +13 to +23 ms across runs (baseline: +275 ms). The RF A/B
feeds are byte-exact in both counts and content; the audio boundary floor
is the USB delivery quantum without hardware timestamping.

Guard: `ci_guard_tests.py` `audio record alignment contract`,
`CXADC synchronized start contract`, and the gated-tap ordering in
`raw direct passthrough contract`.

## GUI Readout + Stats Breakdown

This section documents what the GUI stats/readouts show, where each value comes
from, and what the wait/drop counters actually mean.

### Scope

- Focus is the live GUI readout/counter behavior in `misrc_gui`.
- Definitions below are based on current code paths in:
  - `misrc_tools/misrc_gui/ui/gui_ui.c`
  - `misrc_tools/misrc_gui/input/gui_capture.c`
  - `misrc_tools/misrc_gui/processing/gui_extract.c`
  - `misrc_tools/misrc_gui/output/gui_record.c`
  - `misrc_tools/common/buffer_manager.c`

### Data flow (where counters are produced)

- Capture callback writes raw RF into `BUF_CAPTURE_RF`.
- CXADC direct channels: the capture thread's tap also pushes the card's
  native bytes into `BUF_RECORD_A/B` (see "Direct native RAW recording"
  above); extraction skips those channels while direct.
- Extraction thread reads `BUF_CAPTURE_RF`, updates sample/clip/peak stats, writes:
  - display frames to `BUF_DISPLAY`
  - recording data to `BUF_RECORD_A` / `BUF_RECORD_B` (only while recording,
    and only for channels not recorded direct)
- Writer threads drain `BUF_RECORD_A/B` and update recording byte counters
  (FLAC/converted RAW writers, or the byte-exact direct writer for direct
  channels).

### Bottom status bar readouts

Rendered in `render_status_bar()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `REC dot + HH:MM:SS`
  - Shown only while recording.
  - Time is `GetTime() - recording_start_time`.
- Status text (when not recording)
  - Shows `app->status_message`.
- `Sync: OK` / `Sync: --`
  - Uses `stream_synced`.
- `XX MSPS`
  - From `sample_rate` displayed as integer `sample_rate / 1000000`.
- `Samples`
  - Uses `samples_a` (channel A sample counter).
  - Formatted as raw count with `K/M/G` suffixes.
- `Frames`
  - Uses `frame_count`.
- `Missed`
  - Uses `missed_frame_count`.
  - This counter is debounced in GUI capture callback logic: isolated single miss events are suppressed, and only persistent/consecutive miss conditions increment it.
- `Errors`
  - Uses total `error_count`.
  - This counter is debounced in GUI capture callback logic and tracks persistent parser-error events rather than summing every per-line parser error value.
- `RF Buffer`
  - Percent fill computed from `BUF_CAPTURE_RF` ringbuffer head/tail.
- `Audio Buffer`
  - Percent fill computed from `BUF_CAPTURE_AUDIO` ringbuffer head/tail.

### Side channel stats panels (CH A / CH B)

Rendered by `render_channel_stats()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `Peak: +X% -Y%`
  - Based on `vu_*.peak_pos/peak_neg` (VU peak-hold values, not instantaneous raw ADC values).
  - Source peaks are derived from extraction stats and then smoothed/held in `gui_app_update_vu_meters()`.
- `Clip: +N -M`
  - Cumulative clip counts from extraction thread:
    - positive clip when sample `>= 2047`
    - negative clip when sample `<= -2048`
- `RST` button
  - Clears that channel's clip counters only.
- `Errors`
  - Displays `error_count_a` / `error_count_b`.
  - Current code resets these counters at capture start but does not increment them in active processing paths, so they remain `0` unless future wiring is added.
- During recording only:
  - `RAW: X MB`
    - From `recording_raw_a` / `recording_raw_b`.
  - `FLAC: Y MB` (FLAC mode only)
    - From `recording_compressed_a` / `recording_compressed_b`.
  - `Ratio: Zx` (FLAC mode only)
    - `raw_bytes / compressed_bytes`.

### Record counter placement

- Recording duration counter is in the bottom bar (left side), next to the red record indicator.
- Side channel panels carry per-channel recording size stats (`RAW/FLAC/Ratio`), not the global timer.

### Wait/drop counters: exact meaning

Wait/drop are backpressure metrics tied to ringbuffer write behavior.

#### Buffer-manager definition

In `bufmgr_write_begin()` (`misrc_tools/common/buffer_manager.c`):
- `write_waits` increments when producer must wait for space.
- `write_drops` increments when write is dropped (immediate-drop policy or retries exhausted).

So:
- **wait** = write had to pause because buffer was full.
- **drop** = write could not be queued and was discarded.

#### Policies by path

Default policies in `misrc_tools/common/buffer_manager.c`:
- `BUF_CAPTURE_RF`: wait up to 10 attempts × 5 ms, then drop.
- `BUF_CAPTURE_AUDIO`: immediate drop (no waiting).
- `BUF_RECORD_A/B`: default 200 × 5 ms, but GUI extract record path overrides this.
- `BUF_DISPLAY`: short wait, then drop (display is intentionally lossy).

GUI capture callback overrides in `misrc_tools/misrc_gui/input/gui_capture.c` (aligned to CLI timing):
- RF callback writes use `8 attempts × 1 ms`.
- Audio callback writes use `8 attempts × 1 ms`.

Record-path override in `misrc_tools/misrc_gui/processing/gui_extract.c`:
- `s_record_write_policy = 2 wait attempts x 1 ms timeout, then drop` (a very
  short wait window so a single scheduler hiccup does not force spill mode).
- The CXADC direct tap uses the same policy (`s_direct_write_policy` in
  `gui_record_direct.c`): full buffer -> brief wait -> spill temp file ->
  logged drop (see "Direct native RAW recording" above).
- Recording writes stay effectively non-blocking for extraction; when
  record buffers stay full, record frames spill to disk and only spill
  failure drops them.

#### Counters used by GUI app state

`gui_app_t` has:
- `rb_wait_count`
- `rb_drop_count`

Current behavior:
- `rb_wait_count` and `rb_drop_count` are updated from `BUF_CAPTURE_RF` buffer-manager write stats deltas each callback.
- In upstream mode, `rb_drop_count` can also be incremented by parsed hsdaoh overrun messages.

#### Where wait/drop is visible today

- Capture stop log (`gui_app_stop_capture()`):
  - prints `waits` and `drops` from app-level counters.
- Recording stop logs (`gui_record_stop()`):
  - prints recording-session wait/drop totals computed from `BUF_RECORD_A/B` deltas.
  - also prints per-buffer `A` and `B` wait/drop deltas.
- Periodic debug log from buffer manager:
  - one-line per-buffer fill/wait/drop summary.

### Rawness and interpretation notes

- Most counters are monotonic event counts since capture start.
- `Missed` is an event count ("missed at least one frame" events), not an exact per-frame-loss total.
- `Missed` and `Errors` are intentionally debounced in GUI capture mode to avoid one-off transient spikes from dominating the UI readout.
- GUI capture also applies a callback-gap resync guard: if callback timing stalls for >100 ms (for example due to system/display interruptions), parser sync state is reset before continuing so stale parser state does not generate a long burst of follow-on errors.
- `Peak` in side panels is VU peak-hold representation, not raw unsmoothed instantaneous sample.
- Buffer percentages are instantaneous snapshot values.

## Capture / regression development notes

In-tree capture-path constraints and historical regression notes (macOS
scheduling, parser CRC, tolerated-frame behavior, etc.) live in:

    misrc_tools/misrc_gui/dev/dev_notes_README.md

Key constraints to preserve when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when
  `result.error_count > 0 && result.report_errors`. Do not reject tolerated
  CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null
  checks), before width/height early returns. This prevents false
  timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start,
  explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, the audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO ->
  gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after
  capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`,
  and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.

## HARD DEV NOTE — Playback data pipeline (2026-10-12)

Playback must behave 1:1 with the live capture preview. The constraints below
are verified against real captures (VHS_Tape_08: 130 GB / 225,155,481,120-sample
20 MSPS video RF + 18.4 GB / 56,288,870,044-sample 5 MSPS hifi RF).

### FLAC header conventions (both exist on disk)

- OLD (pre-finalize): STREAMINFO `total_samples` = raw RF sample count (TRUE),
  header `sample_rate` = kHz-domain value (20000 = 20 MSPS). Header DURATION
  is 1000x too long — this is the lie external audio programs show.
- NEW (finalized): STREAMINFO `total_samples` = RF/1000 (kHz domain, so header
  duration is truthful), RF truth in Vorbis tags (`RF_TOTAL_SAMPLES`,
  `RF_SAMPLE_RATE`, `RF_SAMPLE_RATE_KHZ`, `DURATION_SECONDS`).
- `FLAC__stream_decoder_seek_absolute` rejects any seek >= the STREAMINFO
  total and ignores seektable points beyond it, so the header lie breaks
  scrubbing on finalized files.

### Resolution rules (gui_playback.c)

- TRUE stream total: throwaway probe decoder +
  `FLAC__stream_decoder_find_total_samples()` (tail decode — cheap for
  multi-GB files, authoritative for both conventions). Order: probe truth >
  RF tags > legacy wrap/companion/frame-scan fallbacks.
- Real rate: `RF_SAMPLE_RATE` tag > kHz-domain header x1000 > filename
  `<N>msps` hint. Genuine audio-rate files stay in kHz.
- STREAMINFO patch: decoders feed through the owned IO layer
  (`init_stream` callbacks). The first 26 file bytes are served from memory
  with the total_samples field patched — file on disk never modified.
  **The field is only 36 bits (~68.7G samples)**; long captures exceed it, so
  when the true total cannot be represented the patch writes 0 ("unknown"):
  libFLAC then applies NO seek clamp and resolves seeks via the seektable /
  binary search over the real stream. (Patching the true count on a 225G-sample
  file truncated to `total % 2^36` and rejected every seek past that point.)

### Sample scaling (1:1 with live preview)

Recorder conventions (gui_record.c): 16-bit FLAC = 12-bit container `<< 4`;
12-bit = container as-is; 8-bit = container `>> 4`. Playback write callbacks
restore the 12-bit container for EVERY depth (8-bit `<< 4`), so the raw
packing (`2047 - sample`), extraction (`(2047 - field) << 4` into the 16-bit
display container), VU meters, peaks and clipping all match live capture.

### Pause (DAW-style) + watchdog

- While PAUSED the playback thread keeps `last_callback_time_ms` fresh:
  `gui_capture_device_timeout` (2 s) would otherwise fire auto-reconnect and
tear the session down ("playback crashes and resets").
- Seeks while paused push exactly ONE preview batch (`preview_pending`) so
  the waveform/spectrogram render the new position, then hold.
- The spectrogram/waterfall freeze while paused (`gui_spectrogram.c`):
no scroll, new (preview) frames still land. Live capture scroll behavior is
unchanged.

### Timeline UI (gui_ui.c)

- Right-side file info next to each scrub slider (all from the actual
  selected/playing file, cached per path — never guessed): basename,
  `8-bit 20.0 MSPS`, `RAW 838.8GiB FLAC 121.5GiB 6.9x` (RAW = per-channel
  packed u32 stream = total_samples x 4, matching the capture log's raw
  accounting; FLAC = on-disk size). Fixed representative widths per tier
  (full -> compact -> hidden) so value changes never reflow the row.
- Mouse wheel over a timeline row scrubs that channel with 1-10 s
  acceleration: notches within 400 ms ramp the step (+0.75 s/notch, cap 10 s);
  idle resets to 1 s. Wheel-up = back, wheel-down = forward.

### Container limitation — external tools (ocenaudio) show "data < duration"

FLAC's fields cannot represent RF captures:
- `sample_rate` field: 20 bits, max 655,350 Hz — RF rates (5-40 MSPS) cannot
  be stored; the recorder stores the kHz-domain value (20 MSPS -> 20000 Hz).
- `total_samples` field: 36 bits, max 68,719,476,735 — this 3h07m capture has
  225,155,481,120 samples; the recorder stores total/1000 so the header
  DURATION (total/rate) is truthful.

Consequence (verified): the full data IS in the file — real signal decodes at
-ss 11000s and -ss 11230s of the 11257.77s total (ffmpeg volumedetect), and
decoding to EOF yields every sample. But tools that decode a SAMPLE COUNT
from STREAMINFO instead of decoding to EOF (ocenaudio's preview is one) stop
after 225,155,481 "samples" — 0.1% of the data — while still showing the
correct duration. No FLAC encoding can avoid this for >655 kHz captures; the
GUI playback resolves the truth via the probe + tags and decodes to EOF.

### DAW-style paused analysis window (2026-10-12 additions)

- Playing = live 1:1 rolling feed; PAUSED renders analysis windows straight
  from the file: `gui_playback_request_analysis_window` (duration-based span,
  default exactly 3 s, wheel-over-panel 1-60 s) -> async worker ->
  `gui_playback_poll_analysis_window` (512 slices x 4096 samples strided
  across the span; the spectrogram FFTs + renders them across its full
  history width).
- Window decode runs on 8 seek-aligned parallel segment decoders (own FILE
  handles + IO layer + STREAMINFO patch, isolated from the playback thread).
  Benchmarked on the 130 GB capture (3 s @ 20 MSPS): sequential 621 ms ->
  8 segments 82 ms (7.6x). Parallel only when span > 2x the seektable gap
  (8,388,608 samples); small windows decode sequentially.
- Seek while paused: backfills the 3 s pipeline window (waveform) AND
  re-centers the analysis window on the new playhead.
- Space bar = play/pause in playback mode (live capture devices keep
  start/stop; still refuses to disconnect mid-recording).
- Waveform zoom/trigger changes while paused re-process the last display
  frame (config_dirty) so they render live; ZOOM_SCALE_MIN = 1.0 = full
  resolution (1 sample/pixel).

### Verification

- `gui_playback_resolve` meson test (30 checks): synthesizes both header
  conventions + asserts true-total resolution, patched decoder-visible total,
  and seeks past the lying header total.
- Real-file checks: `/tmp/flac_probe_test.c`, `/tmp/flac_seek_verify.c`,
  `/tmp/flac_loop_verify.c`, `/tmp/flac_seg_bench.c` (probe/patch/seek/
  EOF-loop/parallel-segment decode on the actual captures).
