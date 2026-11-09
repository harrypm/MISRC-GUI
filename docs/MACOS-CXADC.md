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

Follow [INSTALLATION.md](INSTALLATION.md) for the existing macOS dependencies.
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
