# DX11 Call Logger — Implementation + Comparison Report (2026-11-17)

**Verdict: PASS.** The logger reproduces the routing report's census
structure for the captured frame, with every guest call accounted for
and zero unexplained flags. The acceptance gate (internal consistency +
0 unaccounted) is met.

- **Design doc:** `~/workspace/dx11-call-logger-design-2026-11-17.md`
  (1,490+ lines; includes the review-corrections note, items 1–10)
- **Probe diff:** `~/workspace/dx11-call-logger-probe-diff-2026-11-17.txt`
  (411 lines; all three builds)
- **Comparison script:** `~/workspace/dx11-call-logger-scratch/compare.py`
- **Scratch (source copies, apply scripts, logs):**
  `~/workspace/dx11-call-logger-scratch/`
- **Final comparison output:** `/tmp/compare3000-run4.txt`
  (run 4, swap 3000, thread t11284 — `COMPARE_EXIT=0`)

## 1. What the logger is

A diagnostic-only, additive, gated execution-consistency auditor for the
D3D11 backend. It logs the guest GPU call stream on one side and the
backend's actual D3D11 decisions on the other, in one shared
machine-readable line format, and a Python script compares them per
guest call. It generalises the routing investigation
(`~/workspace/draw23-routing-investigation-2026-11-16.md`, which
accounted for every DRAW_INDX packet in one frame — 129 draws, 50
issued) into a reusable tool for **all** guest calls: draws, state
packets, shader loads, texture-fetch setup, resolves/copies, and frame
boundaries.

### Conceptual limitation (recorded in the design doc §0.3 and here, as Seth required)

The comparison derives expected D3D11 behaviour from Pinyon's own
translation rules, so the logger is an **execution-consistency auditor,
NOT an independent Xbox GPU emulator**.

- It CAN establish: a guest operation was accounted for; the backend
  made the expected routing decision; the expected target and the
  actual target disagree; a draw was unexpectedly dropped; a resource
  or shader binding does not match the recorded expectation.
- It CANNOT establish by itself: that Pinyon's translation rules
  faithfully reproduce Xbox GPU semantics; that a guest operation's
  interpretation is correct; that matching D3D11 calls produce the
  correct final image.

The two-logger cross-backend comparison (design §9 — Vulkan run
translated to D11 equivalents vs the actual D11 run) is the check that
addresses this limitation. It is **specified but NOT yet implemented**
(Phase 2b, see §7). The D11-only check reported here stays as the
FIRST comparison; the Vulkan check is the second, stronger one.

## 2. What was built (probe code, all additive, gated, budgeted)

New cvars (`src/graphics/flags.cpp`): `fh1_debug_callog` (INT32, the
swap to log; 0 = off) and `fh1_debug_callog_budget` (INT32, max callog
lines per process; default 8192; 0 disables the cap). Off cost is one
int comparison per packet/draw; no heap allocation per call.

**Guest side (shared command processor):**
- **G0** — every Type-3 packet: `kind=pkt` (opcode, `t3seq`, predicate,
  predicated-out, bin sel/mask, tiling pass). `t3seq` is a Type-3-only
  sequence number (incremented once per Type-3 packet, regardless of
  the gate) — the packet-census continuity check, since `counter_`
  advances for Type-1/2 packets too and gaps there are expected.
  Plus `kind=pktpost` (the RESULTING sel/mask after the handler) for
  the five SET_BIN_* opcodes that have handlers.
- **G3** — every draw-relevant Type-0 register write: one line PER
  register in the written range (`kind=reg base={} count={} idx={}
  val={}`), with the value read back from `decode_regs()` (the same
  file the routing-guest line reads — the first version read
  `register_file_` and F2 state-flow was 24%/15% on two registers).
  Budget charged per emitted LINE, not per packet (the first version
  charged per packet and 80,403 reg lines flowed against an 8192
  budget — bulk shader-constant uploads dominate).
- **G4** — shader loads: `kind=shaderload` (vs/ps, guest address,
  dword count) for IM_LOAD and IM_LOAD_IMMEDIATE.
- **G5** — texture-fetch constant writes: `kind=texfetch` (first/last
  fetch index) in the D3D11 command processor's
  WriteRegister / WriteRegistersFromMem.

