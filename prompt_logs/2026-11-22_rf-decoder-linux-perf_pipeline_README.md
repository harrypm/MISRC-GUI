# Prompt Log — 2026-11-22: RF video decoder Linux performance

Repo: `/home/harry/MISRC-GUI` (branch `main`, HEAD `90b809d`). Uncommitted.

## User report

Large Windows/Linux disparity in the RF preview video decoders: on the
18-core i9-7980XE Linux workstation the decoders choke and are barely
reliable with a 3-core budget, while Windows (Ryzen 5950X) is fine at 1 core;
Android (S10e, Doogee Fire 6, test-gen mode) and iPhone 14 were fine.

## Findings (measured with `gui_rf_preview_file`, Tape_02 PAL VHS 20 MSPS)

- Decoder is one serial thread per worker; budgets >1 core cannot speed it.
- Unmodified: ~81% of one core at ~3.6 GHz (23.6 MSPS max). Background load
  (18 busy threads) dropped clocks/HT throughput → 978-1,727 dropped blocks.
- No per-sample OS code difference; compiler flags identical. Ruled out:
  denormals (~1%), aliasing (~2%). glibc 2.35 `hypotf`/`atan2f` are slow
  (`atan2f` approximation rejected by user: would change VHS picture).
- Linux worker reniced itself to nice 19 every block (Windows: one level
  below normal) — starved when sharing a core with nice -5 capture threads.
- 5950X/A15 are ~1.5-2x faster per core than Skylake-X, explaining the
  Windows/iPhone headroom.

## Changes (user approved items 1-3; item 4 atan2f declined)

1. `gui_rf_preview.c`: inline `rf_hypotf` (double sqrt, single rounding) for
   the per-sample envelope — bit-identical, ~3%.
2. `gui_rf_preview_worker.c`: Linux decoder nice 19 → 5.
3. Decoder pipeline: back end extracted to `rf_backend_sample`; new staged
   front end `rf_front_end_job` (16k chunks) with SSE2 4-lane IQ biquads and
   packed mixers; optional `gui_rf_lane_runner_t` (new API
   `gui_rf_decoder_set_lane_runner`) overlaps front end of chunk i+1 with back
   end of chunk i. Worker owns a helper thread, engaged adaptively (backlog
   ≥4 for 16 blocks, never while budget-paced, release after 64 calm blocks);
   helper CPU charged to the budget. ARM/scalar stays on the fused loop
   unless the helper is engaged. Beta/SECAM packed and ED unchanged.
4. Dev note `rf_preview_README.md` updated; worker test stub added.

## Verification

- Frame equality vs old decoder: 0.0015% of colour values differ, max 1/255
  (PAL and NTSC samples); with `-fno-associative-math` frames are identical
  (difference is only -ffast-math rounding order).
- Decoder alone (PAL, 10 s RF): 7.78 s → 5.0 s inline, 4.3 s with helper.
- Worker real-time, idle: 0 drops, CPU 8.2-8.4 s → 6.8 s (budget 1 and 3).
- 18 busy threads, budget 3: 1,727 → 64 drops. 30 busy: ~1,890 → 745-920.
- meson tests: 58/58 runnable pass; `gui_capture_watchdog_test` (link error)
  and `gui_audio_hifi_monitor` (timeout) fail identically without changes.
- `ci_guard_tests.py`: all pass. Windows path not compiled locally (no
  MinGW); uses CONDITION_VARIABLE APIs already used elsewhere (Win10 target).

## Not done / follow-ups

- Live GUI capture on this workstation and a Windows CI build not yet run.
- Packed Beta/SECAM/ED profiles not pipelined; NEON front end for ARM.
