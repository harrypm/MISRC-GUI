# Windows FLAC recording affinity

## Scope

Extend the existing FLAC core-pinning setting to Windows hosts with one
processor group. The setting applies to the A/B RF and RTL-SDR Hi-Fi RF
FLAC writers and their libFLAC compression workers.

The Windows path reuses the CPU list and range syntax, such as
`2-5,8`, and validates it against the process and system affinity masks.
The writer applies the group mask before encoding. The Windows GUI wraps
the static libFLAC `pthread_create` calls for that encoder so its workers
apply the same mask before entering libFLAC. The running GUI checks the
user's CPU topology and available CPU mask. Hosts with multiple processor
groups report that pinning is unavailable.

The existing configuration keys are `show_core_pinning_in_settings`,
`flac_affinity_enabled`, and `flac_affinity_cpu_list`. Pinning defaults to
off.

## Build and verification

Official Windows x64 and ARM64 builds set
`-Dwindows_flac_affinity=enabled`. They build and run the affinity test
before copying the executable for packaging. The test checks CPU list
validation, writer and worker masks on supported hosts, and a sample-exact
FLAC round trip.

Local and custom builds default to `-Dwindows_flac_affinity=disabled`.
FLAC recording and compression threading remain available with libFLAC.
The existing Settings row reports that pinning is unavailable in those
builds. Use `-Dwindows_flac_affinity=enabled` to include and test it locally.

Local Windows x64 Release builds passed with both settings and static
RTL-SDR. With pinning enabled, the affinity test passed through Meson.
With pinning disabled, the test was absent and a synthetic A/B FLAC
recording passed `flac --test` on both files. Local shell checks injected
configure, build, and test failures into both Windows workflow steps
and confirmed that each stopped before copying the executable.

Windows ARM64 CI and recording on capture hardware remain unverified.
The available evidence does not establish a CPU usage improvement.

Harry's sign-off on the Windows settings labels and messages is pending.
