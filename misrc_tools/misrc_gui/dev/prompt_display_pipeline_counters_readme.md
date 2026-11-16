# PROMPT — "counters going wrong": waveform / video monitor shifting under CPU load

Session 2026-11-16 (vectorscope worktree, branch `vectorscope`). Analysis only,
no code changed. User statements:

> Have a look into the major bug of counters going wrong i.g the wavefrom
> monitor shifting all over the place under high CPU load
> This bug has been around since the early versions of the GUI
> This is not a Windows specific bug
> It might be faster to analyse the whole codebase for potentail snags and
> poor quality code points

## Hard data (two live MS2130 sessions on this machine, 32 logical CPUs)

Logs: scratchpad `gui-ms2130-5.err` (4.3 h) and `gui-ms2130-6.err`.
`[BUFMGR] RF:x%/w/d AUD:.. DISP:fill%/waits/drops`, one line per 1220
extraction blocks (~1 s).

Session 5, layout `a:video,vector`, MS2130 upstream backend:

- Display FIFO (`BUF_DISPLAY`, 256 MB = 1024 frames of 65536 samples/ch)
  sat at 70-100 % for most of minutes 63-261 of the run; drops counted the
  whole time (final segment: 75, 115, 195, 272, 352 drops per log line).
- Fill ramp when it tips over: 4 % -> 100 % in ~22 s = the consumer runs
  ~4 % slower than the producer. Capture itself clean: `0 missed, 0 errors,
  0 waits, 0 drops` on the RF path throughout.
- Display thread: 128866 frames in 108 s and 170084 in 147 s.

Session 6 (fresh launch, same layout, MS2130 raw-parser A+B): DISP hit 100 %
within seconds, drops 4581 -> 5264 across 11 log lines; later DISP 99-100 %
with ~100 drops/s; display thread 154966 frames while capture saw 11152
hsdaoh frames (~265 s) = ~585 frames/s against the 610/s the 40 MSPS feed
produces.

Offline cost of ONE Video-panel feed on the real dump (`line_harness.exe
ms2130_ntsc.raw 1`, -O2, static soxr), per 65536-sample frame (1.64 ms of
signal at 40 MSPS):

| stage                              | per frame |
|------------------------------------|-----------|
| soxr HQ resample 40 -> 14.3 MSPS   | 1.55 ms   |
| colour line decode (26.4 us/line)  | 0.68 ms   |
| gray line decode (5.9 us/line)     | 0.15 ms   |

The Vector panel owns a second `video_cvbs_plugin_t` (own soxr HQ instance
+ own full decoder, `gui_cvbs_feed_process` -> `video_cvbs_plugin_process`),
so Video + Vector on one channel runs the resampler twice on the same
samples.

## What the pipeline does when the consumer is slower than the feed

`gui_extract.c:721-743` writes every block (every `ui_stride`-th while the
record FIFOs fill) into `BUF_DISPLAY` with policy `max_wait_attempts = 0`
(`buffer_manager.c:98`): never wait, drop the block when full. The display
thread (`gui_display_thread.c:111`) reads frames in order. Consequences:

1. Latency grows to the full FIFO: 1024 frames = 1.68 s. Everything on every
   panel is then 1.7 s old. Nothing in the UI shows this (no display-FIFO
   fill or drop readout anywhere in `ui/`).
2. Once full, drops arrive as a trickle (one block here, one there), so the
   consumer sees a stream spliced every few frames. No sequence number or
   sample offset travels with a display frame, so no consumer can tell a
   gap happened or how long it was.
3. The CVBS decoder infers each gap from a sync phase error ("stream jump",
   `gui_cvbs.c:3989-4050`, 3889-3946). The H lock re-snaps in one line, but
   the LINE/FIELD COUNTERS are off by gap mod line period (65536 samples =
   25.6 PAL / 25.8 NTSC lines per dropped block) until the next vertical
   train re-anchors, i.e. the picture shifts for up to a field per gap, at
   tens of gaps per second while saturated. These are the counters going
   wrong; `stream_jumps` on the Stats OSD counts them.
