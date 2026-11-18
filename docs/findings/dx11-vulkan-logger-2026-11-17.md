# Vulkan cross-backend logger — Phase 2b final report

**Date:** 2026-11-17 (runs in the afternoon CST; PC clock ~1 h behind CST)
**Runs:** Vulkan run 4 (PID 12352, swap 6000) — the relaunch Seth approved at ~14:43 CST.
**Verdict:** Step 1 census is formally verified (compare.py `--backend vulkan` **PASS**).
The guest **does** issue EDRAM-targeting world triangles on Vulkan — 261 in band 2
at swap 6000. **But the comparison premise is corrected, not confirmed** (see §0):
the guest also issues EDRAM triangles on D11 (thread t27292: 1,667), so the
"decisive divergence" as originally framed (Vulkan issues them, D11 never does)
is **refuted**. The two D11 census frames that showed zero are gating artefacts —
they captured composite presents, not band presents. The full Vulkan-probe +
translator build (Step 2) is **not** done, because the per-call comparison it
would feed cannot run on captures of different present types. What is delivered
instead: the verified Vulkan census, the corrected comparison, and the
compare.py Vulkan mode that makes future cross-backend comparisons formally
checkable.

---

## 0. Correction — read this first

The routing investigation (2026-11-16) concluded, for its captured frame,
"the guest never issues EDRAM-targeting triangles on D11" — carefully qualified
at the time as *for this frame only*. Phase 2b's framing generalised that into
"the guest never issues them on D11, so if Vulkan issues them, that is the
decisive backend divergence." **That generalisation is wrong.**

Evidence (all from `~/workspace/dx11-call-logger-scratch/logs4/`, the D11
run-4 log directory, which also holds rotated logs from earlier D11 runs):

- Thread **t27292**, a D11 run (2026-11-17 ~01:03–01:06 AM PC), issued
  **1,667 EDRAM-targeting triangles** (surf=`14020500`, prim 4/6/13, mode 4)
  across bands 0/1/2 (219 / 1,183 / 265), and the D11 backend **issued 3,242
  draws to the GPU** on that thread. The thread is definitively D11: 6,660
  "FH1 routing issue" lines, 3,242 "FH1 routing bound" lines, 15,097
  "FH1 resolve" lines, explicit "D3D11 FH1 native executor" / "D3D11 FH1
  untile copy-down" lines, zero Vulkan mentions.
- By contrast, **every single-present D11 frame** in the same logs has
  **zero** EDRAM triangles: t16804 / t25476 / t28624 / t8340 (129 draws each
  at swap 1500), t19856 / t21800 (56 each), t27932 (83), and t11284 (131 at
  swap 3000, the Phase 2 census frame). All are composite-heavy (87 of 129
  draws target surf=`14000500` on the 129-draw frames).

The t27292 data is an **accumulation, not a single present**: 3,418 guest draws
logged under one swap value (1500) over ~5 s (01:06:08–01:06:13 PC) while the
present counter was stuck — roughly 20–25 frames' worth, ~70–80 EDRAM triangles
per frame. The single-present frames are one present each.

**Revised understanding (§6):** a guest frame is split across several presents
— one per EDRAM band plus one composite present (banded copy-downs + post/HUD).
A single swap captures **one present**, not one guest frame. The D11 census
frames (swap 1500 routing, swap 3000 Phase 2) landed on **composite presents**
(copy-downs + composite, no world triangles). The Vulkan swap-6000 frame
landed on a **band-2 present** (world triangles, no composite). So the
difference between them is **present type, not backend**. The guest issues
EDRAM world triangles on both backends; the D11 census frames just did not
capture a band present.

Consequences:

1. The "decisive divergence" (Vulkan issues, D11 does not) is **not
   established**. Do not cite it.
2. The flat-grey world on D11 is **not** explained by "the guest never issues
   EDRAM triangles" — on D11 the guest does issue them (t27292) and the
   backend issues them to the GPU (3,242). The loss is **downstream of
   IssueDraw** (shader output, texture binding/fetch, or the EDRAM write
   itself), consistent with the earlier follow-up finding that the loss is in
   the 3D render into EDRAM. The next investigation should be what those
   issued EDRAM triangles' pixel shaders output, not whether the triangles
   are issued.
3. A valid per-call cross-backend comparison needs **the same present type on
   both backends** — a D11 band present (with EDRAM triangles) vs the Vulkan
   band-2 present — or a multi-swap capture (4+ consecutive swaps = one full
   guest frame). Neither exists yet. That is the blocker for the full Step 2
   translator, not a build problem.

