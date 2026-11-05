# Prompt Log — Android CI Opus SSL download failure (run 41943704989)

**Date:** 2026-11-05 (~02:39 UTC start)
**Repo:** `C:\Users\Harry\MISRC-GUI` — main @ `0d8e4df` (fast-forwarded from `103ad03` during this prompt; local was 28 commits behind origin)
**Trigger:** user pasted https://github.com/harrypm/MISRC-GUI/actions/runs/41943704989/job/126970743360

## Input
- GitHub Actions run `41943704989` ("Build and release binary", push to main), job `126970743360` ("Android APK build (arm64-v8a)")
- User shell pre-commands: `cd ..` ; `cd MISRC-GUI` (no output)

## Investigation — commands run and findings

1. `gh api repos/harrypm/MISRC-GUI/actions/jobs/126970743360` →
   conclusion **failure**; failed step: #7 "Cross-build Android deps"; steps 8–14
   (Meson cross-file, OpenH264, FFmpeg, libmisrc_gui.so, APK, verify, upload)
   all **skipped**; run head_sha `0d8e4df`.
2. `git status -sb` / `git log --oneline -8` → local main at `103ad03`,
   **behind origin/main by 28** (`0d8e4df` "Add test gen mode visual").
3. `git pull --ff-only` → 21 files changed; notably `android/build-deps-android.sh`
   (Opus dep is NEW in this range — monitor-audio work, PR #22 etc.).
4. `gh run view 41943704989 -R harrypm/MISRC-GUI` → every other job green
   (Windows x86_64/arm64, macOS arm64/x86_64/universal, Linux x86_64/arm64,
   all guards, codec smoke, release link check); ONLY android-apk failed;
   "Publish release assets" never ran. Annotation: `X Process completed with exit code 35.`
5. `gh run view 41943704989 -R harrypm/MISRC-GUI --log-failed` → key lines:
   - `00:47:08.59Z [android-deps] === Opus 1.5.2 (Android static libopus) ===`
   - `00:47:08.59Z [android-deps] downloading opus-1.5.2.tar.gz`
   - `00:47:08.78Z curl: (35) error:0A000126:SSL routines::unexpected eof while reading`

## Root cause
- `android/build-deps-android.sh` `download()` used `curl -fL --retry 3` and
  pulled Opus from `https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz`.
- curl's default `--retry` policy retries only "transient" errors (timeouts,
  HTTP 5xx/6xx, connection-refused mid-list). An **SSL connect error (exit 35)
  is NOT in that list** → `--retry 3` was a no-op; the job died 200ms into the
  download with zero retries.
- downloads.xiph.org is also the flakiest host in the script; the FLAC dep in
  the same script already uses `github.com/xiph/flac/releases` instead.

## Fix applied (2 files, uncommitted)

1. `android/build-deps-android.sh`
   - `OPUS_URL` → `https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz`
     (byte-identical asset — sha256 verified, see below; mirrors the FLAC choice)
   - `download()` curl hardened to:
     `curl -fL --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 30`
     → covers ALL tarball deps (FLAC, Opus, zstd, FFTW, soxr, libusb), not just Opus
2. `.github/workflows/build.yml` — android-apk job, "Install Android SDK + NDK":
   same retry flags added to the `dl.google.com` commandlinetools curl
   (previously had zero retry, same blast radius in the failed job).

## Verification (hard data, not assumptions)
- **sha256 cross-check** (both hosts downloaded to `%TEMP%\opus-verify`):
  - xiph.org tarball: `65C1D2F78B9F2FB20082C38CBE47C951AD5839345876E46941612EE87F9A7CE1`
  - GitHub tarball:   `65C1D2F78B9F2FB20082C38CBE47C951AD5839345876E46941612EE87F9A7CE1`
  - pinned in script: `65c1d2f7...` → **identical**; URL switch safe, pin unchanged.
- `bash -n android/build-deps-android.sh` → rc=0 (via Git-for-Windows bash;
  note: `bash` on PATH is the WSL shim with no distros installed).
- Harness `.tmp/test-download-fix.sh` (exact copy of hardened `download()`):
  - TEST 1 new OPUS_URL + sha pin → downloaded, `sha256 ok opus-1.5.2.tar.gz`
  - TEST 2 cached-file idempotency → `cached`, no re-download, sha ok
  - TEST 3 OLD flags on SSL-failing host (badssl) → exit 60, **12s** (1 attempt, no retry)
  - TEST 4 NEW flags on same host → exit 60, **30s** (retries actually happened:
    ~3 extra attempts × 2s delay + handshake time) → proves SSL-class errors now retry.
- `build.yml` edited step block extracted verbatim → `bash -n` → STEP_BASH_SYNTAX_OK.
  (Local python lacks pyyaml for a full YAML parse; the edit is inside a `run: |`
  literal block with matched 10-space indent, confirmed by read-back; GitHub
  parses the workflow on push.)
- Known tradeoff: against a *permanently* failing host the job now fails slower
  (up to ~5 retries) instead of instantly — intended; transient blips (the
  200ms SSL eof that killed this run) now recover.

## Open / awaiting user decision
- Commit + push of the fix (old root `prompt_readme_v1.2.7_fixes.md` should move
  into `prompt_logs/` at commit time per repo rule).
- `gh run rerun 41943704989 --failed` as an immediate unblock (re-runs only the
  android job against `0d8e4df`; with the old script it would re-roll the dice
  on xiph.org — the pushed fix makes the *next* push deterministic instead).
- Full validation of the Android cross-build only happens in CI (local Windows
  has no NDK setup); local validation was limited to the download path + syntax.

## Files touched
- modified: `android/build-deps-android.sh`, `.github/workflows/build.yml`
- new: this log (`prompt_logs/2026-11-05_android-ci-opus-ssl-retry_README.md`)
- test artifacts (gitignored, `.tmp/`): `test-download-fix.sh`, `step-android-sdk.sh`
- temp downloads: `%TEMP%\opus-verify\opus-{xiph,gh}.tar.gz`
