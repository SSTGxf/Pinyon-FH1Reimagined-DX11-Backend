# Phase 2e — EDRAM readback RESULTS: the EDRAM DOES get coloured

**Date:** 2026-11-17 (afternoon CST; PC clock ~1 h behind CST)
**Run:** D11 edram3d run 2 (PID 11284, Phase 2e build, probe from swap 3000, budget 12)
**Verdict:** The probe FIRED — all 12 draws, all on the EDRAM surface (`0/32/4x/c3`). **The EDRAM DOES get coloured after the EDRAM triangles draw — it does NOT stay flat grey.** This is the decisive answer Phase 2e was built to give. The flat-grey world on screen is NOT because the triangles fail to write to EDRAM; the loss is downstream of the EDRAM write (resolve or composite).

---

## 0. Game state — entered free roam, probe fired

- Seth pressed Start on the PC (~17:06 CST). The game (PID 11284, Phase 2e build) entered free roam: `music.in_game` 1, `save.backup` 2, memory 842,316 K (checked ~17:25 CST). jsonl `20261117T222813Z-p11284.jsonl` (410 lines).
- The probe fired at 16:23:46–16:23:48 PM PC (17:23–17:25 CST), thread t25948, swaps 54190–54191 — on the first 12 EDRAM draws after the game entered free roam. 24 lines in `runtime.1.log` (12 control + 12 draw). All 12 probes are on `key=0/32/4x/c3`, `rt=1`, `samples=4` (4x MSAA confirmed).
- The game was left running (PID 11284); nothing was killed. Seth can close it when he's done.

## 1. The 12 probes at a glance

| # | swap | band | ps_hash | texmask | verts | prim | before_mean | changed | after_mean | after_distinct |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 54190 | 0 | CAE25D74AD7B16CB | 00000001 | 9300 | 4 | 0.0750,0.0750,0.0750 | **0.062878** | 0.1256,0.1088,0.1088 | 84 |
| 2 | 54190 | 0 | 90CAB86BE8159DA8 | 00000003 | 8700 | 4 | 0.1256,0.1088,0.1088 | 0.000000 | 0.1256,0.1088,0.1088 | 84 |
| 3 | 54190 | 0 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 4 | 0.1256,0.1088,0.1088 | 0.000000 | 0.1256,0.1088,0.1088 | 84 |
| 4 | 54190 | 1 | CAE25D74AD7B16CB | 00000001 | 9300 | 4 | 0.1674,0.1674,0.1674 | **0.180641** | 0.2959,0.3180,0.3251 | 189 |
| 5 | 54190 | 1 | 90CAB86BE8159DA8 | 00000003 | 8700 | 4 | 0.2959,0.3180,0.3251 | **0.010087** | 0.2969,0.3185,0.3250 | 991 |
| 6 | 54190 | 1 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 4 | 0.2969,0.3185,0.3250 | **0.015349** | 0.2962,0.3172,0.3234 | 1451 |
| 7 | 54190 | 2 | CAE25D74AD7B16CB | 00000001 | 9300 | 4 | **0.2598,0.2598,0.2598** | **0.288889** | 0.2945,0.3405,0.3635 | 159 |
| 8 | 54190 | 2 | 90CAB86BE8159DA8 | 00000003 | 8700 | 4 | 0.2945,0.3405,0.3635 | **0.028472** | 0.2973,0.3424,0.3640 | 1233 |
| 9 | 54190 | 2 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 4 | 0.2973,0.3424,0.3640 | **0.095037** | 0.3006,0.3404,0.3574 | 2545 |
| 10 | 54191 | 0 | CAE25D74AD7B16CB | 00000001 | 9300 | 4 | 0.1671,0.1671,0.1671 | **0.062882** | 0.2014,0.1846,0.1846 | 84 |
| 11 | 54191 | 0 | 90CAB86BE8159DA8 | 00000003 | 8700 | 4 | 0.2014,0.1846,0.1846 | 0.000000 | 0.2014,0.1846,0.1846 | 84 |
| 12 | 54191 | 0 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 4 | 0.2014,0.1846,0.1846 | 0.000000 | 0.2014,0.1846,0.1846 | 84 |

