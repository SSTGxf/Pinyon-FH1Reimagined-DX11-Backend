# Pinyon-FH1Reimagined-DX11-Backend

A Direct3D 11 graphics backend for Pinyon Shift — the Forza Horizon 1
recompiler / ReImagined runtime. It exists for one reason: to run FH1 on a
GeForce GTX 570, a card the backend is built on and tuned to the limits of,
rather than written around. The goal is a lean D3D11 path that the weakest
card in the room can carry, so the same code flies everywhere else.

## Target hardware

- **EVGA GeForce GTX 570 HD Superclocked** (GF110, part 012-P3-1572)
- **1.28 GB VRAM** — the binding constraint on the whole backend
- **Driver 391.35** — the last Fermi driver
- **Feature level 11_0** is the card's ceiling for D3D11
- **4-phase VRM** (compact, cost-down non-reference PCB) — keep total power
  under ~240–250 W and core voltage under 1.075 V; the gates run at stock
  1.000 V

Correction (2026-11-16): the GTX 570 **does** expose DirectX 12 at feature
level 11.0 — an earlier version of this README said it had no D3D12 at all,
and that was wrong. FL 11_0 is the lowest D3D12 feature level, and D3D11 is
still the backend that fits this card. The backend is chosen for the card,
not because D3D12 is absent.

Full rationale: [`docs/hardware-constraints.md`](docs/hardware-constraints.md).

## Source status (2026-11-17)

The source in this repo now matches the findings below. The 2026-11-16
README rewrite described the band fix and the EDRAM investigation, but the
committed source was still the Nov 12 baseline — the fix and the probes
were not in the tree. That gap is closed: this commit syncs the live
Nov 16–17 source, including the per-surface band bitmask, the call logger,
the edram3d probe, and the resolve probe. The full reports are in
[`docs/findings/`](docs/findings/).

## Source status (2026-11-18)

- **0.9.1 port — backend in, full game BUILT.** The D3D11 backend is
  ported into the Pinyon Shift 0.9.1 source: the 17-file `d3d11/` backend,
  CMake wiring (`REXGLUE_USE_D3D11`), and DirectX 11 selectable in the
  launcher (the schema-27 Vulkan force is removed). The SDK DLL built:
  `rexgpu-fh1.dll`, 3,578,880 B. **The full 0.9.1 game build SUCCEEDED
  06:01 AM PC time** (`pinyon_shift.exe` 65,656,320 B, `rexgpu-fh1.dll`
  14,447,616 B, `rexruntime.dll` 13,905,408 B) — the `cli11` configure
  block is cleared.
- **D3D11 LAUNCHED on the GTX 570 at 60 FPS — a first.** The DX11 build
  was installed into the launcher location (new exe/DLLs in the install,
  both configs say `d3d11` + GTX adapter, stock files backed up). The
  runtime shows DirectX 11 + FH1 Native Executor. In-game: white screen,
  then past the licence screen — a road visible as a strip at the bottom,
  everything above the line white/blank (no geometry, no 3D, no untiles).
  Sustained free roam is NOT confirmed.
- **source_row_offset fix — applied, NOT proven.** `command_processor.cpp:5172`
  is now `constexpr uint32_t source_row_offset = 0;` (was the stale
  `untile_band`), in both the 0.9.1 port and the D: tree. Row math is
  resolved: `Fh1D3D11UntileBand` returns the row offset (0/256/512), and
  `plan.y0` already holds the band's real rows, so the fix avoids
  double-counting. The D: game was rebuilt with the fix (`rexgpu-fh1.dll`
  11,511,296 B, `pinyon_shift.exe` 61,851,648 B).
- **First test — world gone, unproven.** The rebuilt D: game showed no
  world in the car-preview view (flat blue sky, Corrado black box, HUD
  only). The fix did NOT bring the world back there. That was not free
  roam, and no free-roam retest or probe verdict has landed yet. If free
  roam also shows no world, revert the fix and retest — the pre-fix state
  was flat-grey-but-present. The render bug is NOT fixed.
