# D3D11 call logger + comparison tool — design (Phase 1)

**Date:** 2026-11-17
**Status:** Phase 1 design DONE. Phase 2 first build + first census run DONE
(2026-11-17 ~02:03–02:11 CST — wrong frame: the captured swap-1500 was a
3,418-draw loading frame, not the 129-draw free-roam frame; the comparison
correctly failed it). Second build (G3 budget-per-line + `decode_regs`
read-back, A3 fields, untileflush line, resolve-trace + budget run
settings) DEPLOYED and verified by hash; second run pending. The D11-only
consistency check is the FIRST comparison; the Vulkan cross-backend
comparison (§9) is the SECOND, stronger check — not yet implemented
(Phase 2b).
**Scratch:** `~/workspace/dx11-call-logger-scratch/` (source copy under `src/`)
**Design doc:** `~/workspace/dx11-call-logger-design-2026-11-17.md`
**Phase 2 report target:** `~/workspace/dx11-call-logger-2026-11-17.md`
**Phase 2 probe diff target:** `~/workspace/dx11-call-logger-probe-diff-2026-11-17.txt`

## Review corrections (2026-11-17) — Seth reviewed the design, approved Phase 2; items 1–5 resolved before any build/run, items 6–8 added after (Band 0, #23 split, two-logger structure), item 9 (second-build revisions) and item 10 (third-build revisions) added after the census runs; run 4 PASSED (see the final report)

1. **G3 (Type-0 register logging):** the first draft logged only the first four
   values of a Type-0 write — a packet can write ten registers with
   RB_COLOR_INFO seventh, and its value would never be recorded. **Fixed:**
   G3 now logs the register index AND value for **every** draw-relevant
   register actually contained in the written range, one line per register
   (`kind=reg base={} count={} idx={} val={}`). See §1.3/§1.4.
2. **G0 (Type-3 state changes):** the first draft logged predicate/bin state
   BEFORE the opcode handler — for SET_BIN_MASK / SET_BIN_SELECT the command
   itself changes that state. **Fixed:** G0 keeps the pre-handler line and
   adds a post-handler `kind=pktpost` line (emitted after the handler, for
   the five SET_BIN_* opcodes that have handlers) carrying the resulting
   sel/mask, plus a per-swap Type-3 sequence number `t3seq` on every pkt
   line. See §1.2/§1.4.
3. **Present HRESULT:** the first draft proposed recording `present_result`
   from the command processor, but the HRESULT lives in
   `d3d11_presenter.cpp`, which Phase 2 does not modify. **Decision: Present
   HRESULT verification is explicitly OUT of Phase 2.** The A9 line remains
   a frame-boundary marker (swap + issued/drawn counts) only. A narrowly
   gated presenter-side Present record is a defined Phase 3 item (it would
   need the frame number plumbed into the presenter). See §3.1 A9, §6.6.
4. **Texture-bind absence (srv-bind budget):** the first draft treated a
   missing "FH1 srv bind all" record as a possible failed binding, but those
   records are deduplicated per distinct binding (key has no swap) and capped
   at 400 for the whole process, with no exhaustion signal. **Fixed:** the
   texture cache now logs one `FH1 srv bind all budget exhausted` line when
   the cap is first hit, and the comparison treats srv-bind evidence as a
   per-swap SET (not per-draw) — a missing record is UNDECIDABLE (incomplete
   evidence) when the budget was exhausted, and only a genuine missing bind
   when it was not. See §3.3, §4.3.
5. **Packet-census criterion:** the first draft excluded Type 1/2 packets but
   required the `counter_` sequence to have no gaps — `counter_` advances
   for every packet type, so gaps are expected. **Fixed:** continuity is
   validated with a Type-3-specific sequence number `t3seq` (incremented
   once per Type-3 packet, logged on every G0 line); Type 1/2 packets are
   explicitly excluded from the census. See §1.2, §4.3, §4.4.