4. Not Windows-specific: the policy and the missing stamps are shared code.
   On Linux it is worse because the display thread cannot raise its priority
   without CAP_SYS_NICE (`Thread priority request 3 could not be elevated`,
   dev/prompt_waveform_readout_bug.md:7), so any CPU load starves it.

## Waveform (oscilloscope) panel specifically

Per frame the panel re-finds its trigger inside the frame
(`gui_oscilloscope.c:1345-1449`), so gaps alone do not move a Rising /
Falling / CVBS-triggered trace. Two ways it does move:

- **Ch3 "Sync" (headswitch) trigger** (`gui_headswitch_lock.c:134-209`):
  the predicted trigger index is `now_audio_samples - last_edge_sample`
  mapped into the RF window, i.e. it assumes the RF frame being processed
  ends NOW. The frame is really `backlog` frames old. Error in the window =
  backlog x 65536 samples mod headswitch period: one frame of backlog is
  already 10 % of an NTSC headswitch period, the hsdaoh callback delivers
  ~14 frames per 23 ms burst (so even an idle machine has a 0-14 frame
  backlog swing), and a saturated FIFO (1024 frames) makes the position
  random. This is the "counter" that goes wrong on an RF waveform: the
  audio sample counter is compared against a frame of unknown age. Also on
  Linux an ALSA overrun (`gui_cxadc.c:2326`) is "prepared" and the lost
  frames are never added to `sample_counter`, so the audio clock silently
  falls behind the RF clock after every xrun.
- **Wide zoom**: when `display_window >= 0.9 x frame` the trigger is
  disabled and the trace starts at the frame boundary
  (`gui_oscilloscope.c:1404-1412`). At 1000 px that is >= 59 samples/px;
  the zoom range goes to 128. The trace then scrolls with the frame phase,
  ~600 times a second; a bursty/dropping feed makes it look worse.

## Snag sweep (whole `misrc_tools`, grep-driven, verified by reading)

Ordered by how directly each one feeds the symptom.

1. `common/buffer_manager.c:98-103` BUF_DISPLAY = 256 MB lossy FIFO, drop
   on full, no backlog bound, no gap signalling to consumers. 1.7 s of
   latency before any drop. (Root cause of the picture/field shifts.)
2. `processing/gui_extract.c:721-743` display write path carries no
   sequence number / source sample offset; the extraction already has
   `source_offset` and `rf_stream_marks` stamps for the RF preview path but
   the legacy display frames get none.
3. `signal/gui_headswitch_lock.c:161-176` Ch3 prediction uses the audio
   ingest cursor as "now" for a stale RF frame (see above).
   `gui_cxadc.c:2326-2332` ALSA EPIPE recovery does not advance the
   headswitch sample counter by the lost frames.
4. `signal/gui_cvbs.c:738-761, 1038-1072` process-wide static scratch
   (`g_filtered_line`, `g_pf_*`, `g_chroma_*`, `g_comb_span`, `g_secam_*`)
   shared by EVERY decoder instance. Safe while all decoders run on the
   display thread; the M-Server video path decodes on its own worker
   (`net/gui_monitor_video_server.c:152,231,306`) at the same time = data
   race on this scratch whenever monitor video is active.
5. `visualization/gui_oscilloscope.c:1263-1264` `temp_input[4096*256]`
   (4 MB) / `temp_output` statics shared by all waveform panels and by the
   remote-monitor waveform builder (`gui_oscilloscope.c:2898-2900`, HTTP
   thread) which also rewrites the process-wide `s_trigger_src_*` globals
   that the display thread reads mid-frame. Race when M-Server is active.
6. `visualization/gui_oscilloscope.c:389-391`, `:2839`, `signal/gui_cvbs.c:
   7216`, `ui/gui_dropdown.c:167,181,212,247`, `output/gui_record.c:2363`,
   `misrc_capture.c:370` pure spin loops (no yield). A TIME_CRITICAL display
   thread spinning on a lock held by a preempted normal-priority UI thread
   is a priority inversion exactly under the "high CPU load" condition.
