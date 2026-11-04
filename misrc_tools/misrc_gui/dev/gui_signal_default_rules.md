# DEV NOTE (HARD): Mode-scoped default configs — device defaults never leak across modes

Status: user-mandated project rule (2026-11-03, user-verified the same day).
Read this before adding ANY per-device or per-mode default configuration
(trigger settings, signal mappings, scale modes, decoder presets), and
before closing a change that adds one.

## Why this note exists

The Test Signal generator's channel mapping (CH1 = CVBS with the CVBS
H-sync trigger ENABLED, CH2 = FM RF with a rising-edge trigger ENABLED)
was assigned by `waveform_apply_channel_signal_default` on every panel's
first render/click UNCONDITIONALLY — the selected device was never
checked. Record and playback sessions therefore opened their waveform
panels with the generator's stock trigger configs applied over from test
signal mode: trigger lines hunting CVBS sync on RF data, non-stock
trigger levels, without the user ever selecting anything. The user
verified the leak and its removal in the real GUI on 2026-11-03.

## HARD RULES

1. A default configuration that belongs to a source/mode applies ONLY
   while that source/mode is active. The Test-Gen signal + trigger
   defaults are gated by `waveform_test_mode_active(app)` and are
   assigned nowhere else. Record/playback panels default to the STOCK
   trigger config: trigger OFF, rising edge, level 0, plain `CH A` /
   `CH B` labels.
2. Leaving a mode clears that mode's applied defaults — unless the user
   picked the config themselves. User selections are tracked
   (`signal_user_picked`) and are NEVER overwritten by defaults, mode
   switches, session restarts or device changes. A default that cannot
   distinguish "applied by the mode" from "chosen by the user" is
   incomplete work.
3. Every new user-interactable default follows the same shape: gate the
   assignment on the owning mode, clear on exit (respecting user picks),
   and record the mode scope in the same change. This mirrors the
   automation-flags rule: element + its contract land together.
4. The stock/base trigger config is the single source of truth for
   non-test panels: `trigger_enabled = false`, `TRIGGER_MODE_RISING`,
   `trigger_level = 0`. Nothing may rebase that stock config on another
   mode's values.

## Contracts in force

- Test-Gen mapping assignment: `waveform_apply_channel_signal_default`
  (test-mode gated) + `waveform_clear_generator_signal` (clear-on-exit;
  skips user picks) in `gui_oscilloscope.c`.
- Simulated-start hook: `gui_waveform_apply_generator_defaults` fires
  only from the simulated device's capture start (already mode-scoped).
- Restore point of the verified state:
  `restore_points/2026-11-03_player-display-trigger-defaults-good.zip`.