- **Memexport — the prime suspect.** The command processor deliberately
  drops every draw whose vertex or pixel shader writes memexport (D3D11
  FL 11_0 has no vertex-shader UAVs), and the executor skips them too —
  there is no Step C memexport transfer handling anywhere. The translator
  DOES emit the memexport stores (an earlier "missing store" claim was
  wrong); the defect is the missing CARRIER, not a missing store. Pixel-
  shader carry is dead (cannot preserve one ordered export per vertex);
  stream-output replay is not yet defensible (no SO support, no proven
  event bound, no ordered replay). The open route is CPU-side shader
  interpretation (`interpreter.cpp`, `ExportSink` with `kExportAddress` /
  `kExportData0..4`). See
  [`docs/findings/dx11-memexport-2026-11-18.md`](docs/findings/dx11-memexport-2026-11-18.md).
- **Counter bug — found and fixed in the 0.9.1 tree.** The per-frame
  summary printed `skipped_memexport`, which is NEVER incremented, while
  the drop increments `dropped_memexport`, which was never printed — so
  every `skipped[memexport=N]` log reading was always 0 and meaningless.
  Copilot implemented the fix (summary prints `dropped_memexport` plus
  new `dropped_memexport_vertex` / `dropped_memexport_pixel` split
  counters and a per-draw drop trace) in the 0.9.1 tree only. **The D:
  tree — the source in this repo — does NOT yet have the fix.** A
  translator change in the same tree (removal of the SM 5.0 vertex-shader
  early return) is flagged in the report as NOT verified and needs review
  before it ships.
- **Launcher — DirectX 11 + GPU selector.** The installed launcher now
  shows DirectX 11 and a GPU dropdown (GTX 570 / RX 5700 XT / Auto); see
  [`launcher-gpu-selector/`](launcher-gpu-selector/).
  `set-graphics-experiment.ps1` accepts `d3d11` and a `D3d11Adapter` that
  writes `d3d11_adapter` to `pinyon_shift.toml` (verified saved:
  `graphics_api: d3d11`, `d3d11_adapter: GTX`). The DX11 build is now
  installed in the launcher location, so the setting runs the real D3D11
  backend.
