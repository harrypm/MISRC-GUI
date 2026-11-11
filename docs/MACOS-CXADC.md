# Native macOS CXADC backend

Experimental local integration based on upstream
`9c3894cd764db20c3eca835d041ad972cf986d37`, subsequently merged with upstream
`4245e5839e0da4a872b4b279630606d1aa2ef093` for review.
The driver/client source is maintained separately at
[cxadc-macos](https://github.com/jlnbxn/cxadc-macos).

## Build and launch

Keep `MISRC-GUI` and `cxadc-macos` as adjacent checkouts. The latter must have
its native client API (`client/cxadc_client.h`, ABI 1) and the installed/approved
CXADC DriverKit extension, control API 5 (tested driver build 8).

Follow [INSTALLATION.md](../INSTALLATION.md) for the existing macOS dependencies.
The unmodified `scripts/build-local.sh` builds `build-local/misrc_gui` for arm64.
Then, from the adjacent driver repository:

```sh
scripts/prepare-misrc-gui.sh ../MISRC-GUI
scripts/launch-misrc-gui.sh
```

Preparation builds `libcxadc-client.dylib`, copies it beside the executable and
ad-hoc signs the executable with the driver's existing user-client entitlement.
Re-sign after rebuilding. This is local development signing, not a distribution
installer or a substitute for DriverKit approval/provisioning. No security
settings were changed for this integration. The driver repository contains
extension installation instructions.

Direct launch, after preparation:

```sh
MISRC_GUI_NO_ADMIN=1 build-local/misrc_gui --config /path/to/local-settings.json
```

This environment variable bypasses the existing administrator relaunch. Native
CXADC uses the entitled normal-user executable. Optional `CXADC_CLIENT_LIBRARY`
selects the library explicitly; otherwise it is loaded beside the executable.
Without it the other backends remain available; missing-library details appear
on stderr and no fake macOS CXADC device is created.

## Capture

Select `[CXADC] CX23880 14f1:8800 ... macOS`, open F8 for ADC settings, then
start capture with the existing capture button. F8 controls VMUX (0–3), level
(0–31), sixdb, center offset (0–255) and ADC AGC. Changes require capture stopped
and are read back from the driver. Fn-F8 may be needed on macOS keyboards.
Stop capture before using the standalone capture utility.

The initial mode is single-channel unsigned RAW8 at nominal 28,636,363 Hz,
without resampling or audio. Start/stop recording with the existing record
button. Let finalization complete. The driver launcher defaults to the ignored
`cxadc-macos/artifacts/misrc-gui-recordings/` directory, with timestamped `.u8`
files. Output settings can select another directory. The launcher preserves its
local settings file after first use. Capture logs include actual ADC settings
and nominal rate; these are not the standalone CLI's JSON metadata format.
Review logs for host/user information before sharing. Never commit private RF,
audio or video recordings.

## Architecture and scope

The optional backend is isolated in `misrc_tools/misrc_gui/input/gui_cxadc_macos.*`.
It loads a versioned C function table and reuses the existing CXADC device list,
acquisition thread, direct RAW writer, waveform and FFT pipeline. The mirrored
`cxadc_client.h` originates in cxadc-macos, GPL-2.0-or-later, compatible with this
project's GPL-3.0-or-later. Keep both header copies identical. The driver library
contains the IOKit calls; the GUI contains no PCI register or DMA code.

The existing server `/rf` endpoint carries internal packed frames and does not
provide the needed native ADC controls. A direct adapter avoids a new protocol
server or sysfs/subprocess compatibility layer. Existing Linux and Windows
branches remain in place. The small unsigned visualization helper uses defined
multiplication instead of shifting negative values; saved RAW bytes bypass it.

DMA starts on the RF thread after buffers/workers are ready. Device-visible
DMA mapping, address-width checks, interrupts and ownership remain in the
unchanged driver. A 256 KiB RPC buffer feeds ordered 64 KiB GUI reads. Source
errors/timeouts, native buffer loss and output write failures request capture
stop; automated recording reports native failures with a nonzero exit. Shutdown
logs driver safe-stop status, error/overrun counters and restored PCI command.
These counters do not constitute an authoritative dropped-sample count.

The shared-memory portability fix is needed independently of CXADC: Darwin's
`shm_open` rejects `O_NOFOLLOW` with EINVAL. Only Darwin uses POSIX-allowed flags;
its ringbuffer also closes the backing descriptor after unmapping.

## Test record (2026-10-22)

M3 Max, macOS 26.7.1, CX23880-19 through an OWC enclosure; single card only.
Unmodified upstream build/smoke passed before changes. The smoke flag exits
before window creation. Integrated automated runs initialized a real OpenGL
window and processed live data through display workers; no computer-use/UI
interaction was used. The user subsequently launched the GUI, reported success, and produced an
eight-second RF recording which was decoded and exported with dropout correction.
Individual F8 controls and window-close behavior have not been independently
verified.

Real RF, VMUX 1, level 31, boost off, center 8, AGC on:

| GUI recording | RAW bytes | VHS-Decode PAL SVHS fields |
| --- | ---: | ---: |
| 10 seconds | 286,523,392 | 499 |
| 60 seconds | 1,718,091,776 | 2,998 |

Both decoded with VHS-Decode `177b505`, `--frequency 28.636363 --system PAL
--tape_format SVHS --use_saved_levels --threads 2`. Field sequence, alternating
parity and TBC lengths matched; no duplicate flags. This is decoder evidence,
not a new visual inspection. Decoder dropout regions remain. Recorded writer
read/written counts matched, with zero reported writer drops and DMA overruns;
stop was safe and PCI command restored. Actual recorded duration differs from
the wall-clock limit at chunk/GUI frame granularity. The oscillator is unmeasured.

A separate 5-second GUI run rejected a concurrent CLI start with IOReturnBusy,
kept recording and stopped cleanly. Multiple sequential GUI processes captured
successfully. Same-window repeated start/stop, physical unplug, full disk,
suspend and multi-hour GUI recording remain untested.

Existing full suite: 34/37 passed before adding the unsigned mapping test.
The three failures (`gui_settings_paths`, `gui_audio_hifi_monitor`,
`gui_rf_preview_worker`) also fail in the unchanged pinned upstream checkout.
Upstream's direct writer additionally failed on its Darwin shared-memory bug;
it passes with this fix. New `cxadc_samples`, `macos_ringbuffer` and existing
`gui_record_direct` tests all pass. Static guard tests pass.

```sh
build-local/misrc_gui --smoke-test
meson test -C build-local --print-errorlogs cxadc_samples macos_ringbuffer gui_record_direct
python3 misrc_tools/test/ci_guard_tests.py --static-only
```

The direct writer harness checks a deterministic all-byte sequence, wraparound,
spill, exact ordering and final tails. `cxadc_samples` exhaustively checks all
256 unsigned codes in the actual visualization mapping. `macos_ringbuffer`
checks real mirroring and descriptor closure across 32 allocation cycles.

No ClockGen, multi-card synchronization, audio, 10-bit capture, alternate clock,
FLAC or resampling support is claimed for this native backend. Unsupported RAW
settings fail visibly. Broader recovery and distribution testing remain separate.

## Upstream merge verification (2026-10-23)

Merged upstream `4245e58` without rewriting the tested integration commits.
The center-offset conflict preserves upstream's Windows range (0–63),
Linux/macOS range (0–255) and the native macOS setter.

The merged native arm64 build, smoke and static guards pass. Full local suite:
38/40 pass. `gui_settings_paths` and `gui_audio_hifi_monitor` still fail as they
did in the untouched original upstream baseline. Their test/implementation
sources were unchanged by the intervening upstream commits. The previously
failing timing-sensitive `gui_rf_preview_worker` passes in this run. Native RAW,
unsigned mapping, Darwin ring lifecycle and the new upstream updater tests pass.
This is build/test validation of the merge; the hardware capture evidence above
was recorded on the original integration, not repeated for this merge.
Cross-platform hosted CI status is reported separately in the pull request.

## Two-card installation validation (2026-10-29)

Two CX23880 endpoints attach to driver build 8 through an ASM1184e switch.
Each card individually recorded approximately three seconds of disconnected-input
RAW u8 through this GUI (85,983,232 bytes each), with zero reported writer drops
and DMA overruns and verified teardown. Distinct session registry IDs in names
and device markers identify the selected endpoint. The native reconnect selector
now requires an exact marker: identical names and the shared card-count index
must not silently select another card. A regression test exercises the actual
selector with identical cards and missing/replaced identities. Registry IDs are
session identities, not permanent serial numbers; refresh/reselect after reattach.

Independent concurrent driver-client tests passed at 3, 10 and 60 seconds outside
the GUI. The minute test saved 1,718,353,920 bytes per card with 25 wraps each,
zero reported DMA errors/overruns and restored PCI command state. No authoritative
zero-loss sample count is available. Neither analog input carried a test signal.

This does **not** add simultaneous macOS dual-card GUI capture or claim clock
synchronization. ClockGen hardware and synchronized GUI integration remain pending.
The GUI still captures one selected card per session. Driver PCIe/DMA code and
Linux/Windows capture paths were not changed. No security settings were changed.

Native build, static guards, smoke and reconnect regression pass. Meson: 38/41
pass; existing failures are gui_settings_paths, gui_audio_hifi_monitor and
timing-sensitive gui_rf_preview_worker, previously observed on the upstream
baseline. The GUI recordings also ran successfully on this final build.

Full transport-test reproduction and limitations: see docs/DUAL-CARD-VALIDATION.md
in the cxadc-macos checkout. Raw test files and logs remain private/ignored.


## Native macOS CXADC ClockGen integration — 2026-11-11

This local branch extends the merged single-card backend. No PCIe/DMA/interrupt
changes or replacement DriverKit extension are required: tested build 8/API 5
is unchanged. It requires the updated cxadc-macos library with optional RF ABI
v2 and ClockGen audio ABI v1. The existing RF ABI v1 remains compatible with
older GUI builds. Both mirrored headers must match the driver repository.

Build the normal GUI, then run `scripts/prepare-misrc-gui.sh ../MISRC-GUI`
from the adjacent cxadc-macos checkout. Its `scripts/launch-misrc-clockgen.sh`
uses separate local settings, selects `[CXADC] CXADC Clockgen`, and defaults to
native RAW8 A+B plus the USB stereo input. It does not automatically start a
capture. Optional `CXADC_CAPTURE_DIR` chooses the initial output folder; the
saved config/output settings are preserved on subsequent launches. The
original stock-card launcher/config is retained.

### Operation

1. Power the ClockGen, with both card clock cables attached, before reconnecting
   the modified PCIe assembly. Supply/assembly details are hardware-specific.
2. Select `[CXADC] CXADC Clockgen`. Both real Conexant endpoints and one
   matching ClockGen USB input must be present; no synthetic hardware is added.
3. Before capture, select each individual card entry and use its existing F8
   panel to set VMUX/gain/boost/offset. Then reselect the ClockGen entry. Card
   identities and A/B assignments are logged. Discovery order defines A/B;
   no automatic video/Hi-Fi signal classification is performed.
4. Start capture with the existing button. Waveform/FFT use the existing
   pipeline. The backend checks enumerated cards are idle, sets/read-backs
   BOTH external RF outputs at 40 MHz, and starts independent native streams.
5. Select native 8-bit RF A/B recording without resampling or FLAC, and the
   CH1/2 stereo WAV output. Start/stop recording normally and await finalization.
   The existing ClockGen profile also offers the CH3 head-switch output; it
   is not a third audio channel. CH4 has no source and is zero-filled.
6. Stop capture before another application opens the CX cards. Setting NORM
   on the VCR chooses linear audio for its line outputs; it does not change
   the separate Hi-Fi RF tap. Verify the RCA outputs feed the bracket inputs.

The USB audio API preserves the actual current 46,875 or 48,000 Hz device
rate, rather than resampling or assuming 48 kHz. Only 48 kHz has been hardware
verified after the repaired audio-module/mainboard bring-up. CoreAudio's
native float representation is repacked exactly to signed 24-bit PCM, then
fed into the existing GUI audio ring/writer. Native RF direct taps preserve
unsigned bytes. The GUI's unsupported macOS conversions/resampling/FLAC modes
fail explicitly rather than silently changing capture format.

### Evidence and limits

Real GUI runs passed at 3, 10 and 60 seconds. The 10-second run wrote
400,293,888 bytes per RF channel and a 480,768-frame stereo PCM24 WAV at
48 kHz. The minute run wrote 2,400,452,608 / 2,400,518,144 RF bytes and a
2,880,512-frame stereo WAV. Read/write counts matched, writer drops/waits and
reported DMA/audio overruns were zero, sample-time gaps were zero, and all
streams stopped safely with restored PCI commands. The minute RF sizes differ
by one 64 KiB record-boundary quantum; these are independent captures, not
sample-exact start/stop hardware triggering. No authoritative hardware
zero-loss counter is claimed. Private media stays outside Git.

The short GUI RF output decoded as PAL VHS at 40 MHz and was exported with
Hi-Fi and NORM tracks and dropout correction. Consult the cxadc-macos test
report for field counts, integrity results, exported file and later user
acceptance. This verifies real acquisition, not calibrated analog SNR or
sample-phase synchronization. Capture-start RF and CoreAudio host timestamps
are logged, but physical converter/USB latency and GUI record-boundary offsets
remain uncalibrated. Do not advertise sample-exact audio/video alignment.

The audio producer is a bounded SPSC queue; callbacks do not allocate or do
file I/O. Queue overflow, sample-time discontinuity, disconnect/format changes
and delivery timeout stop the macOS capture visibly. DriverKit retains DMA
ownership/address validation and safe teardown. Active hot-unplug, sleep/wake,
disk-full and multi-hour operation remain unverified. Single-card entries
remain stock-rate profiles; with an active non-stock ClockGen they are rejected
to prevent wrongly labelled files. Single modified-card ClockGen profiles and
additional rates are not implemented here.

Existing device/recording/FFT layouts are reused. No major GUI redesign, shared
Linux sysfs emulation, shell capture pipeline, new decoder or external build
dependency was added. No system-security changes, push or PR submission.

Static guards, native staging/order and reconnect tests, sample packing,
DriverKit client CPU tests and recording/ring/rate tests pass. Four failures
reproduce on untouched upstream `526e59f`: `gui_settings_paths`,
`gui_audio_hifi_monitor`, `gui_rf_preview`, `gui_rf_preview_worker`. The latter
two fail ED phase/worker timing assertions. They are not silently fixed by this
backend change. The initial complete suite had 49 passes and those four fails;
the new native staging test is additional.