7. `signal/gui_trigger.h:22-38` H-sync widths, line lengths and porch are
   fixed sample counts "for ~40 MSPS"; the CVBS trigger and
   `trigger_find_all_cvbs_hsyncs` are wrong at any other capture rate
   (20 / 28.6 / 100 MSPS cards, playback at file rate).
8. `signal/gui_cvbs.c:6418-6423` `lrintf` per resampled sample
   (29 k/frame) on the display thread; MinGW libm `lrint` measured ~40
   us/line earlier in this branch for the same pattern. Add-and-truncate
   is bit-identical for the clamped range.
9. `signal/gui_cvbs.c:6354` SOXR_HQ for a PREVIEW resampler is the single
   biggest CPU item (1.55 ms of a 1.64 ms budget per feed). The waveform
   panel already uses SOXR_QQ for its own preview.
10. Vector panel = second full decoder + second resampler per channel
    (`gui_cvbs.c:7277-7309`). One shared feed per channel would halve it.
11. No UI readout of display-FIFO fill / drops, so a saturated preview path
    is invisible to the user (RF/Audio buffer bars exist, display does
    not).
12. `processing/gui_extract.c:687-695` `ui_stride` throttles the display
    path only from RECORD FIFO pressure, never from display FIFO pressure,
    so preview overload is never fed back to the producer.
13. 84 hard-coded 40 MSPS / 2560 / 2540 occurrences across 22 files
    (gui_cvbs.c 25, gui_cxadc.c 7, gui_playback.c 6 ...), most of them
    comments or defaults, some live constants (item 7).
14. 33 files poll with `thrd_sleep_ms(1)` / `(0)` loops; harmless but each
    is a latency floor and a wakeup source under load.

## Fix plan (CPU-neutral or CPU-reducing, in order)

A. Display FIFO: bound the backlog. Shrink BUF_DISPLAY to ~64 frames and,
   in the display thread, when more than N frames (~8) are queued, discard
   the backlog in one `bufmgr_read_end` and process the newest frame. One
   stream jump per overload event instead of 60/s, latency <= ~15 ms.
B. Stamp display frames with the extraction `source_offset` (a parallel
   SPSC array of uint64 like `s_rf_stamps`, or a 16-byte header in the
   frame). Then: the CVBS decoder advances `global_sample_pos`, PLL phase
   (gap mod period), line and field counters by the exact gap, invalidates
   `h_ring` / comb history, and never derails; the Ch3 predictor maps the
   frame's own position (RF samples -> seconds -> audio samples) instead of
   "now", and ALSA xruns add the lost frames to `sample_counter`.
C. Surface it: display-FIFO fill % and drop count in the stats column next
   to RF/Audio buffer.
D. CPU: SOXR_MQ/LQ (or a fixed-ratio polyphase) for the preview resampler,
   add-and-truncate instead of `lrintf`, one shared CVBS feed per channel
   for Video + Vector. Measure with `line_harness.exe` before/after
   (harness: resample ms total, us/line).
E. Replace the four pure spin loops with the yielding form already used in
   `panel_registry.c:183` / `gui_headswitch_lock.c:28`.
F. Scale `gui_trigger.h` constants from the live sample rate.
G. Give the M-Server decoders their own scratch (move the `g_*` scratch into
   `cvbs_decoder_t` or a per-thread block) before monitor video and a Video
   panel run together.

## Commands run

- `logsum.ps1 -Path gui-ms2130-5.err` (scratchpad): per-second DIAG index,
  per-minute max DISP fill, event timeline.
- `harness_cpu.sh` (scratchpad): `COLOUR=1|0 SECS=6 ./line_harness.exe
  ms2130_ntsc.raw 1` -> resample / decode timing above.
