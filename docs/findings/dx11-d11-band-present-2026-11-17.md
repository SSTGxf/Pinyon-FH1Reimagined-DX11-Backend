# D11 band present capture — Phase 2c final report

**Date:** 2026-11-17 (afternoon CST; PC clock ~1 h behind CST)
**Run:** D11 band run 1 (PID 20208, swap window 3000–3005) — Seth chose "Band present first" ~15:19 CST.
**Verdict:** The run is **NOT stuck** (it entered the game, saves every ~60 s, sim stepping). The multi-swap window **did capture EDRAM triangles on D11 in every swap** — but the per-present census **FAILS** (compare.py exit 1, VERDICT: FAIL for swaps 3000/3002/3005), and the decisive **edram3d probe never fired (0 lines)**. The "one swap = one present" assumption **breaks at D11's ~0.2 FPS present rate**: each swap value accumulates many guest frames, so band and composite draws are mixed in every swap. The flat-grey question (does the EDRAM get coloured after the triangles draw?) is **still unanswered**.

---

## 0. Stuck check (Seth's monitor photo, 15:38 CST) — NOT stuck

Seth's photo showed the D3D11 overlay at **0 FPS** on a loading screen reading "Head to the Ho…" (Head to the Horizon Festival), ~4–5 min after launch. Checked the run state at 2:38–2:52 PM PC:

- **Game entered the game:** `music.in_game` fired at 2:36:29 PM PC (2.7 min after launch, PID 20208, started 2:33:48 PM PC).
- **Saving regularly:** `save.backup` every ~60 s (2:37, 2:38, 2:39 … 15 saves by 2:52 PM). A stuck/loading game does not save.
- **Sim stepping:** `step_gate` raises ~487–596 per 10 s throughout — the simulation is running.
- **Renderer extremely slow, not stalled:** `swap_raises` 0–4 per 10 s (≈0–0.4 presents/sec), overlay 0 FPS. The game is in the **opening drive** (the "Head to the Horizon Festival" objective) and the D11 backend is crawling — the known CPU-decoder bottleneck, not a loading stall and not waiting for input. The screen barely updates, which is why it looks static at 0 FPS.
- **Memory growing:** 544,748 KB → 618,732 KB (content streaming in).

**No kill/relaunch was done.** The game (PID 20208) was left running. The 0 FPS is the D11 bottleneck, not a stuck run.

---

## 1. What was built — the multi-swap trace window (span probe)

### 1.1 The change

A guest frame spans several presents (one per EDRAM band + one composite), so a single-swap gate captures only one present type. Added one new cvar, `fh1_debug_trace_span` (INT32, default 1 = single swap, unchanged behaviour), and widened the callog + routing gates to fire for swaps in `[start, start + span)`.

**Files changed (backups `*.musebak-band-20261117`):**

