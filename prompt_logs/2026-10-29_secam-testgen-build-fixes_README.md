# Prompt README — SECAM test-gen A/B + CI build fixes (2026-10-29)

## Inputs
- "commit push and fix builds" (CI failures on main after b2ae69a)
- "lets continue with the SECAM test signal gen mode fixes"

## Commands run + key outputs

### CI failure triage
- `gh run list --repo harrypm/MISRC-GUI` -> last 3 pushes failed ("Build and release binary")
- `gh run view 40528105327` -> failed jobs: Android APK (arm64-v8a), Linux AppImage (x86_64 + arm64).
  Windows/macOS/guards all pass.
- `gh api .../jobs/122276323336/logs` -> all three fail identically in `meson setup`:
  misrc_tools/meson.build:13:2 ERROR: Unhandled python exception
  TypeError: expected str, bytes or os.PathLike object, not ExternalProgram
  at func_vcs_tag: `vcs_cmd[0] = shutil.which(vcs_cmd[0]) or ...` (meson 0.61.2, ubuntu-22.04)

### Root cause 2 (hidden): git-version.py never ran on CI
- build.ninja passes the `files('git-version.py')` arg as a BUILD-DIR-relative path, but
  vcstagger runs the command with cwd=source_dir -> path does not resolve on CI.
- build-local worked only by accident (sibling layout): build-local/version.h had
  dev-2026-10-27-d647342-dirty.

### Fix + verification
- meson.build: pass `[version_prog.full_path(), meson.current_source_dir() / 'git-version.py']`
  (plain strings); meson_version floor >= 0.55 (Program.full_path).
- Throwaway build dir with unrelated layout:
  `meson setup %TEMP%\vcsver_test C:\Users\Harry\MISRC-GUI\misrc_tools`
  `ninja -C %TEMP%\vcsver_test version.h`
  -> version.h: `dev-2026-10-29-b2ae69a-dirty` (was `unknown_version`)
- `python misrc_tools\test\ci_guard_tests.py --static-only` -> exit 0, no FAIL/ERROR.
- Commit 5ef0d5d pushed to main.

### SECAM A/B (committed generator b2ae69a vs real capture, panel path)
- `testgen_raw.exe secam 125 sim_secam.raw` (recompiled vs committed source)
- `gui_cvbs_test.exe --secam --rate=40 --s16 --panel --full --ppm=sim_secam.ppm sim_secam.raw`
  -> locked=1 accepted=248 rejected=0; black bar amp=0.219 (false chroma, real = 0.003);
  angles scrambled (yellow 84.9 vs real 167.4)
- `gui_cvbs_test.exe --secam --rate=40 --s16 --panel --full real_secam.raw`
  -> real angles: yellow 167.4, cyan 284.3, green 241.0, magenta 59.7, red 103.4, blue 346.5
  (= standard EBU UV angles); amps 0.236-0.387; black 0.003
- Decoder measured sim rest frequencies fob=4168726/for=4324976 vs real
  fob=4249945/for=4406195 -> ~81 kHz low on both (D'b rest 4.25 MHz / D'r 4.40625 MHz expected)

### hacktv reference port (third_party/hacktv, byte-identical to upstream master)
- Constants: SECAM_FM_DEV 1e6, SECAM_FM_FREQ 4328125 (277 fH), CB 4250000 (272 fH), CR 4406250 (282 fH)
- ev_co = -1.902*280e3 (D'R), eu_co = +1.505*230e3 (D'B); lookup u/v = (d + rest - FM)/DEV
- Per line: D buffer (previous-line vertical average) -> fir_low_pass(15 taps, 1.7 MHz, kaiser 7)
  -> IIR video pre-emphasis {1,-0.90456054}/{2.90456054,-2.80912108} (=85/255 kHz horn, DC gain 1,
  HF x3, designed at 16 MHz -> redesigned for 40 MSPS) -> per-type deviation clamps
  (D'b [CB-350k, CB+506k], D'r [CR-506k, CR+350k]) -> FM at 4.328125 MHz with per-line phase
  reset (0/pi by global line %3) -> complex cloche gain 0.115*(1+j16u)/(1+j1.26u),
  u = f/f0 - f0/f, f0 = 4.286 MHz -> raised-cosine envelope window (burst_left 5.6 us,
  burst_width 56.9 us + 1 us IRT1090 rise = the WHOLE active line)
