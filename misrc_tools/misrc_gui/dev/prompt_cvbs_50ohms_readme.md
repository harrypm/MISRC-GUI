# DdD 50 Ω CVBS preview and NTSC picture quality

## Scope

The session added a panel-local 50 Ω correction mode for DdD captures and motion-adaptive NTSC luma/chroma separation. The contributor discussed the control privately with harrypm. The control defaults to Off and follows the panel-local runtime exception in the GUI automation rules. `--50ohms` selects the receiver in the headless CVBS test program.

## Field statistics and RGB regressions

The Stats OSD counts a VBI candidate when the decoder accepts or rejects it. A first usable boundary establishes the reference for the next field. It contributes zero to all three counters. Missing-H and uncertain-phase rejections still count. Tests cover acquisition on the raw and corrected paths, a missing field, and loss followed by a bootstrap boundary and an accepted field. They check the OSD invariant `candidates == accepted + rejected`.

RGB regressions exercise low-contrast cuts, narrow colour changes at a block boundary and separate reference histories for each field parity after relock. Prior real-capture checks compared 317 PAL display outputs including timestamps byte for byte, and all 429 moving NTSC output timestamps against spatial decoding. With correction Off, 18:57 of the long NTSC capture produced 55 distinct RGB outputs with a maximum gap of 19.6608 ms.

## Single-core panel profiles

Windows x64, release build, soxr, fixed sync, full frame, 40 MSPS s16 input. Both harnesses used the same compiler and link flags. The upstream decoder and harness came from `90b809d0077084b7c060bce4f1c37eaf5dc10758`. Each process, including the receiver worker, was restricted to the same logical CPU. Each input was replayed three times per mode in rotating order. The table reports the harness's total and its observed range. The NTSC and PAL files were the 6.64 s and 6.40 s DdD static captures from November 22.

| Input | Decoder | Median | Range |
|---|---|---:|---:|
| PAL | upstream | 0.34x | 0.34–0.35x |
| PAL | off | 0.34x | 0.34–0.34x |
| PAL | on | 0.41x | 0.41–0.41x |
| NTSC | upstream | 0.29x | 0.29–0.30x |
| NTSC | off | 0.24x | 0.24–0.25x |
| NTSC | on | 0.31x | 0.31–0.53x |

Upstream and Off obtained no accepted VBI boundaries on these DdD inputs. On accepted 317 PAL and 396 NTSC boundaries with zero rejects. The modes therefore perform different amounts of useful picture decoding. The target is 0.50x real time. One NTSC On run reached 0.53x.

Representative harness output from the median runs:

```text
PAL upstream
LIVE profile: signal 6.40 s | resample 0.71 s (11%) | process 1.50 s (23%) of which line decode 1.03 s (16%) | 10.3 us/line over 100018 lines | total CPU 2.20 s = 0.34x real time
PAL off
LIVE profile: signal 6.40 s | resample 0.61 s (9%) | process 1.55 s (24%) of which line decode 1.05 s (16%) | 10.5 us/line over 100018 lines | total CPU 2.16 s = 0.34x real time
PAL on
LIVE profile: signal 6.40 s | resample 0.62 s (10%) | process 2.02 s (32%) of which line decode 1.23 s (19%) | 12.3 us/line over 100017 lines | total CPU 2.64 s = 0.41x real time
NTSC upstream
LIVE profile: signal 6.64 s | resample 0.76 s (11%) | process 1.17 s (18%) of which line decode 0.81 s (12%) | 7.8 us/line over 104428 lines | total CPU 1.94 s = 0.29x real time
NTSC off
LIVE profile: signal 6.64 s | resample 0.66 s (10%) | process 0.94 s (14%) of which line decode 0.52 s (8%) | 4.9 us/line over 104428 lines | total CPU 1.59 s = 0.24x real time
NTSC on
LIVE profile: signal 6.64 s | resample 0.77 s (12%) | process 1.26 s (19%) of which line decode 0.71 s (11%) | 6.8 us/line over 104428 lines | total CPU 2.03 s = 0.31x real time
```

## GUI and build verification