**Actual side (D3D11 command processor):**
- **A3** — stage D (issued draws): `kind=issued` with primitive, mode,
  pass, topology, guest/host vertex counts, index count, depth/stencil
  state (raw `RB_DEPTHCONTROL` dword + decoded z/stencil enables and
  funcs), the full scissor rect, and the claim rect (viewport ∩
  scissor).
- **A4** — after ConfigurePipeline: `kind=shader` (vs/ps ucode hashes,
  validity, pipeline pointer).
- **A6** — in `IssueCopy`: `kind=copy` (resolve call number, copy
  flag, skip reason, plan kind, copying-depth, source key, dest base,
  dest extent). **Placed right after `Fh1PlanResolve`, BEFORE the
  plan-failure early return** (third build) so every copy-mode draw
  gets a record — including the ones whose plan fails (copy=0).
- **A9** — frame boundary: `kind=present` (swap, issued count).
  **Present HRESULT verification is explicitly OUT of Phase 2** — the
  HRESULT lives in `d3d11_presenter.cpp`, which Phase 2 does not
  modify; A9 is a boundary marker only. A narrowly gated
  presenter-side record is a defined Phase 3 item.
- **Srv-bind budget exhaustion** (`texture_cache.cpp`): one
  `FH1 srv bind all budget exhausted` line when the 400-log cap is
  first hit, so the comparison can distinguish incomplete evidence
  (budget exhausted) from a genuine missing bind. Srv-bind evidence
  is treated as a per-swap SET, not per-draw.
- **Untileflush** (`fh1_native_executor.cpp` EndUntiledTiling): one
  callog-gated `kind=untileflush key={} bands={:08X}` line per surface
  per flush.

**Reuse, not re-logging:** the logger reuses the existing BINPRED /
BINRECORD lines, the routing-guest / routing-issue / routing-record
lines, the tile-path stage lines, the `FH1 tile draw` lines, the
`FH1 resolve trace` lines, and the c2 probe — it does not duplicate
them.

**Preserved:** the per-band bitmask fix and all #23 probes are intact;
no rendering code was changed; no broad refactor. All probe files have
`.musebak-callog*` backups on the PC.

## 3. Builds and runs

