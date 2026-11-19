# Prompt log: loop icon, cut markers, field-step cue fix (2026-11-19)

Branch `playback-cut-markers`, from main fc96d56.

## Input (harrypm)

- "I want to fix the loop icon"
- "and add a marker option icon next to the play/loop icon for cutting" (FLAC-Chop cut points)
- "< & > keys do not advance/rewind by frames just loop a by 1 sec.."
- "just missing the marker icon continue"

## Output

### Field step / paused playhead (bug)

A seek while paused does not land on the target. The worker rewinds to `target - preview window` (min(3 s, 256 batches): about 0.84 s at 20 MS/s) and plays a preroll up to the target, then stops up to one batch (65,536 samples, about 52 lines at 20 MS/s) past it. The arrow-key field step read the raw playhead, so each tap stepped from somewhere inside the preroll; the result looked like a ~1 s loop.

- `input/gui_playback.c`: a per-channel `cue_sample` holds the paused seek's exact target. The worker clears it whenever the channel plays.
- New `gui_playback_get_cue_samples_channel()`:
  - while paused, returns the pending seek, else the cue;
  - otherwise returns the raw position.
- `gui_playback_get_position_samples_channel()` is unchanged. `gui_rf_playback_start_test` relies on it being the raw preroll playhead. A first attempt that changed it failed that test.
- The cue is used by:
  - the field step (`core/misrc_gui.c`);
  - the timeline timecode (`ui/gui_ui.c`);
  - the wheel scrub;
  - the Spectro paused window centre;
  - Set IN / Set OUT.

### Loop icon

`ui/clay_renderer_raylib.c`: the ring arrow read as an "O" at 12–20 px. It is now a repeat glyph:

- a rounded rectangle, open at the top-right and bottom-left;
- a right-pointing head on the top run;
- a left-pointing head on the bottom run.

The triangles are drawn counter-clockwise on screen.

### Cut markers

- New module `input/gui_cut_markers.{c,h}`. It holds IN / OUT per channel in real RF samples, bound to the file they were set on.
  - IN at or after OUT clears OUT; OUT at or before IN clears IN.
  - It builds `flac-chop --gui "<file>" [--in N] [--out N] --units samples`, quoted for the host shell: `".."` on Windows, `'..'` on POSIX.
  - FLAC-Chop's parser (`flac-chop/gui/main.cpp`) takes exactly these flags.
- `[ | ]` button beside Play / Loop (`PlaybackMarkerBtn`). The IN bracket turns green once IN is set, the OUT bracket red once OUT is set.
- Its menu has six items:
  - Set IN at playhead (I)
  - Set OUT at playhead (O)
  - Go to IN \<time\>
  - Go to OUT \<time\>
  - Clear IN / OUT
  - Copy FLAC-Chop command (to the clipboard; also logged as `[CUT]`)
- The click handler runs before the scrub-track hit test.
- The layout budget now holds three transport buttons.
- Scrub track: IN / OUT ticks (green / red with a dark edge) and a span line under the track. The ticks pass clicks through to the track.
- Spectro paused window: IN / OUT lines with tags.
- Keys: I / O set IN / OUT at the playhead. They use the field-step arrows' gate, and Ctrl / Cmd chords are ignored.
- Flag: `--markers <a|b>:in=<samples>,out=<samples>,menu=open|closed`.

## Tests

- New `gui_cut_markers` test covers:
  - per-file binding;
  - crossing marks;
  - preset binding;
  - command text and quoting;
  - buffer overflow.
- `gui_spectrogram_playback_lifetime_test`: stub for the cue getter.
- Suite 60/61; `update_cli` is environmental. `gui_capture_watchdog_test` still does not link (upstream, since PR #30).
- `--markers` parse checked: good values, out<=in, bad channel, negative, unknown key.

## Not yet confirmed by harrypm

- Arrow keys now step a field / frame from the exact cue.
- Loop and marker icons are legible.
- The menu actions and ticks work.

## Commands

- Patches: scratchpad `patch_cue.py`, `patch_cue2.py`, `patch_loopicon.py`, `patch_marker_icon.py`, `patch_marker_ui.py`, `patch_marker_core.py`.
- Build: `ninja -k 0 -C build-ucrt` (UCRT64).
- Tests: `suite_skip.sh`.

## Follow-up (same session)

### Input (harrypm)

- "Forward advance is now good, backwards needs to be fixed."
- "fix the one last issue of clipping indicators reading data wrong when source files clearly are not clipping (i.g L:\000 RF Captures & Decode Stuff\Tape_03\Diamler_Chrysler_NTSC_SP_Tape_03_video_rf_20msps.flac)"
- "fix player mode persistance": the panel view modes per channel (e.g. FM RF + CVBS monitoring).
- "set the RF Preview and CVBS decoders OSD to off as the stock open setting, rename budget to just Cores : XXX"
- Back step: "Flicker / Replay same as forward action issue before".

### Output

- **Step path** (`gui_playback_step_sample_channel`, used by the arrow keys):
  - Forward hops of up to 1 s from where the paused preroll stopped just run the preroll on: no seek, no rewind.
  - Back steps seek with a 0.2 s lead-in, not the scrub's 3 s / 256 batches.
  - While a step-back lead-in runs (plus an 80 ms settle), `gui_playback_display_hold_channel` is true. The Video panel then skips its frame swaps (CVBS, Tape Decode, RF Preview), so the previous picture stays until the step lands.
  - Held arrows only fire the next frame once the hold has cleared.
  - Log check (`[DBGA]`): back targets were exactly one field (400,000 samples at 20 MS/s) apart; the lead-in was 63 batches.
- **Clipping:** measured on 2 s of the Daimler file (40 million samples):
  - peaks are −2,180 / +2,140 and 84 ppm of samples go past ±2,047;
  - the histogram tails off smoothly with no rail pile-up, so the signal is not clipped.
  - Cause: the conv16 PASS mode clamps those peaks onto the 12-bit container rails, and the extractor counted rail values as clips (about 3,400 every 2 s).
  - Fix: new `app->rf_clip_rail_clamped[ch]` is set by playback while the channel's conv16 mode is PASS. Both extractor paths (paired and independent) skip rail clip counting for that channel. 12-bit, 8-bit, recorder and full-range 16-bit files still count clips.
- **OSD:** CVBS opens with its OSD off (Tape Decode keeps Minimal). RF Preview opens with Stats Off (`overlay.stats_view = 2`).
- **Label:** "Budget: x core (shared)" is now "Cores : x" (Video panel and monitor client).
- **Panel persistence:**
  - Settings keys `panel_view_a` / `panel_view_b` (`--panel-view` names, `left[,right]`) and `video_decoder_a` / `video_decoder_b` (`cvbs|tape|rf`, `-` for non-Video slots).
  - Captured at exit and applied after `gui_app_init`; `--panel-view` still overrides.
  - New `gui_video_panel_get_decoder` / `gui_video_panel_set_decoder`.
  - Round-trip test on a config copy: run 1 set `a:spectro,video` and `b:video`; with B edited to `rf`, run 2 restored both channels and kept `rf` through its own exit save.
- **Side effect, undone:** a test launch with the Daimler file as a positional argument loaded it into channel A of harrypm's real settings. `playback_file_a` was restored to the EBU file; a backup of the settings before the fix is in the scratchpad. Later tests used `--config` copies.

### Not yet confirmed by harrypm

- Back step: no visible replay, and the 80 ms settle is enough.
- Clip indicators stay clear on the Daimler file.
- Saved panels restore in the real app.