Notes:
- **All 12 probes are on the EDRAM surface** (`0/32/4x/c3`, pitch 32, 4x MSAA) — the Phase 2e family check worked exactly as intended. No composite draws in the budget.
- **All 12 draws are textured** (`texmask` 1, 3, or 7 — all nonzero). The world draws are NOT untextured, so the flat grey is not because the draws lack textures.
- **All 12 draws are large world-geometry triangles** (8,700–9,300 verts, prim 4 TriangleList), not fullscreen quads.
- Three shaders cycle in the same order for every band: `CAE25D74AD7B16CB` (texmask 1, 9300 verts), `90CAB86BE8159DA8` (texmask 3, 8700 verts), `FD9976C9E5DB9BB0` (texmask 7, 9300 verts) — the three passes of each band.
- 8 of 12 probes changed the surface (wrote content); 4 wrote nothing (probes #2, #3, #11, #12 — the 2nd/3rd passes of band 0, where pass 0 had already written the content).
- The three bands are confirmed by the changed-region bboxes: band 0 rows 1–255 (#1, #10), band 1 rows 372–511 (#4), band 2 rows 512–719 (#7).

## 2. Per-probe sample pixels

The 5 sample pixels per probe (centre, tl, tr, bl, br) — the before/after colour at fixed points:

| # | before pixels (centre/tl/tr/bl/br) | after pixels (centre/tl/tr/bl/br) |
|---|---|---|
| 1 | (0,0,0) / (0,0,0) / (0,0,0) / (66,66,66) / (66,66,66) | (0,0,0) / (0,0,0) / (0,0,0) / (66,66,66) / (66,66,66) |
| 2 | (0,0,0) / (0,0,0) / (0,0,0) / (66,66,66) / (66,66,66) | same as before |
| 3 | (0,0,0) / (0,0,0) / (0,0,0) / (66,66,66) / (66,66,66) | same as before |
| 4 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) |
| 5 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | same as before |
| 6 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | same as before |
| 7 | **(66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66)** | (66,66,66) / (66,66,66) / (66,66,66) / **(85,124,146)** / **(85,124,146)** |
| 8 | (66,66,66) / (66,66,66) / (66,66,66) / (85,124,146) / (85,124,146) | (66,66,66) / (66,66,66) / (66,66,66) / (85,124,146) / **(93,130,148)** |
| 9 | (66,66,66) / (66,66,66) / (66,66,66) / (85,124,146) / (93,130,148) | same as before |
| 10 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) |
| 11 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | same as before |
| 12 | (0,0,0) / (66,66,66) / (66,66,66) / (66,66,66) / (66,66,66) | same as before |

Probe #7 is the cleanest before/after: all five sample pixels were (66,66,66) before, and after the draw the bottom two are (85,124,146) — a blue-grey (sky/water) colour. The top three stayed (66,66,66) because the draw's changed region (bbox 0,512–1279,719) covers the bottom of the surface, and the sample points centre/tl/tr sit above it.

## 3. The verdict — EDRAM DOES get coloured

**The EDRAM surface changes from uniform 66,66,66 after the EDRAM triangles draw. It does NOT stay flat grey.**

The decisive evidence is probe #7 (band 2, pass 0, swap 54190):
- **Before:** perfectly uniform 66,66,66 — `distinct=1/1` (a single distinct value), all five sample pixels (66,66,66), `before_mean=0.259766,0.259766,0.259766` (≈ 66/255 = 0.2588).
- **The draw:** a 9,300-vert TriangleList, `texmask=00000001` (textured), `ps_hash=CAE25D74AD7B16CB`, scissor covering the bottom band.
- **After:** `changed=0.288889` (28.9% of pixels changed), bbox the full bottom band (0,512–1279,719), `after_distinct=159`, `after_mean=0.294467,0.340546,0.363535` (B channel 0.364, well above grey 0.259), bottom sample pixels (85,124,146) — real sky-blue colour.

