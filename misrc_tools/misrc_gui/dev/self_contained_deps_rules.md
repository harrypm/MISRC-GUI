# DEV NOTE (HARD): All build dependencies must be self-contained — no silent downloads, no unreviewed dep changes

Status: user-mandated project rule (2026-11-08, harrypm). Read this before
adding ANY build dependency, touching the vendored dependency trees, or
editing any `scripts/build-*` / deps preflight script.

## Why this note exists

The pulled Monitor video work made `opus` + `openh264` hard GUI deps.
Local builds then failed on machines without the distro dev packages
(`libopus-dev` / `libopenh264-dev`), and an agent edit tried to make the
build script clone the missing libraries from the network at build time.
Both directions are wrong for this project:

- Depending on distro packages for hard GUI deps makes builds drift from CI
  and from each other (Ubuntu 22.04 ships FLAC 1.3 vs the required 1.5.0).
- Letting a build script fetch new code on its own means the supply chain
  can change with NO human review — "things changing without hard human
  intervention".

## HARD RULES

1. **ALL build dependencies must be self-contained from the repository.**
   Every hard build dependency lives as a committed, pinned source tree
   under `third_party/` (with a `VENDOR.md`: upstream URL, tag, date,
   why, what was stripped) — currently `hsdaoh`, `opus`, `openh264`
   (+ raylib cloned to a pinned tag by the existing flow until it moves
   in-repo). System packages (fftw3/flac/libusb/soxr/libuvc/zstd) are
   allowed only for the set CI also installs as versions, never as a way
   to introduce a NEW hard dependency.
2. **No build-time downloads for dependency sources.** A build must never
   clone, curl, or fetch NEW dependency code at build time. Everything
   needed is committed in the repo first. (Existing pinned refresh flows
   like raylib's `git fetch --branch <tag>` are grandfathered; new deps
   must NOT use this pattern.)
3. **Dependency changes require HARD human intervention.** Adding,
   removing, re-pinning, or updating ANY vendored dependency (or changing
   any `scripts/build-*` deps flow) happens only in a reviewed,
   human-approved commit by harrypm (Harry Munday) — never as a drive-by
   of an agent, a build fix, or an unrelated feature PR. The approval is
   recorded in the session's prompt log.
4. **Agents must not "fix" missing dependencies at build time** — by
   downloading, sudo-installing, or substituting versions. If a dep is
   missing, report it and stop; the human decides (install the distro
   package, or land a reviewed vendoring change).
5. Preflight scripts (`check-build-deps.sh`) stay check-only by default:
   they report and exit, they never install.

## Current self-contained set

| Dependency | Home | Pin | Built by |
|---|---|---|---|
| hsdaoh | `third_party/hsdaoh` | committed tree | `scripts/build-deps-unix.sh` |
| raylib | pinned tag 5.5 | `RAYLIB_TAG` | `scripts/build-deps-unix.sh` (grandfathered fetch) |
| opus | `third_party/opus` | tag v1.5.2 | `scripts/build-deps-unix.sh` |
| openh264 | `third_party/openh264` | tag v2.6.0 | `scripts/build-deps-unix.sh` |

## Contracts in force

- `scripts/build-deps-unix.sh` builds the vendored set from
  `third_party/` into `.deps/install` (stamp-gated, offline).
- `scripts/check-build-deps.sh` verifies; on Linux the `--system` mode
  checks only the distro-provided set so missing vendored codecs cannot
  abort the deps build.
- `android/build-openh264-android.sh` keeps its own cross-build flow;
  same pin (v2.6.0).