---

## 1. Relaunch — run 4 (swap 6000, Vulkan)

Seth said "Relaunch" at ~14:43 CST after quitting the game himself (~14:29 CST);
the stuck run 3 was gone and a clean relaunch was possible.

### 1.1 Verification before interpreting anything (verify_diagnostic_setup skill)

- **Effective config / backend:** scheduled task "PinyonShift Test Run" (Ready)
  points at `E:\PinyonShift\work\.local\run_game.cmd`, line 3:
  `--gpu_backend=vulkan` with all 12 swap flags = 6000, zero 3000,
  `fh1_debug_callog_budget=1000000`, all probe flags present. `run_game_vulkan.cmd`
  was the stale 3000 version — re-gated to 6000 too (backup
  `run_game_vulkan.cmd.3000-20261117`; verified 12×6000, 0×3000, vulkan backend).
- **Binary check PASSES:** `pinyon_shift.exe` 61,851,136 B @ 11/17/2026 2:32:42 AM PC;
  `rexgpu-fh1.dll` 11,496,960 B @ 2:32:38 AM PC — exactly the Phase 2 build.
  Source older than binary: `d3d11/command_processor.cpp` 2:32:03 AM (39 s before
  the exe), shared `command_processor.cpp` 1:35:14 AM, `fh1_native_executor.cpp`
  1:35:24 AM. No separate Vulkan DLL in the out dir — the Vulkan executor is
  in the exe (run 1 proved it works on Vulkan with this binary). **No rebuild
  needed and none done** (no source changes since the Phase 2 build).
- **Game confirmed closed before launch** (no `pinyon_shift.exe` process).

### 1.2 Run 4 outcome

- Launched 1:45:36 PM PC via the scheduled task; process **PID 12352** started
  1:45:45 PM PC.
- **In the game, not stuck:** `music.in_game` fired at 20:47:05Z (1:47:05 PM PC,
  **80 s after launch**; run-1 baseline was 66 s). `save.backup` first at
  20:47:31Z, then every ~60 s. `step_gate` in-game signature confirmed:
  raises=0, holds=299, **swap_raises=298**, held_ms=7499.8.
- Free-roam performance (perf.csv): ~30 FPS, ~2,800–2,900 draw calls/frame
  (33,025 µs / 28 FPS / 2,896 draws; 33,212 µs / 30 FPS / 2,914 draws;
  33,164 µs / 30 FPS / 2,838 draws; 14,330 lines total).
- Session jsonl: `20261117T204546Z-p12352.jsonl` (21,486 B @ 1:52:06 PM).
- **Still running at the time of this report** (PID 12352, started 1:45:45 PM PC)
  — Seth is at the PC and the game was left alone; nothing was killed.
- **Probe output dirs show NO new files from run 4** (newest files all 2:36–2:41 AM,
  Phase 2 D11 runs). The file-based probes (compdump/c2probe/texdump/surfdump/
  guestprobe) are D11-executor code and do not fire on Vulkan, as expected.
  The resolve trace is likewise D11-only.

### 1.3 Swap-6000 burst captured

`runtime.log` rotated 3 times in a burst at ~1:47:48 PM PC (callog burst).
Collected `runtime.1.log` (5,242,755 B), `runtime.2.log` (5,242,789 B),
`runtime.3.log` (5,242,836 B) to
`~/workspace/dx11-vulkan-logger-scratch/logs-run4-6000/` (119,316 lines total).
They are consecutive chunks (runtime.3 ends 13:47:48.048 = runtime.2 starts;
runtime.2 ends 13:47:48.225 = runtime.1 starts). **Swap=6000 line counts:
114,465 total** (36,952 + 38,762 + 38,751). The last line of runtime.3.log is
truncated mid-line (mask cut at 15 chars) — one partial callog line lost at
the rotation boundary, not a routing line.

---

## 2. Step 1 — the swap-6000 census (Vulkan)

### 2.1 Guest side (the decisive data)

All 261 accepted draws are **pass=2, winy=-512, sel=0x30 (band 2), mode=4,
surf=14020500, d0=0x400**. Primitive mix: **224× prim=6 (TriangleStrip),
12× prim=4 (TriangleList, idx up to 10,419 — real world geometry),
25× prim=13 (QuadList)**. c0: 223× f3, 38× f12; c0 base always 0x00000000.