| File | Change |
|---|---|
| `rexglue/src/graphics/flags.cpp` | `REXCVAR_DEFINE_INT32(fh1_debug_trace_span, 1, …)` after `fh1_debug_callog_budget` |
| `rexglue/include/rex/graphics/flags.h` | `REXCVAR_DECLARE(int32_t, fh1_debug_trace_span);` after `fh1_debug_callog_budget` |
| `rexglue/src/graphics/command_processor.cpp` (shared) | `Fh1TraceSpan()` + `Fh1SwapInTraceWindow()` helpers; `Fh1CallogEnabled` uses the window; all 5 routing-gate sites (`trace_swaps_ == routing_swap`) use the window |
| `rexglue/src/graphics/d3d11/command_processor.cpp` | Same helpers + `Fh1CallogEnabled` window (the D3D11 backend's own copy, separate TU) |

Full diff: `~/workspace/dx11-d11-band-present-probe-diff-2026-11-17.txt` (139 lines, 4 files). Patch script: `~/workspace/dx11-d11-band-present-scratch/apply-band-span.py`. Gating-only: no rendering logic, no new probes, no refactor, no WaitForGpu edits. Per-band fix + all #23 probes + Phase 2 D11 logger preserved.

### 1.2 Build — PASS

- `CMAKE_BUILD_PARALLEL_LEVEL=4`, `muse_build_game.bat`, **BUILD_EXIT=0**.
- `pinyon_shift.exe` 61,851,136 B @ 2:30:46 PM PC; `rexgpu-fh1.dll` 11,500,032 B @ 2:30:27 PM PC (Phase 2 was 11,496,960 B; +3,072 B = the new helpers/span logic).
- **Binary check PASSES:** DLL is newer than the patched source (source 2:27:19–2:27:20 PM PC); the string `fh1_debug_trace_span` is present in `rexgpu-fh1.dll` (1 occurrence). The exe is the same size as Phase 2 (61,851,136 B) because the changed code lives in the DLL, not the exe.
- No rebuild was needed beyond this one (no source changes after the build).

### 1.3 Run config (verified)

`run_game.cmd` (backup `run_game.cmd.pre-band-20261117`): `--gpu_backend=d3d11 --d3d11_adapter=GTX`, callog 3000, tile_trace_swap 3000, resolve_trace 3000, **`--fh1_debug_trace_span=6`**, **`--fh1_debug_callog_budget=1000000`** (was 200,000), **`--fh1_debug_edram3d_probe_draws=12`** (was 3), edram3d_probe_from 3000, all 12 swap flags 3000. Launched via the scheduled task "PinyonShift Test Run" at 2:33:43 PM PC; process PID 20208 started 2:33:48 PM PC, session 1 (visible).

---

## 2. The window fired — EDRAM triangles on D11 in every swap

The window [3000, 3006) fired. Callog + routing lines exist for all 6 swaps (thread t11580). Log volume: 20 rotated runtime logs, 104,857,593 B total, burst 2:37:42–2:40:46 PM PC.

| Swap | Callog lines | Routing-guest lines | A3 issued (D3D11) | A9 present issued |
|---|---|---|---|---|
| 3000 | 234,968 | 1,226 | 1,149 | — (no A9 line) |
| 3001 | 380,072 | 3,448 | 3,230 | 3,179 |
| 3002 | 15,500 | 3,808 | 3,588 | 3,230 |
| 3003 | 15,414 | 3,796 | 3,577 | 3,588 |
| 3004 | 13,731 | 3,441 | 3,222 | 3,577 |
| 3005 | 13,791 | 3,455 | 3,236 | 3,222 |

**EDRAM triangles are present in every swap.** Per-swap routing-guest breakdown (surf / prim / mode / pass) shows, in each of swaps 3000–3005:

- **EDRAM band draws:** surf `14020500`, prim 4 (TriangleList), mode 4, passes 0/1/2 (sel `0x80000003` / `0xC` / `0x30`, winy 0 / −256 / −512), idx 324 / 360 / 954 — real world geometry in all three bands.
- **Composite draws:** surf `14000500`, prim 4, mode 4, pass −1, idx 6 / 18 / 48 / 600.
- **Single-point draws:** prim 1, both surfaces (the baseline 48-point pattern).

So the window **did capture EDRAM triangles on D11** — in the same run, alongside the composite draws. This re-confirms the Phase 2b correction (D11 issues EDRAM triangles; t27292 was not a fluke). The D3D11 backend **issued ~3,200–3,600 draws per present to the GPU** (A3), with **202 distinct (vs,ps) shader hashes, 0 invalid translations** (compare.py F4, swap 3002), and 1,071 A4 lines with `pvalid==-1` (the mode-5 depth-only draws, as in the F4 closeout).

**Textures are bound and valid:** 82 distinct `FH1 srv bind all` lines, all `valid=1` (e.g. slot 5/7/3, 64×64 and 128×128, fmt 18, texture=1, srv=1). **0 "FH1 texture load rejected" lines.** The binding mechanism works.

**Resolve sources show content in some bands:** 128 `FH1 resolve source bands` lines. E.g. swap 3005 key `0/16/1x/c2` has bands 6–11 at nz 0.96–1.00 (bottom half nearly fully nonzero) while bands 1–2 are 0.000 — consistent with the #23 split (bottom half populated, top half dark). Swap 3005 key `0/16/1x/c0` (rect_bottom 256) has nz ~0.51–0.57. These are single-sample resolve sources, not the EDRAM itself (see §4).

---

## 3. Why the census FAILS — accumulation at ~0.2 FPS

`compare.py` (d11 mode) was run for swaps 3000, 3002, 3005 (thread t11580). **All three: exit 1, VERDICT: FAIL** (acceptance A1–A4 NOT met). Outputs saved as `~/workspace/dx11-d11-band-present-scratch/compare-band-300{0,2,5}.txt`.

### 3.1 The "one swap = one present" assumption breaks

Each swap value spans **seconds** of wall time and **thousands of draws**:

- Swap 3000: 14:37:42–14:37:54 (~12 s, 1,226 routing-guest draws).
- Swap 3001: 14:37:56–14:38:10 (~14 s, 3,448 draws).
- Swap 3002: 14:38:10–14:38:15 (~5 s, 3,808 draws).
- Swap 3005: 14:38:22–14:38:32 (~10 s, 3,455 draws).

At D11's ~0.2 presents/sec, the sim issues many guest frames between presents (~55 sim steps/sec), and **all of their draws accumulate under the current swap value**. A single "swap" here is ~5–14 seconds of draws (~1,200–3,800), not one present (~131 draws in Phase 2). Every swap therefore contains **all present types mixed** (all three bands + composite) — the band/composite distinction the window was meant to separate is lost. The A9 present `issued` counts (3,179–3,588 per present) confirm it: each present carries ~25× the Phase 2 single-present draw count (52 issued).

This is the same accumulation mechanism as t27292 (present counter stuck, ~20–25 frames under one swap), now seen live and pervasively at D11's slow present rate. **Per-present gating by swap number does not work when presents are ~0.2/sec.** The multi-swap window widens the capture but cannot separate presents that the counter itself does not separate.

### 3.2 Budget exhausted — shared callog died at swap 3001

`FH1 callog budget exhausted swap=3001 budget=1000000` (14:38:00.892, t11580). The per-swap line volume is ~10–50× the Phase 2 composite present (band presents + accumulation generate far more pkt/reg lines: swap 3000 alone is 234,968 callog lines vs 6,655 for the Phase 2 single swap). The **shared** callog counter hit 1,000,000 during swap 3001, so for swaps 3002–3005 the shared side logged **no pkt lines, no reg lines, and no stage-D routing-issue lines** — while the routing (tile_trace) lines and the D3D11 callog lines continued (the two counters are independent; only the shared one exhausted). Consequences in the swap-3002 census:

- A2: **0 Type-3 pkt lines** — "callog gate did not fire?".
- A1: BINPRED 1474 / guest 3808 / draw packets 0 — counts disagree; **unaccounted guest draws 3,808**; callog-issued (3,588) != stage-D issued (0); callog-copy (100) != tile-path copy (0).
- F1/F2: 0 pktpost lines, 0 reg lines. F3: 0 srv-bind lines *for that swap* (the 82 binds are all at swap 3000; see §4).

Swap 3000 also FAILs (BINPRED 1474 / guest 1226 / draw packets 1474; no A9 present line for swap 3000). The census is not salvageable per-swap from this run — the accounting breaks because the shared side is missing for most of the window and each swap is an accumulation, not a present.

---

## 4. The edram3d probe never fired — the decisive question is still open

**`FH1 edram3d probe` lines: 0 across all 20 rotated logs** (and 0 in the newest rotation at 2:52 PM; the game ran 18+ min with the probe armed). The probe was configured (`--fh1_debug_edram3d_probe=…/compdump --fh1_debug_edram3d_probe_from=3000 --fh1_debug_edram3d_probe_draws=12`), the EDRAM triangle draws exist in the window, and textures are bound and valid — yet the probe fired zero times.

The probe fires (D3D11 `command_processor.cpp` ~3556) only when ALL of these hold for a draw: `presented_output_frames_ >= from`, budget not exhausted, `pending_used_bits() & (1u << 1)` (render-target slot 1 pending), the pending key for **slot 1** is the EDRAM colour family (base 0 / pitch 32 / 4× MSAA / not depth), primitive is TriangleList/Strip/Fan, and `used_texture_mask != 0`. The most likely blockers (not yet proven — needs a source read, not a guess):

1. **Slot assumption:** the probe hardcodes slot 1 (`pending_key(1)`). If the world draws in this run target a different render-target slot, the family check fails and the probe silently skips every draw. (In t27292 it fired, so slot 1 was the EDRAM colour then — the slot may vary by draw/frame.)
2. **Texture mask:** `used_texture_mask` is the guest's per-draw texture usage. If the EDRAM draws in this window are the untextured/depth-only subset, the mask is 0 and the probe skips them. (The 82 srv binds are all at swap 3000; no new distinct binds appear in swaps 3001–3005 — consistent with the binds being logged once as a set, so this does not prove the later draws are untextured, but it does not prove they are textured either.)

**Consequence:** there is **no before/after EDRAM readback** from this run. The decisive question — does the EDRAM buffer actually get coloured after the EDRAM triangles draw, or does it stay 66,66,66? — is **still unanswered**. The resolve-source bands (§2) show composite surfaces with content in some bands, but those are single-sample resolve sources, not the EDRAM MSAA surface itself. Do not cite this run as having measured the EDRAM pixel content.

---

## 5. What this means for the render bug

- **Routing is closed (again, on D11, in a single run):** the guest issues EDRAM triangles on D11 in every window swap; the D3D11 backend issues ~3,200–3,600 draws per present to the GPU; shaders translate (0 invalid); textures bind valid (0 rejections). The flat-grey world is **not** a routing, predication, or texture-binding problem. The loss remains downstream of IssueDraw (shader output / texture fetch / EDRAM write), as established in Phase 2b.
- **The #23 split is untouched by this run** (a composite-present question; the window's composite draws are mixed with band draws by accumulation, so no clean composite-present census exists here either).
- **The span probe itself works as built** (the window fired, the routing gates widened, the D3D11 callog widened) — but the **premise it was built on is wrong at D11's present rate**: a swap window cannot separate presents when one swap value spans many guest frames. The tool is correct; the gating signal (present counter) is too coarse at ~0.2 FPS.

---

## 6. Next steps (for the parent / Seth)

1. **Find out why edram3d never fired** (source read, no new run yet): check whether the world draws target slot 1 (`pending_key(1)`) and whether `used_texture_mask` is nonzero for them. If the slot assumption is the blocker, widen the probe to check all render-target slots (or the draw's actual target slot). If the mask is the blocker, let the probe fire on untextured EDRAM triangle draws too (an untextured world draw is itself a finding).
2. **Reconsider the gating for D11's present rate.** Options: (a) a **draw-count trigger** — fire the edram3d probe on the first N EDRAM triangle draws regardless of swap (it is already a threshold probe; the fix is in its slot/mask conditions, not the swap); (b) a **much larger budget** (10M+) plus accepting accumulation and analysing the aggregate rather than per-present; (c) gate on **guest-frame count** instead of present count, if a guest-frame counter exists. Do not re-run the same 6-swap window expecting a clean per-present census — the accumulation will reproduce.
3. **Do not re-run yet** until (1) is answered from source — another run with the same probe conditions will produce the same zero edram3d lines.
4. **WaitForGpu fix** remains queued, untouched, as instructed. The EXE/DLL are the Phase 2c span build (2:30 PM PC); they are **not** stale relative to the current source (no source changes since that build).

---

## 7. Deliverables and state

- **This report:** `~/workspace/dx11-d11-band-present-2026-11-17.md`
- **Probe diff (source only; 4 files):** `~/workspace/dx11-d11-band-present-probe-diff-2026-11-17.txt` (139 lines)
- **Patch script:** `~/workspace/dx11-d11-band-present-scratch/apply-band-span.py`
- **D11 band logs (run 1, window 3000–3005):** `~/workspace/dx11-d11-band-present-scratch/logs-band-run1/` (20 files, 104,857,593 B) + symlinked `logs-band-run1-cmp/` (runtime.N.log names for compare.py)
- **compare.py outputs (all FAIL):** `~/workspace/dx11-d11-band-present-scratch/compare-band-300{0,2,5}.txt`
- **Scratch (scripts):** `~/workspace/dx11-d11-band-present-scratch/` (regatespan6.ps1, launch-band-run.ps1, check-band-run.ps1, check-band-run2.ps1, collect-band-logs.ps1, collect-band-all.ps1, check-band-latest.ps1)
- **Design doc §9:** the present-type understanding is updated (see the design doc — a swap value is not one present at D11's ~0.2 FPS; the window widens the capture but cannot separate presents the counter does not separate).
- **Backups on PC:** `*.musebak-band-20261117` (4 source files); `run_game.cmd.pre-band-20261117`.
- **`run_game.cmd` current state:** D11, `--gpu_backend=d3d11 --d3d11_adapter=GTX`, span 6, budget 1,000,000, edram3d draws 12, all swap flags 3000. (Left as-is; the next run's gating is decision (2) above.)
- **Game:** still running (PID 20208, session 1) at the time of this report — left alone, nothing killed.

### Binary / config (per the verify_diagnostic_setup skill)

- Binary: exe 61,851,136 B @ 2:30:46 PM PC, dll 11,500,032 B @ 2:30:27 PM PC (Phase 2c span build) — matches source (patched 2:27:19–2:27:20 PM PC), `fh1_debug_trace_span` string present in the DLL. No rebuild needed after this build; none done.
- Effective config: `--gpu_backend=d3d11 --d3d11_adapter=GTX`, span 6, budget 1,000,000, edram3d draws 12 (verified from run_game.cmd and from the runtime log: the window fired for swaps 3000–3005, thread t11580, PID 20208; the budget-exhausted line confirms the budget value in the binary).
- Diagnostic files: 20 rotated runtime logs, current (burst 2:37:42–2:40:46 PM PC, matching the window), non-empty (5.24 MB each), attributed by run ID (thread t11580, PID 20208). The current runtime.log is 0 B (rotated at 2:40:46 PM; no new rotations since — the window passed and the callog is off for swaps > 3005).
- **Check that FAILED:** the shared callog budget was exhausted at swap 3001 (1,000,000), so the shared-side pkt/reg/issue lines are missing for swaps 3002–3005 — the per-swap census is incomplete, and the report does not claim a PASS. The edram3d probe produced zero lines (a probe-condition failure, not a logging failure) — the decisive EDRAM readback was not captured, and the report does not claim it was.
- Adapters: D11 run used the **GTX 570** (adapter "GTX"). No Vulkan involvement in this run.
