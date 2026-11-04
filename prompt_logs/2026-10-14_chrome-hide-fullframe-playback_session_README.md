# Prompt Readme — viewer chrome hide (all panels + video modes), stats arrow, full-frame entire-signal render + hybrid CRT levels, playback reader, waveform-playback accuracy

Date: 2026-10-14 (session continues)
Repo: `C:\Users\Harry\MISRC-GUI` (branch `main` at `0cfbc19`, uncommitted Windows MF fix in `misrc_tools/misrc_gui/input/gui_capture.c` preserved throughout)
Plan: `d373df91-bf4c-448b-b015-415dedd360ba` (approved; 4 local child agents + lead)

## User-confirmed scope
1. Double-click chrome-hide on ALL viewer modes: 3 video decoder modes (CVBS, Tape, VHS FM) + all analysis panels (Waveform, FFT, Waterfall, Spectro, Histogram, Demod).
2. VHS FM stats dropdown button: arrow overlaps the centered label — remove it entirely.
3. Full frame mode: show the ENTIRE signal (~625 PAL / ~525 NTSC lines) as one solid interlaced frame, slightly taller than 4:3.
4. Active-area contrast: correct IRE/black level for NTSC/PAL (tbc-tools studio-swing convention).
5. Full-frame VBI: replicate tbc-tools analyse "hybrid" mode (CRT/production-panel contrast, background retains chroma).
6. Player: support raw MISRC + generic files with the same reader abilities as FLAC-Chop.
7. Side note: waveform display during playback not accurate to actual data (investigated; root cause below).

## Pre-work: tbc-tools repo update (user request)
`git -C C:\Users\Harry\tbc-tools pull` failed: `fatal: pack has 147 unresolved deltas` — 3 packs
missing their `.pack` files (orphaned `.idx`/`.rev`), 2 stale `tmp_pack_*`, whole `.git`
ReadOnly-flagged, and a stale remote ref `origin/metadata-improvements` pointing at a lost object.
Repair:
```
Remove-Item -Force (tmp_pack_*, 4 orphaned .idx/.rev pairs)   # initial attempt failed: files ReadOnly -> -Force
git fetch --refetch origin        # 17,436 objects / 6.77 GiB re-downloaded, all deltas resolved
git update-ref -d refs/remotes/origin/metadata-improvements  (after attrib -r)
git fetch origin                  # OK: 820b9184..c6c3d401 + tags v3.2.8-v3.3.0
git merge --ff-only origin/main   # 468 files updated; main = c6c3d401
```
Hybrid-mode reference read from the updated tree: `src/tbc-analyse/tbcsource.cpp`
(`makeYuvRangeScale` studio swing Y=(sample-black)*219/range+16; `applyFullFrameDecodeBounds`) and
`git show 063d83e` (hybrid background black offsets NTSC 0x0d48 / PAL 0x0708; background keeps chroma).