Routing stages agree exactly: **261 guest + 261 record + 261 dispatch = 783
lines, all thread t13516**. Callog independently confirms: all 6,161 packets
sel=0x30, pass=2; op=3F (DRAW_INDX) ×261; t3seq 29940583–29946743 consecutive.
Op mix: 0x60 (2,136), 0x2D (945), 0x61 (810), 0x22 (496), 0x27 (488), 0x2F (479),
0x55 (270), 0x3F (261), 0x5A (130), 0x46 (130), 0x3C (8), 0x3B (4), 0x2B (4).

### 2.2 Formal verification — compare.py `--backend vulkan` **PASS**

`~/workspace/dx11-call-logger-scratch/compare.py` was extended for Vulkan
(§7); run as:

```
python3 compare.py --backend vulkan \
  ~/workspace/dx11-vulkan-logger-scratch/logs-run4-6000 --swap 6000 --thread t13516
```

Exit 0, **VERDICT: PASS**. Full output saved as
`~/workspace/dx11-vulkan-logger-scratch/compare-vulkan-6000-run4.txt` (97 lines).

- **A2 packet census:** 6,161 Type-3 pkt lines, t3seq consecutive, no gaps. PASS.
- **A1 draw census:** BINPRED=496 (in=261, out=235), routing-guest=261,
  routing-record=261, routing-dispatch=261, routing-issue=0 (D11-only). PASS:
  BINPRED-total (496) == draw packets (496, op 0x22), and BINPRED-in (261) ==
  guest == record == dispatch (261).
- **A1 outcome:** dispatched=261; **unaccounted=0**. PASS.
- **A3:** mode==6 draws=0, copy=0 (rule holds vacuously); viz-blocked=0;
  guest-vs-dispatch order join **0 mismatches** (prim/mode/pass). The 235
  predicated-out draws are the bin-mask culls (§4), not flagged.
- **F1 pktpost:** 2,946/2,946 ok, 0 half-violations, carry-ok 2,945, carry-bad 0,
  missing 0. PASS.
- **F2 state-flow:** RB_COLOR_INFO == draw c0: **261/261**; RB_DEPTH_INFO ==
  draw d0: **261/261**; RB_SURFACE_INFO == draw surf: **261/261**. PASS.
- **F3 srv-bind:** 0 lines (D11-only instrumentation; no budget exhaustion).
- **F4 shader loads:** 492 lines (vs=246, ps=246, imm=4). A4 lines: 0 (D11-only;
  no coverage check on Vulkan).
- **F5:** no A9 present line (D11-only presenter record; the frame boundary is
  the callog swap gate). Callog budget not exhausted.
- **B1:** guest draws by band (winy): {512: 261}. Band-0 copy-downs: 0
  (expected 0 by design). No untileflush/copy-down lines (D11-only).
- **B3:** 0 dropped draws; no band-0 culling evidence.
- **Per-key inventory:** no bound keys (D11-only); guest surf keys:
  `14020500`: 261; guest (c0, d0): (0, 1024): 261.

D11 regression check (unchanged mode) still reproduces Phase 2 exactly:
`compare.py ~/workspace/dx11-call-logger-scratch/logs4 --swap 3000 --thread t11284`
→ exit 0, **PASS** (131 draws, rollup 52/31/48). Output saved as
`~/workspace/dx11-vulkan-logger-scratch/compare-d11-3000-run4-regression.txt`.

### 2.3 The capture is band-2 only (a partial guest frame, not a full one)

All 261 accepted draws are in pass 2 (band 2); there are no bands 0/1 and no
composite draws in this present. This does **not** weaken the Step 1 answer
(EDRAM triangles exist on Vulkan — bands 0/1 would only add more), but it
means the census is a **one-band present census**, not a full-frame census
like D11's 131-draw composite present. The reason is the present-type split
(§6): this swap landed on a band-2 present. Whether Vulkan presents once per
band or the probe window just caught one band is not distinguished by this
capture — but the band-2-only shape is fully consistent with a band present.

---

## 3. Surface-key decode — surf=14020500 is the EDRAM

`surf` in the guest line is `routing_regs.values[XE_GPU_REG_RB_SURFACE_INFO]`
(shared command_processor.cpp:3391–3401). The decode is **empirical** (the
RB_SURFACE_INFO bitfield layout was not fully decoded from xenos.h — the
struct is referenced by field name only there, and the time was spent on the
census instead), but it is confirmed on four independent points:

1. **D11 A#0** (the known EDRAM draw from the routing investigation — the only
   draw that binds 0/32/4x/c3, "FH1 draw RT key: 0/32/4x/c3", "D3D11 FH1 native
   surfaces: created 0/32/4x/c3 (1280x720x4 samples)") has surf=14020500,
   c0=0, d0=0x400. All 261 Vulkan draws match it on **every** field.
2. **d0=0x400 = 1024** matches the EDRAM depth surface's Describe() first field
   ("1024/32/4x/d1"). c0 base 0 matches the EDRAM colour Describe() first
   field ("0/32/4x/c3"). The D11 composite triangles have a different pair
   (surf=14000500, d0=0x2D0 = 720).
3. **Banded passes only occur for EDRAM-targeting draws; composite draws are
   pass -1.** All 261 Vulkan draws are in banded pass 2 — itself EDRAM evidence.
4. **F2 state-flow (261/261)** proves surf/c0/d0 are the live register values
   at each draw, not a stale or defaulted field.

Bit-level note (for completeness, not needed for the conclusion):
0x14020500 vs 0x14000500 differ in exactly one bit, **bit 17** (0x20000).
Both have pitch field 0x500 = 1280 (the low 14 bits, matching
kEdramPitchPixelsBits = 14). The exact field bit 17 names was not resolved
from source; the empirical mapping above is what the conclusion rests on.

The alternative (surf=14020500 is not the EDRAM) would require A#0's surf to
match by coincidence **and** all 261 Vulkan draws to coincidentally share that
surf/d0 **and** all be in a banded pass **and** include real triangle geometry,
while the D11 composite triangles use a different surf/d0. Not credible.

---

## 4. Predication — the 235 "predicated-out" draws are correct culling

compare.py's first (unextended) run on the Vulkan logs reported
"BINPRED 496 / guest 261" and flagged 235 predicated-out draws. That is a
D11-centric misread. The full picture:

- BINPRED logs every DRAW_INDX (op 0x22) packet the bin stage sees: **496**.
- **261 accepted** (out=false) — exactly the routing-guest count.
- **235 predicated out** (out=true) — all op 0x22, all sel=0x30, pass=2,
  **all with bin mask 0x0000000000000008** (bit 3 only).
- The accepted draws have masks 0xFFFFFFFF (224), 0x8000003F (26), 0x28 (11)
  — all overlapping the current bin select 0x30 (bits 4,5). The culled draws'
  mask (bit 3) does **not** overlap 0x30: bit 3 belongs to bin 3, part of
  band 1's select (0xC = bits 2,3). So the 235 are **band-1 draws correctly
  culled during the band-2 present** — banded tiling working as designed.

This is the same mechanism on D11 (the routing investigation exonerated
PredicatedOut there: 0 of 129 predicated out, because that frame's mask was
0xFFFFFFFFFFFFFFFF for every draw). The Vulkan present simply has a narrower
bin select, so the culling is visible. **Do not treat the 235 as dropped draws
or a divergence.** The extended compare.py reports them as a finding with the
mask breakdown and does not flag them.

---

## 5. D11 comparison — the t27292 refutation (the correction)

| | D11 census frames | D11 t27292 window | Vulkan swap 6000 |
|---|---|---|---|
| Thread / swap | t16804 etc. @1500; t11284 @3000 | t27292 @1500 (accumulated ~5 s) | t13516 @6000 |
| Guest draws | 129 / 131 | 3,418 | 261 accepted (+235 culled) |
| EDRAM triangles (surf 14020500, prim 4/6/13) | **0** | **1,667** | **261** |
| Present type | composite (copy-downs + post/HUD) | all presents accumulated | band 2 |
| Issued to GPU | 50 / 52 | 3,242 | n/a (no issued lines on Vulkan) |

t27292's EDRAM triangles by prim: TriangleStrip 909, QuadList 556,
TriangleList 202; all mode 4; prim-4 idx range 24–17,556 (large geometry).
By pass: 0: 219, 1: 1,183, 2: 265. The Vulkan band-2 draws are the same kind
of work (all mode 4, surf 14020500, prim 4/6/13, prim-4 idx 324–10,419);
the prim-mix proportions differ because t27292 spans ~20–25 frames across
all bands while Vulkan is one band's present, and the scenes/times differ
(Vulkan band 2 has 261 draws vs D11's ~22–28 EDRAM triangles per band per
frame — a count difference noted as an observation, not a divergence, since
the scenes differ).