- GUI launched twice for the user (pids 43852, 47188), logs
  `gui-ms2130-5/6.err`; ninja: no source changes, only version.h.
- git log on ringbuffer.c / oscilloscope / display thread; ringbuffer.c is
  the SPSC-correct cursor design (double-mapped on Windows via
  VirtualAlloc2/MapViewOfFile3, memfd on POSIX) - not the cause any more.

Signed-off-by: (pending harrypm review of the findings)

## CPU-reducing cleanup (2026-11-16, "begin CPU reducing cleanup work")

Live display-thread profile added first (`[PANELPROF]`, MISRC_DEBUG=1,
panel_registry.c: per-panel-type us per frame, lock wait, frames/s, DISP
fill/drops every 2 s). Before any change, MS2130 raw-parser, A = Video |
Vector, B = default wave | fft:

    490 frames/s: wave=16us fft=62us video=912us vector=1068us | per-frame 2059us | DISP filling

Budget is 1638 us per 65536-sample frame at 40 MSPS (610 frames/s).

1. `cvbs_plugin_resample_block`: `lrintf` per resampled sample replaced by
   clamp + add-and-truncate. soxr micro-benchmark (scratchpad soxr_bench.c):
   the HQ filter itself costs 0.18 ms/frame, the lrintf loop 1.50 ms/frame
   (MinGW UCRT libm call per sample). Harness resample total on the real
   NTSC dump 2000 -> 234 ms (-88 %), PAL testgen 6519 -> 859 ms. Line
   stability metrics identical to the third decimal. Resampler quality
   spec unchanged (HQ, passband 0.95) - it was never the cost. Same loop in
   net/gui_monitor_video_server.c, the SECAM notch (lrint per sample) and
   the RF preview byte conversion (lrint per pixel x3) fixed alike.
2. `cvbs_decoder_t.pixels_wanted` (default true; the Vector panel sets its
   feed false via `gui_cvbs_feed_set_pixels`): a vectorscope-only feed
   stops after the sync/porch/vectorscope tap - no luma kernel, chroma
   chain, pixel loop, SECAM line decode, per-field deinterlace or frame
   copies. Harness scope-only feed 23 -> 10 us/line. Live: vector
   1068 -> ~700 us/frame.
3. Vectorscope line tap: contiguous copy of the comb partner + plain 4-tap,
   two cascaded half-cycle stages fused into one 5-tap (same response).
   Scope-only feed 10 -> ~9 us/line.
4. Per-frame resample cache (`g_rs_cache`, keyed on the display frame
   sequence `g_cvbs_display_frame_seq` + frame pointer/count/rates): the
   second feed on a channel reuses the first's resampled frame; a feed
   that skipped its own soxr clears it before resuming. Headless harnesses
   keep the sequence at 0 = cache off. Saves one 0.25 ms resample per
   frame when Video and Vector share a channel.

Also: Vector panel Graticule gains a "Both" mode (75 % and 100 % targets,
one label per colour at the outer target). Suite 57/58 after each step
(update_cli environmental).

Live after steps 1-2 (same layout): 600 frames/s, video ~820us vector
~700us, per-frame ~1640us - at the budget, DISP creeping ~1 %/s. Steps 3-4
measured next.

Live after steps 1-4 (MS2130 raw-parser, A = Video | Vector, B = wave | fft,
MISRC_DEBUG=1, build 12:34):

    610 frames/s: wave=26us fft=64us video=~800us vector=~460us | per-frame ~1380us | DISP 0-1% drops 0

Display thread per-frame cost 2059 -> ~1380 us (-33 %), under the 1638 us
budget with ~15 % headroom; the display FIFO no longer fills in this
layout. Harness split of the remaining Video feed (NTSC dump, colour):
resample ~0.2 ms/frame, line decode ~14 us/line (~0.37 ms/frame),
per-sample sync/PLL loop ~8 us/line (~0.2 ms/frame).

Signed-off-by: (pending harrypm: Both graticule look, picture and vector
unchanged, no drops in the stats under a recording load)