## Orchestration + merge (lead)
4 children in isolated worktrees (from main `0cfbc19`, `MISRC_DEPS_PREFIX` reusing the main
checkout's `.deps/install`), each validated build + meson suite in its worktree, patch handed back
uncommitted as `C:\Users\Harry\handoff-<name>.patch`:
- `handoff-video-panel-modes.patch` — applied first. gui_cvbs.c/gui_vhs_fm.c/h: stats arrow
  removed (padding 24->16); panel-level `chrome` state shared by all 3 video modes (survives
  decoder cycling); control clicks reset the tracker; double-click toggles all chrome incl.
  CVBS/Tape OSD; restore works while hidden.
- `handoff-rf-full-frame.patch` — applied in two stages (harness flag first, then the DSP) so the
  BEFORE full-mode PPM could be captured against the OLD DSP: Full mode renders every field line
  (PAL 626 / NTSC 526 frame height, `GUI_RF_FRAME_MAX_HEIGHT` 576->640), per-parity tail row
  cleared, `timing0` fixed to `line_first+y`, studio-swing active levels, hybrid background
  levels (IRE-domain constants translated from tbc-tools 0x0d48/0x0708; NTSC hybrid black
  -44.184 IRE / span 197.680; PAL -38.754 / 174.147), synthetic full-frame contracts in
  `gui_rf_preview_test.c`, `--full` + seconds cap in `gui_rf_preview_file.c`.
  Two reasoned test-tolerance changes (documented in code comments +
  `C:\Users\Harry\rf-full-frame-prompt-readme.md`): NTSC noisy-bars window RMS .012->.015
  (studio swing stops white clipping), VBI row minimum .05->.08 (2.3 us EQ pulses cannot fully
  settle through the NTSC deemph chain; 4.7 us H and 27 us broad sync DO reach floor).
- `handoff-panel-chrome-hide.patch` — applied. New `ui/gui_chrome_hide.h/.c` (350 ms / 6 px
  detection mirroring the VHS FM pattern) wired into all 5 analysis panels (Waveform, FFT,
  Waterfall+Spectro, Histogram, Demod): chrome gated, control clicks gated on hidden flag +
  reset tracker, double-click toggles, out-of-bounds resets. meson.build: new source + 7 test
  entries.
- `handoff-playback-reader.patch` — DONE (lead takeover). The child's run errored mid-package (runtime error at 22:47; user
  confirmed "playback reader stopped"), but its implementation survived complete in the worktree
  (4 files, +3,177/-125; no new source files, meson.build untouched by this package). Lead validated
  it directly in its worktree: ninja build OK (a first attempt appeared hung/cancelled but the
  .ninja_log shows it completed: gui_playback.c.obj + misrc_gui.exe link), --smoke-test exit 0,
  its extended gui_playback_resolve harness 301 checks / 0 failures (was 29 checks; covers raw
  u8/s8/u16/s16 + .raw/.bin/.pcm with msps hints + disambiguation errors, packed .lds at the
  40 MSPS default + msps override + partial-group byte dropping, WAV 48 kHz as-is vs 20/40 kHz
  RF /1000 + stereo rejection, Ogg-FLAC via bounded remux with exact last-page granule totals,
  per-codec magic sniffing with specific errors for Vorbis/Opus/Speex/Theora Ogg, ID3-prefixed
  FLAC with an ffmpeg strip hint, unknown types), full meson suite 28/28 in its worktree, then
  patch applied to the user's tree.
- Lead integration edit: `gui_vhs_fm.c` draw aspect generalized to the actual frame
  height/width (Active 4:3 unchanged; Full PAL 626/768, NTSC 526/640; 4:3 fallback).

## Central validation (merged state, before the playback-reader patch)
```
ninja -C build-local misrc_gui.exe            # meson auto-regen (new source) -> link OK
.\build-local\misrc_gui.exe --smoke-test     # exit 0
python -m mesonbuild.mesonmain test -C build-local   # 27/28 (gui_rf_playback_start FAILED once)
.\build-local\gui_rf_playback_start_test.exe # exit 0 (known timing flake under parallel load;
                                             # child verified 10/10 on the BASE commit too)
```

## Real-tape verification (L:\Tape_03\Diamler_Chrysler_NTSC_SP_Tape_03_video_rf_20msps.flac)
ffprobe: flac, 20000 Hz header (Msps-in-Hz convention), mono s16, ~9e9 samples (~450 real s at
20 MSPS), NO Vorbis tags. Excerpt: `& "C:\Program Files\DecodeTools\ffmpeg.exe" -ss 3600 -i
<flac> -t 5000 -f s16le -ac 1` -> `.ci-artifacts\tape03_ntsc_excerpt.raw` (200 MB = 5 real s).
("ffmpeg" alone resolves to a ps1 wrapper — use ffmpeg.exe explicitly.)

| Run | PPM | Result |
|---|---|---|
| BEFORE full (old DSP + new harness flag) | `tape03_before_full.ppm` | height 480 (the crop/displacement), 149 frames, all locks, 0 rejects |
| AFTER full (merged DSP) | `tape03_after_full.ppm` | height 526, 149 frames, all locks, 299/300 fields, 0 rejects, "NTSC VHS SP color locked" |
| AFTER active (merged levels) | `tape03_after_active.ppm` | height 480, all locks |
| Baseline active (pre-change, for reference) | `tape03_baseline_active.ppm` | recorded before any patch |

