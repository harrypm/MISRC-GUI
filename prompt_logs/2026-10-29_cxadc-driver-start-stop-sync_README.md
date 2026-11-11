# Prompt Log — 2026-10-29 — CXADC Linux driver capture start/stop sync breakdown

## Prompt input

1. "I need a breakdown of the sync for capture start/stop for the CXADC
   linux implimentation"
2. "I want some clear tehcnical breakdowns in the tehcnical.md"
3. "I also want timing accurancy info added"

## Commands run (with outcomes)

1. `ls /home/harry` — located the kernel driver tree at `/home/harry/cxadc`
   (plus `cxadc-capture-server`, `cxadc-win`).
2. `ls -R /home/harry/cxadc | head -50; ls /home/harry/cxadc-capture-server`
   — confirmed the driver tree contents: `cxadc.c`, `cx88-reg.h`,
   `README.md`, `Tips-&-Notes.md`, `utils/`, build artifacts (`cxadc.ko`).
3. `grep` for `MO_PCI_INTMSK / MO_VID_INTMSK / MO_VID_INTSTAT /
   MO_VID_DMACNTRL / MO_DEV_CNTRL2 / MO_VBI_GPCNT / RISC_* / MO_DMA24_PTR2 /
   CHN24_CMDS` in `/home/harry/cxadc` — matched `cxadc.c` and `cx88-reg.h`
   (register offsets + RISC instruction bit definitions).
4. `grep` for `open/close/irq/interrupt/buffer/ring/wrap/zero/start/stop/sync`
   in `/home/harry/cxadc/Tips-&-Notes.md` — no matches (no documented
   start/stop notes there; source is the only authority).
5. `ls /home/harry/MISRC-GUI/*.md` + `ls -t prompt_logs | head` — found this
   repo's prompt-log naming convention (`YYYY-MM-DD_topic_README.md`).

## Files read (hard data this breakdown is based on)

- `/home/harry/cxadc/cxadc.c` — full driver source (1476 lines,
  cxadc-linux3 tree, v0.7-era). All line citations in TECHNICAL.md verified
  against this file.
- `/home/harry/cxadc/cx88-reg.h` — register offsets and RISC opcode bits
  (`RISC_IRQ1`, `RISC_CNT_INC/RESET`, `PCI_INT_VIDINT` etc.).
- `/home/harry/cxadc/README.md` — parameter semantics, "Re-opening the
  device will update the CX2388x's registers" (clock changes need re-open).
- `/home/harry/MISRC-GUI/TECHNICAL.md` — full pre-edit read (840 lines) to
  match style and place the new section before the existing GUI-side
  "Multi-stream capture start alignment" section.

## Findings (condensed)

Driver-side capture start/stop sync in cxadc:

- No userspace start/stop ioctl exists. The RISC/DMA engine starts once at
  `cxadc_probe` (`MO_DEV_CNTRL2` bit 5, `MO_VID_DMACNTRL` bits 7|3,
  cxadc.c:1044-1047) and free-runs for the module's lifetime.
- open() = reader sync: exclusive open, config re-applied, `lgpcnt = -1`,
  `MO_PCI_INTMSK = 1`, block on `readQ` until the first IRQ, then
  `initial_page = lgpcnt` anchor (cxadc.c:699-713).
- Producer grid: RISC program writes 2 KiB clusters, `RISC_CNT_INC` per page
  (GPCNT = page index), `RISC_CNT_RESET` at page 8191, `RISC_IRQ1` every 512
  pages = 2 MiB, JUMP loop (cxadc.c:560-599).
- IRQ handler: GPCNT rounded DOWN to the 512-page grid before publishing
  (GPCNT can lead in-flight data; a received IRQ guarantees residency up to
  its boundary) (cxadc.c:843-853).
- read(): position = pure function of fd offset + anchor; blocks on readQ
  when caught up; destructive zeroing after copy; silent lapping when the
  reader is too slow; no flow control (cxadc.c:730-796).
- close() = reader detach only (`MO_PCI_INTMSK = 0`, cxadc.c:722) — the
  engine keeps running. True stop is `disable_card()`
  (cxadc.c:480-491) via remove/shutdown/suspend; resume fully re-inits and
  GPCNT restarts at 0 (stale anchors require re-open).

Timing accuracy:

- Release granularity quantized to the 2 MiB grid: 73.2 ms stock 28.636 /
  58.6 ms @ 35.8 / 52.4 ms @ 40 / 38.8 ms @ 54 MHz; ring 2.34 / 1.87 /
  1.68 / 1.25 s.
- Newest readable data is 0..1 grid period behind the live edge.
- Wakeup latency never accumulates into drift (IRQ guarantees RAM residency
  before wakeup; late wakeup = backlog only).
- No timestamps: absolute accuracy = clock accuracy (stock crystal class
  spec ±20-50 ppm = 1.2-3 ms/min, 72-180 ms/h vs wall clock); sysfs
  `crystal` is PLL math input only, no ongoing correction.
- Driver arithmetic: PLL 20-bit fractional ≈ 0.68 Hz steps (stock);
  SCONV integer truncation — tenxfsc=1 `131072*4/5` = 104857 vs 104857.6
  (~5.7 ppm), generic formula up to ~10 ppm at 40 MHz scale.
- A/B: independent crystals drift at relative ppm; shared clockgen clock is
  the precondition for the GUI's byte-exact A/B sync-start.
- Exact boundary instants measurable via the GUI's 1-byte blocking read
  (tens of µs residual, near-identical per card).

## Output written

- `TECHNICAL.md` — new section "CXADC kernel driver — capture start/stop +
  stream sync (driver side)" inserted at line 439 (before "Multi-stream
  capture start alignment"), covering: sync-grid constants, no-userspace-
  start/stop, RISC producer, open() handshake, IRQ guaranteed-pages rule,
  read() steady-state sync, two-level stop semantics, suspend/resume, pace
  + timing accuracy, and how the GUI layers on top.
- This prompt log.

## Verification status

- All cxadc.c line references verified against the source on disk
  2026-10-29. All arithmetic recomputed from the source constants
  (2,097,152 B grid; 104857.6 -> 104857 = 5.7 ppm; (28,636,363/40)/2^20
  = 0.68 Hz; drift = ppm x span).
- Crystal ±20-50 ppm is a class-spec figure, NOT measured on this rig —
  flagged as such in the doc.
- Documentation only; no code changed, no hardware capture run performed in
  this prompt, so nothing here is runtime-validated (the GUI-side numbers
  quoted are from the 2026-10-02/2026-10-06 rig runs documented in the
  adjacent sections).