Corroboration from the other bands:
- **Probe #4** (band 1, pass 0): `changed=0.180641` (18.1%), `after_distinct=189` (from 2), `after_mean=0.295873,0.317959,0.325099` — the mean nearly doubled from 0.1674.
- **Probe #1** (band 0, pass 0): `changed=0.062878` (6.3%), `after_distinct=84` (from 2).
- **Probes #5, #6, #8, #9** (passes 1–2 of bands 1–2): smaller changes (1.0–9.5%) but the distinct counts keep growing across passes (band 1: 189→991→1451; band 2: 159→1233→2545) — each pass adds finer detail, so the content is real and accumulating, not noise.
- **Probe #10** (swap 54191, band 0, pass 0): `changed=0.062882`, the same 6.3% as probe #1 in the previous frame — the band-0 write is reproducible frame to frame.

So the triangle draws write real, varied, coloured, textured content into EDRAM. The "flat grey world" (RGB 66,66,66) that appears on screen is **not** because the EDRAM stays grey after the triangles draw.

### What this rules out and what it narrows to

- **REFUTED:** "the world triangles fail to reach/write to EDRAM" (the Phase 2b correction already showed they reach the GPU; this probe now shows they write coloured content into EDRAM).
- **REFUTED:** "the world draws are untextured, so they output flat grey" (all 12 draws have nonzero texmask).
- **REMAINS:** the loss is **downstream of the EDRAM write** — in the resolve (EDRAM → resolve target) or the composite (resolve → present). The EDRAM has content, but the presented frame is still flat grey, so the content disappears somewhere between the EDRAM write and the screen.
- **Tension to note:** the earlier resolve-source probe (swap 1500, a composite present) showed the resolve source as uniform 66,66,66 at every sample. That was a different frame/present, and the resolve source may have been read at a time when the EDRAM was genuinely grey (e.g. before the world draws, or for a different band). It does not contradict this probe — but it means the next step must pin down whether the coloured EDRAM content survives the resolve. The cleanest next probe: read the resolve output (or the resolve source at the same moment as an edram3d probe) and compare it to the EDRAM content the probe just measured.

## 4. Full probe lines (all 24)

Saved at `~/workspace/dx11-edram3d-2e-scratch/edram3d-2e-lines.txt`. Pasted here verbatim (file prefix is the rotated log name):