6. **Band 0 correction (2026-11-17 ~02:07):** a pasted analysis hypothesised
   band 0 (rows 0–256, window_y == 0) was missing its copy-down. **Ruled
   out by design, not a discrepancy:** both record sites guard on
   `untile_band != 0`, `Fh1D3D11CopyDownRows(720, 0) = 0`, and band 0's
   rows are already in place (window_y == 0, no shift) — the per-band fix
   review documents this explicitly ("band 0 as a no-op — exactly as
   before, by design, not a missing copy-down"; Seth-verified). **The
   logger's EXPECTED behaviour for band 0 is ZERO copy-downs** — the
   comparison must NOT flag the absence of a band-0 copy-down. The
   per-band check is: bands 256 and 512 must have their copy-downs
   (1,998 / 2,298 in the fix run); band 0 must have none, by design.
   Band 0 being wiped by a clear/resolve is ruled out AT THE RESOLVE
   STAGE for the captured frame (the resolve-source probe sampled exactly
   band 0's rect 0,0–1280,256 and got uniform grey 0.258789, not black).
   Nothing to cull in the captured frame (the guest issues no EDRAM
   triangles in any band; the only EDRAM draw is the fullscreen grey
   rectangle). **Still open:** the photo's top-dark / bottom-sky split
   itself (the c2 bottom-half anomaly, deferred-tape hypothesis). See §8.
7. **#23 split additions (2026-11-17 ~02:08–02:14):** Seth's analysis of
   the live-gameplay photo (top ~360 rows wiped to black, bottom ~360
   rows left blue — a split at the horizon line, NOT at a 256-pixel band
   seam) proposes four mechanisms: (1) depth/stencil rejecting the
   bottom half, (2) Y-driven VS interpolants, (3) RECTLIST expansion,
   (4) leftover scissor (0,0,1280,360). **Checked vs evidence:**
   (2) is RULED OUT for swap-1500 — the hand-simulation gives
   o0=(0,0,0,1), o1=(0,0,0,0) at EVERY vertex, W from constants
   c16.w×c17.w, no Y-dependence. (3) does NOT apply to the captured
   #23 — **draw #23 in swap-1500 is a TRIANGLELIST (prim 4, 48 verts),
   NOT a RectangleList**; the RectangleList draw is A#0, the EDRAM grey
   rectangle (prim 8, 3 verts). **Added to the per-draw capture (§3.1
   A3):** depth/stencil state (enable + funcs), the full scissor rect,
   the claim rect (viewport ∩ scissor), the index count and the
   guest/host vertex counts — the same kind of per-draw state the logger
   already records, not a scope expansion. **Vertex Y-range honesty
   note:** a true post-VS screen-space Y-range is NOT computable on the
   CPU in IssueDraw (the VS runs on the GPU; the primitive processor
   computes no vertex bounds); Phase 2 logs the claim rect + counts on
   A3 and takes the TRUE rasterised extent from the existing c2-probe
   footprint (changed-pixel bbox per draw, 24-draw budget) — the
   comparison joins them per draw. A primitive-processor guest-space
   bounds computation is a defined Phase 3 item (and even it yields
   guest-space Y, not post-VS). **NaN/sky-dome scope note:** the
   sky-shader-0/NaN, far-plane depth-fail and NaN-in-HDR-bloom
   hypotheses all presuppose the sky dome IS drawn; in the captured
   swap-1500 frame it is NOT, so they cannot explain that frame.
   NaN-in-bloom is still LIVE only for the Corrado car-preview black
   box (a different scene, offscreen buffer, never investigated) — a
   NaN scan of that buffer is a follow-up probe idea, NOT Phase 2 scope.
   See §8.
8. **Two-logger cross-backend design change (2026-11-17 ~02:16, Seth's
   direction — supersedes the D11-only comparison as the PRIMARY
   check):** the logger is TWO loggers / two runs. Logger 1 runs the
   game on VULKAN and translates what Vulkan does into D11 equivalents
   (the same surface keys, draw-accounting fields and outcome
   categories) — the EXPECTED D11 behaviour, derived from an
   independent backend, not from Pinyon's own D11 translation rules.
   Logger 2 is the D11 run as already built. The comparison is
   translated-Vulkan vs actual-D11, per guest call. **The D11-only
   consistency check (guest → expected via Pinyon's rules → actual)
   stays as the FIRST comparison; the Vulkan cross-backend comparison
   is the SECOND, stronger check. Do NOT drop the D11-only check.**
   This addresses the conceptual limitation above: the D11-only
   comparison cannot catch translation-rule errors. The decisive
   question: the routing investigation proved the guest issues NO
   EDRAM-targeting world triangles in the captured D11 frame — if the
   guest DOES issue them on Vulkan in the same frame, D11 is dropping
   or misrouting them; if the guest issues none on Vulkan either, the
   missing world is guest-side. **Assumption to verify:** the comparison
   is only meaningful if the Vulkan run is the working reference —
   verify from the Vulkan run's log that the guest issues
   EDRAM-targeting world triangles on Vulkan and that the EDRAM gets
   world content; if it does not, say so explicitly, do not assume
   Vulkan renders the world. See §9.

9. **Second-build revisions (2026-11-17, from the first census run's
   findings — implemented before the second run):**
   (a) **G3 budget charged per PACKET, not per LINE:** the first version
   called `Fh1CallogEnabled` once per Type-0 packet and then emitted up
   to `count` reg lines, so 80,403 reg lines flowed against an 8192
   budget (bulk shader-constant uploads dominate: 2,032 distinct
   constant regs, 192 fetch regs). **Fixed:** the budget is charged
   inside the G3 loop, once per emitted line. **Run setting:** the
   second run sets `--fh1_debug_callog_budget=200000` (a busy frame
   emits ~90k G-lines + ~7k A-lines). See §5.3.
   (b) **G3 read-back source (F2 state-flow):** the first version read
   the value back from `register_file_` (the global register file),
   but the routing-guest line reads `decode_regs()`. `decode_regs()`
   returns the decode register file when `record_split_` is active
   and `register_file_` otherwise
   (`include/rex/graphics/command_processor.h:390`), so when the tape
   recorder is on the two files diverge — first run: RB_COLOR_INFO
   3373/3418 (98.7%), RB_DEPTH_INFO 829/3418 (24%), RB_SURFACE_INFO
   512/3418 (15%). **Fixed:** G3 reads back from
   `decode_regs().values[idx]` — the same file the routing-guest
   reads. When `record_split_` is off, `decode_regs()` IS
   `register_file_`, so nothing changes in that case. See §1.3.
   (c) **Tile-path drop stages are outcomes, not just `copy_mode`:**
   the first comparison counted only `copy_mode` tile-path lines as
   outcomes and missed the drop stages (`no_guest_visible_output`,
   `memexport_not_supported`, etc.), which are logged ONLY via the
   tile-path line — there is NO `FH1 routing drop` line for them
   (the routing-drop lines cover a DISJOINT set: `hide_hud`,
   `dropped_targets`, `dropped_shader`, `dropped_pipeline_state`,
   `dropped_vbuf_residency`, `dropped_bindings`,
   `dropped_host_prim`, `dropped_index_buffer`). The 120
   `no_guest_visible_output` tile-path lines in the first run were
   exactly the 120 unaccounted draws. **Fixed in `compare.py`:** every
   non-`issue_enter` tile-path stage is a drawstream outcome event.
   See §4.1.
   (d) **`draw_counters_.drawn` is a DEAD counter on D3D11:** never
   incremented anywhere in the D3D11 backend (only the struct default
   `uint64_t drawn = 0`), so A9's `drawn` is always 0 and carries no
   information. **The comparison reconciles the A9 boundary on
   `issued` only.** (First run: A9 issued=3259 vs stage-D issued=3242
   — a 17-draw / 0.5% difference, most likely a frame-boundary
   straddle between the `trace_swaps_` gate and the
   `presented_output_frames_` counter; recorded as a finding, not a
   gate.) See §3.1 A9.
   (e) **A3 fields implemented (review addition 7):** the stage-D line
   now carries `verts={guest}/{host} idx={} depth={:08X} z={} sten={}
   zfunc={} stenfunc={} scissor={},{},{}x{} claim={},{}-{}, {}` — see
   §3.1 A3. The comparison's B4 section reports the scissor/claim/
   depth distribution (the #23-split decision-tree inputs). The
   `(void)index_count;` at the top of `IssueDraw` was removed (the
   parameter is now used).
   (f) **Untileflush line implemented (§8.1 B1):** one callog-gated
   `FH1 callog side=A swap={} kind=untileflush key={} bands={:08X}`
   line per surface per flush in `EndUntiledTiling`
   (`fh1_native_executor.cpp`), gated on the callog swap via
   `command_processor_.presented_output_frames()` (the same counter
   the tile-path / A-lines gate on). No budget charge: at most one
   line per surface, only at one swap.
   (g) **Run settings for the second run:** `run_game.cmd` now also
   passes `--fh1_debug_resolve_trace=1500` (B2 resolve rect/base) and
   `--fh1_debug_callog_budget=200000`.
   (h) **BINPRED/BINRECORD regex:** the first `compare.py` expected
   `0`/`1` for the `out` / `untile_tiling` / `untile_select_all` /
   `viz_blocked` fields, but the logger formats bools as
   `true`/`false` — so it matched ZERO BINPRED lines. **Fixed:**
   the regex accepts `true|false|0|1`. The routing-guest regex now
   also accepts the older 2026-11-16 format (no `vb=`, no `d0=`).
   (i) **Run filtering:** a collected log directory spans MANY runs
   (rotation keeps ~20 files). `compare.py` now analyses exactly ONE
   run, identified by the GPU thread id (`--thread tXXXX`, auto-
   detected from the A9 present lines; it refuses to guess when
   several threads have one). **The captured swap is accepted ONLY
   if its routing census matches the free-roam profile
   (129/50/31/48)** — otherwise it is discarded and re-gated (§5.4).
   *(Correction, 2026-11-17 after the swap-3000 runs: the exact 129
   count is a property of the routing run's frame, not a law — the
   guest issued 131 draws in both swap-3000 runs, confirmed by three
   independent counts agreeing. The acceptance gate is now the census
   STRUCTURE: the three independent draw counts agree with each other,
   every guest draw has an outcome (unaccounted = 0), and the
   callog cross-checks (A-issued, A-copy) match the tile-path counts.
   A difference from the 129/50/31/48 baseline is reported as a
   frame-content finding, not a logger failure. See item 10(b) and
   §4.4.)*

10. **Third-build revisions (2026-11-17, from the swap-3000 run-3
    comparison — implemented before run 4, which PASSED):**
    (a) **A6 (kind=copy) placement — plan-failure copies were
    unlogged:** the A6 block sat AFTER the plan-failure early return
    in `IssueCopy` (`if (!resolve_plan_succeeded) return plan.empty;`
    — "Nothing to copy or clear: Xenos does no work either"), so
    copy-mode draws whose resolve plan fails never reached it. Run 3
    (t6756): tile-path counted 31 copies but only 25 A6 lines fired
    (all copy=1, consecutive resolve numbers 41854–41878, no gaps).
    **Fixed:** the A6 block moved to just after `Fh1PlanResolve`
    (before the plan-failure return), so EVERY `IssueCopy` call emits
    exactly one A6 line — successful plans copy=1, failed plans
    copy=0 with the skip reason. A failed plan's `plan.kind` is an
    empty string, so its A6 line reads `pkind= ` — and the
    `resolve=` field is `resolve_call_count_`, which only increments
    for successful resolves, so a failed plan's A6 line shares the
    previous successful call's resolve number; failed plans are
    distinguished by copy=0 (and by having no resolve-trace line).
    Run 4 (t11284): 31 A6 lines — 25 copy=1 + 6 copy=0. See §3.1 A6.
    (b) **A1 acceptance reinterpreted (see the correction to 9(i)
    above):** the logger gate is internal consistency + unaccounted =
    0; the 129/50/31/48 baseline is a reference. Run 4's frame is
    131/52/31/48 — the +2 issued draws are real guest draws (the
    guest issued 131 DRAW_INDX packets, confirmed by BINPRED,
    routing-guest and the packet census all agreeing at 131).
    (c) **A6 regex — empty pkind silently dropped the copy=0 lines:**
    the first `compare.py` A6 regex required `pkind=\S+`, but a
    failed plan's A6 line has an empty `pkind`, so the 6 copy=0 lines
    did not match and run 4 first counted 25, not 31 — the same
    symptom as the bug the third build fixed, now in the parser.
    **Fixed:** `pkind=\S*`. The A1 rollup now also prints the A6
    copy=1 / copy=0 split.
    (d) **B3 false positive fixed:** the first B3 flagged all 48
    window_y==0 drops as possible band-0 culling — but all 48 are
    the single-point `no_guest_visible_output` baseline pattern
    (prim==1, idx==1, mode=4, pass=-1, c0=00000000, surf=14020500),
    not culling. **Fixed:** B3 excludes that pattern and reports it
    separately; only a drop at a different stage (or a
    non-single-point draw) is culling evidence. Run 4: 0 culling
    candidates. See §8.1 B3.
    (e) **B4 scissor reporting fixed:** the first B4 flagged every
    non-full-frame scissor as a possible leftover half-screen
    scissor — but most are legitimate (EDRAM-height 8192 scissors
    for the 720-row EDRAM surface; small rects for small render
    targets). **Fixed:** B4 classifies scissors (full-frame /
    HALF-SCREEN split-relevant / EDRAM-height / small target /
    other); only a half-screen (1280-wide, height 360 or y-offset
    360) scissor is a #23-split mechanism-4 candidate. Run 4: the
    half-screen pair IS present — draws 25/26, prim=8 mode=4 pass=-1
    idx=3 verts=3/3, depth test OFF (z=0 sten=0), scissor
    (0,0,1280,360) / (0,360,1280,360), claim (0,0–1280,360) /
    (0,360–1280,720). So in the swap-3000 frame the composite is
    drawn as TWO half-screen rectangles, not one fullscreen draw —
    and neither has depth/stencil on, so the split in that frame is
    produced by two separate draws, not by a scissor or a depth test
    on a single draw. See §8.2 B4.

**Conceptual limitation (recorded here and in the final report):** the
comparison derives expected D3D11 behaviour from Pinyon's own translation
rules, so the logger is an **execution-consistency auditor, NOT an
independent Xbox GPU emulator**. What it CAN establish: a guest operation
was accounted for; the backend made the expected routing decision; the
expected target and actual target disagree; a draw was unexpectedly
dropped; a resource or shader binding does not match the recorded
expectation. What it CANNOT establish by itself: that Pinyon's translation
rules faithfully reproduce Xbox GPU semantics; that a guest operation's
interpretation is correct; that matching D3D11 calls produce the correct
final image. See §0.3.

## 0. What this tool is, and what it is not

Seth's request: a logger that logs the guest GPU calls, auto-translates
("auto corrects") each one to its expected D3D11 equivalent, runs the game,
and compares the result against the actual D3D11 calls the backend logs.

This tool generalises the routing investigation
(`~/workspace/draw23-routing-investigation-2026-11-16.md`), which accounted
for every DRAW_INDX packet in one frame (129 draws, 50 issued) at three
pipeline stages, into a reusable tool for **all** guest calls — draws,
state-setting packets, shader loads, texture-fetch setup, resolves/copies,
and frame presents — not just draws.

It is a **diagnostic comparator**, not a translator and not a fix:

- It does **not** re-implement the backend's translation. The expected
  D3D11 call for each guest call is derived from the backend's own decision
  rules, evaluated on the logged state (see §2). The comparison is
  expected-vs-actual, not guest-vs-D3D11 raw.
- It does **not** change rendering behaviour. Every probe is an additive
  log line, gated, budgeted. The per-band bitmask fix, the WaitForGpu state,
  and all existing probes are preserved untouched.
- It does **not** prove a rendering bug by itself. A mismatch is a finding
  that needs review (either the backend is wrong or the rule table is stale),
  exactly like the routing report's adjudication.

### 0.1 Baseline and drift (read this first)

All hook line numbers in this doc are as of the **2026-11-17 07:17 UTC**
source fetch (copied from the audit scratch, hashes verified):

| File | Lines | SHA256 (first 16) |
|---|---|---|
| `src/graphics/d3d11/command_processor.cpp` | 7,023 | `9d53077cd883b3f9` |
| `src/graphics/command_processor.cpp` (shared) | 4,085 | `e7c0c842a4c36ec2` |
| `src/graphics/d3d11/fh1_native_executor.cpp` | 2,886 | `47b792f06243cd80` |
| `src/graphics/d3d11/texture_cache.cpp` | 1,358 | `a3ed4e3f6b1bdcdb` |
| `src/graphics/flags.cpp` | 450 | (see audit) |

**Drift warning.** The live PC copy of D3D11 `command_processor.cpp`
already differs from this baseline: a read-only hash check at 07:17 UTC
returned `FE07347A70F767F4…`, not `9d53077c…`. That is the WaitForGpu fix
agent's in-flight edit (the fix has since been stopped and the file
restored from its `.musebak-waitforgpu-20261117` backup — byte-identical
after restore, per the parent — but **Phase 2 must re-verify every hook
line number against the live tree before editing**, and must re-hash after
the restore). The design's hook points are given as
**function + line number in the baseline**; if a function has moved, the
function name governs, not the line number.

**Stale EXE warning.** The PC's `pinyon_shift.exe` was built at 00:18 PC
from the EDITED (pre-restore) source and is now stale relative to the
restored source. **Phase 2 MUST rebuild before any test run** and verify
the binary matches the restored source (DLL string search for
`"FH1 callog"` + exe/dll mtimes) before claiming a run used it.

**Free-roam flag.** The game does NOT reliably reach free roam (one run
stuck on the Dolby splash for 3.4 min). Do not assume swap 1500 is free
roam. Phase 2 uses the fixed swap 1500 (proven free roam in routing runs
1–2 and footprint runs 2–4) **with a run-procedure free-roam check** (the
log must contain composite draws for swap 1500; if the boot sticks on
Dolby, stop and re-run). A first-occurrence re-gate is defined as a
Phase 3 item (§5.4).

### 0.2 The two frame counters (join key foundation)

There are two "swap" counters, and the routing report established they are
aligned — both report 129 draws for "swap 1500":

- **Guest/decode side — `trace_swaps_`** (shared `command_processor.cpp`):
  incremented once per `PM4_XE_SWAP` packet in `ExecutePacketType3`
  (`:2323`, inside the `opcode == PM4_XE_SWAP` branch at `:2309`).
  `ExecutePacketType3_XE_SWAP` itself is at `:2674`.
- **D3D11/present side — `presented_output_frames_`** (D3D11
  `command_processor.cpp`): incremented once per presented frame at
  `:6640`, in the frame-boundary code that also calls
  `primitive_processor_->EndFrame()` and emits the per-frame
  `D3D11 draws frame=` summary (`:6664`).

Every log line in this design carries its side's swap. The comparison
script joins on the swap number, and the routing report's order-join
validation (0 mismatches) is the precedent for joining by order within a
swap.

### 0.3 Conceptual limitation — what the logger can and cannot establish

The comparison derives expected D3D11 behaviour from **Pinyon's own
translation rules** (the rule table in §2.2 is a restatement of what the
backend's own functions do). That makes the logger an
**execution-consistency auditor, NOT an independent Xbox GPU emulator**.

What it **CAN** establish:

- A guest operation was accounted for (it appears in the census, with an
  outcome).
- The backend made the expected routing decision (its logged outcome
  matches the rule table re-evaluated on the logged state).
- The expected target and the actual target disagree (bound key ≠ derived
  key).
- A draw was unexpectedly dropped (expected `issued`, got a drop stage the
  rules do not sanction, or no outcome at all).
- A resource or shader binding does not match the recorded expectation.

What it **CANNOT** establish by itself:

- That Pinyon's translation rules faithfully reproduce Xbox GPU semantics
  (the rules are the thing being audited against, not an independent
  reference).
- That a guest operation's interpretation is correct (e.g. whether a draw
  the guest issued is the draw the game intended).
- That matching D3D11 calls produce the correct final image (the logger
  checks calls, not pixels — pixel evidence still comes from the c2 /
  composite / surface probes).

A clean comparison (zero unexplained flags) means the backend executed
its own rules consistently for that frame. It does not, on its own, prove
the world should render.

---

## 1. Guest-call log — what is logged, where, and with what fields

### 1.1 Call taxonomy (guest side)

The guest command stream is a sequence of packets decoded by
`CommandProcessor::ExecutePacket` (shared `command_processor.cpp:2161`),
which dispatches by packet type:

- **Type 0** — register writes, `ExecutePacketType0` (`:2223`): writes
  `count` registers in sequence starting at `base_index` (or one register
  if `write_one_reg`). `WriteRegisterRangeFromRing` / `PacketWriteRegister`
  apply them. This is how ALL draw state is set (colour/depth info,
  window offset, shader constants, texture-fetch constants, viz query).
- **Type 1** — `ExecutePacketType1` (`:2259`).
- **Type 2** — `ExecutePacketType2` (`:2271`).
- **Type 3** — `ExecutePacketType3` (`:2277`): the opcode switch at
  `:2481-2560+` covers: `PM4_ME_INIT`, `PM4_NOP`, `PM4_INTERRUPT`,
  `PM4_XE_SWAP`, `PM4_INDIRECT_BUFFER` / `PM4_INDIRECT_BUFFER_PFD`,
  `PM4_WAIT_REG_MEM`, `PM4_REG_RMW`, `PM4_REG_TO_MEM`, `PM4_MEM_WRITE`,
  `PM4_COND_WRITE`, `PM4_EVENT_WRITE` / `_SHD` / `_EXT` / `_ZPD`,
  `PM4_DRAW_INDX` (0x36), `PM4_DRAW_INDX_2` (0x22),
  `PM4_SET_CONSTANT`, `PM4_SET_CONSTANT2`, `PM4_LOAD_ALU_CONSTANT`,
  `PM4_SET_SHADER_CONSTANTS`, `PM4_IM_LOAD`, `PM4_IM_LOAD_IMMEDIATE`,
  `PM4_INVALIDATE_STATE`, `PM4_VIZ_QUERY`, `PM4_SET_BIN_MASK_LO` /
  `PM4_SET_BIN_MASK_HI`, `PM4_SET_BIN_SELECT_LO` / `PM4_SET_BIN_SELECT_HI`,
  `PM4_SET_BIN_MASK`, `PM4_WAIT_FOR_IDLE`.

The logger covers **Type 0 and Type 3** (the packets that carry GPU work
and state). Type 1/2 are not logged in Phase 2 (they are not observed to
carry draw-affecting work in FH1; if the packet census in §4 finds
unaccounted Type 1/2 packets, they become a Phase 3 item).

Per-packet identity: the shared decoder's packet counter `counter_`
(incremented once per packet at `:2734`). Note the routing report's caveat:
the `vb` value in BINPRED repeats across indirect buffers (53 distinct
values over 129 lines, IB depth 1 ×81 / depth 2 ×48), so **`counter_`/`vb`
alone is not a global key — decode order within the swap is the join key**,
with `counter_` as a cross-check.

### 1.2 Guest hook points (shared `command_processor.cpp`)

| # | Hook | Location | What it logs |
|---|---|---|---|
| G0 | Packet dispatch (Type 3) | `ExecutePacketType3`, `:2277` (after opcode decode, `:2287-2391` predicate/bin evaluation, before the per-opcode handler at `:2481`) | One `FH1 callog` line per Type-3 packet: opcode, `counter_`, `t3seq` (a Type-3-specific sequence number, incremented once per Type-3 packet — see §4.3), predicate flag (`packet & 1`), predicated-out flag, bin select/mask, tiling pass. This is the **packet census**. **Plus a post-handler line** (`kind=pktpost`, emitted after the opcode switch, before `return result;` at `:2671`) for the five SET_BIN_* opcodes that have handlers (SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI, SET_BIN_MASK) carrying the **resulting** sel/mask — because those commands change the state G0 logs (review correction 2). VIZ_QUERY has no pktpost line: it does not change sel/mask, and its resulting viz state is verified at the next draw's BINRECORD `viz_blocked` field. |
| G1 | Draw packet | `ExecutePacketType3Draw`, `:3144` (called from `ExecutePacketType3_DRAW_INDX` `:3743` and `_DRAW_INDX_2` `:3769`) | **REUSED, not re-logged:** the existing `FH1 routing guest` line (`:3295-3300`) + `BINPRED` (`:2404-2411`) + `BINRECORD` (`:3337-3341`). Fields: swap, `vb`, prim, idx, source, ok, mode, pass, sel/mask, winy, c0 base:fmt, d0 base:fmt, surf raw, split; plus BINPRED's pred/out/sel/mask/pass/untile flags and BINRECORD's viz_blocked. |
| G2 | Backend intake | `ExecuteDrawRecord`, `:3376` | **REUSED:** `FH1 routing record` (`:3408`), `FH1 routing dispatch` (`:3563`), skip/suppress drops (`:3497` skip_draws, `:3532` band_suppress), viz-block drop (`:3419`). |
| G3 | Register writes (Type 0) | `ExecutePacketType0`, `:2223` | **NEW in Phase 2 (corrected per review):** one `FH1 callog` line **per draw-relevant register actually contained in the written range** — not the first four values of the packet. Each line carries the packet's `base`/`count` plus the register's `idx` and `val` (read back from `decode_regs().values` after the write — the same file the routing-guest line reads; see review correction 9b). A packet that writes ten registers with RB_COLOR_INFO seventh logs RB_COLOR_INFO's index and value. The budget is charged per emitted line (see §5.3). See §1.3/§1.4. |
| G4 | Shader load | `ExecutePacketType3_IM_LOAD`, `:2542` (and `ExecuteLoadShaderRecord`, `kRecordLoadShader` at `:644`/`:727`) | **NEW in Phase 2:** one `FH1 callog` line per IM_LOAD / IM_LOAD_IMMEDIATE: shader type (VS/PS), guest address, dword count, microcode hash. |
| G5 | Texture-fetch setup | D3D11 `command_processor.cpp:887` (`TextureFetchConstantWritten`) and `:987` (`TextureFetchConstantsWritten`) — the register-write path that notifies the texture cache | **NEW in Phase 2:** one `FH1 callog` line per fetch-constant write: first/last fetch index. Per-draw fetch reads are already covered by `FH1 srv bind all` (texture_cache.cpp:1135, §3.3). |

### 1.3 Which Type-0 registers are "draw-relevant" (G3 filter — corrected)

A Type-0 packet can write a range of registers, and the draw-relevant one
can sit anywhere in that range — not just in the first four values
(review correction 1). G3 therefore works **per register, not per packet**:
for each index in the written range `[base_index, base_index + count)`
(`write_one_reg` packets write `count` values to the single register
`base_index`, so only that index is considered), it checks whether that
index is draw-relevant, and if so emits one line carrying that register's
index and its value (read back from `decode_regs().values` after the
write — for `write_one_reg` this is the final value written; the
`decode_regs()` read-back is review correction 9b, so G3 logs the same
file the routing-guest line reads). The budget is charged per emitted
line, inside the loop (review correction 9a / §5.3).

A register index is draw-relevant if it is one of the singles, or inside
one of the ranges:

- Singles: `RB_COLOR_INFO` (`registers.h:748`), `RB_DEPTH_INFO` (`:846`),
  `RB_MODECONTROL` (`:665`), `RB_SURFACE_INFO`, `PA_SC_WINDOW_OFFSET`
  (`:617`), `PA_SC_VIZ_QUERY` (`:528`), `SQ_VS_CONST` (`:265`),
  `SQ_PS_CONST` (`:279`).
- Ranges: texture-fetch constants
  (`XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0` ..
  `XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5`, 192 registers) and float
  constants (`XE_GPU_REG_SHADER_CONSTANT_000_X` ..
  `XE_GPU_REG_SHADER_CONSTANT_511_W`, 2048 registers).

Logging every value in a full float-constant upload would be up to 2048
lines for one packet; in practice FH1's SET_CONSTANT packets write a
handful of float4s, and the callog budget (§5.1) caps the total. The
budget-exhausted line (§5.3) tells the script when the cap was hit, so a
truncated G3 record is reported as incomplete evidence, not as a missing
register.

Packets that write no draw-relevant register emit no G3 line. The packet
itself is still accounted for by the Type-0 path (the write happened); the
census in §4 covers Type-3 packets, and Type-0 packets are accounted for
through the G3 lines they produce plus the per-swap count of Type-0
packets that wrote a relevant register (reported by the script as
informational, not as a gap check — see §4.3).

### 1.4 What each guest line carries (fields)

Every guest line carries: `swap` (`trace_swaps_`), `seq` (the packet's
`counter_`, plus implicit decode order), and kind-specific fields:

- **pkt (G0):** `op={:02X}` (PM4 opcode), `t3seq={}` (Type-3-specific
  sequence, incremented once per Type-3 packet — the continuity check in
  §4.3), `pred={0|1}`, `out={0|1}` (predicated-out, draws only; - for
  non-draws), `sel={:016X}`, `mask={:016X}`, `pass={}` (tiling pass, draws
  only).
- **pktpost (G0, post-handler):** `op={:02X}`, `sel={:016X}`,
  `mask={:016X}` — the resulting bin state after the handler, for the five
  SET_BIN_* opcodes only. Joins to its pkt line by order (the pktpost line
  is the next callog line after its pkt line for that packet).
- **draw (G1, existing lines):** as in the routing report §1.1/§2.
- **reg (G3):** `base={:04X}`, `count={}`, `idx={:04X}`, `val={:08X}` —
  one line per draw-relevant register in the written range.
- **shaderload (G4):** `type={vs|ps}`, `addr={:08X}`, `dwords={}`,
  `hash={:016X}`.
- **texfetch (G5):** `first={}`, `last={}` (fetch constant indices).

---

## 2. Auto-translation to expected D3D11 calls ("auto correct")

### 2.1 What "auto correct" concretely means

Seth's phrase "auto correct them to dx11 calls" does **not** mean the
logger re-implements the backend's translation in the backend, and it does
**not** mean the logger fixes anything. It means this:

> For each logged guest call, the comparison derives the **expected**
> D3D11 action from the guest call's logged fields using an explicit rule
> table (§2.2) — the same rules the backend itself applies — and then
> checks the actual D3D11 log for a matching action. The derivation happens
> **offline, in the comparison script**, from the logged data. It is
> expected-vs-actual, not guest-vs-D3D11 raw.

Two consequences follow:

1. **The expected action is computable from the log alone.** Every rule's
   inputs must be present in the logged fields. If a rule needs a value
   the log does not carry, the rule is marked **undecidable from the log**
   and the comparison reports it as such (rather than guessing).
2. **The backend also logs its own decision outcomes** (predicated-out,
   viz-blocked, kCopy classification, early-exit stage, bound target).
   Where the backend's logged outcome and the script's re-derived
   expectation disagree, the comparison flags it — and the flag means
   **either the backend is wrong or the rule table is stale**. Both are
   findings, and the report must say which the evidence supports.

### 2.2 The rule table (guest call → expected D3D11 action)

Rule sources are the backend functions that implement each rule. Line
numbers are the §0.1 baseline.

| # | Guest call (from the log) | Expected D3D11 action | Rule source |
|---|---|---|---|
| R1 | DRAW_INDX / DRAW_INDX_2, `pred=0`, or `pred=1` and `(sel & mask) != 0` (with the `untile_select_all_` branch when active); `viz=0`; `mode=4` (colour/depth); not skipped; passes all IssueDraw gates | `IssueDraw` → `Draw` / `DrawIndexed` on the derived target (`Fh1SurfaceKey` from RB_COLOR_INFO / RB_DEPTH_INFO / RB_SURFACE_INFO), topology from primitive, shaders from vs/ps hashes, SRVs from the texture-fetch constants | `PredicatedOut` (shared `:2604`); `viz_query_blocks_draw` (shared `:3334`, `:3399`); `edram_mode` (`RB_MODECONTROL`); `BindDrawRenderTargets` (D3D11 `:1004`); `ConfigurePipeline` (D3D11 `:2028`); `UpdateBindings` (D3D11 `:1605`) |
| R2 | DRAW_INDX, `pred=1` and `(sel & mask) == 0`, or the `untile_select_all_` branch rejects it | **No D3D11 call** — outcome `predicated_out` | `PredicatedOut` (shared `:2604`); bin mask all-ones makes the check vacuous (routing report §3) |
| R3 | DRAW_INDX, `viz=1` (`viz_query_ena && kill_pix_post_hi_z`) | **No D3D11 call** — outcome `viz_blocked` | `viz_query_blocks_draw` (shared `:3334`, `:3399`) |
| R4 | DRAW_INDX, `mode=6` (kCopy) | `IssueCopy` → resolve/copy per `Fh1PlanResolve` (`plan.copy` with `plan.skip` reason, or skip) | `IssueCopy` (D3D11 `:4406`); `Fh1PlanResolve` (D3D11 `:4439`); plan log (`:4492`, `:4543`) |
| R5 | DRAW_INDX, no guest-visible output (no colour mask, no depth/stencil, no memexport) | **No D3D11 call** — outcome `no_guest_visible_output` | `IssueDraw` (D3D11 `:1853-1856`) |
| R6 | DRAW_INDX, memexport used (`memexport_eM_written() != 0`) | **No D3D11 call** — outcome `memexport_not_supported` | `IssueDraw` (D3D11 `:1859-1863`); `dropped_memexport` counter |
| R7 | DRAW_INDX, tessellation | **No D3D11 call** — outcome `tessellation_not_supported` | `IssueDraw` (D3D11 `:1879-1880`); `dropped_tessellation` counter |
| R8 | DRAW_INDX, no vertex shader | **No D3D11 call** — outcome `no_vertex_shader` | `IssueDraw` (D3D11 `:1816`) |
| R9 | DRAW_INDX, zero surface pitch | **No D3D11 call** — outcome `zero_surface_pitch` | `IssueDraw` (D3D11 `:1826`) |
| R10 | DRAW_INDX, zero host vertex count | **No D3D11 call** — outcome `zero_host_vertex_count` | `IssueDraw` (D3D11 `:1874`) |
| R11 | DRAW_INDX, `fh1_debug_skip_draws` or `band_suppress` drops it | **No D3D11 call** — outcome `skip_draws` / `band_suppress` | Shared `:3497`, `:3532`; cvars `fh1_debug_skip_draws`, `fh1_debug_band_suppress` (`flags.cpp:29`) |
| R12 | DRAW_INDX, `fh1_debug_hide_hud` drops it | **No D3D11 call** — outcome `hide_hud` | D3D11 `:1924-1931` |
| R13 | DRAW_INDX, `BindDrawRenderTargets` fails / `ConfigurePipeline` fails / `UpdateBindings` fails / vertex-buffer residency fails / unsupported host primitive / missing index buffer | **No D3D11 call** — outcome `dropped_targets` / `dropped_shader` / `dropped_pipeline_state` / `dropped_bindings` / `dropped_vbuf_residency` / `dropped_host_prim` / `dropped_index_buffer` | D3D11 `:1975`, `:2034-2044`, `:3354`, `:3388`, `:3632`, `:3695`, `:3704` |
| R14 | SET_CONSTANT / SET_CONSTANT2 / SET_SHADER_CONSTANTS / LOAD_ALU_CONSTANT | Float-constant update, consumed as VS/PS constant buffers at the next draw (`VSSetConstantBuffers` / `PSSetConstantBuffers`, D3D11 `:1682-1683`) | `UpdateBindings` (D3D11 `:1605`) |
| R15 | SET_BIN_SELECT / SET_BIN_MASK (Type-3) | Predicate state update; affects the next draw's `PredicatedOut` evaluation (R2). **The resulting state is captured by the post-handler `pktpost` line (§1.2)** — the script verifies the pktpost sel/mask equals the value the handler should have produced (LO/HI half-write merged into the previous 64-bit value, or full 64-bit set for SET_BIN_MASK), and that the next draw's pre-handler sel/mask matches it. | `bin_select_` / `bin_mask_` (shared); the inline handlers at `:2626-2651` (SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI, SET_BIN_MASK). Note: `PM4_SET_BIN_SELECT` (full 64-bit) has **no handler** in the switch — if the guest ever issues it, it hits the `default:` "Unimplemented GPU OPCODE" path, and there is no pktpost line for it. |
| R16 | VIZ_QUERY | Viz-query state; affects `viz_query_blocks_draw` (R3). Verified at the **next draw's BINRECORD `viz_blocked` field** (already logged), not by a pktpost line — VIZ_QUERY does not change sel/mask, so it has no pktpost line. | `PA_SC_VIZ_QUERY`; `viz_query_blocks_draw` (shared `:3334`, `:3399`) |
| R17 | INDIRECT_BUFFER | The IB's packets execute as their own calls (logged under their own opcodes); the IB itself has no separate D3D11 action | `ExecutePacket` (`:2161`) |
| R18 | IM_LOAD / IM_LOAD_IMMEDIATE | Shader load → `AnalyzeShaderUcode` → `ConfigurePipeline` (translation outcome `is_valid`, pipeline identity) | `ExecutePacketType3_IM_LOAD` (`:2542`); `AnalyzeShaderUcode` (pipeline_cache.cpp:171); `ConfigurePipeline` (D3D11 `:2028`); `is_valid` checked at D3D11 `:2041` and dxbc_translator.cpp:1243 |
| R19 | Texture-fetch constant writes | Texture-cache entry → SRV bind at the next draw that samples the slot (`VSSetShaderResources` / `PSSetShaderResources`, D3D11 `:1705-1707`) | `TextureFetchConstantWritten` (D3D11 `:887`); `CreateTexture` (texture_cache.cpp:444); `FH1 srv bind all` (texture_cache.cpp:1135) |
| R20 | XE_SWAP | `Present` on the swap chain + frame boundary (`presented_output_frames_++`) | `ExecutePacketType3_XE_SWAP` (shared `:2674`); `Present` (d3d11_presenter.cpp:1190, HRESULT checked `:1191-1201`); frame boundary (D3D11 `:6640`) |
| R21 | EVENT_WRITE / WAIT_REG_MEM / REG_RMW / MEM_WRITE / COND_WRITE / WAIT_FOR_IDLE | Synchronisation / memory effects; **no direct D3D11 draw call**. Expected outcome is "no draw; state effect only", verified by the absence of a draw and (where applicable) the fence/wait completing. | — (no D3D11 equivalent by design) |
| R22 | INVALIDATE_STATE | State invalidation; the next draw re-binds. No direct D3D11 call. | — |
| R23 | Type-0 register writes | Register state update; verified at the **next draw's state snapshot** (the routing stage-A line's c0/d0/surf fields), not by a per-packet D3D11 call. | `PacketWriteRegister` / `WriteRegisterRangeFromRing` (shared `:2223`) |

### 2.3 Mechanical vs decision rules

**Mechanical (direct mapping, no judgement):** primitive type →
`D3D11_PRIMITIVE_TOPOLOGY` (via `IASetPrimitiveTopology`, D3D11 `:3641`);
`edram_mode` → IssueDraw vs IssueCopy; target registers →
`Fh1SurfaceKey::Describe()`; shader hashes → pipeline identity;
`Draw` vs `DrawIndexed` (indexed iff the draw has an index buffer,
D3D11 `:3658` vs `:3723`); XE_SWAP → Present.

**Decision (needs the logged deciding values):** R2 (predication —
needs `sel`, `mask`, `untile_tiling`, `untile_select_all`); R3
(viz — needs `viz_query_ena`, `kill_pix_post_hi_z`); R4 (kCopy —
needs `edram_mode`); R5–R13 (early exits — need the state the backend
checked: colour mask, depth/stencil enable, memexport flags, shader
presence, pitch, vertex count, skip/suppress flags). The logger handles
decision cases by **logging the deciding values on the guest side and the
decided outcome on the actual side**, so the script re-evaluates the
predicate from the logged inputs and compares it with the backend's
logged outcome. Where an early exit's deciding value is not in any
existing log line, Phase 2 adds it to the actual-side callog line for
that exit (see §3.4) — it does not guess.

**Known undecidable-from-the-log cases (Phase 2):** the drop exits are
verified by their **named stage** (the routing/tile-path drop line names
the exit — e.g. `no_guest_visible_output`, `memexport_not_supported`).
The **deciding values** for R5–R13 (colour mask, depth/stencil enable,
memexport flags, shader presence, surface pitch, vertex count,
skip/suppress flags) are **not** in the Phase 2 log, so the script cannot
re-derive those exits from first principles: a draw that exited at a
named stage is reported as "accounted by named stage" (not flagged), and
the deciding-value gap is recorded as a known limitation (Phase 3: add
the deciding values to the drop lines). The exception is the derivable
rules — R2 (predication: sel/mask in the guest log), R3 (viz: the
viz_blocked flag in BINRECORD), R4 (kCopy: mode in the guest log) — whose
deciding values ARE in the log and are re-evaluated by the script.
Whether a `dropped_shader` exit was caused by translation failure vs
pipeline configuration failure IS decidable: the routing drop line at
`:2034-2037` names `dropped_shader` vs `dropped_pipeline_state` by
`is_valid()`. Whether a texture-format failure caused a draw to sample
black is decidable only via the `FH1 srv bind all` set for that slot
(see §3.3) — and only as a per-swap set, not per draw (the dedup key has
no draw identity).

---

## 3. Actual D3D11 log — what the backend logs, where, and how it keys back

### 3.1 The D3D11-side hook points (D3D11 `command_processor.cpp`)

| # | Hook | Location | What it logs (existing unless marked NEW) |
|---|---|---|---|
| A0 | IssueDraw entry | `:1735` (entry), tile-path lambda `:1763-1774`, routing issue `:1788-1808` | **REUSED:** `FH1 tile path … stage=issue_enter` (swap, pass, issued count, window_y, mode, sel/mask); `FH1 routing issue` (swap, prim, idx, mode, pass, winy, c0 base:fmt, surf, vs/ps hashes). |
| A1 | Early exits (17) | `:1816` no_vertex_shader, `:1826` zero_surface_pitch, `:1842` no_rasterization_no_memexport, `:1856` no_guest_visible_output, `:1862` memexport_not_supported, `:1869` primitive_processor_failed, `:1874` zero_host_vertex_count, `:1879` tessellation_not_supported, `:1927` hide_hud, `:1975` dropped_targets, `:2034` dropped_shader/pipeline_state, `:3354` dropped_vbuf_residency, `:3388` dropped_bindings, `:3632` dropped_host_prim, `:3695`/`:3704` dropped_index_buffer; `copy_mode` at `:1808` | **REUSED:** `FH1 tile path … stage=<exit>` + `FH1 routing drop: … stage=<exit>` lines. The 9 tile-trace exits and the 8 routing-drop exits together cover all 17. |
| A2 | Render-target selection | `BindDrawRenderTargets` `:1004`; bound log `:2014` | **REUSED:** `FH1 routing bound` (swap, prim, mode, pass, bits, `c0={Describe()}`, `d={Describe()}`) — the **actual** bound key, not the intended one. |
| A3 | Draw call | Stage D, after the `FH1 routing issued` block (implemented at `:3939` in `IssueDraw`; the `:3860` in the first draft was the baseline estimate) | **REUSED:** the tile-path `issued` counter and the `FH1 routing bound` line. **NEW in Phase 2:** one `FH1 callog side=A` line at stage D carrying the draw ordinal + outcome `issued` + the topology actually set — so the script can match it 1:1 with the guest draw without re-deriving it from counters. **NEW in Phase 2 (review additions 7):** the same line also carries, all in scope at the stage-D site in `IssueDraw`: (a) **depth/stencil state** — `normalized_depth_control` (`:1894`, `reg::RB_DEPTHCONTROL`, registers.h:798-818) as the raw dword plus decoded `z_enable`, `stencil_enable`, `zfunc`, `stencilfunc` (the effective state the backend applies; the raw guest register is NOT in the G3 filter, so this line is the only record); (b) **the full scissor rect** — `scissor.offset[0,1]` / `scissor.extent[0,1]` (`:1982-1988`, already scaled by `draw_resolution_scale_*`); (c) **the claim rect** — `claim_x0/claim_y0/claim_x1/claim_y1` (`:1983-1991`, viewport ∩ scissor, "what the draw can write, in host pixels"); (d) **the index count** — the `index_count` parameter of `IssueDraw` (currently `(void)`, now logged); (e) **guest/host vertex counts** — `primitive_processing_result.guest_draw_vertex_count` / `host_draw_vertex_count`. **Implemented format (second build):** `FH1 callog side=A swap={} kind=issued prim={} mode={} pass={} topo={} verts={guest}/{host} idx={} depth={:08X} z={} sten={} zfunc={} stenfunc={} scissor={},{},{}x{} claim={},{}-{}, {}` — scissor is offset_x,offset_y,extent_w×extent_h (already scaled); claim is x0,y0–x1,y1. The comparison's B4 section reports the scissor/claim/depth distribution (the #23-split decision-tree inputs). **Vertex Y-range honesty note:** a true post-VS screen-space Y-range is NOT computable on the CPU in `IssueDraw` — the VS runs on the GPU, `ProcessingResult` (primitive_processor.h:125-150) carries no vertex bounds, and no CPU vertex decode exists. Phase 2 does NOT claim the claim rect IS the mesh extent: the claim rect is the rasterisable extent given the bound viewport/scissor, and the TRUE rasterised extent is the existing c2-probe footprint (changed-pixel bbox per draw, 24-draw budget at the traced swap) — the comparison joins A3 (draw N) with the c2-probe bbox (draw N). Decision tree (§8): footprint top-only + scissor full + depth/stencil open → the mesh itself is top-only; footprint full + wipe top-only → scissor or depth/stencil; all open + footprint full → the split comes from elsewhere. A primitive-processor guest-space bounds computation is a defined Phase 3 item — and even it yields guest-space Y, not post-VS. |
| A4 | Shader / pipeline | `ConfigurePipeline` call `:2028`; `AnalyzeShaderUcode` `:1820`, `:1834`; `is_valid` check `:2041` | **REUSED:** the `dropped_shader` / `dropped_pipeline_state` routing drop (`:2034-2037`) names the failure by `is_valid()`. **NEW in Phase 2:** one `FH1 callog side=A` line at `:2028` (after ConfigurePipeline returns) carrying vs/ps hashes + `is_valid` + pipeline pointer identity — so a shader-load (G4) joins to its translation outcome by hash. |
| A5 | Bindings | `UpdateBindings` `:1605` (called `:3385`); SRV/constant-buffer sets `:1682-1707` | **REUSED:** `FH1 srv bind all` (texture_cache.cpp:1135) per slot. No new line in Phase 2; the script joins srv-bind lines to the draw by (swap, slot) and order. |
| A6 | IssueCopy / resolve plan | `IssueCopy` `:4406`; `Fh1PlanResolve` `:4439`; plan logs `:4492`, `:4543`; skip branch `:4552` | **REUSED:** the plan log lines (carry `plan.copy`, `plan.skip` reason, kind, `copying_depth`). **NEW in Phase 2:** one `FH1 callog side=A` line at `:4552` carrying `plan.copy`, `plan.skip`, `kind`, `copying_depth`, source/dest surface keys — in callog grammar for the parser. |
| A7 | Texture creation | `CreateTexture` (texture_cache.cpp:444); `LogUnsupportedFormatOnce` (`:435-443`); rejected-load log (`:588-599`, capped at 60) | **REUSED:** `FH1 srv bind` (`:1069`, `:1080`) and `FH1 srv bind all` (`:1135`) per slot (valid, host WxH, hostfmt, hostdim, texture, srv, dimok, signed); `FH1 texture load rejected` (`:599`) for rejected loads. See the §2.3 note on the once-per-format suppression. |
| A8 | State setting (D3D11 API calls) | OMSetRenderTargets `:1126`, RSSetViewports `:1169`, OMSetBlendState `:1245`, OMSetDepthStencilState `:1261`, VS/PSSetConstantBuffers `:1682-1683`, VS/PSSetShaderResources `:1689-1707`, IASetPrimitiveTopology `:3641`, VS/PS/GSSetShader in `pipeline_cache.cpp:893-895` | **Not logged per call in Phase 2.** The state-setting calls are verified **indirectly**: the bound target (A2), the bound pipeline/shaders (A4), the bound SRVs/constants (A5), and the topology (A3) are the observable effects of the state-setting calls. Logging every D3D11 state call would multiply the log volume for no additional joinable information. (If a mismatch is ever traced to a state-setting call, a Phase 3 probe can log that call specifically.) |
| A9 | Frame boundary / Present | `presented_output_frames_++` (`:6640`); per-frame summary (`:6664`); `Present` (d3d11_presenter.cpp:1190, HRESULT checked `:1191-1201`) | **REUSED:** `D3D11 draws frame={}: passes={} skipped[…] drawn={} …` (`:6664`, from `draw_counters_`, header `include/rex/graphics/d3d11/command_processor.h:62-94`). **NEW in Phase 2:** one `FH1 callog side=A` line at `:6640` carrying `swap` + `issued` + `drawn` — a **frame-boundary marker only**. **Correction (2026-11-17, verified in source): `draw_counters_.drawn` is NEVER incremented anywhere in the D3D11 backend — only the struct default `uint64_t drawn = 0` (command_processor.h:78) exists — so A9's `drawn` is always 0 and carries no information, and the per-frame summary's `drawn` is likewise always 0 on D3D11. The script must reconcile the boundary on `issued` only** (`issued` IS incremented at stage D, `:3931`, per frame, reset at `:6794`). Do not treat `drawn=0` as a failure or as "no draws reached the API". **Present HRESULT is explicitly NOT captured in Phase 2 (review correction 3, decision):** the HRESULT lives in `d3d11_presenter.cpp`, which Phase 2 does not modify, and the command processor's frame-boundary record cannot independently establish it. The script verifies the boundary exists for the swap (issued count reconciles with the draw census); it does **not** claim the Present succeeded. A narrowly gated presenter-side Present record is a defined Phase 3 item — it would need the frame number plumbed into the presenter, which does not track `presented_output_frames_`. |

### 3.2 The actual-side fields (per draw)

For each draw that reaches IssueDraw, the actual side is fully
determined by the existing lines:

- Entry: `FH1 tile path … stage=issue_enter` + `FH1 routing issue`
  (prim, idx, mode, pass, winy, c0, surf, vs/ps hashes).
- Outcome: exactly one of — `stage=copy_mode` (→ IssueCopy, A6),
  one of the 16 drop stages (A1), or `issued` (→ A3, with the bound
  line A2).
- Bound target: `FH1 routing bound` — the actual `c0`/`d` keys.

### 3.3 Texture outcomes (the §2.13 audit gap — corrected)

`CreateTexture` returns `nullptr` when the host format has no DXGI
format or no converter (texture_cache.cpp:447-451). The rejection is
logged **once per format** (`LogUnsupportedFormatOnce`, `:435-443`,
bitmask `unsupported_format_mask_logged_`) and the rejected-load log is
capped at 60 (`:592-596`). After the first log, a texture that samples
nothing (unbound SRV samples `(0,0,0,1)` in D3D11) fails silently at
runtime.

The per-slot evidence is the **per-slot** `FH1 srv bind all` line
(`:1135`, capped at 400 logs) — but that cap and its deduplication need
careful handling (review correction 4):

- The dedup key is a hash of 12 fields (slot, base, W, H, fmt, tiled,
  dimension, host W/H/fmt, valid, texture/srv/dimok flags) — **the swap
  is NOT in the key**. The same binding in a later draw (or a later swap)
  does **not** re-log.
- `bind_all_logs` and `bind_all_seen` are **function-static, per-process,
  never reset** (`:1099-1100`). The cap is 400 **distinct** bindings for
  the whole process (counting only from the gated swap,
  `fh1_debug_srv_bind_all_from`), and once hit, further distinct bindings
  are silently dropped.
- **Fix (Phase 2):** when a distinct binding arrives after the cap is
  hit, the texture cache logs one line
  `FH1 srv bind all budget exhausted swap={} distinct={}` (once per
  process). The comparison script checks for this line.

**The script's rule (§4.3):** srv-bind evidence is a **per-swap SET**
keyed by (slot, base, fmt) — not a per-draw record. For a draw that
reached the binding stage (i.e. it has a routing-bound line) and samples
a slot:

- If the set contains a matching entry → use its
  `valid`/`texture`/`srv`/`dimok`.
- If the set has **no** matching entry **and** the budget-exhausted line
  is present for the swap → **UNDECIDABLE** (incomplete evidence). The
  comparison must NOT treat it as a failed binding.
- If the set has **no** matching entry **and** the budget was **not**
  exhausted → the binding is genuinely missing → flag MISTRANSLATED.

A missing record must never be treated as a failed binding when the
budget was exhausted.

### 3.4 Join keys (how each actual line keys back to its guest call)

| Actual line | Joins to guest call by |
|---|---|
| `FH1 routing issue` / tile-path `issue_enter` | (swap, draw-ordinal in decode order) — the routing report validated the order join (0 mismatches over 129 draws; `record_split_` false, so decode order == execution order). |
| `FH1 routing bound` | Same (swap, draw-ordinal); the bound line follows its issue line in log order. |
| `FH1 routing drop` / tile-path exit stage | Same (swap, draw-ordinal); the drop line is emitted in place of the bound line for that draw. |
| `FH1 callog side=A` (stage D, A3) | (swap, draw-ordinal) — carried explicitly in the line. |
| `FH1 callog side=A` (ConfigurePipeline, A4) | Shader hash (vs/ps `ucode_data_hash`) — joins to the G4 shader-load line and to the draw's vs/ps hashes. |
| `FH1 srv bind all` | (swap, slot) + order within the draw — the slot's fetch constant index joins to the G5 texfetch line. |
| `FH1 callog side=A` (IssueCopy plan, A6) | (swap, draw-ordinal) — the copy draw's ordinal. |
| `D3D11 draws frame=` summary | swap number (the `frame=` field). |
| `FH1 callog side=A` (frame boundary, A9) | swap number. The line is a boundary marker only — it does **not** establish the Present HRESULT (§3.1 A9). |
| `FH1 srv bind all budget exhausted` | swap number. Marks the swap's srv-bind evidence as incomplete (§3.3). |
| `FH1 callog budget exhausted` | swap number. Marks the swap's callog evidence as incomplete — any line that would have been emitted after the cap is missing, so the script reports the affected kinds as UNDECIDABLE, not as passes. |

---

## 4. Comparison — the script, the format, and what it flags

### 4.1 The comparison script (Python, runs locally on collected logs)

Location (Phase 2): `~/workspace/dx11-call-logger-scratch/compare.py`
(kept in the scratch dir; a copy of the exact script used is saved with
the Phase 2 evidence). It runs on the VM against the collected
`runtime*.log` files — **never on the PC, never during the game run**.

Inputs: all `runtime*.log` files from the run (the log rotates at ~5 MB;
**always read ALL rotated files**, per the routing report's lesson —
the tile trace fired in rotated logs, not the current one).

**Run filtering (correction from the first run, 2026-11-17): the
collected log directory spans MANY runs** (rotation keeps ~20 files;
the first collection held runs from 11/16 evening through 11/17
01:06, with EIGHT threads' swap-1500 frames mixed together). **The
script MUST analyse exactly one run.** Every log line carries the GPU
thread id in its prefix (`[t27292]`). The script takes `--thread
tXXXX`; if omitted and exactly one thread has an A9 present line for
the swap, it uses that thread; if several threads have one, it lists
them and refuses to guess. All counts, censuses and joins are computed
on that thread's lines only. A second run's swap-1500 data in the same
directory is a different frame and must never be mixed in (the first
run's unfiltered comparison mixed eight runs and produced nonsense
censuses). The script also accepts `--swap N` (default 1500).

**Log-line formats the parser must accept (correction from the first
run):** `BINPRED` / `BINRECORD` lines use the words `true` / `false`,
not `0` / `1`, for `pred`, `out`, `untile_tiling`,
`untile_select_all` (e.g. `BINPRED vb 21795 d 1 op 36 pred 1 out
false sel 00000000FFFFFFFF mask 00000000FFFFFFFF pass -1
untile_tiling false untile_select_all false woff 00000000`). The
parser accepts `true|false|0|1` for those fields. `BINPRED` /
`BINRECORD` carry no swap field — the script attributes them to the
swap by the thread + the A9 frame boundary (lines between the previous
swap's A9 and this swap's A9 belong to this swap's frame), NOT by any
swap value in the line.

### 4.2 The shared record model (what "one machine-readable line per call" means)

The script normalises every parsed line into a record:

```
(side, swap, seq, kind, fields...)
```

- `side`: `G` (guest) or `A` (actual).
- `swap`: the frame number (guest `trace_swaps_`, D3D11
  `presented_output_frames_`).
- `seq`: the order index of the call within its (swap, kind) on its
  side, assigned by the script in log order. For draws this is the
  draw-ordinal; for packets it is the packet ordinal. (`counter_`/`vb`
  are parsed as cross-checks, not as keys — see §0.2 and the routing
  report §1.3.)
- `kind`: `pkt`, `reg`, `draw`, `shaderload`, `texfetch`, `texcreate`,
  `shader`, `copy`, `present`, `state`.

**Line-type → record mapping (the parser table):**

| Log line (prefix) | Side | Kind | Key fields parsed |
|---|---|---|---|
| `FH1 callog side=G … kind=pkt` (NEW) | G | pkt | op, t3seq, pred, out, sel, mask, pass |
| `FH1 callog side=G … kind=pktpost` (NEW) | G | pktpost | op, sel, mask (resulting bin state) |
| `FH1 callog side=G … kind=reg` (NEW) | G | reg | base, count, idx, val (one line per relevant register) |
| `FH1 callog side=G … kind=shaderload` (NEW) | G | shaderload | type, addr, dwords, hash |
| `FH1 callog side=G … kind=texfetch` (NEW) | G | texfetch | first, last |
| `BINPRED` | G | draw | vb, op, pred, out, sel, mask, pass, untile flags, woff |
| `BINRECORD` | G | draw | vb, op, split, viz_blocked, indices, source, sel, mask, woff |
| `FH1 routing guest` | G | draw | vb, prim, idx, src, ok, mode, pass, sel, mask, winy, c0, d0, surf, split |
| `FH1 routing record` / `dispatch` | G | draw | prim, idx, mode, pass, sel, mask, winy, viz |
| `FH1 routing issue` | A | draw | prim, idx, mode, pass, winy, c0, surf, vs, ps |
| `FH1 tile path … stage=` | A | draw | stage, pass, issued, window_y, mode, sel, mask |
| `FH1 routing bound` | A | draw | prim, mode, pass, bits, c0, d |
| `FH1 routing drop` | A | draw | stage, prim, mode, pass |
| `FH1 callog side=A … kind=issued` (NEW, A3) | A | draw | ord, prim, mode, pass, outcome, topology |
| `FH1 callog side=A … kind=shader` (NEW, A4) | A | shader | vs, ps, is_valid, pipeline |
| `FH1 callog side=A … kind=copy` (NEW, A6) | A | copy | copy, skip, kind, copying_depth, src, dst |
| `FH1 srv bind all` | A | texcreate | slot, base, fetch WxH, fmt, tiled, shader_dim, valid, host WxH, hostfmt, hostdim, texture, srv, dimok, signed |
| `FH1 srv bind all budget exhausted` (NEW) | A | texcreate | swap, distinct (count at exhaustion) |
| `FH1 texture load rejected` | A | texcreate | base, WxHxD, format, tiled, pages |
| `FH1 callog side=A … kind=present` (NEW, A9) | A | present | swap, issued, drawn (boundary marker only — **not** the Present HRESULT) |
| `FH1 callog budget exhausted` | — | — | swap, budget (marks the swap's callog evidence incomplete) |
| `D3D11 draws frame=` | A | present | passes, issued, drawn, dropped_* counters |

**The literal `FH1 callog` grammar (new lines only):**

```
FH1 callog side=<G|A> swap=<n> kind=<kind> <key=value ...>
```

- Fields are space-separated `key=value`, values are decimal unless
  prefixed (`0x` hex, `{:016X}` for hashes/selects/masks, `{:08X}` for
  bases/registers). No spaces inside a value. One line per call per
  stage. No heap allocation in the backend to build it (fields are
  passed directly to `REXGPU_INFO`, as the existing probes do).

### 4.3 What the script flags

For each swap in the window, the script:

1. **Builds the guest census.** Counts every guest call by kind from
   the G-side lines (pkt census from G0; draws from BINPRED +
   routing-guest, cross-checked against each other; reg writes from G3;
   shader loads from G4; texfetch writes from G5). **Continuity is
   validated with `t3seq`, not `counter_`** (review correction 5):
   the pkt lines' `t3seq` values for the swap must be consecutive
   (each exactly one more than the previous). `counter_` advances for
   Type-0/1/2 packets too, so gaps in `counter_` are expected and are
   **not** flagged. Type-1/Type-2 packets are explicitly excluded from
   the census (Type 1 writes two registers, Type 2 is a no-op — neither
   carries draw-affecting work in FH1).
2. **Derives the expected D3D11 action** for each guest call using the
   §2.2 rule table, from the logged fields. For SET_BIN_* packets the
   pktpost line's resulting sel/mask is checked two ways: it differs
   from the pkt line's pre-handler sel/mask only in the half the opcode
   writes (LO changes the low 32 bits, HI the high 32, SET_BIN_MASK sets
   the full 64-bit value), and it equals the next packet's pre-handler
   sel/mask (the state carried forward). The pkt line carries no payload,
   so the exact new value is not independently re-derived — the check is
   consistency of the state transition, not a re-computation.
3. **Matches against the actual log** by the §3.4 join keys.
4. **Flags four classes:**
   - **DROPPED** — a guest call with no D3D11 equivalent and no
     rule-sanctioned drop outcome. (A guest draw whose expected outcome
     is `issued` but which has no bound/issued line, and no drop line,
     is DROPPED. A guest draw whose expected outcome IS a named drop
     stage, and which has that drop line, is **not** flagged — it is
     accounted for.)
   - **MISTRANSLATED** — a D3D11 action that does not match the expected
     translation. Concretely: bound target key ≠ derived target key;
     topology ≠ derived topology; bound shader hashes ≠ guest shader
     hashes; a draw issued when the rules say it should have been
     dropped (or vice versa); a texture slot bound with `valid=0` when
     the fetch setup says it should be valid (or the reverse); a
     pktpost sel/mask that does not equal the handler's expected result.
   - **SPURIOUS** — a D3D11 action with no guest origin: an issued draw
     with no guest draw at that ordinal; a bound target with no
     corresponding guest draw; a Present with no XE_SWAP in the swap.
   - **UNDECIDABLE** — a rule whose inputs are not in the log (see
     §2.3), **or whose evidence is incomplete**: the callog budget was
     exhausted for the swap (`FH1 callog budget exhausted` present), or
     the srv-bind budget was exhausted (§3.3) for a slot whose binding
     is not in the per-swap set. Reported separately, never silently
     counted as a pass — and a missing srv-bind record is **never**
     treated as a failed binding when the budget was exhausted.
5. **Emits the rollup**, modelled on the routing report §2/§7:
   - Per-swap census table (guest calls by kind; actual actions by
     kind; joins; flags by class).
   - Outcome rollup by (primitive, mode, pass) — the routing report's
     table, extended with the new kinds.
   - Per-key inventory (actual bound keys, as in routing §8.1).
   - The four flag lists with the exact log lines for each flag.

### 4.4 Acceptance criteria (Phase 2)

1. **The logger reproduces the routing report's census STRUCTURE for
   the captured swap:** the three independent draw counts (BINPRED,
   routing-guest, draw packets) agree with each other; every guest
   draw has an outcome (issued / copy / named drop stage) with 0
   unaccounted; and the callog cross-checks (A-issued == stage-D
   issued, A-copy == tile-path copy) match. The routing report's
   129/50/31/48 numbers are the REFERENCE values from its frame, not
   a law — the guest may genuinely issue a different number of draws
   in another frame (the swap-3000 runs both have 131/52/31/48, the
   +2 confirmed by three independent counts agreeing at 131). A
   difference from the baseline is reported as a frame-content
   finding, not a logger failure. *(Corrected 2026-11-17 after the
   swap-3000 runs; see the review-corrections note, items 9(i) and
   10(b). The original wording required the exact 129-draw census.)*
2. **Packet census reconciles (corrected per review):** every Type-3
   packet in swap 1500 has a G-pkt callog line, and the pkt lines'
   `t3seq` values are consecutive within the swap (each exactly one more
   than the previous). `counter_` gaps are expected (it advances for
   Type-0/1/2 packets too) and are **not** a failure. Type-1/Type-2
   packets are explicitly excluded from the census. Every draw-relevant
   Type-0 write has a G-reg callog line **for each relevant register in
   its range** (index + value).
3. **Zero DROPPED / MISTRANSLATED / SPURIOUS flags for swap 1500**
   that are not already explained by the routing report (the 48 point
   draws and the 31 copies are rule-sanctioned outcomes, not flags).
   Any flag the script raises must be investigated and either
   explained (rule-table correction) or reported as a finding.
4. **No rendering-behaviour change:** the run's swap routing counts
   have the same STRUCTURE as the routing report's (issued + copy +
   named drops = guest draws, 0 unaccounted), and the per-band fix
   and all existing probes are intact in the binary. (The exact
   counts may differ by frame content — see criterion 1.)

---

## 5. Gating and cost

### 5.1 New cvars (`src/graphics/flags.cpp`)

| Cvar | Type | Default | Meaning |
|---|---|---|---|
| `fh1_debug_callog` | INT32 | 0 | The swap to log. `0` = off. When set to N, callog lines fire for swap N only. |
| `fh1_debug_callog_budget` | INT32 | 8192 | Max `FH1 callog` lines per process (all kinds, both sides). When the budget is exhausted, logging stops silently (one final `FH1 callog budget exhausted` line). |

Declare with `REXCVAR_DECLARE` where used (D3D11 `command_processor.cpp`
already declares `fh1_debug_tile_trace_swap` at `:47` and
`fh1_debug_hide_hud` at `:46`; the shared file defines
`gpu_trace_bins_vblank` at `:60` — follow the same pattern). Definitions
go in `flags.cpp` next to the other `fh1_debug_*` cvars (near
`fh1_debug_tile_trace_swap`, `:23`).

The run sets:
`--fh1_debug_callog=1500 --fh1_debug_tile_trace_swap=1500`
(plus the existing run flags). `gpu_trace_bins_vblank=1500` is set as
well for the BINPRED/BINRECORD cross-check.

### 5.2 Window

One swap per run (the callog swap). The window is the same swap the
existing probes use (1500), so the callog lines and the routing/BIN
lines describe the same frame and can be cross-checked line for line.

### 5.3 Cost and hot-path behaviour

- **Off (`fh1_debug_callog == 0`):** one integer comparison per packet
  (Type-3 dispatch), per Type-0 packet, per draw (IssueDraw entry),
  per IssueCopy, per frame. No allocation, no formatting, no I/O.
  Negligible — the same cost profile as the existing probe gates.
  (The `t3seq` counter increments once per Type-3 packet regardless of
  the gate — one integer increment, negligible.)
- **On:** one `REXGPU_INFO` line per call per stage, same as the
  existing routing probes. The budget (`fh1_debug_callog_budget`,
  default 8192) caps total output; the G-reg line logs one line **per
  relevant register** (not per packet — a packet writing many relevant
  registers emits many lines, bounded by the budget). When the budget is
  exhausted the helper logs one `FH1 callog budget exhausted` line and
  stops, so the script can tell incomplete evidence from a clean run
  (§4.3). **Budget accounting correction (2026-11-17, from the first
  run): the budget MUST be charged per emitted LINE, not per
  `Fh1CallogEnabled` call.** The first implementation called
  `Fh1CallogEnabled` once per Type-0 packet in the G3
  `callog_reg_write` lambda and then emitted up to `count` lines inside
  the loop — so a bulk shader-constant upload (0x4000–0x47FF, up to
  2048 registers per packet) emitted thousands of lines against one
  budget unit: the first run emitted **80,403 reg lines** against an
  8192 budget, and the budget-exhausted line fired (for the pkt lines)
  while reg lines kept flowing. **Fix:** check/charge the budget
  inside the G3 loop (one `Fh1CallogEnabled` call per emitted line, or
  an explicit lines-remaining counter decremented per line). **Run
  setting:** the second run sets a higher budget
  (`--fh1_debug_callog_budget=200000`) — a busy frame emits ~90k
  G-lines, mostly bulk constant uploads, and the packet census
  (t3seq) must not be cut off mid-frame by the cap. Log volume at
  that budget is ~11 MB for the traced swap, acceptable (the first
  run wrote ~110 MB of logs overall).
- **No heap allocation per call:** fields are passed directly to
  `REXGPU_INFO` (as the existing probes do). Do not build
  `std::string`s or `std::vector`s in the probe code. (This matters
  because the audit found per-draw heap allocations in `UpdateBindings`
  to be the cheapest per-draw win on the bottleneck thread — the logger
  must not add more.)
- **No GPU readbacks:** unlike the c2/guest/copydown probes, the
  callog lines are pure log lines (register values and decision
  outcomes already in CPU memory). No `ResolveSubresource`, no staging
  copy, no `Map`. No GPU stall.
- **Threading:** all callog lines fire on the decode/GPU thread (the
  same thread the existing probes use). The budget counter is a
  function-static `uint32_t` (same pattern as the existing probe
  budgets, e.g. `c2_probe_draws` at D3D11 `:3412`) — process lifetime,
  single-threaded use assumed.

### 5.4 Free-roam / first-occurrence (Phase 3, defined here)

Phase 2 uses the fixed swap 1500 with a run-procedure free-roam check
(§6.3). The first-occurrence re-gate is defined for Phase 3 as:
a new mode where `fh1_debug_callog` takes the value `-1` (meaning "first
swap at which a composite draw is issued"), the D3D11 side arms the
logger when the composite-draw counter for a swap becomes non-zero,
and the shared side logs the same swap (communicated via the existing
`trace_swaps_` / `presented_output_frames_` alignment, with the
D3D11-side arming recorded in the log so the script can verify the two
sides logged the same swap). Do not implement this in Phase 2.

**First-run evidence (2026-11-17) for why this matters:** the fixed
swap-1500 gate is NOT a reliable free-roam gate. The first callog run's
swap 1500 (thread t27292) was a **3,418-draw loading/menu frame**
(A9 issued=3259), not the routing report's 129-draw free-roam frame —
while four other runs in the same log window reached swap 1500 as the
129-draw free-roam frame (threads t16804, t25476, t28624, t8340). The
game's menu presents fast, so a fixed swap can land anywhere from
loading to free roam depending on menu dwell time. **Rule for the
second run onward: the captured swap is accepted as the comparison
frame ONLY if its routing census matches the free-roam profile
(§4.4) — otherwise the run is discarded and re-gated.** The
first-occurrence trigger above is the robust fix; until it is
implemented, use a late swap + the census check as the gate (run long,
verify the captured swap's census, re-run if it is not free roam).
The same rule applies to the Vulkan run (§9): both runs must capture
the same game state, verified by the frame-equivalence pre-check
(§9.4).

### 5.5 What is NOT re-logged (reuse)

- Draws: `BINPRED`, `BINRECORD`, `FH1 routing guest/record/dispatch/
  issue/bound/drop`, `FH1 tile path` — all reused as the detailed draw
  evidence. The callog lines for draws are the A3/A4 stage-D and
  ConfigurePipeline outcome lines only (§3.1).
- Texture binds: `FH1 srv bind all` — reused.
- Resolve plan: the IssueCopy plan log lines (`:4492`, `:4543`) —
  reused (plus the A6 callog line for the parser).
- Frame summary: `D3D11 draws frame=` — reused.
- Surface/pixel evidence: the c2 probe, composite dump, guest probe,
  surface probe, vertex dump — reused where they already exist; the
  callog does not duplicate pixel data.

---

## 6. Implementation plan for Phase 2

### 6.1 Probe code locations (all additive, 0 lines removed)

| File | Change |
|---|---|
| `src/graphics/flags.cpp` | Add `fh1_debug_callog` (INT32, 0) and `fh1_debug_callog_budget` (INT32, 8192), with descriptions, next to `fh1_debug_tile_trace_swap` (`:23`). |
| `src/graphics/command_processor.cpp` (shared) | G0: `FH1 callog side=G kind=pkt` in `ExecutePacketType3` (`:2277`), after the predicate/bin evaluation (`:2287-2391`), before the per-opcode handler — with `t3seq` (a file-static Type-3 sequence counter incremented once per Type-3 packet). G0-post: `FH1 callog side=G kind=pktpost` after the opcode switch, before `return result;` (`:2671`), for the five SET_BIN_* opcodes only. G3: `FH1 callog side=G kind=reg` in `ExecutePacketType0` (`:2223`) — one line **per draw-relevant register in the written range** (idx + val, read back after the write). G4: `FH1 callog side=G kind=shaderload` in `ExecutePacketType3_IM_LOAD` (`:2542`) and the IMMEDIATE variant. Add `REXCVAR_DECLARE` for the two new cvars at the top (follow `gpu_trace_bins_vblank`, `:60`). |
| `src/graphics/d3d11/command_processor.cpp` | G5: `FH1 callog side=G kind=texfetch` at `:887` and `:987` (fetch-constant write notifications). A3: `FH1 callog side=A kind=issued` at stage D (`:3860`). A4: `FH1 callog side=A kind=shader` after `ConfigurePipeline` returns (`:2028`). A6: `FH1 callog side=A kind=copy` at the plan branch (`:4552`). A9: `FH1 callog side=A kind=present` at the frame boundary (`:6640`) — boundary marker only, **no Present HRESULT** (§3.1). Add `REXCVAR_DECLARE` for the two new cvars (follow `:46-47`). |
| `src/graphics/d3d11/texture_cache.cpp` | **NEW (review correction 4):** one `FH1 srv bind all budget exhausted` line when a distinct binding arrives after the 400-log cap is hit (`:1129-1136` — restructure the `if` into `bind_all_new && bind_all_logs < 400` for the log path and an `else if (bind_all_new)` for the exhaustion line; the normal-path log output is identical). Add a `static bool bind_all_exhausted_logged` next to `bind_all_logs`/`bind_all_seen` (`:1099-1100`). |
| `include/rex/graphics/flags.h` | Add `REXCVAR_DECLARE(int32_t, fh1_debug_callog)` and `REXCVAR_DECLARE(int32_t, fh1_debug_callog_budget)` after the `fh1_debug_tile_trace_swap` DECLARE (`:66`) — required: without the header DECLAREs, `REXCVAR_GET` in the shared command_processor.cpp TU fails to compile (`FLAGS_fh1_debug_callog_storage_` undeclared). |
| (No change) | `fh1_native_executor.cpp` (per-band fix), `texture_cache.cpp`, `pipeline_cache.cpp`, `d3d11_presenter.cpp`, `dxbc_translator.cpp` — read-only references only. |

**Backups first.** Before editing, back up each touched file on the PC
as `*.musebak-callog-20261117`. Verify the backups byte-match the
pre-edit files. The existing `.musebak-*` backups (per-band, routing,
footprint-vs, WaitForGpu-restore) must not be overwritten.

**Apply-script discipline (proven by the footprint agent):** write a
Python apply script in the scratch dir, dry-run it against a local copy
of each file first, verify the diff, then deploy to the PC. One script
per file, or one script with per-file functions — either way, the diff
must be reviewable before it touches the PC. Save the final diff as
`~/workspace/dx11-call-logger-probe-diff-2026-11-17.txt`.

### 6.2 Build

- `E:\PinyonShift\work\.local\muse_build_game.bat`,
  `CMAKE_BUILD_PARALLEL_LEVEL=4`, expect `BUILD_EXIT=0`.
- **Mandatory (stale EXE):** rebuild even if the only change is the
  probe code — the PC's `pinyon_shift.exe` (built 00:18 PC from the
  pre-restore edited source) is stale relative to the restored source.
- Verify before any run: DLL contains the string `"FH1 callog"`,
  `rexgpu-fh1.dll` and `pinyon_shift.exe` mtimes are newer than the
  probe deployment, and the per-band fix + existing probes are still
  present (string search for `"FH1 routing"` and `"FH1 c2 probe"`).

### 6.3 Run

- Via the scheduled task **"PinyonShift Test Run"** (session 1 — SSH
  launches land in session 0, Seth hears but doesn't see),
  `E:\PinyonShift\work\.local\run_game.cmd` with
  `--gpu_backend=d3d11 --d3d11_adapter=GTX --fh1_debug_callog=1500
  --fh1_debug_tile_trace_swap=1500` (plus the existing flags already in
  `run_game.cmd`; do not remove them).
- **Second-run settings (from the first run's lessons):** add
  `--fh1_debug_resolve_trace=1500` (enables the `FH1 resolve trace`
  lines, §8.1 B2 — no source change) and
  `--fh1_debug_callog_budget=200000` (the default 8192 budget is far
  too small once bulk shader-constant uploads are logged per register;
  the first run emitted ~90k G-lines for the swap).
- **Free-roam check (strengthened 2026-11-17):** after the run,
  confirm swap 1500's log contains composite draws (the
  `FH1 composite draw` lines / c2 probe lines) AND that its routing
  census matches the free-roam profile (129 guest draws / 50 issued /
  31 copy / 48 no_guest_visible_output — §4.4). The first callog run's
  swap 1500 was a 3,418-draw loading/menu frame (A9 issued=3259) with
  composite draws present — composite presence alone does NOT prove
  free roam. **If the census does not match the free-roam profile,
  discard the run and re-gate** (late swap + re-run, or the
  first-occurrence trigger of §5.4 once implemented). Do not analyse
  a loading-frame log as if it were free roam. **Run the comparison
  with `--thread tXXXX`** for the run's GPU thread (§4.1) — never
  analyse a multi-run log directory unfiltered.
- Stop the game after collection. Collect **all** `runtime*.log` files
  from `E:\PinyonShift\work\.local\preview\logs\` (rotate at ~5 MB;
  `runtime.log` locks exclusively while the game runs — copy to Temp
  first, then scp).

### 6.4 Evidence to collect

- All `runtime*.log` files (guest + actual logs in one place).
- The comparison script (`compare.py`) and its output.
- The probe diff (`~/workspace/dx11-call-logger-probe-diff-2026-11-17.txt`).
- The build log (`muse_build_game.log`) excerpt showing `BUILD_EXIT=0`
  and the DLL/EXE mtimes.
- The per-band fix + existing-probe presence verification (string
  search results).

### 6.5 Acceptance criteria (restated from §4.4)

1. The comparison script reproduces the routing report's census
   STRUCTURE for the captured swap (three independent draw counts
   agree; 0 unaccounted; A-issued and A-copy match the tile-path
   counts) — the 129 / 50 / 31 / 48 numbers are the reference values
   from the routing run's frame, and a difference is a frame-content
   finding, not a logger failure (see §4.4 criterion 1).
2. Packet census reconciles (every Type-3 packet has a G-pkt line;
   `t3seq` consecutive within the swap; `counter_` gaps expected and
   not a failure; Type-1/2 explicitly excluded; every draw-relevant
   Type-0 register has a G-reg line with its index and value).
3. Zero unexplained DROPPED / MISTRANSLATED / SPURIOUS flags for swap
   1500.
4. No rendering-behaviour change (swap-1500 routing counts identical
   to the routing report; per-band fix and all existing probes intact).

### 6.6 What Phase 2 deliberately does NOT do

- No rendering fix, no broad refactor, no `IssueDraw`/`IssueCopy`
  restructuring.
- No per-call logging of D3D11 state-setting API calls (§3.1 A8).
- No first-occurrence re-gate (§5.4 — Phase 3).
- No Type-1/Type-2 packet logging (§1.1).
- No Present HRESULT capture (§3.1 A9 — explicitly out of Phase 2;
  presenter-side record is a Phase 3 item).
- No WaitForGpu fix (separate, stopped; the logger is now first).
- No change to the per-band fix, the WaitForGpu state, or any existing
  probe.

---

## 8. Band 0 + #23-split checks (added 2026-11-17)

Two hypotheses from Seth's photo analyses, and the exact logger checks
that settle them. Neither is a rendering change; both are read off the
log by the comparison script (checks B1–B3 below, added to §4.3).

### 8.1 Band 0 — expected-zero copy-down, resolve rect/base, culling

**Hypothesis (pasted AI analysis, ~02:05):** band 0 (rows 0–256,
window_y == 0) is (a) missing its `UntileBandBit(0)` copy-down,
(b) wiped by a subsequent clear/resolve targeting EDRAM base 0, or
(c) having its draws culled by viewport/scissor clipping before
IssueDraw — while bands 1/2 render the sky/horizon fine. The
car-preview block is hypothesised to be a sub-rect EDRAM resolve /
tile-owner issue on the left-hand tiles.

**Evidence gap it exposed:** the copy-down pixel verification covered
bands 256 and 512 only — band 0's copy-down was never pixel-verified,
and the fix-run counts reported band-256 (1,998) and band-512 (2,298)
with no band-0 count.

**Checked vs evidence (correction 6):**
- (a) is ruled out BY DESIGN: both record sites guard on
  `untile_band != 0` (fh1_native_executor.cpp:2677, :2691),
  `Fh1D3D11CopyDownRows(720, 0) = 0`
  (fh1_native_surfaces.h:101: `band && band < untile_height ?
  min(untile_height - band, band) : 0`), and the source comment
  (:126–133) states "Bit 0 is the band at offset 0, which needs no
  copy-down (Fh1D3D11CopyDownRows returns 0 for it) and is never
  recorded." The per-band fix review (:408, :549, :579) documents
  "band 0 as a no-op — exactly as before, by design, not a missing
  copy-down" (Seth-verified). **Expected: ZERO band-0 copy-downs.
  The comparison must NOT flag their absence.**
- (b) is ruled out AT THE RESOLVE STAGE for the captured frame: the
  follow-up investigation's resolve-source probe sampled exactly band
  0's rect (0,0–1280,256) and got uniform grey (0.258789) at every
  sample, not black.
- (c): nothing to cull in the captured frame — the guest issues no
  EDRAM triangles in any band; the only EDRAM draw is the fullscreen
  grey rectangle.
- **Still open:** the photo's top-dark / bottom-sky split itself
  (the c2 bottom-half anomaly, deferred-tape-execution hypothesis).
  That is what check B3 below is for.

**Coverage verdict (what the logger already captures vs what is added):**
- (a) Per-band copy-downs 256/512: COVERED by the ungated
  `D3D11 FH1 untile copy-down: key={} band={} rows=0..{} samples={}`
  line (fh1_native_executor.cpp:2844) — one line per copy-down issued.
  It carries NO swap field, so per-swap counts need bracketing by the
  A9 frame boundaries. **Minimal addition (Phase 2): one
  callog-gated line in `EndUntiledTiling`, per surface per flush:**
  `FH1 callog side=A swap={} kind=untileflush key={} bands={:08X}`
  (bands = the `untile_bands_` mask for that surface at flush time;
  gated on `fh1_debug_callog`, one line per surface per traced swap,
  no budget pressure). The comparison then asserts: bit 0 clear
  (expected-zero by design — if bit 0 were ever set it would be a
  finding, since the guard says it cannot be); bits 1/2 set iff draws
  occurred in those bands (from the tile trace / routing-guest
  `winy`); and the count of `untile copy-down` lines bracketed by the
  flush line equals the popcount of the mask (each set bit yields one
  copy-down; `copy_rows` for band 256 = 256, band 512 = 208, per
  `Fh1D3D11CopyDownRows(720, band)`).
- (b) Resolve rect/base + window_y/scissor for 3D passes: COVERED by
  existing lines, no new code — the `FH1 tile draw` line (D3D11
  `:2001`, trace-tile-gated, per issued draw: `window_y`,
  `viewport_y/h`, `scissor_y/h`, `claim_y0..y1`) plus the routing-guest
  `winy`, plus the `FH1 resolve trace` line (D3D11 `:4893`: swap,
  call, key, `rect=x0,y0-x1,y1`, `dest_base`, `dest_pitch`, info,
  extent, src, src_fmt, owner, staging — gated by
  `fh1_debug_resolve_trace`, capped at 4000 logs/process). **Run
  setting (no source change): add `--fh1_debug_resolve_trace=1500`
  to `run_game.cmd` for the second run.** The `FH1 tile source hash`
  line (`:4010`: swap, resolve_count, key, rows, hash) covers the
  resolve SOURCE rows but lives in the tape path
  (`command_lists_enabled_`, default off) — it does NOT fire in the
  current runs; do not rely on it.
- (c) Viewport/scissor culling of rows 0–256 draws: COVERED at the
  stage level — the tile-path drop lines carry `window_y` + stage per
  dropped draw, and the tile-draw line carries `viewport_h` /
  `scissor_h` / `claim_y` per ISSUED draw (it sits after the early
  exits, `:1971`). A draw dropped at a named stage with `window_y==0`
  is visible; a draw with an empty viewport/scissor that still issues
  shows `viewport_h==0` / `scissor_h==0` / an empty claim. **Known
  gap (same class as §2.3):** for DROPPED draws, the viewport/scissor
  values at drop time are NOT logged (`viewport_info`/`scissor` are
  computed after the early exits, `:1982+`) — extending the drop
  lines would require computing them earlier, which is more than
  minimal. The tape `empty_viewport` counter (`:4455`,
  `LogTapeCounters`) is a per-process shutdown summary, not per-swap,
  and is NOT usable for the swap check.

**Comparison checks (B1–B3, added to §4.3):**
- **B1 (per-band copy-down):** from the `untileflush` mask + the
  bracketed `untile copy-down` lines: band 256 and 512 copy-downs
  present with rows 0..256 / 0..208 iff draws occurred in those
  bands; band-0 copy-down count == 0 (expected by design — flag if
  nonzero, flag if a band with draws has no copy-down).
- **B2 (resolve rect/base):** from the `FH1 resolve trace` lines for
  the swap: every resolve's rect + dest_base + dest_pitch recorded;
  the band-0 rect (0,0–1280,256) resolve's dest_base reported (the
  wipe question is answered by the resolve-source probe's grey
  reading, already established — B2 records the rect/base so a
  future frame can be checked the same way).
- **B3 (band-0 culling):** distribution of dropped draws by
  `window_y` (from routing-guest `winy` + routing-drop stage); flag
  any dropped draw that targets the EDRAM colour surface
  (`c0` base 0x000 / key `0/32/4x/c3`) with `window_y==0` for review.
  In the captured frame the expected result is: no EDRAM-targeting
  draw is dropped in any band (the only EDRAM draw is issued).

### 8.2 #23 split — depth/stencil vs scissor vs mesh (review addition 7)

**Observation (live-gameplay photo):** top ~360 rows wiped to black,
bottom ~360 rows left blue — a split at the horizon line, NOT at a
256-pixel band seam. Four mechanisms proposed: (1) depth/stencil
rejecting the bottom half, (2) Y-driven VS interpolants, (3)
RECTLIST expansion, (4) leftover scissor (0,0,1280,360).

**Checked vs evidence:** (2) RULED OUT for swap-1500 (hand-simulation:
o0=(0,0,0,1), o1=(0,0,0,0) at EVERY vertex; W from constants
c16.w×c17.w; no Y-dependence). (3) does NOT apply to the captured #23
— **#23 is a TRIANGLELIST (prim 4, 48 verts), NOT a RectangleList**;
the RectangleList draw is A#0, the EDRAM grey rectangle (prim 8, 3
verts). The logger must not treat #23's interpolants as Y-varying in
that frame, and must not apply a RECTLIST-expansion failure to it.
The live frame's #23-equivalent is a different frame and its state is
unmeasured — that is exactly what the A3 fields below are for.

**Per-draw capture (A3, §3.1):** depth/stencil state
(`normalized_depth_control`: raw dword + `z_enable`,
`stencil_enable`, `zfunc`, `stencilfunc`), the full scissor rect,
the claim rect, the index count, guest/host vertex counts. The
c2-probe footprint supplies the true rasterised extent (§3.1 A3
Y-range note).

**Decision tree (one capture run, per #23-equivalent draw):**
- Footprint top-only + scissor full-frame + depth/stencil open →
  the mesh itself is top-only (the guest's screen-space mesh covers
  only Y = 0..360; 48 verts = 16 triangles, a grid/mesh, not a
  2-triangle quad).
- Footprint full-range + wipe top-only → scissor or depth/stencil
  (read the A3 fields: a scissor of (0,0,1280,360), or depth/stencil
  rejecting the bottom half).
- All three open + footprint full-range → the split comes from
  somewhere else entirely (not the mesh, not the scissor, not
  depth/stencil).

**Out of scope (recorded, not Phase 2):** sky-shader-0/NaN,
far-plane (z=1.0) depth-fail, NaN-in-HDR-bloom — all presuppose the
sky dome IS drawn; in the captured swap-1500 frame it is NOT.
NaN-in-bloom stays LIVE only for the Corrado car-preview black box
(different scene, offscreen buffer, never investigated); a NaN scan
of that buffer is a follow-up probe idea, NOT Phase 2 scope.

---

## 9. Two-logger cross-backend comparison (Seth's direction, 2026-11-17)

The D11-only comparison (§4) derives expected behaviour from Pinyon's
own D11 translation rules, so it cannot catch translation-rule errors
(§0.3). The cross-backend comparison fixes that: the Vulkan run is the
independent reference. **The D11-only check stays as the FIRST
comparison; the cross-backend check is the SECOND, stronger check and
now the PRIMARY one. Do NOT drop the D11-only check.**

### 9.1 The two loggers

**Logger 1 — Vulkan run, translated to D11.** Run the game on Vulkan
(`--gpu_backend=vulkan`). Log the guest calls plus what Vulkan does
with them, then TRANSLATE those logs into their D11 equivalents — the
same surface keys, the same draw-accounting fields, the same outcome
categories — so the comparison is apples-to-apples. This is the
EXPECTED D11 behaviour, derived from an independent backend.

**Logger 2 — D11 run, D11 logs.** The D11 run as already built
(§1–§6): guest calls + D11's actual calls/decisions, in the same
record format.

**Compare:** translated-Vulkan D11 calls vs actual D11 calls, per
guest call. Divergences are where D11 differs from the working Vulkan
reference.

### 9.2 What already fires on Vulkan (no new code) vs what is added

The guest side lives in the SHARED command processor, so it fires on
ANY backend with the same cvars (`--fh1_debug_callog=N`,
`--fh1_debug_tile_trace_swap=N`, `--gpu_trace_bins_vblank=N`):
- G0 pkt/pktpost, G3 reg, G4 shaderload (shared CP) — the full guest
  call log, identical grammar on Vulkan and D11.
- `BINPRED` (shared `:2473`), `BINRECORD` (shared `:3423`) — the
  predicate/bin/viz record per draw.
- `FH1 routing guest` stage A (shared `:3381`) — one line per draw
  packet decoded at the traced swap (prim, idx, mode, pass, sel/mask,
  winy, c0/d0/surf). **This is the line that decides the headline
  question:** it shows whether the guest issues EDRAM-targeting world
  triangles at all, on either backend.
- `FH1 routing record` stage B (shared `:3494`) and the stage-B drops
  (`:3583` skip_draws, `:3614` band suppression).
- `FH1 band2` packet trace (shared `:2427`: swap/counter/depth/op/
  pred/...) — exists; not required for the comparison, noted for
  forensics.

The backend side is per-backend and must be mirrored. D11 has it
(§3.1 stages C/D/E + drops + bound key + A-lines). **Vulkan has
NONE of it today** (verified in the fetched
`src/graphics/vulkan/command_processor.cpp`, 530,256 bytes): no
`FH1 routing` / `FH1 tile` / `BINPRED` lines in the Vulkan CP at all.
**Minimal addition (Phase 2b):** mirror lines in
`VulkanCommandProcessor::IssueDrawImpl`
(`src/graphics/vulkan/command_processor.cpp:5728`), in the shared
record grammar (§9.3):
- Entry (≈ stage C): at `IssueDrawImpl` entry, one line carrying
  prim, idx, mode, pass, winy, c0/d0/surf, vs/ps hashes — the same
  fields as D11's `FH1 routing issue` (D3D11 `:1842`). The register
  file (`regs`), `edram_mode` and the window offset are in scope at
  entry (the function already reads them for its `draw_fail` lambda,
  `:5749-5760`, which logs prim/index/source/major/path/tess/edram
  on failure).
- Early exits (≈ stage A1): one line per exit site naming the stage,
  mirroring D11's routing-drop vocabulary. The sites in
  `IssueDrawImpl` (verified): `:5797` vertex-shader memexport without
  `vertexPipelineStoresAndAtomics` (`memexport_vs_unsupported`);
  `:5905` pixel-shader memexport without
  `fragmentStoresAndAtomics` (`memexport_ps_unsupported`); `:5983`
  triangle fan not converted (`fan_not_converted`); `:5996`
  unsupported host vertex shader type
  (`unsupported_host_vs_type`); `:6732`/`:6736` invalid vertex fetch
  constant (`invalid_vfetch`); `:6749` vertex buffer request failed
  (`vbuf_request_failed`); `:6781` memexport stream request failed
  (`memexport_range_failed`); `:6792` shared-memory residency failed
  (`memexport_residency_failed`). The implementer confirms each
  site's condition against the source at build time; the stage names
  above are the specified vocabulary.
- Draw call (≈ stage D): at the `CmdVkDrawIndexed` site (`:6911`,
  gated by `!REXCVAR_GET(fh1_debug_skip_draw_calls)`), one line
  carrying prim/mode/pass/verts — the same fields as D11's
  `FH1 routing issued` (`:3931`) + A3 `kind=issued`.
- Bound render target (≈ stage E): at the render-target/framebuffer
  selection in `IssueDrawImpl` (the Vulkan equivalent of D11's
  `BindDrawRenderTargets` `:1004`), one line carrying the bound
  surface key (`c0`/`d` in `Describe()` form) — the same fields as
  D11's `FH1 routing bound` (`:2059`). The exact site is identified
  during implementation from the fetched Vulkan source (render passes
  begin via `CmdVkBeginRenderPass` at `:4744`/`:5106`/`:5199`, in
  helper functions); the design requires the line to name the surface
  the draw actually targets, in guest EDRAM-key form, so it joins
  with the D11 bound key.
- Copy path: `VulkanCommandProcessor::IssueCopy` (`:7162`) — one
  line mirroring D11's A6 `kind=copy` (resolve/copy/skip/kind/
  copying_depth/src/dest/dest_extent).
- Frame boundary: one line per presented frame carrying swap +
  issued count, mirroring D11's A9 (boundary marker only; same
  Present-HRESULT exclusion as correction 3 — the Vulkan presenter's
  result is not plumbed either).
- The Vulkan FH1 native executor
  (`src/graphics/vulkan/fh1_native_executor.cpp`, 177,084 bytes)
  owns the Vulkan untile/banding machinery; the per-band copy-down
  accounting (§8.1 B1) is specified against the D11 executor's flush
  line — the Vulkan equivalent is a follow-up if the cross-backend
  comparison shows a band divergence (do not duplicate the flush
  line speculatively).

Gating: the Vulkan lines use the SAME cvars (`fh1_debug_callog`,
`fh1_debug_tile_trace_swap`) and the SAME budget helper semantics
(per emitted line, §5.3 correction). The Vulkan run sets
`--gpu_backend=vulkan` (no `--d3d11_adapter` flag) with the same
swap + budget values as the D11 run.

### 9.3 The shared per-draw record (translation layer)

Both backends fill the SAME record per guest draw; the comparison
never reads a backend-native field directly. Fields (all guest-side
values are backend-independent; the backend side fills the outcome):

| Field | Meaning | Source (both backends) |
|---|---|---|
| `swap` | Presented frame | `presented_output_frames_` / `trace_swaps_` |
| `ord` | Guest draw ordinal within the swap (0-based, by guest packet order) | Order of `FH1 routing guest` lines |
| `prim` | Guest primitive type (xenos enum) | `vgt_draw_initiator.prim_type` |
| `idx` | Index/vertex count | `vgt_draw_initiator.num_indices` |
| `mode` | EDRAM mode (`RB_MODECONTROL.edram_mode`) | Guest registers |
| `pass` | Tiling pass (`TilingPassOfSelect()`: -1, 0, 1, 2) | Bin select |
| `sel` / `mask` | Bin select / mask | Guest state |
| `winy` | Window Y offset (band: 0 / -256 / -512) | `PA_SC_WINDOW_OFFSET` |
| `c0` / `d0` / `surf` | Intended colour/depth/surface keys (guest) | `RB_COLOR_INFO` / `RB_DEPTH_INFO` / `RB_SURFACE_INFO` |
| `vs` / `ps` | Shader hashes | Guest shader load / translator |
| `outcome` | `issued` \| `copy` \| `drop:<stage>` | Backend IssueDraw path |
| `bound_c0` / `bound_d` | ACTUAL bound target key | Backend RT selection (stage E) |
| `verts` | Host vertex count actually drawn | Backend primitive processing |
| `depth` | Depth/stencil state (A3 fields, §3.1) | Backend IssueDraw (D11 now; Vulkan mirrors) |
| `scissor` / `claim` | Scissor rect / claim rect (A3 fields) | Backend IssueDraw (D11 now; Vulkan mirrors) |

**Translation rules (Vulkan → D11 terms):** the mapping is
deliberately thin because every field above is already a guest or
backend-agnostic value:
- Surface keys are guest EDRAM keys (`Describe()` form) on both
  sides — no translation; a Vulkan surface and a D11 surface with
  the same key are the same guest surface.
- Outcome categories use the SHARED vocabulary: `issued`, `copy`
  (mode 6 / kCopy-equivalent — the Vulkan copy path is `IssueCopy`),
  `drop:<stage>` with the stage names from §9.2 (Vulkan) / §3.1 A1
  (D11). A Vulkan-only stage name and a D11-only stage name that
  describe the same guest-visible condition are recorded as
  equivalent in the comparison's stage map (e.g. Vulkan
  `memexport_vs_unsupported` / `memexport_ps_unsupported` ≡ D11
  `memexport_not_supported`); a stage with no counterpart is
  reported as a backend-specific outcome, not silently equated.
- `verts` is the host vertex count after the backend's primitive
  processing (fan-to-list conversion etc.) — the two backends may
  legitimately differ here (e.g. different fan handling); a verts
  difference with the same outcome + same bound key is reported as
  a PRIMITIVE-PROCESSING difference, not a routing divergence.
- The `FH1 routing guest` (stage A) lines are IDENTICAL on both
  backends (shared CP) — they are the join key, not translated at
  all. If the stage-A sequences differ between the runs, the frames
  are not the same game state (§9.4) and the outcome comparison
  does not run.

### 9.4 Comparison (second, stronger check)

The script (`compare.py`, extended) runs in two phases:

**Phase 1 — D11-only consistency (§4).** Unchanged: guest → expected
via Pinyon's D11 rules → actual D11. Must pass first (it validates
the logging itself).

**Phase 2 — cross-backend.** Inputs: the Vulkan run's log + the D11
run's log for the same swap.
1. **Frame-equivalence pre-check (gate — must pass before any
   outcome comparison):** the stage-A (`FH1 routing guest`)
   sequences must match: same draw count, same (prim, idx, mode,
   pass, winy, c0, surf) sequence in order. If they differ, the
   runs captured different game states — report INCOMPARABLE (with
   the first differing ordinal and both sides' values), NOT
   divergences. Do not compare outcomes across different frames.
   (Evidence for why this gate is load-bearing: the first D11 callog
   run's swap 1500 was a 3,418-draw loading/menu frame while four
   other runs' swap 1500 was the 129-draw free-roam frame — a fixed
   swap does not guarantee the same frame. Both runs must pass the
   §5.4 free-roam census check, and then this pre-check.)
2. **Per-draw outcome join:** by guest draw ordinal. For each draw,
   compare `outcome`, `bound_c0`/`bound_d`, `verts` (with the
   primitive-processing tolerance above), and the A3 state fields
   (depth/stencil, scissor, claim) where both sides logged them.
3. **Flags:** DIVERGED (same guest draw, different outcome or
   different bound key); D11-DROPPED (Vulkan issued, D11 dropped);
   D11-SPURIOUS (D11 issued, Vulkan dropped — a D11-only draw);
   VULKAN-ONLY-STAGE / D11-ONLY-STAGE (backend-specific outcome with
   no counterpart — reported, not equated).
4. **Headline question (§9.5):** answered from the stage-A lines +
   the outcome join, not from either backend's rules.

### 9.5 The decisive question + the Vulkan-reference verification

> **CORRECTION (2026-11-17, Phase 2b final report): the decisive question as
> framed here is REFUTED.** The premise — "the routing investigation proved
> the guest issues NO EDRAM-targeting world triangles in the captured D11
> frame" — was true only for that one frame. D11 thread t27292 (2026-11-17
> ~01:03–01:06 AM PC, in the D11 run-4 log directory) issued 1,667
> EDRAM-targeting triangles (surf `14020500`, prim 4/6/13, mode 4) across
> bands 0/1/2, and the D11 backend issued 3,242 draws to the GPU on that
> thread. The guest issues EDRAM world triangles on **both** backends. The
> D11 census frames (swap 1500 routing, swap 3000 Phase 2) that showed zero
> captured **composite presents**, not band presents (§9.7) — a gating
> artefact, not a backend difference. Do not use the "Vulkan issues them,
> D11 never does" framing. The refined question is: in a **band present** on
> each backend, does the guest issue the same EDRAM work, and do the issued
> triangles produce world content on D11 (the t27292 EDRAM pixel content is
> still unmeasured)?

**Decisive question (as originally framed):** the routing investigation proved the guest
issues NO EDRAM-targeting world triangles in the captured D11 frame.
- If the guest DOES issue them on Vulkan in the same frame (stage-A
  lines with triangle prims targeting `0/32/4x/c3`, issued on
  Vulkan), D11 is dropping or misrouting them — a D11 backend bug,
  pinpointed to the first diverging draw.
- If the guest issues none on Vulkan either, the missing world is
  guest-side, not a D11 bug — record that as the finding.

**Assumption to verify (do not assume Vulkan renders the world):**
before the cross-backend comparison is treated as meaningful, the
report must verify from the Vulkan run's log that (a) the guest
issues EDRAM-targeting world triangles on Vulkan (stage-A lines:
triangle prims, `c0` key `0/32/4x/c3`, outcome `issued` on Vulkan),
and (b) the EDRAM gets world content. (b) is verified by the
Vulkan-side equivalent of the D11 resolve-source evidence: the
resolve source must read non-grey content for the world rect — if
no Vulkan surface/pixel evidence exists in the log, the report says
so explicitly and the verification falls back to Seth's visual
confirmation of the Vulkan run (screenshot/video of free roam on
Vulkan showing the textured world). If (a) or (b) fails, the report
states the Vulkan run is NOT an established working reference for
that frame, and the cross-backend divergences (if any) are reported
as backend differences, not as D11 bugs.

### 9.6 What Phase 2b (Vulkan logger) implements

- The §9.2 Vulkan mirror lines in
  `src/graphics/vulkan/command_processor.cpp` (IssueDrawImpl entry,
  early exits, `:6911` draw site, RT selection, IssueCopy `:7162`,
  frame boundary), gated by the same cvars, same budget semantics.
- The shared-record emission: the Vulkan lines use the §9.3 field
  names so `compare.py` parses both backends with one grammar
  (the `side=` tag distinguishes them: `side=V` for Vulkan backend
  lines, `side=A` for D11; `side=G` stays the guest side on both).
- `compare.py` Phase 2 (frame-equivalence pre-check + per-draw
  outcome join + divergence flags) + the §9.5 verification section.
- Run procedure: Vulkan run first (it is the reference — its log
  decides whether the reference assumption holds), then the D11 run
  at the same swap, both passing the §5.4 free-roam census check.
  The two runs must use the same game state (same save / same
  free-roam entry); if the stage-A sequences differ, re-run until
  they match or report INCOMPARABLE.

### 9.7 What Phase 2b actually delivered (2026-11-17) — implemented vs specified, and corrections

Full report: `~/workspace/dx11-vulkan-logger-2026-11-17.md`. Probe diff:
`~/workspace/dx11-vulkan-logger-probe-diff-2026-11-17.txt` (compare.py only;
**no PC source was changed, no rebuild was done**).

**Implemented:**

- **Vulkan run 4 relaunched and captured** (PID 12352, swap 6000, thread
  t13516): in-game at 80 s, ~30 FPS / ~2,900 draw calls per frame, swap-6000
  burst 114,465 lines in three rotated logs. Binary verified (Phase 2 build:
  exe 61,851,136 B @ 2:32:42 AM PC; dll 11,496,960 B @ 2:32:38 AM PC);
  effective config verified (`--gpu_backend=vulkan`, all 12 swap flags 6000,
  callog budget 1,000,000). `run_game.cmd` restored to D11 afterwards.
- **Step 1 census, formally verified:** `compare.py --backend vulkan`
  **PASS** (exit 0). 261 accepted EDRAM-targeting draws (all band 2, mode 4,
  surf `14020500`, d0 `0x400`; prim 6: 224, prim 13: 25, prim 4: 12 with idx
  up to 10,419). BINPRED 496 (in 261 / out 235), guest = record = dispatch =
  261, unaccounted 0, F1 2,946/2,946, F2 261/261 on all three registers,
  guest-vs-dispatch order join 0 mismatches. 235 predicated-out draws are
  bin-mask culls (mask `0x08`, band-1 draws during the band-2 present) —
  correct banded-tiling behaviour, not drops.
- **`compare.py` Vulkan mode** (`--backend vulkan`): dispatch-stage parsing
  (the dispatch line carries no `idx`, so the join uses (prim, mode, pass));
  A1 agreement as BINPRED-total == draw packets AND BINPRED-in == guest ==
  record == dispatch; outcome = dispatch (unaccounted = guest − dispatch);
  D11-only record types (routing issue/issued/bound, A9, A4, srv-bind,
  resolve trace) reported as "not emitted" instead of failing; guest surf
  keys reported in the per-key inventory. D11 mode unchanged — the Phase 2
  PASS reproduces exactly. **F4 closeout one-line fix included:** A4 lines
  with `pvalid==-1` are now counted and reported (D11 regression shows 2,
  exactly the two mode-5 depth-only draws).
- **Surf decode (empirical):** surf `14020500` + d0 `0x400` = the EDRAM
  (matches D11 A#0 on every field; d0 1024 = EDRAM depth Describe() first
  field; all draws in a banded pass). The RB_SURFACE_INFO bitfield layout
  was not fully decoded from xenos.h; bit 17 is the one differing bit
  (0x14020500 vs 0x14000500).

**NOT implemented (and why):**

- **Vulkan backend probes (§9.2 mirror lines) + translator (§9.3) + Phase 2
  per-call comparison (§9.4):** not built. The comparison they would feed
  needs the same present type on both backends, and the captures in hand are
  different present types (D11 composite presents vs Vulkan band-2 present).
  Building them now would compare a composite present against a band present
  — a category error. Correct order: (1) capture a D11 **band present** (or
  extend the probe to a multi-swap range covering a full guest frame), then
  (2) build the Vulkan probes + translator and run the per-call comparison.
  The compare.py Vulkan mode delivered here is the prerequisite (verified
  input format: guest/record/dispatch + callog, dispatch as the Vulkan
  outcome stage).
- **§9.5 verification (b) (EDRAM pixel content on Vulkan):** not measured from
  a log (no Vulkan surface/pixel probe exists). It rests on Seth's visual
  confirmation (the 2026-11-17 ~12:56 monitor photo: Horizon festival scene
  textured, 27 FPS on the RX 5700 XT) plus the run-4 perf signature
  (~30 FPS, ~2,900 draw calls/frame in free roam). The report says this
  explicitly, as §9.5 requires.

**Corrections found (change how §9.4–§9.6 are read):**

1. **The "same swap" frame-equivalence gate (§9.4 step 1) is insufficient.**
   A fixed swap does not guarantee the same frame — and, more importantly, a
   single swap captures **one present, not one guest frame**. A guest frame is
   split across several presents (one per EDRAM band + one composite present).
   The D11 gates landed on composite presents; the Vulkan gate landed on a
   band-2 present. The pre-check must additionally require **the same present
   type** (band present vs composite present), or the capture must span 4+
   consecutive swaps (one full guest frame). §9.4's note about the 3,418-draw
   loading frame is now joined by a second, sharper example: the t27292
   accumulation (3,418 draws under one swap value while the present counter
   was stuck ~5 s) vs the 129-draw single presents at the same swap.
2. **The decisive question (§9.5) is refuted as framed** (see the correction
   note at the top of §9.5). The guest issues EDRAM triangles on both
   backends. The refined question is per-present-type, and the D11 EDRAM
   pixel content when triangles are issued is still unmeasured.
3. **Predicated-out draws are not automatically drops.** On a single-band
   present the bin predication visibly culls other-band draws (235 here, all
   mask `0x08`). The A3 predicated-out flag must be mask-aware (the Vulkan
   mode does this); a raw BINPRED − guest count is not a divergence.
4. **§9.2's "what already fires on Vulkan" list is confirmed, with one
   addition:** the **dispatch** line (`FH1 routing dispatch`, shared command
   processor) fires on Vulkan and is the third routing stage — it is now
   parsed by compare.py and is the Vulkan outcome event. The §9.2 list
   (guest/record, callog, BINPRED/BINRECORD) otherwise holds exactly as
   specified; the D11-only list (issue/issued/bound, A9, A4, srv-bind,
   resolve trace, tile-path copy/untileflush) is confirmed absent on Vulkan.

### 9.8 What Phase 2c actually delivered (2026-11-17) — the D11 band present capture, and a correction to §9.7

Full report: `~/workspace/dx11-d11-band-present-2026-11-17.md`. Probe diff:
`~/workspace/dx11-d11-band-present-probe-diff-2026-11-17.txt` (source only;
4 files). The run is **not stuck** (music.in_game at 2:36:29 PM PC, saves
every ~60 s, sim stepping; the 0 FPS overlay is the D11 CPU-decoder
bottleneck in the opening drive, not a loading stall or an input wait).

**Built:**

- **Multi-swap trace window (`fh1_debug_trace_span`, default 1).** New INT32
  cvar; the callog gates (shared + D3D11 copies of `Fh1CallogEnabled`) and
  all 5 routing-gate sites now fire for swaps in `[start, start + span)`.
  Run with span 6, callog budget 1,000,000, edram3d draws 12, D11/GTX.
  Build BUILD_EXIT=0; binary verified (dll 11,500,032 B @ 2:30:27 PM PC,
  `fh1_debug_trace_span` string present in the DLL). The window fired:
  callog + routing lines for all 6 swaps 3000–3005, thread t11580, PID 20208.
- **EDRAM triangles on D11 in every window swap** (surf `14020500`, prim 4,
  mode 4, passes 0/1/2, idx 324/360/954) alongside composite draws
  (surf `14000500`, pass −1). D3D11 issued ~3,200–3,600 draws per present
  (A3), 202 distinct (vs,ps) hashes, 0 invalid translations. Textures bound
  and valid (82 distinct `FH1 srv bind all` lines, all valid=1), 0 texture
  load rejections. This re-confirms the §9.7 correction #2 (D11 issues EDRAM
  triangles) in a single run.

**NOT delivered (and why):**

- **Per-present census: FAIL.** `compare.py` (d11 mode) exit 1, VERDICT: FAIL
  for swaps 3000/3002/3005. Two causes, both about the present counter at
  D11's present rate — see the correction below.
- **edram3d probe: never fired (0 lines).** The decisive before/after EDRAM
  readback was not captured. The probe's conditions (render-target slot 1
  via `pending_key(1)`, `used_texture_mask != 0`) were never all met, though
  EDRAM triangle draws exist and textures bind valid. The flat-grey question
  (does the EDRAM get coloured after the triangles draw?) is still open.

**Correction to §9.7 correction #1 — a swap window does NOT separate presents at D11's present rate:**

§9.7 said the fix was "the same present type, or 4+ consecutive swaps (one
full guest frame)". Phase 2c shows that is **insufficient on D11**: at
~0.2 presents/sec, one swap value spans **seconds and thousands of draws**
(swap 3000: 14:37:42–14:37:54, 1,226 routing-guest draws; swap 3002:
3,808 draws), because the sim issues many guest frames between presents and
they all accumulate under the current swap value. Every window swap therefore
contains **all present types mixed** (all three bands + composite) — the
band/composite distinction the window was meant to separate is lost, and the
A9 `issued` counts (3,179–3,588 per present vs 52 in Phase 2) confirm each
"present" carries ~25× a single guest frame. **A swap window widens the
capture but cannot separate presents the counter itself does not separate.**
The t27292 accumulation (§9.7) is not an anomaly — it is the normal regime
at D11's present rate. A valid per-present comparison needs a finer gating
signal than the present counter (e.g. a draw-count trigger, or a
guest-frame counter if one exists), not a wider swap window.

Two further consequences, both measured in this run:

- **Budget:** the shared callog counter hit 1,000,000 at swap 3001 (per-swap
  line volume is ~10–50× the Phase 2 composite present: swap 3000 alone is
  234,968 callog lines). The shared side then logged no pkt/reg/issue lines
  for swaps 3002–3005, while the routing (tile_trace) lines and the D3D11
  callog lines continued (independent counters). A wider window needs a
  proportionally larger budget — and the 21-file rotation cap (105 MB for
  this 6-swap window) bounds how wide a window can be before data is lost.
- **The edram3d probe is a threshold probe, not a swap-gated one** — it
  should have fired on the first qualifying EDRAM draws at/after swap 3000
  regardless of present type. That it fired zero times means its *draw*
  conditions (slot 1 / texture mask) are the blocker, not the swap gating.
  Do not re-run the same window expecting it to fire; read the probe
  conditions from source first (§6 of the Phase 2c report).

---

## 10. File:line index (baseline, §0.1)

| Item | Location |
|---|---|
| Packet dispatch | shared `command_processor.cpp:2161` (`ExecutePacket`), `:2223` (Type 0), `:2259` (Type 1), `:2271` (Type 2), `:2277` (Type 3) |
| Type-3 opcode switch | shared `:2481-2560+` (DRAW_INDX `:2523`, DRAW_INDX_2 `:2526`, IM_LOAD `:2542`) |
| Predicate / bin evaluation | shared `:2287-2391`; `PredicatedOut` `:2604`; BINPRED `:2404-2411`; BINTRACE `:2342`; BINSHARED `:2381` |
| `trace_swaps_++` | shared `:2323` (XE_SWAP branch `:2309`); `ExecutePacketType3_XE_SWAP` `:2674` |
| `counter_++` | shared `:2734` |
| `ExecutePacketType3Draw` | shared `:3144`; DRAW_INDX wrapper `:3743`; DRAW_INDX_2 wrapper `:3769` |
| `ExecuteDrawRecord` | shared `:3376`; BINRECORD `:3337-3341`; viz check `:3334`, `:3399`; skip/suppress `:3497`, `:3532`; dispatch `:3558-3570` |
| `DrawRecord` struct | `include/rex/graphics/command_processor.h:584-601` |
| `gpu_trace_bins_vblank` cvar | shared `:60` |
| `fh1_debug_*` cvars | `src/graphics/flags.cpp` (tile_trace_swap `:23`, band_suppress `:29`, hide_hud `:33`, c2_probe `:317`, c2_probe_from `:327`, c2_probe_draws `:331`, vertex_dump `:274`, post_clip `:369`, composite_draws `:263`) |
| IssueDraw | D3D11 `:1735-3874`; tile-path lambda `:1763-1774`; routing issue `:1788-1808`; early exits `:1816-1879`, `:1927`, `:1975`, `:2034`, `:3354`, `:3388`, `:3632`, `:3695`, `:3704`; copy_mode `:1808` |
| `BindDrawRenderTargets` | D3D11 `:1004`; bound log `:2014` |
| `ConfigurePipeline` / `is_valid` | D3D11 `:2028`, `:2041`; `AnalyzeShaderUcode` `:1820`, `:1834` (pipeline_cache.cpp:171) |
| `UpdateBindings` | D3D11 `:1605` (called `:3385`); SRV/CB sets `:1682-1707` |
| Draw / DrawIndexed | D3D11 `:3658`, `:3723`; topology `:3641`; stage D `:3860` |
| IssueCopy / resolve plan | D3D11 `:4406`; `Fh1PlanResolve` `:4439`; plan logs `:4492`, `:4543`; skip branch `:4552` |
| Texture-fetch writes | D3D11 `:887`, `:987` (`TextureFetchConstantWritten(s)`) |
| `FH1 srv bind` / `srv bind all` / budget exhausted | texture_cache.cpp:1069, :1080, :1135; exhaustion line at `:1129-1136` (cap 400, dedup by 12-field key without swap; `bind_all_logs`/`bind_all_seen` function-static, never reset, `:1099-1100`) |
| SET_BIN_* inline handlers | shared `:2626-2651` (SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI, SET_BIN_MASK); `PM4_SET_BIN_SELECT` has **no** handler (hits `default:`) |
| ExecutePacketType3 return | shared `:2671` (`return result;`) — pktpost insertion point |
| `fh1_debug_callog` DECLAREs | `include/rex/graphics/flags.h:67-68` (after `fh1_debug_tile_trace_swap`, `:66`); definitions in `src/graphics/flags.cpp` (after `fh1_debug_tile_trace_swap`, `:23`) |
| `CreateTexture` / unsupported format | texture_cache.cpp:444-454; `LogUnsupportedFormatOnce` `:435-443`; rejected-load log `:588-599` |
| Frame boundary / summary | D3D11 `:6640` (`presented_output_frames_++`), `:6664` (`D3D11 draws frame=`); `DrawCounters` header `include/rex/graphics/d3d11/command_processor.h:62-94` |
| Present | `src/ui/d3d11/d3d11_presenter.cpp:1190` (HRESULT checked `:1191-1201`); bounded poll `:263-281` |
| Shader binding (D3D11 calls) | pipeline_cache.cpp:893-895 (`VSSetShader` / `PSSetShader` / `GSSetShader`) |
| `WaitForGpu` (in flight, do not touch) | D3D11 `:6363-6381` |
| Per-band fix | fh1_native_executor.cpp:125-142 (helpers), :2678, :2699 (recording), :2745 (drain) |
| c2 probe | D3D11 `:3401-3477` (control), `:3749-3769` (draw); `MeasureFootprint` `:574` |
| Composite state probe | D3D11 `:2964` |
| Guest / surface probes | D3D11 `:2497` (guest), `:2540` (surface) |
| Vertex dump / post-clip | D3D11 `:2554-2618` |