(One stale-binary pitfall found and fixed: after applying the DSP patch only `misrc_gui.exe`
had been rebuilt — `gui_rf_preview_file.exe` must be rebuilt too or it reports the old height.)

Numeric checks (python on the PPMs):
- AFTER full row profile: VBI rows 0..~43, picture extends past row 480 to ~505 (the previously
  cropped bottom lines), tail VBI + cleared tail row (row 525 = 0).
- Registration (rows 100..400): adjacent even/odd diff 7.0 vs displaced 7.9 — GOOD (no
  misregistration introduced; before 7.4 vs 8.4).
- Black level (Active, same footage): darkest-0.1% 11.4 (old, crushed at floor) -> 25.4 (new),
  darkest-1% 19.9 -> 33.0; whites keep headroom (only >100 IRE overshoots reach 255).
- VBI contrast (full): row-mean spread 38.1 -> 49.7 with a brighter blanking band (hybrid
  background mapping) — sync tips at floor, EQ/broad structure inspectable.

## Waveform-playback accuracy — ROOT CAUSE (confirmed, fix in the playback-reader patch)
`gui_playback.c` decoder write callbacks apply the MISRC RECORDER bit-depth convention to every
16-bit FLAC (`in[i] >> 4`, "12-bit container << 4"). Hard data from the user's tape FLAC:
93.8% of the first 1M excerpt samples are NOT multiples of 16, min/max -2144/+2093 — the file
stores 12-bit-range samples DIRECTLY (generic/third-party, no Vorbis tags). The unconditional
`>>4` crushes it to ~1/16 amplitude -> the waveform panel shows ~6.7% of the actual data. The
RF preview is amplitude-invariant, which is why video decode locked fine while the waveform
"was not accurate to actual data". Requested fix (in the playback-reader child's package):
per-file convention detection (mod-16 + range) — recorder files keep the existing restore
(bit-exact), direct-12-bit-domain files pass through as-is, full-range generic files scale
into the 12-bit display container; per-format mapping for the new raw/WAV/Ogg/.lds inputs;
harness cases for each convention.

## Final central validation (fully merged state: all 4 packages + lead integration edits)
```
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\build-local.ps1
  # 67/67 steps, misrc_gui.exe linked, --smoke-test PASS,
  # "[build-local.ps1] OK: build path is connected and local build succeeded."
  # (The leading 'No module named ninja' Python-probe message is the same non-fatal artifact
  #  as every earlier build; the script proceeds with MSYS2's ninja.)
python -m mesonbuild.mesonmain test -C build-local   # 28/28 OK, 0 Fail
```
Final merged diff: 18 files, +4,008/-339 (meson.build, gui_capture.c MF fix preserved,
gui_cvbs/gui_vhs_fm/gui_rf_preview + headers, gui_chrome_hide new, 5 analysis panels,
gui_playback + header + resolve harness + start test, RF preview tests + file harness).

Waveform-accuracy fix on the REAL tape FLAC (derived from verified data): the file measures
93.8% non-mod-16, peak +-2190 -> the implemented decision table routes it to PASS (direct
values, no >>4 crush) -> the waveform panel now sees the actual data. The harness tested the
exact same convention fixture (frame range [-2190, 2185]) -> 301 checks, 0 failures. GUI
visual confirmation of the waveform against this file stays on the user checklist.

## User GUI feedback round 1 (2026-10-15 ~01:20) — three fixes, all merged + validated
User ran the merged GUI and reported: (1) hybrid/active contrast a little off — image area washed
out/flat (confirmed via question: slight tweak, keep the CRT look with more punch);
(2) stats colours should be yellow with a slight background opacity for readability;
(3) the colour-lock reason line still showed with Stats: Off.

Fixes (lead, direct edits):
- `gui_rf_preview.h`: NEW shared `rf_display_level()` + `RF_DISPLAY_BLACK_255 10.0` /
  `RF_DISPLAY_WHITE_255 245.0` — the studio-swing transfer punched up from pure 16..235
  (blacks ~6/255 deeper, whites ~10/255 hotter; ~4% footroom + headroom remain: sync -40 IRE
  still clamps at floor, ~110 IRE overshoots still reach 255). Single shared definition used by
  BOTH the renderer (`gui_rf_preview.c` leveled path now calls rf_display_level) and the
  synthetic-test oracle (`display_level()` delegates to it) — the mapping can never drift
  between renderer and tests again.
- `gui_vhs_fm.c`: stats lines now yellow (255,220,120) matching the reason line; a slight
  translucent black backing (alpha 110) covers exactly the bottom-stacked text block
  (stats + reason lines; top clamped for short panels where lines are skipped);
  Stats: Off now hides the reason line too (show_stats gates the whole readout).

Validation (after the user closed the running GUI — the first relink failed with
`cannot open misrc_gui.exe: Permission denied` while their test session held the exe):
```
ninja -C build-local misrc_gui.exe gui_rf_preview_file.exe gui_rf_preview_test.exe  # OK
.\build-local\misrc_gui.exe --smoke-test                    # exit 0
.\build-local\gui_rf_preview_test.exe                      # ALL contracts pass (incl. new
  # full-frame level asserts: PAL black .039 / NTSC .043 = 10/255 lifted floor, sync at floor,
  # porch hybrid unchanged, registration .027/.014; NTSC noisy-bars window RMS .0101 <= .015 —
  # the wider span held WITHOUT any tolerance change)
python -m mesonbuild.mesonmain test -C build-local         # 28/28 OK, 0 Fail
```
Real-tape re-verification (same excerpt): full 640x526 all locks 0 rejects; punchier levels
measured: darkest-0.1% 11.4 (crushed) -> 25.4 (16/255) -> **20.3** (10/245), darkest-1% 19.9 ->
33.0 -> **28.3**; whites unchanged (only >100 IRE overshoots reach 255); registration 7.2 vs
8.1 GOOD; VBI spread 49.7 unchanged. Updated PPMs: .ci-artifacts\tape03_after_full.ppm,
tape03_after_active.ppm.

## Commit + push round (2026-10-17, user request "commit and push")
Two logical commits on top of main 0cfbc19: (1) the user's Windows MF MS2130 fix
(gui_capture.c, +64); (2) the feature package (20 files incl. the new
gui_chrome_hide.c/h + the prompt log). First push was REJECTED: PR #8
(JunliangRen, "support Unicode file I/O and UI text on Windows and Linux", 34
files) landed between my pre-push fetch and the push.

Integration (git pull --rebase; 3 conflicts: meson.build, gui_playback.c, the
resolve harness — resolved as unions/our-structure-where-overlapped), plus a
semantic pass so the new reader honors the PR's UTF-8-at-boundaries convention:
- file_utils gains file_remove_utf8 (their file_wide_path style; needed for the
  Ogg remux temp cleanup).
- gui_playback.c: every user-path open in the NEW reader code (sniffer, raw/WAV
  open, Ogg header/scan/remux-src, native-FLAC open) now goes through
  file_open_read_utf8; the remux temp is created via file_open_utf8 and removed
  via file_remove_utf8; gui_playback_temp_path builds a UTF-8 temp path from
  GetTempPathW+WideCharToMultiByte (was GetTempPathA) so a non-ASCII TEMP
  directory round-trips.
- One rebase-resolution defect caught by the build: the harness resolution
  missed test_long_path's closing brace (function definitions nested inside a
  function -> invalid storage class). Fixed + amended into the (still unpushed)
  feature commit before pushing.