```
runtime.1.log: [2026-11-17 16:23:46.575] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#1 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x720 before_mean=0.0750434,0.0750434,0.0750434 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:46.738] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#1 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.062878 bbox=827,1-1279,255 cols=00000000000000011223 rows=012200000000 after_distinct=84 after_mean=0.12563,0.10881,0.10881 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.030] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#2 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x720 before_mean=0.12563,0.10881,0.10881 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.074] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#2 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.12563,0.10881,0.10881 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.131] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#3 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x720 before_mean=0.12563,0.10881,0.10881 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.380] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#3 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.12563,0.10881,0.10881 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.704] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#4 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x464 before_mean=0.167405,0.167405,0.167405 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.719] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#4 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.180641 bbox=0,372-1279,511 cols=11111111111111111111 rows=000000695000 after_distinct=189 after_mean=0.295873,0.317959,0.325099 after=1.00 1.00 1.00 1.00 0.27 0.00 0.63 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.744] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#5 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=189/189 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x464 before_mean=0.295873,0.317959,0.325099 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.758] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#5 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.010087 bbox=0,433-1279,511 cols=00000000000000000000 rows=000000000000 after_distinct=991 after_mean=0.296875,0.318462,0.32499 after=1.00 1.00 1.00 1.00 0.27 0.00 0.63 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.783] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#6 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=991/991 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x464 before_mean=0.296875,0.318462,0.32499 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.798] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#6 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.015349 bbox=544,431-1273,511 cols=00000000000000000000 rows=000000001000 after_distinct=1451 after_mean=0.296168,0.317193,0.323446 after=1.00 1.00 1.00 1.00 0.27 0.00 0.63 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.823] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#7 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x208 before_mean=0.259766,0.259766,0.259766 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:47.838] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#7 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=9300/9300 changed=0.288889 bbox=0,512-1279,719 cols=22222222222222222222 rows=000000004999 after_distinct=159 after_mean=0.294467,0.340546,0.363535 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(85,124,146) br=(85,124,146)
runtime.1.log: [2026-11-17 16:23:47.862] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#8 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=159/159 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x208 before_mean=0.294467,0.340546,0.363535 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(85,124,146) br=(85,124,146)
runtime.1.log: [2026-11-17 16:23:47.878] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#8 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=8700/8700 changed=0.028472 bbox=0,512-1279,719 cols=00000000000000000000 rows=000000000110 after_distinct=1233 after_mean=0.297276,0.342404,0.363988 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(85,124,146) br=(93,130,148)
runtime.1.log: [2026-11-17 16:23:47.902] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54190 probe#9 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=1233/1233 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x208 before_mean=0.297276,0.342404,0.363988 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(85,124,146) br=(93,130,148)
runtime.1.log: [2026-11-17 16:23:47.916] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54190 probe#9 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=9300/9300 changed=0.095037 bbox=213,512-1279,719 cols=00000000001121122211 rows=000000001432 after_distinct=2545 after_mean=0.300571,0.340384,0.357379 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(85,124,146) br=(93,130,148)
runtime.1.log: [2026-11-17 16:23:48.037] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54191 probe#10 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x720 before_mean=0.167057,0.167057,0.167057 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:48.052] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54191 probe#10 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.062882 bbox=827,1-1279,255 cols=00000000000000011223 rows=012200000000 after_distinct=84 after_mean=0.201431,0.18461,0.18461 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:48.076] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54191 probe#11 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x720 before_mean=0.201431,0.18461,0.18461 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:48.091] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54191 probe#11 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.201431,0.18461,0.18461 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:48.116] [info] [gpu] [t25948] FH1 edram3d probe control: swap=54191 probe#12 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x720 before_mean=0.201431,0.18461,0.18461 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.1.log: [2026-11-17 16:23:48.131] [info] [gpu] [t25948] FH1 edram3d probe draw: swap=54191 probe#12 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.201431,0.18461,0.18461 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
```

## 5. Deliverables and state

- **This report:** `~/workspace/dx11-edram3d-2e-results-2026-11-17.md`
- **Probe lines (all 24):** `~/workspace/dx11-edram3d-2e-scratch/edram3d-2e-lines.txt`
- **Collect script:** `~/workspace/dx11-edram3d-2e-scratch/collect-edram3d-2e-lines.ps1`
- **PPMs:** NOT fetched — the log lines already contain the before/after pixel data (means + 5 sample pixels per probe), so the PPMs are redundant for the verdict. Paths on PC: `E:\PinyonShift\work\.local\compdump\edram3d-3000-{1,2,3}-{before,control,after}.ppm` (note: these are from the Phase 2d run's probe numbering; the Phase 2e PPMs, if written, are in the same directory — the log lines are the authoritative record).
- **Game:** still running (PID 11284, Phase 2e build), in free roam. Nothing was killed. Seth can close it when he's done.
- **Phase 2e probe design:** the family check (`pitch_tiles == 32 && msaa == k4X`) worked — all 12 probes are on the EDRAM surface, none on composite. The slot scan + dropped texture-mask gate are intact. The probe is a working EDRAM readback tool for future runs.
- **Next step (not done here):** probe the resolve output (or the resolve source at the same moment as an edram3d probe) to find where the coloured EDRAM content disappears between the EDRAM write and the presented frame. The WaitForGpu fix remains queued, untouched.
