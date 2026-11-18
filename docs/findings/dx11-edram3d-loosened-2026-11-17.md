# Loosened edram3d probe — Phase 2d final report

**Date:** 2026-11-17 (afternoon CST; PC clock ~1 h behind CST)
**Run:** D11 edram3d run 1 (PID 6452, probe from swap 3000, budget 12) — Seth (via ChatGPT) confirmed the plan ~16:05 CST.
**Verdict:** The loosened probe **FIRED — all 12 draws** (24 lines: 12 control + 12 draw). The `used_texture_mask != 0` gate **was the blocker**: 10 of the 12 draws are untextured (`texmask=00000000`), and the old probe would have skipped them. **But the EDRAM readback is NOT captured**: all 12 draws are on composite surfaces (`0/16/1x/c1`, `0/16/1x/c2`), not the EDRAM surface (`0/32/4x/c3`). The probe fired during loading (swap 3000 landed before the game entered) and the budget was used up on composite draws. **The flat-grey question (does the EDRAM get coloured after the triangles draw?) is still open.** The game still had not entered at the time of this report (~8 min after launch) and may be waiting for input — see §4.

---

## 0. What was patched

The trigger block in `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\command_processor.cpp` (lines 3592–3609, verified against source before editing) was replaced:

- **Slot scan instead of hardcoded slot 1:** loop over `rt_slot` 0..`kMaxColorRenderTargets` (the pending_key index: 0 = depth, 1..N = colour RTs), checking `used_bits & (1u << rt_slot)` and `pending_key(rt_slot)`. This covers all four colour RTs. (Note: the task/ChatGPT text said "0..3"; in pending_key indexing that would miss colour RT 3 / slot 4, so the loop uses the codebase's own idiom `rt_slot < 1 + xenos::kMaxColorRenderTargets`, as in the same file's line 1086. The EDRAM surface was on slot 1 in the Phase 2c logs, so this does not change what this run catches — it is belt-and-braces.)
- **Family check loosened:** require only `!is_depth && base_tiles == 0`. Dropped the `pitch_tiles == 32` and `msaa == k4X` checks.
- **Texture-mask gate DROPPED entirely.** The triangle-prim check (List/Strip/Fan) and the budget logic are kept.
- **`texmask={:08X}` and `rt={}` added to the "FH1 edram3d probe control" log line**, so each probe reports the slot and whether the draw was textured.

Backup: `command_processor.cpp.musebak-edram3d-20261117`. Full diff: `~/workspace/dx11-edram3d-loosened-probe-diff-2026-11-17.txt` (70 lines). Patch script: `~/workspace/dx11-edram3d-loosened-scratch/apply-edram3d-loosen.py`. Gating-only change; no rendering logic, no refactor, no WaitForGpu edits. Per-band fix + all #23 probes + Phase 2 D11 logger + Phase 2c span probe preserved.

## 1. Build — PASS

- `CMAKE_BUILD_PARALLEL_LEVEL=4`, `muse_build_game.bat`, **BUILD_EXIT=0**.
- `pinyon_shift.exe` 61,851,136 B @ 3:10:03 PM PC; `rexgpu-fh1.dll` 11,500,032 B @ 3:09:58 PM PC (same size as Phase 2c — the added code replaced removed code of similar size).
- **Binary check PASSES:** DLL is newer than the patched source (source 3:08:54 PM PC); the strings `fh1_debug_trace_span`, `FH1 edram3d probe control`, and `texmask=` are all present in `rexgpu-fh1.dll`. The binary matches the patched source.

## 2. Run config (verified)

`run_game.cmd` (backup `run_game.cmd.pre-edram3d-20261117`) rewritten minimal for this check:

```
--gpu_backend=d3d11 --d3d11_adapter=GTX --fh1_debug_tile_trace_swap=3000
--gpu_trace_bins_vblank=3000
--fh1_debug_edram3d_probe=E:/PinyonShift/work/.local/compdump
--fh1_debug_edram3d_probe_from=3000 --fh1_debug_edram3d_probe_draws=12
```