Combined-state validation: canonical build 69 steps + smoke PASS; merged
resolve harness 1143 checks / 0 failures (their Unicode/long-path suite + our
conv16/format suite in one binary); full meson suite 33/33 OK (their 5 new
tests: gui_font, gui_record_paths, gui_settings_paths, gui_text_edit, path_io).
Pushed: 69e5386..e34dce9 main -> main (5d72391 = MF fix, e34dce9 = features).

## FFT toggleable log frequency scale (2026-10-18 later, user request — uncommitted)
Added a Scale: Lin/Log toggle button (top-left of the FFT panel, next to the
annotation chrome; hidden with the double-click chrome toggle like every other
control) to gui_fft.c:
- Axis transforms fft_freq_norm_to_axis/fft_axis_to_freq_norm operate in
  normalized axis space, so the existing zoom/pan work unchanged in both modes;
  log starts at the first non-DC bin (bin 1 at axis 0, DC excluded).
- Log gridlines are 1-2-5 per decade (fft_log_grid_lines); a zoomed view
  spanning under a decade falls back to the linear 1-2-5 grid (near-linear
  over a sub-decade log span). The /div readout becomes the visible span
  (e.g. "4.88kHz-20MHz") in decade mode. Trace segments, peak-hover bin
  mapping and peak marker positions all route through the transform; a
  segment off-screen cull was added.