**Bottom line:** the guest issues EDRAM world triangles on **both** backends.
The D11 census frames showing zero are the wrong present type for the
question, not evidence the backend cannot receive them.

---

## 6. Why the census frames differ — present-type hypothesis

The coherent explanation for all the evidence:

- A guest frame is split across several presents: one per EDRAM band (world
  triangles) plus one composite present (banded copy-downs, mode 6, + post/HUD).
- A single swap value captures **one present**. The tile-trace / callog gate
  fires at one swap, so a single run captures one present type.
- The D11 gates (swap 1500, swap 3000) landed on **composite presents** (hence
  0 EDRAM triangles, 12 banded draws that are all mode-6 copies, 119 composite
  draws). The Vulkan gate (swap 6000) landed on a **band-2 present** (hence
  261 EDRAM triangles, 0 composite).
- The t27292 window accumulated all present types because the present counter
  was stuck at 1500 for ~5 s, so draws from many presents were all logged
  under swap=1500 — which is why it is the only D11 source showing EDRAM
  triangles.

This hypothesis explains the counts, the prim mixes, and why the D11 census
frames are uniformly composite-heavy while t27292 is not. It is a hypothesis
about **gating/present alignment**, not about the backend dropping draws —
and it is directly testable: gate a D11 run to a swap that lands on a band
present (or capture 4+ consecutive swaps and combine them into one guest
frame) and the EDRAM triangles should appear on D11 in a single present.

**Seth's question "do we have to do frame 6000?":** the decisive frame must be
a **free-roam frame on Vulkan (≥ ~frame 5400)** — 6000 is the chosen round
number in free roam, not magic. But the new finding adds a qualifier: a
single swap captures one present, so to compare world rendering you want a
**band present** on both backends (or a multi-swap capture covering a full
guest frame). Switching the Vulkan swap number now would need re-gating and
re-running, and would not by itself fix the comparison — the D11 side needs
the matching present type first.

---

## 7. Step 2 — what was built, what was not, and why

### 7.1 Built: compare.py Vulkan mode + F4 fix

`~/workspace/dx11-call-logger-scratch/compare.py` (1,167 → 1,315 lines) was
extended; the full diff is
`~/workspace/dx11-vulkan-logger-probe-diff-2026-11-17.txt` (314 lines).
Backup of the D11 version:
`~/workspace/dx11-call-logger-scratch/compare.py.bak-d11-20261117`.
**No PC source was changed in Phase 2b; no rebuild was done.** The diff is
compare.py only.

Changes:

- `--backend {d11,vulkan}` (default `d11`). D11 mode is unchanged — the Phase 2
  PASS reproduces exactly (regression run above).
- **Dispatch parsing:** new `RE_ROUTING_DISPATCH` + `rdispatch` list. The
  dispatch line carries no `idx`, so the guest-vs-dispatch order join compares
  (prim, mode, pass) only. Dispatch is the third routing stage (guest →
  record → dispatch), logged by the shared command processor, so it fires on
  both backends; on Vulkan it is the last routing stage and therefore the
  outcome event.
- **A1 census (Vulkan):** agreement is BINPRED-total == draw packets AND
  BINPRED-in == guest == record == dispatch. The predicated-out (mask-culled)
  count is reported with its mask breakdown as a finding, not flagged.
- **A1 outcome (Vulkan):** unaccounted = guest − dispatch (no stage-D
  issued/copy/drop records exist on Vulkan); the D11 baseline comparison and
  callog cross-checks are skipped in Vulkan mode.
- **A3 (Vulkan):** predicated-out draws are not flagged (mask culls, §4);
  order join is guest-vs-dispatch.
- **F4:** the A4 coverage check is skipped on Vulkan (A4 is D11-only); plus
  the **F4 closeout one-line fix** (applies to both modes): A4 lines with
  `pvalid==-1` (pixel shader unbound at ConfigurePipeline — the mode-5
  depth-only draws) are now counted and reported as a finding. The D11
  regression run shows it working: "2 A4 lines with pvalid==-1 … consistent
  with depth-only draws" (exactly the two mode-5 draws from the F4 closeout).
- **F5 (Vulkan):** a missing A9 present line is reported, not failed (the
  presenter record is D11-only; the frame boundary is the callog swap gate).