- **Callog OFF** (no `--fh1_debug_callog` flag; default 0 = off) and **no trace span** (default 1). No composite/texture/c2/surface/frontbuffer/srv-bind/resolve probes. The edram3d probe is gated on its own cvars, not the callog — and it fired, confirming the independence.
- `--fh1_debug_edram3d_probe_draws=12` is **explicitly present** (verified in the file). (Phase 2c's "draws 12" was actually the default 3 — the regate replace had not matched because the flag was absent. This run really has 12.)
- Launched via the scheduled task "PinyonShift Test Run" at ~3:10:39 PM PC; process PID 6452, session 1 (visible). The previous run (PID 20208) was already closed by Seth before launch (tasklist confirmed empty).

## 3. The probe fired — all 12 draws (swap 3000, thread t23900, 15:13:17–15:13:18 PM PC)

24 lines in `runtime.3.log` (12 control + 12 draw). All on `rt=1`, all `samples=1`. Full lines saved at `~/workspace/dx11-edram3d-loosened-scratch/edram3d-lines.txt`.

### 3.1 The 12 probes at a glance

| # | key | texmask | ps_hash | verts | prim | before_mean | changed | after_mean |
|---|---|---|---|---|---|---|---|---|
| 1 | 0/16/1x/c1 | 00000007 | 31511D87CC0C94B9 | 4/4 | 6 | 0,0,0 | 0.000000 | 0,0,0 |
| 2 | 0/16/1x/c2 | 00000000 | A39E23E2FD1F8A8B | 48/48 | 4 | 0.352,0.176,0.176 | **1.000000** | 0,0,0 |
| 3 | 0/16/1x/c2 | 00000001 | CAE1DB68AFFA9D3C | 48/48 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 4 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 624/624 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 5 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 684/684 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 6 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 438/438 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 7 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 576/576 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 8 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 576/576 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 9 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 576/576 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 10 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 330/330 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 11 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 438/438 | 4 | 0,0,0 | 0.000000 | 0,0,0 |
| 12 | 0/16/1x/c2 | 00000000 | 57B9400F6B398736 | 684/684 | 4 | 0,0,0 | 0.000000 | 0,0,0 |

- **10 of 12 draws are untextured (`texmask=00000000`).** Only #1 (`texmask=7`) and #3 (`texmask=1`) have a nonzero mask. The old probe (which required `used_texture_mask != 0`) would have fired at most twice — and both of those draws wrote nothing (`changed=0`). **The texture-mask gate was the blocker, confirmed by the data, not by reasoning.**
- **All 12 draws are on `0/16/1x` surfaces (pitch 16, 1x MSAA)** — the composite/post surfaces. **None is on the EDRAM surface (`0/32/4x/c3`).** The loosened trigger dropped the pitch/MSAA check, so it caught the first 12 triangle draws at/after swap 3000 — and those were composite draws, because swap 3000 landed during loading (see §4).
- **All draws are on `rt=1`** (the slot scan found the surface on slot 1 every time), consistent with the Phase 2c finding that the colour surface is on slot 1.

### 3.2 Probe #2 — a #23-like wipe, caught live

Probe #2 is the one draw that changed anything:

- **Before:** `before_mean=0.351562,0.175791,0.175791`, `distinct=5/5`, pixels centre `(0,0,0)`, tl `(255,255,255)`, tr `(255,255,255)`, bl `(0,0,0)`, br `(0,0,0)` — the surface had content (white top corners on a dark field).
- **The draw:** prim 4 (TriangleList), 48 verts, `texmask=00000000` (untextured), `ps_hash=A39E23E2FD1F8A8B`, viewport 0,0 1280x720, scissor 0,0 1280x720.
- **After:** `changed=1.000000`, bbox `0,0-1279,719` (the whole surface), `after_distinct=1`, `after_mean=0,0,0`, all five sample pixels `(0,0,0)`.

A 48-vert untextured TriangleList wiped the entire `0/16/1x/c2` surface to uniform black. This is the same shape as composite draw #23 (48 verts, TriangleList, wipes c2 to black) — now caught live with a before/after readback, and it is **untextured**, which is exactly why the old probe (with the texture-mask gate) could never have caught it. This is independent confirmation that the #23 wipe is produced by an untextured fullscreen triangle draw, not by a textured world draw.

All other 11 probes: the surface was uniform `(0,0,0)` before and after, `changed=0.000000` — the draws wrote nothing (or wrote black onto black).

### 3.3 The before/after pixel verdict for the EDRAM question

**Not answered.** The probe fired, but on composite surfaces, not the EDRAM MSAA surface. There is no before/after readback of `0/32/4x/c3` in this run, so the question "does the EDRAM buffer get coloured after the EDRAM triangles draw, or stay 66,66,66?" is **still open**. What this run does answer:

- The loosened trigger works (12/12 fired) and the texture-mask gate was the blocker.
- The composite draws that the probe caught are overwhelmingly untextured (10/12 with `texmask=0`), including the one draw (#2) that actually changed a surface — a fullscreen wipe to black.
- The EDRAM surface was not among the first 12 triangle draws at/after swap 3000, because swap 3000 landed during loading.

### 3.4 Full probe lines (control + draw)

All 24 lines are in `~/workspace/dx11-edram3d-loosened-scratch/edram3d-lines.txt`. The control lines (12):

```
[2026-11-17 15:13:17.770] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#1 key=0/16/1x/c1 rt=1 texmask=00000007 host=1280x2048 res_fmt=11 view_fmt=11 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=31511D87CC0C94B9 verts=4/4 vp=0,0 8192x8192 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:17.919] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#2 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=5/5 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=A39E23E2FD1F8A8B verts=48/48 vp=0,0 1280x720 before_mean=0.351562,0.175791,0.175791 px centre=(0,0,0) tl=(255,255,255) tr=(255,255,255) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.016] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#3 key=0/16/1x/c2 rt=1 texmask=00000001 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=CAE1DB68AFFA9D3C verts=48/48 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.111] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#4 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=624/624 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.196] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#5 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=684/684 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.280] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#6 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=438/438 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.365] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#7 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=576/576 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.449] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#8 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=576/576 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.534] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#9 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=576/576 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.618] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#10 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=330/330 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.703] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#11 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=438/438 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.789] [info] [gpu] [t23900] FH1 edram3d probe control: swap=3000 probe#12 key=0/16/1x/c2 rt=1 texmask=00000000 host=1280x2048 res_fmt=24 view_fmt=24 samples=1 distinct=1/1 control_changed=0.000000 control_bbox=2147483647,2147483647--1,-1 ps_hash=57B9400F6B398736 verts=684/684 vp=0,0 1280x720 before_mean=0,0,0 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
```

The draw lines (12) — same order, one per probe:

```
[2026-11-17 15:13:17.851] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#1 key=0/16/1x/c1 vp=0,0 8192x8192 scissor=0,0 1280x720 prim=6 verts=4/4 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:17.961] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#2 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=48/48 changed=1.000000 bbox=0,0-1279,719 cols=99999999999999999999 rows=99999999999900000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.056] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#3 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=48/48 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.144] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#4 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=624/624 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.229] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#5 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=684/684 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.314] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#6 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=438/438 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.398] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#7 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=576/576 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.483] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#8 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=576/576 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.567] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#9 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=576/576 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.651] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#10 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=330/330 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.736] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#11 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=576/576 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
[2026-11-17 15:13:18.822] [info] [gpu] [t23900] FH1 edram3d probe draw: swap=3000 probe#12 key=0/16/1x/c2 vp=0,0 1280x720 scissor=0,0 1280x720 prim=4 verts=684/684 changed=0.000000 bbox=2147483647,2147483647--1,-1 cols=00000000000000000000 rows=00000000000000000000000000000000000 after_distinct=1 after_mean=0,0,0 after=0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 0.00 px centre=(0,0,0) tl=(0,0,0) tr=(0,0,0) bl=(0,0,0) br=(0,0,0)
```

### 3.5 PPMs

The probe wrote before/control/after PPMs for probes #1–3 (index ≤ 3) to `E:\PinyonShift\work\.local\compdump\`:

- `edram3d-3000-1-{before,control,after}.ppm` (7,864,337 B each, 1280x2048)
- `edram3d-3000-2-{before,control,after}.ppm`
- `edram3d-3000-3-{before,control,after}.ppm`

They were not fetched to the VM (9 × 7.8 MB over the slow scp link); the log lines above already contain the before/after pixel data (means + 5 sample pixels per probe), so the PPMs are redundant for the verdict. Paths are recorded here for reference. (Older PPMs from 11/16 and 11/17 1:06 AM in the same directory are from earlier probe runs, not this one.)

## 4. Game state — not entered, may be waiting for input

The game (PID 6452, launched 3:10:39 PM PC) had **not entered** at the time of this report (last check 3:20:23 PM PC, ~10 min after launch):

- `music.in_game`: 0; `save.backup`: 0 (jsonl `20261117T221039Z-p6452.jsonl`).
- `step_gate` (last): raises 84–90, holds ~160, `swap_raises` 115–118, timeouts 42–45, `held_ms` ~7,600 per 10 s. Presents are happening (~12/s, loading rate), so it is not frozen on a static screen — but the sim is held (holds 160, timeouts rising) and it has not entered.
- Memory: 186 MB at 3:16:25 → 197 MB at 3:17:24 → **86 MB at 3:18:35** → 189 MB at 3:20:23 — fluctuating sharply, consistent with the game loading/unloading content or returning to a menu/title, not with steady loading growth; Phase 2c entered at 2.7 min with memory 544 MB and growing.

This run is taking far longer to enter than Phase 2c (2.7 min). It may be on the GTX 570 "licence" screen waiting for input, or in a slow loading loop. **The game was left running (PID 6452); nothing was killed.** Per the task instructions, a run that looks stuck/waiting is reported, not killed/relaunched in a loop.

Note the timing consequence: the probe fired at swap 3000 at 15:13:17 — only ~2.5 min after launch — because the present counter advances at loading rate (~12–20/s) while the game is still loading. In Phase 2c, swap 3000 landed after entry; in this run it landed during loading. **Swap number is not a reliable proxy for "in free roam"** — it depends on loading speed. That is why the budget was spent on loading/composite draws.

## 5. What this means and the next step

- **The loosened trigger works.** The probe fired 12/12. The `used_texture_mask != 0` gate was the blocker — proven by the data (10/12 draws untextured, and the only draw that changed a surface, #2, was untextured).
- **The EDRAM readback is still not captured.** The probe caught composite draws because the family check (pitch/MSAA) was dropped and the budget was spent during loading.
- **Probe #2 is a live #23-like wipe** (48-vert untextured TriangleList, whole c2 surface to black) — independent confirmation of the #23 mechanism, and it shows the wipe draw is untextured.
- **Next step (one small patch + relaunch):** restore the EDRAM family check (`pitch_tiles == 32 && msaa == k4X`) in the loosened trigger, so the probe only fires on the EDRAM surface (`0/32/4x/c3`) — keep the slot scan and keep the texture-mask gate dropped. That way the budget cannot be spent on composite/loading draws. Then relaunch, but only after confirming the game state (this run may be waiting for input at the licence screen — Seth should check the monitor / press a key if it is). Do NOT relaunch in a loop if the game is waiting for input.
- **WaitForGpu fix** remains queued, untouched, as instructed. The EXE/DLL are the Phase 2d loosened build (3:10 PM PC).

## 6. Deliverables and state

- **This report:** `~/workspace/dx11-edram3d-loosened-2026-11-17.md`
- **Probe diff (source only; 1 file):** `~/workspace/dx11-edram3d-loosened-probe-diff-2026-11-17.txt` (70 lines)
- **Probe lines (all 24):** `~/workspace/dx11-edram3d-loosened-scratch/edram3d-lines.txt`
- **Scratch (scripts):** `~/workspace/dx11-edram3d-loosened-scratch/` (verify-trigger.ps1, find-surfacekey*.ps1, read-surfacekey-h.ps1, apply-edram3d-loosen.py, diff-and-regates.ps1, rebuild-j4.ps1, check-build.ps1, launch-edram3d-run.ps1, check-edram3d-run.ps1, dump-edram3d-lines*.ps1, collect-edram3d-ppms.ps1, check-entered.ps1, save-edram3d-lines*.ps1)
- **PPM paths (on PC, not fetched):** `E:\PinyonShift\work\.local\compdump\edram3d-3000-{1,2,3}-{before,control,after}.ppm`
- **Backups on PC:** `command_processor.cpp.musebak-edram3d-20261117`; `run_game.cmd.pre-edram3d-20261117`.
- **`run_game.cmd` current state:** D11, `--gpu_backend=d3d11 --d3d11_adapter=GTX`, tile_trace_swap 3000, gpu_trace_bins_vblank 3000, edram3d probe dir `…/compdump`, from 3000, draws 12. Callog OFF, no span, no other probes.
- **Game:** still running (PID 6452, session 1) at the time of this report — left alone, nothing killed.

### Binary / config (per the verify_diagnostic_setup skill)

- Binary: exe 61,851,136 B @ 3:10:03 PM PC, dll 11,500,032 B @ 3:09:58 PM PC (Phase 2d loosened build) — matches the patched source (3:08:54 PM PC); strings `fh1_debug_trace_span`, `FH1 edram3d probe control`, `texmask=` present in the DLL.
- Effective config: `--gpu_backend=d3d11 --d3d11_adapter=GTX`, tile_trace_swap 3000, edram3d probe from 3000, draws 12 (verified in run_game.cmd and in the runtime log: the probe fired 12 times at swap 3000, thread t23900, PID 6452).
- Diagnostic files: the edram3d lines are in `runtime.3.log` (rotated 3:14:37 PM PC), current and non-empty; the probe lines are attributed by run (thread t23900, PID 6452, swap 3000). The current `runtime.log` is locked while the game runs (expected).
- **Check that FAILED (the point of the report):** the probe fired, but on composite surfaces, not the EDRAM surface — the EDRAM readback was not captured, and the report does not claim it was. The game had not entered after ~8 min — the run state is reported as uncertain (may be waiting for input), not as a clean free-roam capture.
- Adapters: D11 run used the **GTX 570** (adapter "GTX"). No Vulkan involvement in this run.