- gui_text.c linked into the gui_fft_label test (the button uses the shared
  gui_text_measure/gui_text_draw pair, like the other panels' buttons).
- Harness additions: identity/endpoints/monotonicity/round-trip transform
  checks + decade-grid enumeration (12 lines on a 40 MSPS full-range view,
  sparse sub-decade fallback, degenerate ranges) — 846 checks / 0 failures.
Validation: build + smoke PASS; full meson suite 33/33. NOT committed
(user has not asked); GUI confirmation of the toggle pending.

## PR #9 merge + 2x40/200 MSPS rate provisions round (2026-10-18, user request)
Merged PR #9 (r3dfx080, "Use an 8 MB stdio buffer for capture output files",
forked from 0cfbc19): one merge conflict in gui_record.c (both open sites)
resolved by keeping PR #8's UTF-8 opens + inserting PR #9's
setvbuf(8 MB, _IOFBF) calls after them. Merge commit db13ba1 (GitHub-style
message); PR #9 marked MERGED on GitHub after the push (fork-PR reachability
detection, confirmed via gh).

User tweaks (commit 27f95b5, 6 files +91/-8):
- RF input-rate windows raised 160 -> 200 MSPS: gui_rf_resampler.c
  (RF_MAX_INPUT_RATE_HZ), gui_rf_preview_worker.c (RF_MAX_RATE), both offline
  harnesses; gui_rf_resampler_test now rejects >200 MSPS and covers a 200 MSPS
  soxr continuity/gain case. 2x 40 MSPS dual-channel stays provisioned via
  per-channel exact-path workers.
- HARD dev note: misrc_tools/misrc_gui/dev/output_stdio_buffer_and_rf_rate_provisions.md
  — documents that the 8 MB output stdio buffer affects ALL capture devices
  (hsdaoh/MISRC, CXADC incl. direct RAW taps, DdD, simple_capture/MF,
  simulated, playback re-record) and ALL output/network types (local/USB/
  SMB/NFS), with the byte-rate qualification table (2x40 16-bit RAW = 160
  MB/s, 200 MSPS 16-bit RAW = 400 MB/s vs 1 GbE SMB ~26 MB/s sustained),
  the Stop/kill finalize window, and every rate cap location.

Validation: canonical build 69 steps + smoke PASS; full meson suite 33/33
(incl. gui_rf_resampler with the 200 MSPS case). Pushed e34dce9..27f95b5.

## State — all 7 items implemented; headless validation complete
- Build + smoke: PASS. Full meson suite: 28/28 (twice; gui_rf_playback_start flaked once in an
  earlier parallel-load run and passed standalone + in both final suites).
- Real-tape verification: before/after PPMs + numeric checks recorded above (height 480->526,
  bottom lines present, registration GOOD, black lifted 11.4->25.4 darkest-0.1%, VBI spread
  38.1->49.7).
- Evidence artifacts kept: .ci-artifacts\tape03_*.ppm/raw/png, C:\Users\Harry\handoff-*.patch
  (4), C:\Users\Harry\rf-full-frame-verify\, C:\Users\Harry\rf-full-frame-prompt-readme.md.
- Worktrees cleaned up after merge (git worktree remove --force + branch refs deleted).
- PENDING (user, per rules): GUI confirmation checklist (prompt_logs/2026-10-15_...README.md);
  zip restore point ONLY after the user confirms "fully working".