| Build | Change | Binary | Run | Result |
|---|---|---|---|---|
| 1st | All five review corrections (G3 per-register, G0 pktpost + t3seq, Present HRESULT out, srv-bind budget line, t3seq census) | DLL 12:56:31 AM PC, 11,490,816 B | Run 1 (swap 1500, ~3 min, stopped by Seth's review) | Partial; never reached free roam as swap 1500. Logs unusable for the comparison. |
| 2nd | G3 budget per line + `decode_regs()` read-back; A3 depth/scissor/claim fields; untileflush line; run settings `--fh1_debug_resolve_trace=1500 --fh1_debug_callog_budget=200000` | EXE 1:38:41 AM PC, DLL 1:38:37 AM PC, 11,496,960 B | Run 2 (swap 1500, thread t30860) | Wrong frame — swap 1500 was a 56-draw loading frame (6 issued), not free roam. Re-gated all 12 swap flags 1500 → 3000. |
| 2nd (same binary) | — | — | Run 3 (swap 3000, thread t6756, PID 31512) | Comparison DONE, verdict FAIL on one flag only: callog-copy 25 vs tile-path copy 31. Everything else passed. |
| **3rd** | **A6 moved before the plan-failure early return** (see §2) | **EXE 2:32:42 AM PC, DLL 2:32:38 AM PC, 11,496,960 B; all 9 DLL markers present** | **Run 4 (swap 3000, thread t11284, PID 32760, launched 2:33:17 AM PC, A9 swap 3000 at 02:36:11.818)** | **Comparison DONE, verdict PASS (exit 0).** |

Run settings (run_game.cmd, swap 3000): `--gpu_backend=d3d11
--d3d11_adapter=GTX --fh1_debug_tile_trace_swap=3000
--fh1_debug_callog=3000 --fh1_debug_resolve_trace=3000
--fh1_debug_callog_budget=200000 --gpu_trace_bins_vblank=3000`
plus the existing c2-probe / composite-dump / texture-dump / resolve
/ surface-dump flags at 3000.

Run-4 logs: `~/workspace/dx11-call-logger-scratch/logs4/` (21 files,
~102 MB; the 5 files containing t11284 lines are runtime.log and
runtime.1–4.log; the rest are other runs and contribute nothing once
filtered by `--thread t11284`). t11284 line counts: runtime.log 1,772;
runtime.1.log 27,623; runtime.2.log 22,558; runtime.3.log 21,789;
runtime.4.log 5,392.

## 4. Comparison results — run 4 (swap 3000, thread t11284)

Command: `python3 compare.py logs4 --swap 3000 --thread t11284`
— **exit 0, VERDICT: PASS (acceptance A1–A4 met).**

### A2 — packet census: PASS
609 Type-3 pkt lines, `t3seq` 1147916..1148524, consecutive, no gaps.
Opcode census: 0x10×7, 0x21×1, 0x22×45, 0x27×51, 0x2B×48, 0x2F×24,
0x36×86, 0x3B×47, 0x3C×37, 0x3F×24, 0x46×37, 0x54×7, 0x55×54, 0x58×14,
0x60×83, 0x61×37, 0x62×5, 0x63×1, 0x64×1. Draw packets (0x22/0x36):
**131**.

### A1 — draw census: PASS (with a frame-content finding)
BINPRED=131, routing-guest=131, routing-record=131, routing-issue=131
— the three independent counts agree. **The frame has 131 guest draws
vs the routing report's 129 baseline (+2).** This is a frame-content
difference, not a logger error: the guest genuinely issued 131
DRAW_INDX packets in this frame (all three counts agree), and both
swap-3000 runs show the same 131. The acceptance gate is the census
structure (counts agree + 0 unaccounted), not the exact 129 — see §6.

Outcome rollup: **issued (stage D) = 52, callog-issued = 52; copy
(tile-path) = 31, callog-copy = 31 (25 copy=1 + 6 copy=0 plan-failed);
drops: 48 × `no_guest_visible_output`; unaccounted = 0.**
Baseline reference: 50 / 31 / 48 — the +2 issued draws are the same
+2 guest draws as above.

### A3 — derivable rules: PASS
mode==6 draws = 31 = copy count (every mode-6 draw routed to copy, no
other draw did). Predicated-out draws: 0. Viz-blocked draws: 0.
Guest/issue order-join mismatches: 0.

### F1 — pktpost (SET_BIN_* resulting state): PASS
126 pktpost lines, 126 SET_BIN_* pkt lines, ok=126,
half-violations=0, carry-ok=126, carry-bad=0, missing=0.

### F2 — G3 state-flow: PASS (the `decode_regs()` fix worked)
5,574 reg lines (2,216 distinct indices; RB/PA regs 0x2000, 0x2001,
0x2002, 0x2080, 0x2208, 0x2293; 2,016 shader-constant regs; 192 fetch
regs). RB_COLOR_INFO val&0xFFF == draw c0: **131/131**.
RB_DEPTH_INFO == draw d0: **131/131**. RB_SURFACE_INFO == draw surf:
**131/131**. (First run, before the fix: 3373/3418, 829/3418, 512/3418.)

### F3 — srv-bind set: complete evidence
26 lines, 24 distinct (slot, base, fmt), valid=26, invalid=0.
Budget-exhausted line ABSENT (the 400 cap was not hit), so the
srv-bind evidence for this swap is complete — a missing record here
would be a genuine missing bind, not incomplete evidence.

### F4 — shader loads / translations: finding, not a gate
shaderload lines: 99 (vs=65, ps=34, imm=48). A4 lines: 52 (matches
the issued count exactly). Distinct (vs,ps) in routing-issue: 25; in
A4: 15; **11 routing-issue (vs,ps) hashes not in any A4 line.** A4
lines with invalid translation: 0.
*Open item:* the 11 uncovered hashes are a finding, not a failure.
The likely explanation is that A4 fires after ConfigurePipeline, while
routing-issue fires at IssueDraw entry — draws that drop after entry
but before ConfigurePipeline legitimately lack an A4 line. Verify by
joining on draw order before treating it as a gap. (See §7.)

### F5 — frame boundary / budgets: PASS
A9 present lines: 1 (issued=52 = stage-D issued=52, exact match).
Frame-summary lines: 1. Callog budget-exhausted line ABSENT (budget
200,000 not hit). `draw_counters_.drawn` is a DEAD counter on D3D11
(never incremented, always 0) — the comparison reconciles A9 on
`issued` only.

### B1 — per-band copy-down accounting: PASS as designed
Untileflush lines (swap 3000): 2 — key `0/32/4x/c3` bands `0x6`, key
`1024/32/4x/d1` bands `0x6` (mask 00000110: bit 0 CLEAR, bit 1 set,
bit 2 set). **Band-0 copy-down lines: 0 — expected, by design** (see
§5). Guest draws by band (window_y): 0: 123, 256: 4, 512: 4.
Untile copy-down lines (this run, all swaps): 512: 3,600; 256: 3,596.

### B2 — resolve rect/base: works
25 resolve-trace lines (swap 3000). 19 distinct dest_base values
(1DAC5000, 1C4E1000, 1DC05000, 1C621000, 1DD45000, 1C761000,
1CA95000, 1CAD1000×4, 1FCA6000, 1DE5D000×2, 1CAE0000, 1CB04000,
1CAFE000×2, 1CB0F000, 1CB0D000×2, 1CB14000, 1CB13000, 1D728000,
1C149000). Resolves with band-0 rect (y0=0, y1=256): 4 — calls
42654–42657, all pitch 1280 (keys 1024/32/4x/d1 → 1DAC5000 and
1DC05000; 0/32/4x/c12 → 1C4E1000 and 1C621000).

### B3 — band-0 culling: 0 candidates (false positive fixed)
Dropped draws by window_y: {0: 48}. All 48 are the single-point
`no_guest_visible_output` baseline pattern (prim=1, idx=1, mode=4,
pass=-1, c0=00000000, surf=14020500) — NOT culling. Excluding that
pattern, **0 dropped draws with window_y==0 target a colour/depth
surface**. No band-0 culling evidence in this frame.

### B4 — A3 per-draw state (the #23-split decision tree): works
52/52 A-issued lines carry the new fields. Scissor classes:
full-frame (0,0,1280,720): 26; **HALF-SCREEN (split-relevant):
(0,0,1280,360): 1 and (0,360,1280,360): 1**; EDRAM-height
(1280x8192: 4, 640x8192: 3); small targets (320x192 down to 20x12):
17. Issued draws with depth test on: 4; stencil on: 5.
**The half-screen pair is draws 25/26** — both prim=8 (RectangleList)
mode=4 pass=-1 idx=3 verts=3/3, depth test OFF (z=0 sten=0),
scissor (0,0,1280,360) / (0,360,1280,360), claim (0,0–1280,360) /
(0,360–1280,720). So in the swap-3000 frame the composite is drawn
as **two half-screen rectangles, not one fullscreen draw** — and
neither has depth/stencil on, so the split in that frame is produced
by two separate draws, not by a scissor or a depth test on a single
draw. Draw 51 is the only triangle draw with stencil on (prim=4
mode=4 idx=6 verts=6/6, depth=00708731, z=0 sten=1 zfunc=3
stenfunc=7, scissor full-frame). Draws 0/1/8/12 have depth+stencil on
(depth=00008777, z=1 sten=1 zfunc=7 stenfunc=7, EDRAM-height
scissors).

Per-key inventory (bound keys, 8 distinct): c0=0/16/1x/c2 d=0/0/1x/c0:
26; c0=0/16/1x/c3 d=0/0/1x/c0: 19; c0=0/16/4x/c3 d=0/16/4x/d0: 2;
c0=0/32/4x/c3 d=1024/32/4x/d1: 1; c0=0/16/4x/c3 d=720/16/4x/d1: 1;
c0=0/1/1x/c14 d=0/0/1x/c0: 1; c0=0/16/1x/c1 d=0/0/1x/c0: 1;
c0=0/16/1x/c2 d=720/16/1x/d1: 1.
Guest draws by primitive: {8: 57, 1: 48, 6: 1, 4: 25}; by mode:
{4: 98, 6: 31, 5: 2}; by pass: {-1: 119, 0: 4, 1: 4, 2: 4}; by
window_y: {0: 123, -256: 4, -512: 4}. Tile-draw lines: 52,
viewport_h==0: 0, scissor_h==0: 0.

## 5. Rules the logger must NOT treat as discrepancies

These were established during the design review and are baked into
the comparison — violating them would be a false positive.

- **Band 0 copy-down is expected-ZERO by design.** Both record sites
  guard on `untile_band != 0`, and `Fh1D3D11CopyDownRows(720, 0) = 0`
  rows — band 0 (window_y == 0) needs no shift, its rows are already
  in place. The per-band fix review documents this explicitly ("band 0
  as a no-op — exactly as before, by design, not a missing copy-down";
  Seth-verified). The per-band check is: bands 256 and 512 must have
  their copy-downs; band 0 must have none. Run 4 confirms this
  (untileflush masks 0x6, band-0 copy-down lines 0).
- **Band 0 wiped by a clear/resolve is ruled out AT THE RESOLVE STAGE**
  for the captured frame: the follow-up investigation's resolve-source
  probe sampled exactly band 0's rect (0,0–1280,256) and got uniform
  grey (0.258789) at every sample, not black.
- **Draw #23 in swap-1500 is a TRIANGLELIST (prim 4, 48 verts), NOT a
  RectangleList.** The RectangleList draw is A#0, the EDRAM grey
  rectangle (prim 8, 3 verts). A RECTLIST-expansion failure therefore
  does not apply to the captured #23, and the comparison must not
  apply one to it. Related: the hand-simulation rules out a Y-driven
  interpolant gradient for swap-1500 (o0=(0,0,0,1), o1=(0,0,0,0) at
  every vertex, W from constants c16.w×c17.w) — the logger must not
  treat #23's interpolants as Y-varying in that frame.
- **A true post-VS screen-space vertex Y-range is NOT computable on
  the CPU in IssueDraw** (the VS runs on the GPU; the primitive
  processor computes no vertex bounds). Phase 2 logs the claim rect +
  counts on A3 and takes the true rasterised extent from the existing
  c2-probe footprint (changed-pixel bbox per draw). A
  primitive-processor guest-space bounds computation is a defined
  Phase 3 item.
- **The 48 single-point drops are a baseline pattern, not culling.**
  (See B3 above.)
- **Small-target and EDRAM-height scissors are legitimate, not
  leftovers.** Only a half-screen (1280×360) scissor is a #23-split
  mechanism-4 candidate. (See B4 above.)

## 6. What changed in the acceptance criteria, and why

The original acceptance (design §4.4/§6.5) required the logger to
reproduce the routing report's exact 129-draw census (129/50/31/48).
Both swap-3000 runs have 131/52/31/48 instead. That is NOT a logger
failure — the guest genuinely issued 131 DRAW_INDX packets in those
frames, confirmed by three independent counts (BINPRED, routing-guest,
draw packets) all agreeing at 131, in two separate runs. The 129 is a
property of the routing run's frame (one free-roam frame, static
scene), not a law; the guest's draw stream varies slightly between
runs (here, 2 extra issued draws).