- Windows static guards, official `scripts/build-local.ps1 -NoAutoInstall` with a prebuilt dependency prefix, and its GUI smoke test passed. All 63 Meson tests passed in the RTL VGA release configuration.
- The original 72-second GUI replay reported 665 jumps and nine rejections. Each event was matched to the 40 MSPS ADC recording and compared with a .NET decode of the opening 50 seconds (2,994 fields). Of the jumps, 334 followed vertical-interval pulses, 319 returned to the horizontal phase after the vertical interval, and 11 occurred on a continuous horizontal grid. At 9.547 s the grid actually moved by 0.475 line across a damaged vertical interval.
- Four parity rejections at 9.265, 9.281, 23.663 and 23.679 s had intact alternating field boundaries. Three cadence rejections at 23.879, 25.848 and 25.948 s followed a missed intact boundary. Two source anomalies remain. At 7.796 s the interval measured about 430 lines and the intervening train was incomplete, although .NET maintained alternating output. Around 9.548 s only nine of 18 pulse pairs were intact and .NET also reported low sync confidence.
- With the fix the GUI at 72.27 s wall time showed 2,513 accepted boundaries, one cadence rejection, one parity rejection and one jump. A longer GUI playback covered 72.56 s of source and retained the same three events. Regression tests check displaced pulses, a genuine phase step, complete PAL and NTSC vertical trains, incomplete trains and parity. The 18:57 Off window still produced 55 distinct RGB updates with a maximum gap of 19.6608 ms.
- Repeating the single-core On profiles three times on this build gave 0.32x real time for NTSC in all runs and 0.41–0.42x for PAL (median 0.42x).
- After rebasing onto 8826212, PAL and NTSC panel regressions apply a 0.4-line phase step for 60 lines with alternating 4.7 and 10 µs sync tips. Each reports one jump and reaches the phase measured with constant 4.7 µs tips. Truncating the vertical train after the second broad pulse leaves the accepted field count unchanged in both systems.
- In the updated GUI replay, 72.797 s of the long NTSC recording produced one jump and two cadence rejections. Suppressing the incomplete train near 9.55 s leaves a longer interval to the next usable boundary. The previous version reported this region as a parity rejection. Correction Off still gave 55 distinct RGB updates at 18:57, with a 19.6608 ms maximum gap. The PAL and NTSC menu captures produced 317 and 396 accepted boundaries with zero rejections, taking 0.42x and 0.31x real time on one logical CPU.
- The mixed-H regression now includes a stationary sync grid with a displaced narrow/wide interference pair. Both systems stayed on the original phase with zero jumps, and the genuine 0.4-line step still produced one jump. A complete train with 10% post-EQ amplitude and band-limited noise was accepted. Truncation followed by a 14-times-line-frequency sine or a single surviving EQ did not produce an accepted boundary.
- The final GUI replay reached 72.398 s of source with one jump and two cadence rejects. At 18:57, 50 Ω Off produced 55 different RGB outputs with a 19.6608 ms maximum gap. The PAL menu, NTSC menu and moving NTSC recordings yielded 317, 396 and 429 accepted boundaries with no rejection. Their single-core profile totals were 0.41x, 0.32x and 0.31x real time.
- On the latest receiver, a narrow/wide pair can confirm a shifted line grid across one missing H. Both 0.4-line step directions still produced one jump. All 80 weak-tail noise cases per system (five amplitudes and 16 seeds) accepted the intended boundary without rejection. Late broad pulses are checked in both raw ADC matching and the sliced-candidate fallback. Neither path accepted the truncated train with half-line narrow pulses or the 16-times-line-frequency sine.
- The GUI played 72.425 s of the long NTSC source with one jump and two cadence rejections. The 18:57 Off window produced 55 distinct RGB updates with a maximum gap of 19.6608 ms. The PAL menu, NTSC menu and moving NTSC runs accepted 317, 396 and 429 boundaries respectively with no rejections. Their single-core profiles were 0.43x, 0.32x and 0.32x real time.
- The specified one-minute PAL EBU HackDAC reference was unavailable. The available PAL capture contains a menu. The white/yellow bar edge probe found no usable bar, and the steady-threshold raw VBI probe found no broad pulse at 0.30 s. EBU colour angles, coherence and bar registration remain unverified on the reference.
- No connected USB device matched either supported DdD VID/PID during verification. The 60-second physical live-input and operator OSD check remains pending.