- **Per-key inventory (Vulkan):** guest surf keys and (c0, d0) bases are
  reported, since bound keys are D11-only.

### 7.2 Not built: Vulkan backend probes + translator (Step 2, blocked)

The Vulkan executor currently emits **no** issue/issued/bound/resolve lines
(the D11 command_processor.cpp sites do not exist in the Vulkan executor).
Building those probes plus the translation layer (Vulkan logs → D11
equivalents) was Step 2's core. It is **not done**, for a reason that is now
evident: the per-call comparison it would feed needs **the same present type
on both backends**, and the captures in hand are different present types
(D11 composite presents vs Vulkan band-2 present). Building the probes now
would produce a comparison of a composite present against a band present —
a category error, not a backend divergence. The correct order is:

1. Capture a **D11 band present** (gate the D11 probe to a swap that lands on
   a band present, or extend the probe to fire across 4+ consecutive swaps so
   one run covers a full guest frame). Confirm EDRAM triangles appear on D11
   in a single present (t27292 already proves they exist on D11 in aggregate;
   a single-present capture makes the comparison like-for-like).
2. Then build the Vulkan issue/issued/bound/resolve probes + translator and
   run the per-call comparison between the D11 band present and the Vulkan
   band present.

The compare.py Vulkan mode delivered here is the prerequisite for step 2 —
it makes the Vulkan census formally checkable and gives the translator a
verified input format (guest/record/dispatch + callog pkt/reg, with the
dispatch stage now parsed).

### 7.3 The translator (design §9) — status

Specified, **not implemented**. The design's assumption that the two captures
would be "the same frame" does not hold for free-roam captures at different
swaps/times (different present types, different scenes). The translator's
input contract is now concrete (Vulkan guest/record/dispatch + callog, same
surface keys, same draw-accounting fields, same outcome categories — the
dispatch stage gives the per-draw outcome on Vulkan), but the comparison it
feeds needs like-for-like presents first (§7.2).

---

## 8. What this means for the render bug

- The **routing/predication/classification is exonerated on both backends**
  for the frames captured: on Vulkan, every accepted draw is accounted for
  (261/261, unaccounted=0, order join 0 mismatches, F1/F2 pass); the
  predication correctly culls other-band draws by mask. On D11, the backend
  issues EDRAM triangles to the GPU (t27292: 3,242 issued) — it does not drop
  them at the routing stage.