- **Built binaries.** The DLLs are binary and are NOT committed here. They
  are collected on the PC in `D:\PinyonShift\work\testkit-dlls-20261118\`
  (`rexgpu-fh1-d11-rowfix.dll`, `rexgpu-fh1-091-port.dll`, and the rebuilt
  WPF + Desktop launcher exe/dll).

## Source status (2026-11-19)

- **Upstream DX11 backend — it renders the full world.** Upstream Pinyon
  lead nerijs built a D3D11 backend as a port of his own D3D12 backend
  (bindful path, runtime shader translation). It renders the FULL world
  correctly in DX11 — The Gauntlet, road, terrain, trees, mountains, sky,
  car, HUD, minimap — the first correct full-world DX11 render. Merged
  into upstream dev as commit `7404c62a` (`feat/d3d11`), SDK `0c2187cb`.
  Seth swapped that SDK into his 0.9.1 tree, built it, installed it, and
  tested it on the GTX 570: **the game works properly in D3D11** — title
  screen 60 FPS, full world renders correctly in free roam, no
  white/missing world. FPS: ~30 idle/title, **avg 10 while driving**
  (the old build was 4–5). Correctness is solved; performance is the
  remaining job. Adapter index is session-dependent (SSH: GTX 570 is 0;
  interactive desktop: RX 5700 XT is 0, GTX 570 is 1), so the live
  setting is `d3d11_adapter = 1`. Launch via `pinyon.py` scheduled task
  with PowerShell Bypass. Upstream performance: RTX 4080 (FL 11_0) DX11
  11.1 ms vs Vulkan 8.4 ms (~32% gap); the backlog has no dedicated
  performance items.
- **Memexport — exonerated, supersedes the 2026-11-18 "prime suspect".**
  FH1 issues **ZERO memexport draws** (census over 28,560 frames, and a
  3,207-frame / 2.29M-draw free-roam run also returned 0). The memexport
  drop was never the render blocker. Do not build the CPU memexport route
  to fix the render bug.
- **Executor port — upstream's model into Seth's backend.** Copilot
  ported upstream's `Fh1NativeExecutor` PrepareTargets signature cache
  into Seth's D: tree executor (2 files, build `BUILD_EXIT=0`, DLL
  `CFD2861B`). Verified against source: signature cache + `prepare_cache_hits`
  in place, row/per-band/memexport-counter fixes intact, resolve path
  uses `MarkRangeAsResolved` which does call `RangeWrittenByGpu`. Not
  ported: upstream CPU timing fields, depth-overwrite interface. Seth's
  executor already had the ownership / `ClaimRect` / `BindTargets`
  machinery, so the port was mainly the cache. See
  [`docs/findings/dx11-executor-port-2026-11-19.md`](docs/findings/dx11-executor-port-2026-11-19.md)
  and the prompt in
  [`docs/dx11-executor-port-copilot-prompt.md`](docs/dx11-executor-port-copilot-prompt.md).
  **The ported backend was never in-game tested**: its DLL is not
  ABI-compatible with the installed runtime ("Failed to load GPU plugin
  'fh1'"), so it cannot be tested by DLL swap — it needs its matching
  exe/runtime. The working upstream DLL (`0B5A8F44…`) was restored.
- **Launcher — DX11 + GPU selection, set-graphics fixed.** The rebuilt
  launcher shows Graphics API: Direct3D 11 and GPU: NVIDIA GeForce GTX
  570. It errored on load because the install location's
  `set-graphics-experiment.ps1` was the stock upstream script (no
  `D3d11Adapter`, `GraphicsApi` Vulkan-only, Apply forced `gpu_backend`
  back to Vulkan). Fixed: the script now accepts `D3d11Adapter` and
  `d3d11`, reports the actual backend/adapter, and Apply writes
  `d3d11_adapter` without forcing Vulkan. Verified: Get and Apply return
  `graphics_api: d3d11`, `d3d11_adapter: 1`. The fixed script is in
  [`launcher-gpu-selector/`](launcher-gpu-selector/).

## How it works

The guest (Xbox 360) EDRAM is modelled as a 2048-tile grid. Front-buffer
tiles live at 720..1439 (base 720, pitch 16, 45x16 tiles). Surfaces are keyed
by base / pitch / MSAA / kind, and per-tile ownership (`Fh1EdramTiles`)
enforces one writer per address; claims follow the draw rect. Transfers go
through `CopySubresourceRegion` or SM 5.0 shaders. Depth goes through an
EDRAM words pass that binds its words with typed `R32_UINT` views, one
element per guest EDRAM word — FL 11_0 / Fermi refuses raw UAVs. The
front-buffer resolve is a full rect, 1280x720; the presenting resolve is
pack=1, fmt 54.

Guest shaders arrive stamped SM 5.1 and are retargeted to SM 5.0, because
FL 11_0 rejects 5.1 outright. With 1.28 GB of VRAM, EDRAM render targets
are pooled and aliased aggressively instead of each guest surface getting
its own host texture.

## Progress

Everything below is measured, not asserted. Each finding was verified
against a runtime trace or a pixel-level probe before it was written down.

### Gates — passed

- **Phase 1 gate — PASS.** 4,165 cleared frames / 70 s, median 59.98 FPS
  on the GTX 570.
- **Phase 2 gate — PASS.** 4,122 frames / 70 s, 17,000 texture uploads,
  0 failures.

### Colour, speed, stutter — done

- **Colour fix — DONE.** The R↔B channel swap is fixed (upload-side swap
  restored in `texture_cache.cpp`). The neon blue cast is gone — frames are
  warm/red.
- **Optimization — DONE.** Median free-roam 9.0 → 13.8–15.4 FPS (+53–71%)
  by skipping the GPU wait + readback per texture load when no new resolve
  landed since the last refresh.
- **Stutter fix — DONE.** Spike untile halved (75.5 → 37 ms) via a fast
  path in `CopyGuestLevelSlice` that computes the block offset once per run
  instead of once per block. p95 274 → 232–235 ms, p99 333–384 → 276–291 ms,
  median 16.1–18.8 FPS. Stutter reduced but not eliminated — p95 is still
  ~233 ms vs 33 ms needed for 30 FPS.
- **Refresh fix — IN PROGRESS.** The remaining stutter is the refresh
  (~117 ms in spike frames: GPU waits ~73 ms + readback ~43 ms). Fix: batch
  the refreshes into 2–3 waits, defer the readback via the copy engine with
  double-buffered staging, and only refresh ranges that actually changed
  (dirty tracking).

### The band bug — found and fixed (2026-11-16)

FH1 draws the 720-row scene in three predicated tiles (rows 0–256, 256–512,
512–720). The middle band was black: rows 256–512 never reached the
presented frame.

- **Root cause — Case B, confirmed by tile trace.** Band-256 draws *did*
  occur (8 draws, pass 1, window_y −256), but `NativeDrawIssued` stored
  only the *maximum* band per surface, so `EndUntiledTiling` issued one
  copy-down per surface at the max band. Rows 256–512 never copied down.
- **Fix — per-surface band bitmask.** The max-band tracking was replaced
  with a bitmask (`UntileBandBit(band) = 1u << (band/256)`); one copy-down
  is now issued per recorded band, in ascending order. The change is in
  `fh1_native_executor.cpp` only — no header change, no viewport, resolve,
  shader, or presenter change.
- **Verified at runtime.** Clean fix run on the GTX 570: band-256 1,998
  copy-downs, band-512 2,298. The fix was reviewed and verified as
  appropriately narrow with no obvious logical error.
- **Copy-down exonerated.** A pixel-level probe confirmed the copy-down
  itself is correct: all 12 probe lines 100% nonzero at source, dest, and
  copyback, for both bands and both surfaces (colour and depth).

An earlier attempt — the **owner-split fix** (2026-11-14, D3D11 splits a
resolve by EDRAM tile owner like D3D12/Vulkan) — populated the 4x sources
(964/1280 sampled texels nonzero vs 0–4 before) but did *not* fix the
visible band bug. The band cause was the max(band) collapse above, not the
surface selection.

### The flat grey world — narrowed to the 3D render (2026-11-16)

The world renders flat grey. The investigation narrowed it in stages, and
each stage exonerated a suspect:

- **Texture upload — exonerated.** An early audit ranked upload rejection
  as the top cause. It was refuted: guest memory at the scene target holds
  only 2 distinct 32-bit words in 3.6 MB, the GPU mirror matches exactly,
  refresh works, and there are zero "texture load rejected" lines. The
  scene target is already nearly black *before* the texture cache reads it.
- **EDRAM resolve conversion — superseded.** An early probe found the
  resolve source uniform grey 0.258789 (= RGB 66,66,66) at *every* sample,
  which then pointed the loss at the 3D render into EDRAM. That conclusion
  is refuted: Phase 2e (2026-11-17) proved EDRAM *does* get coloured
  (probe #7, band 2: 28.9% changed, sky-blue). The loss is downstream of
  the EDRAM write — in the resolve or the composite.
- **World geometry reaches the EDRAM — confirmed (2026-11-17).** The
  earlier claim that no triangle-geometry draw targets the EDRAM surface
  is refuted. Phase 2b (Vulkan cross-backend logger) showed D11 issuing
  1,667 EDRAM-targeting triangles across bands 0/1/2; Phase 2c confirmed
  EDRAM triangles on D11 in every swap; Phase 2e's edram3d probe fired on
  large textured world triangles (8,700–9,300 verts) and read back
  coloured pixels. The flat grey world is NOT a routing problem and NOT
  missing world draws — the colour is lost downstream of IssueDraw, in
  the resolve or the composite. The prime suspect was
  `source_row_offset = untile_band` (`src/graphics/d3d11/command_processor.cpp:5172`) —
  the stale band shifts the resolve source rect, so a Band-2 resolve likely
  reads Band 0's rows. That fix is now applied (`source_row_offset = 0`) in
  both the 0.9.1 port and the D: tree, but it is NOT proven in-game: the
  first D: rebuild test showed the world gone in the car-preview view, and
  a free-roam retest is still pending. See
  [`docs/findings/dx11-resolve-probe-2f-followup-2026-11-17.md`](docs/findings/dx11-resolve-probe-2f-followup-2026-11-17.md)
  and [`docs/findings/`](docs/findings/).
- **3D texture binding — mechanism works.** All 26 observed binds are
  valid (valid=1, texture=1, srv=1, dimok=1), including small fetches. But
  valid SRVs prove the *binding mechanism* only — not that the shader
  samples the intended coordinates, the format/swizzle is interpreted
  correctly, the guest constants reach the translated shader, or the shader
  output is correct.

### Draw #23 — the black wipe, narrowed upstream (2026-11-16)

Composite draw #23 (swap 1500, fullscreen 1280×720, no textures) wipes the
35%-nonzero world image to black, every frame. Its blend state, colour
mask, colour control, alpha test, render target, and geometry are *all
identical* to draws #24/#25 — only its pixel shader differs
(`A39E23E2FD1F8A8B` vs `CAE1DB68AFFA9D3C`), and it has no constants or
textures.

- **Pixel-shader translation — verified faithful.** The guest computes
  `oC0 = (sqrt(|r0+r1|), r0.w)` and the generated DXBC does exactly that
  (plus alpha-test / alpha-to-coverage and the `xe_color_exp_bias`
  multiply). Translation is not the bug.
- **Blend state — verified correct.** Read back from the bound
  `D3D11BlendState`, draws #23/#24/#25 are identical: blend enabled,
  SrcBlend `SRC_ALPHA`, DestBlend `INV_SRC_ALPHA`, BlendOp `ADD`, same for
  alpha, write mask `0xF`. That is standard alpha blending matching the
  guest src6/dst7. A blend misinterpretation is refuted at the blend-state
  level.
- **Remaining suspect — the shader's inputs.** For #23 to wipe to black
  under SrcAlpha blending, the shader must output opaque black
  `o0=(0,0,0,1)`, which given the faithful shader requires the interpolants
  to satisfy `r0.w=1` and `r0+r1=0`. So if the wipe is wrong, the fault is
  upstream of the pixel shader — the vertex shader's interpolant outputs
  (`v0`/`v1`) or the `xe_color_exp_bias` system constant. Those inputs have
  not been captured yet. Whether #23 is *intended* to produce transparent
  output or opaque black is not established by the disassembly alone.

### t7 — a separate defect, kept isolated

Texture t7 (136FB000, a 16×16×16 lookup texture) has a stale mirror
(guest ≠ mirror, 8,206 distinct words) and *is* sampled by the world
draws — but it is a lookup texture, so its staleness would cause wrong
colours, not a flat grey world. It is a real bug, tracked separately, and
not used as an explanation for the grey EDRAM surface.

## Current status (2026-11-18)

| Area | Status |
|---|---|
| Band 256 / copy-down | Fixed and verified (per-surface bitmask, in source) |
| EDRAM resolve conversion | Superseded — EDRAM does get coloured (Phase 2e) |
| Texture upload chain | Exonerated |
| 3D texture binding mechanism | Working |
| World geometry reaching EDRAM | Confirmed — triangles reach EDRAM, EDRAM gets coloured |
| Loss location | Downstream of the EDRAM write: resolve or composite |
| source_row_offset fix | Applied (`= 0`, both 0.9.1 port and D: tree) — NOT proven in-game; first test showed world gone in car-preview view, free-roam retest pending |
| Resolve probe gate | Fixed in the port (plan.y0 == 512 / height 208 check); D: gate still uses stale untile_band |
| Draw #23 pixel-shader translation | Verified faithful |
| Draw #23 blend state | Verified correct |
| Draw #23 interpolants / system constant | Not captured |
| t7 mirror | Confirmed separate defect |
| 0.9.1 port | Full-game build SUCCEEDED (06:01 AM PC time); D3D11 launched on GTX 570 at 60 FPS (first); in-game white screen + road strip only, sustained free roam NOT confirmed |
| Upstream remake (2026-11-19) | **Full world renders correctly in D3D11 on the GTX 570** (upstream SDK 0c2187cb, merged as feat/d3d11); title 60 FPS, free roam works, avg 10 FPS driving |
| Memexport draws | **EXONERATED (2026-11-19)** — FH1 issues ZERO memexport draws (census 28,560 frames; 3,207-frame free-roam run also 0). The drop was never the render blocker; do not build the CPU memexport route for the render bug |
| Memexport counter | Bug found: summary printed `skipped_memexport` (never incremented) instead of `dropped_memexport` (never printed); Phase 1 fix (split counters + per-draw trace) in the 0.9.1 tree only — NOT in this repo's D: source yet |
| Executor port (2026-11-19) | Upstream PrepareTargets signature cache ported into Seth's D: executor (build OK, DLL CFD2861B); verified in source; **never in-game tested** — the ported DLL is not ABI-compatible with the installed runtime (plugin load failure), needs its matching exe/runtime |
| Launcher | DirectX 11 + GPU selector (GTX 570 / RX 5700 XT / Auto); setting saves (`d3d11`, `d3d11_adapter: 1`); `set-graphics-experiment.ps1` fixed to accept `d3d11` + `D3d11Adapter` (integer); DX11 build installed in the launcher location |

Menus, cutscenes, and the 2D HUD render fine. **The world now renders in
D3D11 on the GTX 570** — the upstream remake (SDK 0c2187cb) shows the full
world correctly in free roam. EDRAM gets coloured (Phase 2e probe #7:
28.9% changed, sky-blue), and the upstream executor's surface-ownership
model is what makes the world land. Memexport is exonerated (zero draws),
so the render bug is solved upstream, not by a memexport fix. Seth's own
backend still needs its executor port tested in-game (via its matching
exe/runtime, not a DLL swap).

**Performance:** the upstream DX11 build runs ~30 FPS idle/title and
**avg 10 FPS while driving** on the GTX 570 (the old build was 4–5).
The frame is ~100 ms; 30 FPS needs ~33 ms. The GPU is the most likely
bottleneck on the 2010 card at 720p 4x MSAA, with CPU per-draw work,
transfers, and the cross-adapter present copy as the other costs. 30 FPS
at 720p is the realistic target.

**Next, in order:** (1) run a profiling driving session with
`fh1_native_gpu_profile` on the upstream build and get the CPU timing
breakdown (prepare_targets / transfers / bind_targets / resolves) plus
the upload stats, to find the real bottleneck; (2) optimize from that
data — the executor signature cache is already in Seth's backend, the
remaining gains depend on what the timing says; (3) test Seth's ported
executor in-game via its matching exe/runtime (D: tree `run_game.cmd`)
and confirm the world renders with his own backend; (4) apply the
Phase 1 counter fix to the D: tree so it lands in this repo's source.
Full detail: [`docs/findings/`](docs/findings/).

## Upstream
- Frame generation (DLSS, FSR, LGSS) is landing upstream; the plan here is
  textures first, then FSR on top — FSR is not a fix for the textures.

## Layout

- `src/graphics/d3d11/` — the backend implementation (17 .cpp files:
  command processor/list, executor, resolve, surfaces, graphics system,
  pipeline/texture/render-target caches, shader, shared memory, submission
  queue, resource state tracker, …)
- `include/rex/graphics/d3d11/` — the matching headers (17 .h files)
- `include/rex/graphics/fh1_edram_tiles.h`,
  `include/rex/graphics/fh1_edram_surfaces.h` — EDRAM tile ownership and
  surface keys
- `docs/DX11_FINISH_NOTES.md` — running log, phase by phase
- `docs/findings/` — investigation reports and probe diffs, Nov 15–18
  (call logger, Vulkan cross-check, edram3d + resolve probes, 0.9.1
  alignment, backend audit, memexport), with an index in `docs/findings/README.md`
- `docs/hardware-constraints.md` — why the backend is shaped the way it is
- `docs/dx12-phase0/` — the D3D12 phase 0–3 inventory and results (context
  for where this backend came from)
- `cmake/` — CMake helper modules (helpers, install, version, PCH, …)
- `scripts/` — developer scripts (PSReX build/configure/test, Vulkan stack
  check, …)
- `tools/build-d3d11.bat` — Release build from the Pinyon checkout
- `tools/check_d3d11_gate.py` — gate checker
- `launcher-gpu-selector/` — the WPF launcher DirectX 11 + GPU selector
  (MainWindow.xaml/.xaml.cs) and the `set-graphics-experiment.ps1` fix
  (accepts `d3d11` and `d3d11_adapter`)

Built DLLs are binary and are NOT committed here; they are collected on
the PC in `D:\PinyonShift\work\testkit-dlls-20261118\`.

## Build

These files live in the Pinyon Shift tree (`rexglue`) at the matching
paths — `src/graphics/d3d11/` and `include/rex/graphics/d3d11/` under the
checkout. Drop them into a Pinyon checkout at those paths, then build the
Release config from the checkout:

```bat
tools\build-d3d11.bat
```

That runs VS 2022 BuildTools (`VsDevCmd.bat -arch=amd64`) and
`cmake --build <checkout>\out\build\win-amd64 --config Release`. The
checkout's canonical location is `D:\PinyonShift\work\.local\rexglue`
(the live tree; `E:\PinyonShift\work` is the baseline/spare copy). The
build script carries that path. A clean rebuild is 1225 steps and
exits 0. Build with `CMAKE_BUILD_PARALLEL_LEVEL=4` to avoid clang OOM,
and verify the DLL strings and exe/dll mtimes after a build before
claiming a change is in the binary.

## Known gaps (not bugs to work around silently)

- **Multisampled depth cannot be read back on D3D11.** Staging cannot
  take MSAA and `ResolveSubresource` has no depth support. That is an open
  instrument gap.
- **The flat grey world — solved upstream, Seth's backend unproven.**
  The upstream remake renders the full world correctly in D3D11 on the
  GTX 570, so the loss is fixed by the upstream executor's
  surface-ownership model, not by the row fix or a memexport route.
  Seth's own backend (with the ported signature cache) is built but has
  NOT been in-game tested — its DLL is not ABI-compatible with the
  installed runtime, so it needs its matching exe/runtime to test.
- **Draw #23 wipe.** Pixel-shader translation and blend state are both
  verified correct; the fault, if any, is upstream in the shader inputs.
- **MSAA.** The original game's MSAA implementation is extremely heavy,
  and the bands are buggy with MSAA so far. FSR 1 shaders are ready for
  after the world renders — textures first, then FSR on top.

## Baseline

`trace-short` on defaults reproduces md5
`b8f68e89c6192577747ab452aa5598ef` on both captures. If that hash moves,
the default path changed.

## End of support 

As of Pinyon 9.5. dx11 is semi implemented and will continued to be updated through the PinyonShift repo. All future updates will have dx11 proper support. Enjoy!

The running log is [`docs/DX11_FINISH_NOTES.md`](docs/DX11_FINISH_NOTES.md)
— it is updated at every step boundary and is the source of truth for
where the work stands.