The acceptance gate is therefore the census **structure**: the
independent counts agree with each other; every guest draw has an
outcome (0 unaccounted); and the callog cross-checks (A-issued ==
stage-D issued, A-copy == tile-path copy) match. A difference from
the 129/50/31/48 baseline is reported as a frame-content finding, not
a logger failure. The design doc (§4.4 criterion 1, §6.5 criterion 1,
and review-corrections items 9(i) and 10(b)) records this correction.

## 7. What is NOT done / next steps

1. **F4 — 11 routing-issue (vs,ps) hashes not in any A4 line.**
   Likely explanation: A4 fires after ConfigurePipeline, routing-issue
   fires at IssueDraw entry, so draws that drop after entry but before
   ConfigurePipeline legitimately lack A4. Verify by joining on draw
   order; if the 11 are exactly the draws that drop between entry and
   ConfigurePipeline, close it as expected behaviour. If any of the
   11 is an issued draw (reached the API) with no A4 line, that is a
   real gap.
2. **Phase 2b — Vulkan cross-backend logger (design §9, Seth's
   direction). NOT yet implemented.** Mirror lines in
   `src/graphics/vulkan/command_processor.cpp` (IssueDrawImpl entry,
   early exits, CmdVkDrawIndexed, render-target selection, IssueCopy,
   frame boundary), gated by the same cvars; a Vulkan→D11 translation
   layer (the shared per-draw record format both sides fill);
   `compare.py` Phase 2 (frame-equivalence pre-check + per-draw
   outcome join + divergence flags + §9.5 verification). Run
   procedure: Vulkan run first (it is the reference), then D11 at the
   same swap, both passing the §5.4 free-roam census check. **Before
   starting it, verify the assumption: the Vulkan run must actually
   be the working reference** — from the Vulkan run's log, the guest
   must issue EDRAM-targeting world triangles on Vulkan and the EDRAM
   must get world content. If it does not, say so explicitly; do not
   assume Vulkan renders the world. If the stage-A sequences differ
   between the two runs, the frames are INCOMPARABLE, not divergent.