- The flat-grey world on D11 is therefore **not a routing problem**. The
  triangles reach the GPU on D11, yet the EDRAM ends up flat grey (66,66,66).
  The loss is in what those triangles **do** once issued — pixel-shader output,
  texture fetch/binding, or the EDRAM write. This is the same direction the
  follow-up investigation already pointed ("loss is in the 3D render into
  EDRAM"); the routing question is now closed as a cause, on both backends.
- The **#23 wipe** and the **c2 bottom-half anomaly** are unchanged by this
  report (separate threads; #23 is a composite-present draw, and the Vulkan
  band-2 present has no composite draws, so it does not bear on #23).
- **NaN-in-bloom** (Corrado car-preview black box) is still live as a
  follow-up probe idea, not addressed here.
- **WaitForGpu fix** remains queued behind Phase 2b — untouched, as instructed.
  The EXE is the Phase 2 build (2:32:42 AM PC); it is **not** stale relative
  to the current source (no source changes since that build).

---

## 9. Open questions / next steps

1. **Capture a D11 band present** (the immediate next step for a valid
   comparison): gate the D11 probe to a swap that lands on a band present, or
   extend the probe to a multi-swap range. Confirm EDRAM triangles in a
   single D11 present, then run the per-call comparison against the Vulkan
   band-2 present using the compare.py Vulkan mode.
2. **EDRAM pixel content on D11 when triangles are issued:** does the t27292
   D11 run's EDRAM contain world content, or stay flat grey despite 1,667
   issued EDRAM triangles? A resolve-source probe on a D11 band present
   answers it. If it stays grey, the loss is definitively in the shader/
   texture path of the issued triangles (not the routing).
3. **Vulkan probes (Step 2, after 1):** add issue/issued/bound/resolve
   records to the Vulkan executor, then the translator (Vulkan logs → D11
   equivalents) and the per-call comparison.
4. **Present-type confirmation:** verify the "one present per band + one
   composite present" framing directly (e.g., by logging the present counter
   against the band/composite content over consecutive swaps).
5. **Seth's open question:** "do we have to do frame 6000?" — no, 6000 is not
   magic; any free-roam Vulkan frame (≥ ~5400) works, but the present type
   matters more than the number (§6).

---

## 10. Deliverables and state

- **This report:** `~/workspace/dx11-vulkan-logger-2026-11-17.md`
- **Probe diff (compare.py only; no PC source changed):**
  `~/workspace/dx11-vulkan-logger-probe-diff-2026-11-17.txt` (314 lines)
- **compare.py (extended):** `~/workspace/dx11-call-logger-scratch/compare.py`
  (backup of the D11 version:
  `~/workspace/dx11-call-logger-scratch/compare.py.bak-d11-20261117`)
- **Vulkan compare output (PASS):**
  `~/workspace/dx11-vulkan-logger-scratch/compare-vulkan-6000-run4.txt`
- **D11 regression output (PASS, reproduces Phase 2):**
  `~/workspace/dx11-vulkan-logger-scratch/compare-d11-3000-run4-regression.txt`
- **Vulkan logs (run 4, swap 6000):**
  `~/workspace/dx11-vulkan-logger-scratch/logs-run4-6000/` (runtime.1/2/3.log,
  119,316 lines; swap-6000 lines 114,465)
- **Scratch (scripts + earlier runs):** `~/workspace/dx11-vulkan-logger-scratch/`
  (check-state-relauch.ps1, relaunch-run4.ps1, collect-run4-logs.ps1,
  find-surfkey.ps1, find-rb-surfinfo.ps1, read-rb-struct.ps1, find-surf-fields.ps1,
  restore-d11-cmd.ps1, vulkan-run1-runtime.log, vulkan-run1-only.log,
  d11-run4-routing-guest-3000.txt, p17348/p9184/p15100 jsonl + perf.csv, src/)
- **Design doc §9:** updated with what was actually implemented vs specified
  and the corrections found (see the design doc; the §9.4 "incomparable"
  note now has a mechanism — present type — and the translator's input
  contract is fixed by the dispatch stage).
- **`run_game.cmd` restored to D11** from `run_game.cmd.d11-20261117`
  (verified after restore: `--gpu_backend=d3d11 --d3d11_adapter=GTX`,
  swap flags 3000, budget 200000). The game (run 4, PID 12352) was still
  running on Vulkan at restore time; the restore only affects the next
  launch (the cmd file is read at launch). `run_game_vulkan.cmd` remains at
  the re-gated 6000 version (it is the Vulkan cmd; its 3000 backup is
  `run_game_vulkan.cmd.3000-20261117`).

### Rules preserved (from Phase 2)

- Band-0 copy-down is expected-ZERO by design (confirmed again: 0 in the
  Vulkan frame; the per-band fix's `untile_band != 0` guard stands).
- #23 is a TRIANGLELIST (prim 4, 48 verts), not a RECTLIST — unchanged.
- The 48 single-point drops are a baseline pattern, not culling — unchanged
  (the Vulkan frame has no single-point draws; its 235 predicated-out draws
  are mask-based band culls, §4, a different mechanism).
- Small/EDRAM-height scissors are legitimate — unchanged.
- No rendering changes, no refactor, no WaitForGpu edits, no expansion into a
  full API interception layer. Diagnostic-only throughout.

### Binary / config (per the verify_diagnostic_setup skill)

- Binary: exe 61,851,136 B @ 2:32:42 AM PC, dll 11,496,960 B @ 2:32:38 AM PC
  (Phase 2 build) — matches source, no rebuild needed, none done.
- Effective config: `--gpu_backend=vulkan`, all 12 swap flags 6000, callog
  budget 1,000,000 (verified from the runtime report / run_game.cmd).
- Diagnostic files: the three rotated runtime logs are current (burst
  13:47:47.869–13:47:48.429 PM PC, matching run 4's swap-6000 present),
  non-empty (5.24 MB each), and attributed by run ID (thread t13516, PID 12352).
  One partial callog line was lost at a rotation boundary (noted in §1.3);
  it does not affect any census count (all three routing stages and the
  packet census agree independently).
- Adapters: Vulkan run used the **RX 5700 XT** (AMD Radeon RX 5700 XT, vendor
  0x1002, device 0x731F; NVIDIA driver removed by the loader — the GTX 570
  has no Vulkan driver). D11 runs used the **GTX 570** (adapter "GTX").