- Field ident: 9 lines after each field train (hacktv lines 7-15 / 320-328): rest + ramp,
  D'r +350 kHz over 15 us, D'b -350 kHz over 18 us, from active_left
- Luma: band-reject notch (51 taps) 3.328-5.328 MHz mixed 50% over the active area
- iir_int16_process semantics: y = x*b[0] + x_prev*b[1] - y_prev*a[1] (verified fir.c:742)

## Files touched this session
- misrc_tools/meson.build (CI fix, commit 5ef0d5d)
- misrc_tools/misrc_gui/signal/gui_testgen.h/.c (SECAM chain port, frame_index param)
- misrc_tools/misrc_gui/input/gui_simulated.c (two-frame stream for alternating cadence)
- temp harness: %TEMP%\testgen_check\testgen_raw.c (frame_index arg)

## SECAM port results (commit 8ddd6d9)
- Fixes applied after the first A/B: SECAM ev_co/eu_co are the Hz deviations
  (the old port double-scaled them with the PAL 0.877/0.493); removed hacktv's
  transmit-side vertical averaging (the real card transmits raw D per line -
  derived from the real bar values: cyan D'b = 0.75x standard with no D'r
  blend); envelope starts at the sync end with 0.3 us edges (a slow rise
  inside the decoder's porch window put 13 kHz MAD on the per-line zero and
  a false ~0.08 chroma on black); cadence set to the real card's drOnEven=1.
- A/B decode (panel path, sim vs real):
  fob 4249973 / for 4406223 vs real 4249945 / 4406195 (within 28 Hz)
  offset -27 Hz (real -55), noiseMad 57 Hz (real 321), drOnEven=1 (matches)
  black 0.000 (real 0.003)
  angles: 167.3/283.6/239.8/59.3/104.1/345.3 vs real
  167.4/284.3/241.0/59.7/103.4/346.5 (all within 1.5 deg)
  amps within +-17% (the real card's per-bar deviation is non-uniform
  0.70-0.93x standard; the uniform 0.75x chain gain is the best fit)
- Decoded bar RGB hues match the real decode on every bar.
- Validation: build-local.ps1 gate OK; meson test 44/45 (the single FAIL is
  the pre-existing update_cli dev-version test, blocked on user decision);
  gui_rf_secam + gui_rf_mesecam OK.
- CI: run 40844023101 (5ef0d5d, meson fix): Linux AppImage x86_64 + arm64
  and Android APK all SUCCESS (the original failures are fixed); the run's
  failure was only the macOS arm64 job CANCELLED mid-flight by the newer
  push. Run 40847441487 (8ddd6d9) queued for the full picture.

## Session close (user confirmed the SECAM fix good)
- User confirmed: "good fixed". Restore point created per the restore
  rule: `restore_points/2026-10-29_secam-fm-chain-good.zip` (gui_testgen.c/
  .h, gui_simulated.c, meson.build, this log).
- Rebased local main onto origin/main (6caded7, the TECHNICAL.md +
  prompt_logs tidy-up) with autostash; no conflicts.
- Signal-generator notes added to TECHNICAL.md ("Test Gen — simulated
  CVBS test signal generator (hacktv port)" section: slot model, chain
  calibrations, the SECAM FM chain + the three real-card differences,
  cadence contract, verification numbers); this log moved into
  prompt_logs/ with the date-prefixed name. Commit 98564f1 pushed.
- CI on the tip: run 40870316686 first completed failure with ONLY the
  "macOS APP build (universal)" job CANCELLED ~15 min in (infra flake:
  no timeout-minutes configured, the job's log blob was never retained);
  every platform build job succeeded. Rerun of the failed jobs
  (`gh run rerun --failed`) -> completed SUCCESS: all builds pass on
  every platform (Linux AppImage x86_64+arm64, Windows EXE x86_64+arm64,
  macOS APP x86_64+arm64+universal, Android APK, all guard tests).