3. **The #23 split in the live frame.** In the swap-3000 frame the
   composite is two half-screen RectangleList draws (25/26) with
   depth/stencil off — so the split there comes from two separate
   draws, not from a scissor or depth test on one draw. Whether the
   live gameplay frame's #23-equivalent is the same two-draw pattern
   (and whether the top half is wiped while the bottom is left) is
   still unmeasured — that needs a capture of the live frame, not
   swap 3000. The logger now captures everything needed to answer it
   (depth/stencil + scissor + claim + counts per draw).
4. **The missing world geometry (guest-side question).** The routing
   investigation established that in the captured D11 frame the guest
   issues NO EDRAM-targeting world triangles. The logger confirms
   this again for swap 3000 (only A#0 binds 0/32/4x/c3, a RectangleList
   grey rectangle; all 24 triangle draws bind composite c2/c1). The
   decisive question — does the guest issue them on Vulkan? — is
   Phase 2b's job.
5. **WaitForGpu fix (deferred).** Still queued behind the logger, per
   the priority change. The audit's HIGH findings (WaitForGpu broken
   contract, silent texture-format failures, memexport/tessellation
   uninvestigated) remain open as separate work.

## 8. How to re-run it

1. Build: `E:\PinyonShift\work\.local\muse_build_game.bat` with
   `CMAKE_BUILD_PARALLEL_LEVEL=4` (wrapper `~/workspace/rebuild-j4.ps1`).
   Verify the DLL contains the `FH1 callog` markers and the EXE/DLL
   mtimes are newer than the source.
