# Tile trace evidence — frame 2200, driving in first-person

Captured 2026-11-15 ~13:48 PM PC time (14:48 CST). Seth driving in first-person
(cockpit view). Tile trace fired at presented frame 2200. 287 "FH1 tile draw"
lines, draw numbers 1419–1705. Full log: `runtime-tile-2026-11-15.log`.

Cvars on: `fh1_debug_tile_trace_swap=2200`, `gpu_trace_bins_vblank=2200`,
`pinyon_transition_log=true`, `pinyon_resolve_trace=true`, `fh1_swap_fetch_trace=true`.

---

## Tile draw distribution at frame 2200 (287 draws)

| window_y | viewport / scissor / claim | draws | meaning |
|---|---|---|---|
| -512 | vy=0 vh=208, sy=0 sh=208, claim 0..208 | 112 | **Band 3** (screen rows 512–720) |
| 0 | vy=0 vh=720, sy=0 sh=720, claim 0..720 | 89 | Full-frame draws (HUD/composite) |
| 0 | vy=0 vh=256, sy=0 sh=256, claim 0..256 | 59 | **Band 1** (screen rows 0–256) |
| 0 | vy=0 vh=8192, sy=0 sh=8192, claim 0..8192 | 6 | **8192-wide world pass** |
| 0 | vy=0 vh=32, claim 0..32 | 4 | small |
| 0 | vy=0 vh=192, claim 0..192 | 4 | |
| 0 | vy=0 vh=96, claim 0..96 | 3 | |
| 0 | vy=0 vh=360, claim 0..360 | 3 | |
| 0 | vy=0 vh=48, claim 0..48 | 2 | |
| 0 | vy=0 vh=24, claim 0..24 | 2 | |
| 0 | vy=360 vh=360, sy=360 sh=360, claim 360..720 | 1 | **bottom-half draw** (rows 360–720) |
| 0 | vy=0 vh=64, claim 0..64 | 1 | |
| 0 | vy=0 vh=12, claim 0..12 | 1 | |
| **-256** | — | **0** | **Band 2 (screen rows 256–512) — ABSENT** |

Every draw: `vtx_window_offset_enable=1`, `window_offset_disable=0`.

Representative lines:
```
FH1 tile draw: swap=2200 resolve_count=76252 draw=1419 window_y=-512 vtx_window_offset_enable=1 window_offset_disable=0 viewport_y=0 viewport_h=208 scissor_y=0 scissor_h=208 claim_y=0..208
FH1 tile draw: swap=2200 resolve_count=76252 draw=1452 window_y=-512 vtx_window_offset_enable=1 window_offset_disable=0 viewport_y=0 viewport_h=208 scissor_y=0 scissor_h=208 claim_y=0..208
```

---

## Findings

### 1. Band 2 (window_y=-256) has ZERO draws at frame 2200
This is the new, critical finding. Band 1 (wy=0) has 59 draws, Band 3 (wy=-512)
has 112 draws, but Band 2 (wy=-256, screen rows 256–512, the centre of the
screen) has **no draws at all**. The three 4x colour resolves still fire for
all three bands (dest bases stepped by 1,310,720 B), but Band 2's resolve reads
an EDRAM region that Band 2 never drew into, so its dest region gets zeros /
stale content. The middle band of the screen is black because Band 2 drew
nothing there — not (only) because the resolve reads the wrong rows.

Open question for the fix: is Band 2 empty because of per-band culling at this
frame/view, or because its bin predicate (select 0xC) is not being honoured on
D3D11? Per-band predication is enforced in the shared command processor, so a
culling/predicate cause needs to be distinguished from a genuinely empty frustum.

### 2. window_offset_disable is NEVER set
`window_offset_disable=0` on all 287 draws, `vtx_window_offset_enable=1` on
all 287. The window offset is always applied. Rank 8 (window_offset_disable
making draws vanish) is **ruled out**.

### 3. The 8192-wide world pass is PRESENT during driving
6 draws with `viewport_h=8192`, `scissor_h=8192`, `claim_y=0..8192` at frame
2200, while Seth is driving in free-roam. ChatGPT item #5 answered: the
8192-wide world-rendering pass does **not** disappear during free-roam. It
continues. The "pass disappears" hypothesis is ruled out.

### 4. A bottom-half draw exists (y=360 split)
One draw has `viewport_y=360 viewport_h=360 scissor_y=360 scissor_h=360
claim_y=360..720` — a draw targeting exactly rows 360–720. This matches the
earlier observed hard split at y=360 in every frame. Likely a composite pass.

### 5. Bins trace produced no output
`gpu_trace_bins_vblank=2200` produced zero matching lines in runtime.log. It
may log to a different sink or need a different trigger. The tile trace alone
answers the mapping question, so the bins trace is not blocking.

---

## Mapping table (ChatGPT's milestone — now filled in)

| Band | Expected source rows | Actual source rows (resolve) | Destination | Draws at frame 2200 |
|---|---|---|---|---|
| 1 | 0–256 | 0–256 | Base | 59 (wy=0, vh=256) |
| 2 | 256–512 | 0–256 (same rect as band 1) | Base + 1,310,720 | **0 (wy=-256 absent)** |
| 3 | 512–720 | 0–208 | Base + 2,621,440 | 112 (wy=-512, vh=208) |

"Expected source rows" = the screen rows that band is responsible for.
"Actual source rows" = the EDRAM rows the resolve reads for that band
(from the resolve-key lines: bands 1–2 read rect 0,0-1280,256; band 3 reads
0,0-1280,208, all from the same source surface).

The defect is now two-fold, not one-fold:
- **(a) All bands resolve the same EDRAM rows** (0–256 / 0–208) instead of
  their own rows — the missing untile separation (Rank 1).
- **(b) Band 2 draws nothing at frame 2200** — so even if the resolve read
  the right rows, there would be no Band 2 content to show. The middle band
  is black for this reason as well.

A correct D3D11 untile must do both: separate the bands' EDRAM rows (so they
stop overwriting each other) AND ensure Band 2's draws actually land
(i.e. the band-2 bin predicate / draw path must produce content, not just a
resolve of an empty region).

---

## Corroborating evidence from this run
- Swap trace: last swap 2328, `guest_words_nz=28510`, fetch base alternates
  1C149000 / 1BDB1000, last-resolve range matches fetch base (no address
  disconnect — re-confirmed).
- Resolve trace (38,731 bytes): the three large resolves reproduced with the
  same hashes and the same 4x mirror (9,000 nonzero bytes).
- First-person (cockpit) view renders in the band: steering wheel, dashboard,
  gauges and the 1.6 MI waypoint are visible in the single band that resolves
  — the draws DO produce content; it is the band mapping that confines it.
