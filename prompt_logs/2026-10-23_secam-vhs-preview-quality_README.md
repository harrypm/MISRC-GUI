# SECAM VHS preview quality and full-decode comparison

The native preview recovered colour but showed dense horizontal chroma noise
and jagged horizontal placement. The same RF fixture was used throughout:
`les coulisses de notre damme-singing_10s-8fsc.flac`, explicitly 28.636 MSPS.

## Changes

- Filter decoded native C at 250 kHz after the causal LF inverse and account
  for its group delay. Keep the pre-FM bandwidth and original Y filter.
- Scale C frequency with the robust tape-clock estimate rather than noisy
  individual H spacing. Keep measured per-line geometry.
- Filter like components in the same field with a range-weighted spatial
  kernel, then center the missing component between measured neighbours.
  VBI/picture boundaries and field parities are isolated. Invalid carrier
  rows fall back to grayscale instead of showing only one colour component.
- Native H render anchors use each pulse's measured trailing edge and the
  4.7 us physical pulse duration. Original pulse classification and field
  cadence remain unchanged. The fixture has false falling-edge alternatives
  about 1 us apart; relying on the trailing edge removes visible jaggedness.

## Validation

The full Windows suite passed 41/42, with only the previously reproduced
upstream `update_cli` directory-creation failure. After the final native H
change, SECAM, worker and route tests passed again (3/3). New independent
physical RF cases test vertical colour registration, constant Y, separate
fields, a large colour boundary, VBI isolation and a carrier-free row.
Injected +/-0.025 line colour noise produces 0.00413 R-Y / 0.00199 B-Y RMS
error, with Y difference 0.00033 RMS. Existing 85 kHz, bars, clock/jitter,
identity, weak-carrier, gap and Full 626-row contracts remain covered.

The final source-paced production worker processes the complete 9.998819 s
fixture at 24.987 fps, 499 accepted fields / 249 frames, no rejects, queue
drops or colour losses at a 1.5-core budget. This is local playback evidence,
not hardware capture or sustained recording qualification.

Production final-RGB diagnostics at about 3 s show lower U/V row fluctuations
after the colour-only change. The flat-background same-component row-difference
RMS falls about 46% / 69%, without a Y transfer change. These values precede
the separate geometry fix and are image diagnostics affected by RGB clipping
and scene structure, not absolute RF SNR or a whole-capture quality score.

## Full-decode boundaries

Our .NET 2.14.0+ff03b9bd rejects `--system SECAM`; MESECAM is not substituted.
Its full Exact PAL luma-only decode is a valid luma reference. Original Python
vhs-decode 177b505e successfully decoded a 30-frame native SECAM RF/TBC window.
The two full decoders have matching source fileLoc values near 3 s. The
preview output is approximately aligned, not proven to be the exact field.

Python Y/C TBC rendered with the installed local SECAM backend still has
chroma streaks and edge colour errors. That backend identifies itself as
0.0.0 with debug probes and has no traceable source revision. Do not call this
a fully provenance-matched upstream tbc-tools reference or attribute all
remaining defects to the tape. A stronger SECAM click-NR setting did not help.

Local commands, source hashes, raw outputs, timing and images are preserved in
`F:\Repos\MISRC-GUI\.ci-artifacts\secam-vhs-quality`. Other repositories'
sources were not modified. All saved source/document files use CRLF.
