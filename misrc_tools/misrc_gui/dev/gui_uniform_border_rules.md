# DEV NOTE (HARD): Clay container borders — visible panels derive their width from content, never a fixed constant

Status: incident-driven (2026-11-03, the toolbar AudioLevelBars panel: the
contrasting box around the four audio level meters had 16 px of dead space
on its right border at full width; the user read it instantly as
"bordering is not uniform"). Read this before adding or editing any Clay
container whose background or border is VISIBLE (contrasting panels,
meter boxes, grouped control backgrounds).

## The incident (why this note exists)

The toolbar's AudioLevelBars panel (backgroundColor {25,25,30} against the
toolbar) was sized `CLAY_SIZING_FIXED(bars_panel_width)` from a toolbar
profile constant (240 at the full tier). Its content is fixed: 4 meter
columns + 3 child gaps + 2x4 px padding. Two independent mistakes put
dead space on the RIGHT border:

1. The FULL profile was missing `.meter_gap` — the profile struct is
   memset to 0 first, so the gap silently became 0 (the other tiers set
   4). Content: 216 px inside a 240 px panel.
2. The panel width was a hand-picked round constant, not derived from the
   content, so even with the gap fixed the wider tiers were 4 px over.

Clay left-aligns children by default, so ALL the slack landed on the
right border: left border 4 px, right border 20 px. NARROW/VERY_NARROW
tiers had the same 4 px drift; ULTRA/TINY happened to match.

## HARD RULES

1. A container whose background/border is visible SIZES TO ITS CONTENT:
   derive `padding*2 + sum(child widths) + gap*(children-1)` at the use
   site. A fixed constant drifts the moment any child width changes —
   the exact bug class this note forbids.
2. Profile/struct tiers built with memset + designated initializers have
   NO safe defaults: a field missing from one tier silently becomes 0.
   Every layout-affecting field must be listed in EVERY tier (or the
   value must be derived, per rule 1, so the constant does not exist).
3. The tier-fit / required-width calculation must use the SAME derived
   formula as the render path — lock-step, ideally one shared expression
   — otherwise the fit test lies about what actually renders.
4. Left-aligned children put all container slack on the right side. With
   rule 1 there is no slack to distribute; if a container deliberately
   keeps slack, distribute it (center or space-between), never let it
   pool on one border.
5. A uniform-border fix is only "cleanly fixed" when checked at more than
   one size tier (the incident was invisible at ULTRA/TINY widths and
   obvious at FULL).

## Contracts in force

- `misrc_tools/misrc_gui/ui/gui_ui.c`: the AudioLevelBars width is
  `8 + meter_gap*3 + meter_col_width*4` in THREE lock-step sites —
  `render_toolbar`, `gui_ui_toolbar_required_width`, and the two-row
  overflow path. Editing the meter column/gap constants requires no other
  change; editing the FORMULA requires all three (plus this note).
- `gui_ui_toolbar_profile_t.bars_panel_width` is a historical constant,
  NOT the rendered width (see the comment on the field).

## Applies to future work

- Any new grouped-control background in the toolbar, status bar, sidebars
  or overlays: derive the container from its children.
- Any new toolbar/layout profile tier: list every layout field explicitly;
  grep the struct for fields the other tiers set.
