# Findings — Nov 15–17, 2026

Investigation reports and probe diffs from the DX11 render-bug work, in
roughly chronological order. Each report was verified against a runtime
trace or a pixel-level probe before it was written down.

## The flat grey world — how it was narrowed

- `dx11-render-bug-search-2026-11-15.md` — early search: texture audit,
  draw #23, and the first EDRAM hypotheses.
- `dx11-texture-audit-2026-11-16.md` — texture-fetch audit: 3D draws resolve
  to null or never-uploaded SRVs; HUD uploads fine. Cause #1 (upload
  rejection) was later refuted.
- `dx11-baseline-evidence-2026-11-15.md`, `dx11-evidence-2026-11-15.md`,
  `dx11-testa-evidence-2026-11-15.md`, `dx11-testb-evidence-2026-11-15.md`,
  `dx11-tile-trace-evidence-2026-11-15.md` — raw evidence notes behind the
  early investigation.
- `dx11-receiving-surface-hypothesis-2026-11-15.md` — the receiving-surface
  hypothesis for where world draws land.
- `dx11-d3d12-vulkan-band-mapping-2026-11-15.md` — how the three bands map
  across the D3D12 and Vulkan backends.
- `dx11-hud-hide-diagnostic-setup-2026-11-16.md` — HUD-hide diagnostic setup.

## The band bug — found and fixed (2026-11-16)

- `per-band-fix-review-2026-11-16.md` — the per-surface band bitmask fix
  (`UntileBandBit`), copy-down verification (all 12 probe lines 100%
  nonzero), and the draw #23 / texture investigation that ran alongside it.
- `dx11-030-usable-for-d3d11-2026-11-16.md` — 0.3.0 usability notes for D3D11.

## Call logger and the routing question (2026-11-17)

- `dx11-call-logger-design-2026-11-17.md` — design doc for the guest-draw
  call logger (through §9.8).
- `dx11-call-logger-2026-11-17.md` — implementation report, verdict PASS
  (run 4, swap 3000, 131 guest draws, 0 unaccounted).
  `dx11-call-logger-probe-diff-2026-11-17.txt` — the exact patch.
- `dx11-f4-closeout-2026-11-17.md` — F4 closeout: all 11 uncovered
  (vs,ps) hashes are on non-issued draws, as expected.
- `dx11-vulkan-logger-2026-11-17.md` — Phase 2b, Vulkan cross-backend
  logger. **Major correction: the "decisive divergence" premise is
  refuted** — D11 issues EDRAM triangles (1,667 across bands 0/1/2); a
  single swap captures one present, not one guest frame. The flat-grey
  world is NOT a routing problem; the loss is downstream of IssueDraw.
  `dx11-vulkan-logger-probe-diff-2026-11-17.txt` — the exact patch.
- `dx11-d11-band-present-2026-11-17.md` — Phase 2c, D11 band-present
  capture. EDRAM triangles confirmed on D11 in every swap, but the
  per-present census fails at D11's ~0.2 FPS present rate and the decisive
  edram3d probe never fired (0 lines).
  `dx11-d11-band-present-probe-diff-2026-11-17.txt` — the exact patch.

## EDRAM probes — the colour is there (2026-11-17)

- `dx11-edram3d-loosened-2026-11-17.md` — Phase 2d, loosened edram3d
  probe. Fired 12/12; 10 of 12 draws untextured, proving the texture-mask
  gate was the blocker — but all 12 were composite surfaces, not EDRAM.
  `dx11-edram3d-loosened-probe-diff-2026-11-17.txt` — the exact patch.
- `dx11-edram3d-2e-2026-11-17.md` — Phase 2e patch/build report
  (EDRAM-only probe).
  `dx11-edram3d-2e-probe-diff-2026-11-17.txt` — the exact patch.
- `dx11-edram3d-2e-results-2026-11-17.md` — **Phase 2e results, decisive:
  EDRAM DOES get coloured.** Probe #7 (band 2): before uniform 66,66,66,
  after 28.9% changed, 159 distinct colours, bottom pixels sky-blue
  (85,124,146). The loss is DOWNSTREAM of the EDRAM write (resolve or
  composite).
- `dx11-edram3d-2e-keypress-2026-11-17.md` — remote keypress does not reach
  the game (raw input); the licence screen needs a real keypress on the PC.

## Resolve probe — the gate was the blocker (2026-11-17)

- `dx11-resolve-probe-2026-11-17.md` — Phase 2f, resolve probe build
  (compute-shader dispatch, staging + copy production path).
  `dx11-resolve-probe-diff-2026-11-17.txt` — the exact patch.
- `dx11-resolve-probe-2f-followup-2026-11-17.md` — **Phase 2f follow-up:
  edram3d reproduces, resolve probe still at 0.** Its gate
  (`untile_band == 2`) is the blocker: `untile_band` reads 0 at resolve
  time even when the resolve rect is band-2-sized. 20 band-2-sized EDRAM
  resolves did happen. Fix: drop the gate or replace it with a rect check.
  Store probe burned out in 2.6 s during loading (no free-roam data);
  staging log also at 0 (enclosing condition undetermined).

## Backend quality and the 0.9.1 port

- `dx11-backend-build-quality-2026-11-17.md` — build-quality audit:
  3 HIGH / 7 MED / 7 LOW. Priority: 1 WaitForGpu, 2 texture formats,
  3 memexport/tessellation, 4 per-draw allocations, 5 pipeline lifetime.
- `dx11-091-alignment-2026-11-17.md` — Pinyon Shift 0.9.1 alignment:
  the 0.9.1 install ships no rexglue source and no D3D11 backend; adding
  D3D11 is a feature integration, not a flag flip. Includes the
  baseline→0.9.1 graphics-layer diff (51 files).

## Open items (2026-11-17)

- **Prime suspect: `source_row_offset = untile_band`**
  (`src/graphics/d3d11/command_processor.cpp:5172`). The stale
  `untile_band` (0 at resolve time) shifts the resolve source rect, the
  compute-shader constants, and the copy-down rows, so a Band-2 resolve
  likely reads Band 0's rows instead of 512–719. Fix direction: derive the
  band from the plan rect (`plan.y0`/`plan.y1`). Row-math check pending:
  whether `Fh1D3D11UntileBand` returns an index (0/1/2) or a row offset
  (0/256/512), and whether `plan.y0` is already 512 (double-offset risk).
- **Resolve probe gate fix pending** (drop `untile_band == 2` or use a rect
  check), store-probe budget raise, staging-log enclosing condition.
- **0.9.1 port in progress** — ShiftGlue SDK source obtained; D3D11
  backend integration (CMakeLists, flags, backend selection) is the next
  step, plus carrying the D3D11 shaders 0.9.1 dropped.
- **Draw #23 wipe** in a live gameplay frame (unmeasured); **t7 mirror
  staleness** (separate bug); **blue tint** (fetch-level R/B swizzle);
  **NaN-in-bloom** (Corrado car-preview black box).