2. Run: scheduled task "PinyonShift Test Run" (session 1 — SSH
   launches land in session 0 and Seth cannot see them). `run_game.cmd`
   is currently gated to swap 3000 with
   `--fh1_debug_callog=3000 --fh1_debug_callog_budget=200000
   --fh1_debug_resolve_trace=3000 --gpu_trace_bins_vblank=3000`.
   Swap 3000 is reached ~3 min after launch (c2p-3000 files appear in
   `E:\PinyonShift\work\.local\c2probe`); let the game run a few
   minutes past it, then stop it.
3. Collect: the game locks `runtime.log` while running — after
   stopping, copy the `runtime*.log` files containing the run's GPU
   thread to a local directory (find the thread from the A9 present
   line for the swap).
4. Compare: `python3 compare.py <logdir> --swap 3000 --thread tXXXX`.
   Exit 0 = PASS. The script refuses to guess when several threads
   have an A9 line for the swap.
5. Notes: always grep ALL rotated `runtime*.log` files (rotation is
   ~5 MB); never assume swap 1500/3000 is free roam (verify the
   routing census first); the game does not reliably reach free roam
   (one run stuck on the Dolby splash for 3.4 min).

## 9. Files

- Design: [dx11-call-logger-design-2026-11-17.md](sandbox://workspace/dx11-call-logger-design-2026-11-17.md)
- Probe diff (all three builds): [dx11-call-logger-probe-diff-2026-11-17.txt](sandbox://workspace/dx11-call-logger-probe-diff-2026-11-17.txt)
- Comparison script: `~/workspace/dx11-call-logger-scratch/compare.py`
- Scratch (source copies, apply scripts, logs):
  `~/workspace/dx11-call-logger-scratch/` — logs3 (run 3, t6756),
  logs4 (run 4, t11284)
- PC backups (`.musebak-callog*`, `.musebak-callog2*`,
  `.musebak-callog3*`): `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\`
  and `E:\PinyonShift\work\.local\rexglue\src\graphics\`
- This report: [dx11-call-logger-2026-11-17.md](sandbox://workspace/dx11-call-logger-2026-11-17.md)
