# Phase 2f follow-up — edram3d fired, resolve probe still at 0: why

**Date:** 2026-11-17 (evening CST; PC clock ~1 h behind CST)
**Run:** D11 resolve-probe run 1 (PID 9712, Phase 2f build)
**Verdict:** The edram3d probe fired all 12 and the Phase 2e result **reproduces** — EDRAM gets coloured in this run too. The resolve probe is still at **0 lines**, and the reason is now identified: its gate requires `untile_band == 2`, but `untile_band` (derived from the guest's `PA_SC_WINDOW_OFFSET.window_y_offset`) is not 2 at resolve time even when the resolve rect is band-2-sized. The probe uses the wrong signal to identify band-2 resolves. Separately, the existing store probe shows the mirror was **never updated in any of the 160 resolves it measured** — but its budget was exhausted during loading, so it has no free-roam data.

---

## 0. Game state — entered free roam, ran ~45 min, now closed

- Game PID 9712 (Phase 2f build) **entered free roam**: `music.in_game` 1, `save.backup` **17**, jsonl `20261117T234635Z-p9712.jsonl` 270 lines, memory ~804 MB at entry.
- **The game is now CLOSED** (Seth closed it; `tasklist` shows no `pinyon_shift.exe`). `runtime.log` is therefore **unlocked** and readable (1,509,934 bytes, last written 5:19:58 PM PC). The rotated logs span 4:49:23 PM (runtime.20.log) to 5:19:24 PM (runtime.1.log).
- The edram3d probe fired at 17:02:51–17:02:52 PM PC (swaps 13003–13004), ~13 min after the store probe's burst (see §3).

## 1. edram3d reproduces — EDRAM gets coloured in this run too

All 24 lines in `runtime.8.log`, thread t25052, swaps 13003–13004. All on key `0/32/4x/c3`, rt=1, samples=4, all textured (texmask 1/3/7), all large world triangles (8,700–9,300 verts, prim 4). Same three shaders cycling in the same order (CAE25D74AD7B16CB / 90CAB86BE8159DA8 / FD9976C9E5DB9BB0).

| # | swap | band | ps_hash | texmask | verts | before_mean | changed | after_mean | after_distinct |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 13003 | 0 | CAE25D74AD7B16CB | 00000001 | 9300 | 0.0750,0.0750,0.0750 | **0.062879** | 0.1256,0.1088,0.1088 | 84 |
| 2 | 13003 | 0 | 90CAB86BE8159DA8 | 00000003 | 8700 | 0.1256,0.1088,0.1088 | 0.000000 | 0.1256,0.1088,0.1088 | 84 |
| 3 | 13003 | 0 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 0.1256,0.1088,0.1088 | 0.000000 | 0.1256,0.1088,0.1088 | 84 |
| 4 | 13003 | 1 | CAE25D74AD7B16CB | 00000001 | 9300 | 0.1674,0.1674,0.1674 | **0.172334** | 0.2804,0.3033,0.3112 | 223 |
| 5 | 13003 | 1 | 90CAB86BE8159DA8 | 00000003 | 8700 | 0.2804,0.3033,0.3112 | **0.012941** | 0.2817,0.3039,0.3111 | 1247 |
| 6 | 13003 | 1 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 0.2817,0.3039,0.3111 | **0.022548** | 0.2811,0.3023,0.3089 | 1951 |
| 7 | 13003 | 2 | CAE25D74AD7B16CB | 00000001 | 9300 | **0.2598,0.2598,0.2598** | **0.288889** | 0.2818,0.3261,0.3508 | 190 |
| 8 | 13003 | 2 | 90CAB86BE8159DA8 | 00000003 | 8700 | 0.2818,0.3261,0.3508 | **0.029047** | 0.2848,0.3282,0.3515 | 1309 |
| 9 | 13003 | 2 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 0.2848,0.3282,0.3515 | **0.059850** | 0.2880,0.3282,0.3484 | 2361 |
| 10 | 13004 | 0 | CAE25D74AD7B16CB | 00000001 | 9300 | 0.1671,0.1671,0.1671 | **0.062878** | 0.2014,0.1846,0.1846 | 84 |
| 11 | 13004 | 0 | 90CAB86BE8159DA8 | 00000003 | 8700 | 0.2014,0.1846,0.1846 | 0.000000 | 0.2014,0.1846,0.1846 | 84 |
| 12 | 13004 | 0 | FD9976C9E5DB9BB0 | 00000007 | 9300 | 0.2014,0.1846,0.1846 | 0.000000 | 0.2014,0.1846,0.1846 | 84 |

Decisive probe #7 (band 2, pass 0, swap 13003): before perfectly uniform 66,66,66 (before_mean 0.2598, distinct 1/1, all pixels 66,66,66); after **changed 0.288889 (28.9%)**, after_mean 0.2818,0.3261,0.3508 (B 0.351, well above grey 0.259), bottom pixels **(77,115,138)** — sky-blue. Same 28.9% change as Phase 2e's probe #7. Probe #9 reaches after_distinct 2361, br pixel (110,134,141). Probe #10 (swap 13004, band 0) reproduces probe #1's 6.3% change frame to frame.

**Phase 2e reproduces exactly.** The EDRAM write is fine in this run too. The loss is downstream.

## 2. Why the resolve probe is at 0 — the `untile_band == 2` gate

### 2.1 The probe gate (command_processor.cpp:5104–5113, read from source)

```cpp
const bool resolve_probe_gate =
    presented_output_frames_ >=
        uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_debug_resolve_probe_from), 0)) &&
    !plan.copying_depth && !plan.resolve_key.is_depth &&
    plan.resolve_key.base_tiles == 0 && plan.resolve_key.pitch_tiles == 32 &&
    plan.resolve_key.msaa == uint32_t(xenos::MsaaSamples::k4X) && untile_band == 2 &&
    plan.info.copy_dest_extent_length > 0 &&
    plan.info.copy_dest_extent_start < SharedMemory::kBufferSize &&
    plan.info.copy_dest_extent_length <=
        SharedMemory::kBufferSize - plan.info.copy_dest_extent_start;
```

The before block (line 5085) sets `resolve_probe_pending = true` only if this gate passes; the after block (line 6277, `if (resolve_probe_pending)`) logs the `FH1 resolve probe` line. Budget is `fh1_debug_resolve_probe_draws` (12, set in run_game.cmd — verified present).

### 2.2 Every gate condition except `untile_band == 2` is confirmed met

The store probe (see §3) measured 80 EDRAM color resolves (surface `0/32/4x/c3`, samples=4) with valid extents (e.g. range `1DAC5000+1310720`, 1.25 MB, within the 512 MB mirror), at swaps ≥ 3000 (the store probe itself is gated on swap ≥ 3000 and fired). So: swap ≥ 3000 ✓, not depth ✓, base_tiles 0 ✓, pitch_tiles 32 ✓, msaa k4X ✓, extent valid ✓. The **only** condition that can be failing is **`untile_band == 2`**.

### 2.3 Band-2 resolves DID happen — 20 of them — but the probe never fired

Rect heights in the 160 store probe lines:

| rect | count | meaning |
|---|---|---|
| 1280x256 | 80 | band 0 or 1 |
| 1280x208 | 40 | **band 2** (rows 512–719) |
| 1280x720 | 40 | full frame (composite) |

Of the 40 band-2-rect lines, **20 are EDRAM color** (`0/32/4x/c3`). So 20 EDRAM band-2 resolves happened (by rect), yet the resolve probe fired **0 times**. The probe's band signal (`untile_band`) and the resolve's actual band (rect) disagree.

### 2.4 Why `untile_band` is not 2 at resolve time

`untile_band` (command_processor.cpp:4556–4560) is derived from the guest register `PA_SC_WINDOW_OFFSET.window_y_offset` via `Fh1D3D11UntileBand`. That register is set by the guest per band for its **draws** (band 2 draws use window_y_offset −512). But the resolve is triggered by a **copy** command (`IssueCopy`), not a draw. By the time the guest issues the resolve/copy, it has evidently already moved `window_y_offset` on (most likely reset to 0 for the composite pass), so `untile_band` reads 0 — not 2 — even though the resolve's own plan rect is band-2-sized. The plan rect comes from the resolve's destination registers, not from `window_y_offset`, which is why the rect and `untile_band` disagree.

**Conclusion: the probe's `untile_band == 2` condition is the blocker. It uses the wrong signal to identify band-2 resolves.** The fix is to drop that condition (fire on any EDRAM resolve) or replace it with a rect check (e.g. `plan.y1 - plan.y0 == 208` for band 2, or `plan.y0 == 512`).

## 3. The store probe: mirror never updated — but only loading was measured

- **160 `FH1 resolve store probe` lines** (all in runtime.19.log, calls 44328–44827, 16:49:52.259–16:49:54.866 PM PC). **All 160 show `changed=false`.** The mirror hash is identical before and after every measured resolve, and the whole-mirror sweep (3 lines) found **no changed chunks** (`changed mirror chunks (4 MB each): none`).
- Surfaces: 80 × `0/32/4x/c3` (EDRAM color), 40 × `1024/32/4x/d1` (EDRAM depth), 20 × `0/16/1x/c1`, 20 × `0/16/1x/c2` (composite).
- Mirror-after nonzero words: 80 × `.../0` (EDRAM color — extent stayed all zeros), 40 × `.../0` (composite), 20 × `A1D3BEF37F601383/66560` (an extent that already had 66,560 nonzero words before the resolve — the resolve added nothing, `changed=false`).
- **Critical caveat: the store probe budget is 160, and it was exhausted in 2.6 seconds during LOADING (16:49:52–16:49:54), ~13 minutes before the game entered free roam (17:02:51).** It measured zero free-roam resolves. So this proves the mirror was not updated by the *loading* resolves, but says nothing about free-roam resolves. During loading the EDRAM may have been empty/cleared, so "mirror stayed zero" could be correct behaviour for those resolves rather than a failure. Do not over-claim: the store probe does **not** prove the resolve fails during free roam.
- The store probe's budget (160) is far too small — it burns out in seconds during loading. For free-roam data it needs to be raised (or gated to swap ≥ the free-roam range).

## 4. The staging log is also at 0 — open question

- **`FH1 resolve staging`: 0 lines** in any log, despite `--fh1_swap_fetch_trace=true` in run_game.cmd (verified present).
- The staging log (command_processor.cpp:5962) is gated on `REXCVAR_GET(fh1_swap_fetch_trace) && staging_logs < (fh1_debug_resolve_store_probe ? 4000 : 600)`. Both should pass (flag true, budget 4000). Its 0 count means the enclosing block is not entered for these resolves — I did not fully determine the enclosing condition from the snippets read (it sits after an owner-probe block at lines 5803–5887; it may be inside a per-region loop or a `use_staging_copy` conditional). This is the "did the dispatch write?" measurement, so it matters for separating dispatch-failure from copy-failure. It needs a source read of the enclosing block (lines ~5700–5896) before the next run.

## 5. What to do next

1. **Fix the resolve probe gate** (Phase 2f patch): drop `untile_band == 2` from the gate (fire on any EDRAM `0/32/4x/c3` resolve at/after swap 3000), or replace it with a rect check. Keep the budget at 12. This is a one-line change in command_processor.cpp:5109, then rebuild + verify the DLL.
2. **Raise the store probe budget** so it survives past loading (it burned out in 2.6 s at 160). Either raise `fh1_debug_resolve_store_probe` budget or gate the store probe to fire only at/after the free-roam swap range, so it measures free-roam resolves.
3. **Determine the staging log's enclosing condition** (read command_processor.cpp ~5700–5896) so the "did the dispatch write?" measurement actually fires.
4. **Re-run** (new game, Seth presses Start past the licence screen as before) and collect: the `FH1 resolve probe` lines (staging words_nz vs mirror-after means — coloured / grey / zero?) and the `FH1 resolve staging` lines. That splits the remaining loss into dispatch-failure (staging empty) vs copy-failure (staging full, mirror empty/grey) vs composite (mirror coloured, screen grey).
5. The game is closed; no relaunch is needed until the probe gate is fixed (a relaunch now would just reproduce 0 resolve probe lines).

## 6. Deliverables

- This report: `~/workspace/dx11-resolve-probe-2f-followup-2026-11-17.md`
- edram3d lines (24, all in runtime.8.log, swaps 13003–13004): `~/workspace/dx11-resolve-probe-scratch/edram3d-2f-lines.txt`
- Store probe lines (160, all changed=false): `~/workspace/dx11-resolve-probe-scratch/resolve-store-2f-lines.txt`
- Scratch scripts: `~/workspace/dx11-resolve-probe-scratch/collect-2f-followup.ps1`, `collect-2f-followup-lean.ps1`, `count-2f-fast.ps1`, `fetch-store-lines.ps1`, `inspect-probe-trigger.ps1`, `read-probe-gate.ps1`, `read-untile-band.ps1`, `read-staging-gate.ps1`

---

## Appendix — the 24 edram3d probe lines (runtime.8.log, thread t25052, swaps 13003–13004)

```
runtime.8.log: [2026-11-17 17:02:51.530] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#1 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x720 before_mean=0.0750434,0.0750434,0.0750434 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.549] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#1 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.062879 bbox=827,1-1279,255 cols=00000000000000011223 rows=012200000000 after_distinct=84 after_mean=0.125632,0.108811,0.108811 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.606] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#2 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x720 before_mean=0.125632,0.108811,0.108811 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.625] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#2 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.125632,0.108811,0.108811 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.671] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#3 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x720 before_mean=0.125632,0.108811,0.108811 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.709] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#3 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.125632,0.108811,0.108811 after=0.04 0.12 0.21 0.29 0.09 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.737] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#4 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x464 before_mean=0.167405,0.167405,0.167405 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.753] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#4 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.172334 bbox=0,375-1279,511 cols=11111111111111111111 rows=000000595000 after_distinct=223 after_mean=0.280449,0.303262,0.311238 after=1.00 1.00 1.00 1.00 0.27 0.00 0.53 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.778] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#5 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=223/223 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x464 before_mean=0.280449,0.303262,0.311238 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.793] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#5 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.012941 bbox=0,425-1279,511 cols=00000000000000000000 rows=000000000000 after_distinct=1247 after_mean=0.281703,0.303922,0.311141 after=1.00 1.00 1.00 1.00 0.27 0.00 0.53 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.819] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#6 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=1247/1247 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x464 before_mean=0.281703,0.303922,0.311141 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.834] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#6 key=0/32/4x/c3 vp=0,0 1280x464 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.022548 bbox=418,426-1279,511 cols=00000000000000000000 rows=000000001000 after_distinct=1951 after_mean=0.281125,0.302324,0.308919 after=1.00 1.00 1.00 1.00 0.27 0.00 0.53 1.00 1.00 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.860] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#7 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x208 before_mean=0.259766,0.259766,0.259766 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:51.875] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#7 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=9300/9300 changed=0.288889 bbox=0,512-1279,719 cols=22222222222222222222 rows=000000004999 after_distinct=190 after_mean=0.281798,0.326138,0.350824 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(77,115,138) br=(77,115,138)
runtime.8.log: [2026-11-17 17:02:51.900] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#8 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=190/190 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x208 before_mean=0.281798,0.326138,0.350824 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(77,115,138) br=(77,115,138)
runtime.8.log: [2026-11-17 17:02:51.915] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#8 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=8700/8700 changed=0.029047 bbox=0,512-1279,719 cols=00000000000000000000 rows=000000000110 after_distinct=1309 after_mean=0.284768,0.328191,0.35146 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(78,115,138) br=(77,115,138)
runtime.8.log: [2026-11-17 17:02:51.939] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13003 probe#9 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=1309/1309 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x208 before_mean=0.284768,0.328191,0.35146 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(78,115,138) br=(77,115,138)
runtime.8.log: [2026-11-17 17:02:51.954] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13003 probe#9 key=0/32/4x/c3 vp=0,0 1280x208 scissor=0,0 1280x208 prim=4 verts=9300/9300 changed=0.059850 bbox=442,512-1279,719 cols=00000000000101111111 rows=000000001220 after_distinct=2361 after_mean=0.287977,0.328187,0.348416 after=1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 px centre=(66,66,66) tl=(66,66,66) tr=(66,66,66) bl=(78,115,138) br=(110,134,141)
runtime.8.log: [2026-11-17 17:02:52.048] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13004 probe#10 key=0/32/4x/c3 rt=1 texmask=00000001 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=2/2 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE25D74AD7B16CB verts=9300/9300 vp=0,0 1280x720 before_mean=0.167057,0.167057,0.167057 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:52.063] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13004 probe#10 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.062878 bbox=827,1-1279,255 cols=00000000000000011223 rows=012200000000 after_distinct=84 after_mean=0.201429,0.184609,0.184609 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:52.089] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13004 probe#11 key=0/32/4x/c3 rt=1 texmask=00000003 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=90CAB86BE8159DA8 verts=8700/8700 vp=0,0 1280x720 before_mean=0.201429,0.184609,0.184609 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:52.104] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13004 probe#11 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=8700/8700 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.201429,0.184609,0.184609 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:52.129] [info] [gpu] [t25052] FH1 edram3d probe control: swap=13004 probe#12 key=0/32/4x/c3 rt=1 texmask=00000007 host=1280x720 res_fmt=10 view_fmt=10 samples=4 distinct=84/84 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=FD9976C9E5DB9BB0 verts=9300/9300 vp=0,0 1280x720 before_mean=0.201429,0.184609,0.184609 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
runtime.8.log: [2026-11-17 17:02:52.144] [info] [gpu] [t25052] FH1 edram3d probe draw: swap=13004 probe#12 key=0/32/4x/c3 vp=0,0 1280x720 scissor=0,0 1280x256 prim=4 verts=9300/9300 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=000000000000 after_distinct=84 after_mean=0.201429,0.184609,0.184609 after=1.00 1.00 1.00 1.00 0.27 0.00 0.00 0.00 0.47 1.00 1.00 1.00 px centre=(0,0,0) tl=(66,66,66) tr=(66,66,66) bl=(66,66,66) br=(66,66,66)
```
