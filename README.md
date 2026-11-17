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
- **EDRAM resolve conversion — exonerated.** The resolve source is uniform
  grey 0.258789 (= RGB 66,66,66) at *every* sample, so the EDRAM dump is
  representative of the resolve's source. The loss is in the 3D render
  into EDRAM, upstream of the resolve.
- **3D texture binding — mechanism works.** All 26 observed binds are
  valid (valid=1, texture=1, srv=1, dimok=1), including small fetches. But
  valid SRVs prove the *binding mechanism* only — not that the shader
  samples the intended coordinates, the format/swizzle is interpreted
  correctly, the guest constants reach the translated shader, or the shader
  output is correct.
- **World geometry never reaches the EDRAM.** The first EDRAM-targeting
  draw of the frame is a texture-less fullscreen RectangleList pass that
  writes uniform grey (66,66,66) — the grey is the shader's output, not the
  clear value (the top band was black before the draw and grey after it; the
  control readback was pixel-identical). A refined probe for textured
  triangle draws targeting the EDRAM colour surface (0/32/4x/c3) captured
  *nothing*: in the investigated frames, **no triangle-geometry draw
  targets the EDRAM surface at all**. The EDRAM only ever receives the grey
  fullscreen pass — the most direct explanation for the flat grey world.
  *Why* the triangle world draws are absent is under investigation now:
  whether the guest never issues them, the backend drops them before
  `IssueDraw`, they are routed through the `kCopy` path, or they target a
  different surface.

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

## Current status (2026-11-16)

| Area | Status |
|---|---|
| Band 256 / copy-down | Fixed and verified |
| EDRAM resolve conversion | Exonerated |
| Texture upload chain | Exonerated |
| 3D texture binding mechanism | Working |
| World geometry reaching EDRAM | No triangle draws observed |
| Reason world draws are absent | Unknown — under investigation |
| Draw #23 pixel-shader translation | Verified faithful |
| Draw #23 blend state | Verified correct |
| Draw #23 interpolants / system constant | Not captured |
| t7 mirror | Confirmed separate defect |

Menus, cutscenes, and the 2D HUD render fine. In free roam the world is
flat grey and composite draw #23 wipes the composed frame to black.

**Performance:** D3D11 free roam runs 4–5 FPS on the GTX 570. The
bottleneck is a single CPU decoder thread (~76% of frame time) plus
workload streaming. 30 FPS at 720p is the realistic target.

**Next, in order:** (1) trace the world draw's journey through the command
processor — does it exist in the guest command stream, reach the backend,
get classified as rendering or copy, reach `IssueDraw`, and have the EDRAM
surface selected as its render target; (2) capture draw #23's vertex-shader
inputs and outputs (interpolant data and `xe_color_exp_bias`) to calculate
what the faithfully translated pixel shader should output versus the
observed black wipe. No rendering patch is authorized on the current
evidence alone.

## Upstream

- **nerijs (arcanite24, Pinyon Shift creator/lead)** starred this repo on
  2026-11-16 — its first stargazer — and wants the DX11 backend in
  Pinyon's main repo and in lavender-shift (the FH2 native PC port),
  contingent on the world-rendering fix landing.
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
- `docs/hardware-constraints.md` — why the backend is shaped the way it is
- `docs/dx12-phase0/` — the D3D12 phase 0–3 inventory and results (context
  for where this backend came from)
- `tools/build-d3d11.bat` — Release build from the Pinyon checkout
- `tools/check_d3d11_gate.py` — gate checker

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
checkout's canonical location is `E:\PinyonShift\work\.local\rexglue`;
the build script carries that path. A clean rebuild is 1225 steps and
exits 0. Build with `CMAKE_BUILD_PARALLEL_LEVEL=4` to avoid clang OOM,
and verify the DLL strings and exe/dll mtimes after a build before
claiming a change is in the binary.

## Known gaps (not bugs to work around silently)

- **Multisampled depth cannot be read back on D3D11.** Staging cannot
  take MSAA and `ResolveSubresource` has no depth support. That is an open
  instrument gap.
- **The flat grey world / missing triangle draws.** No triangle-geometry
  draw targets the EDRAM surface in the investigated frames (see above).
- **Draw #23 wipe.** Pixel-shader translation and blend state are both
  verified correct; the fault, if any, is upstream in the shader inputs.
- **MSAA.** The original game's MSAA implementation is extremely heavy,
  and the bands are buggy with MSAA so far. FSR 1 shaders are ready for
  after the world renders — textures first, then FSR on top.

## Baseline

`trace-short` on defaults reproduces md5
`b8f68e89c6192577747ab452aa5598ef` on both captures. If that hash moves,
the default path changed.

The running log is [`docs/DX11_FINISH_NOTES.md`](docs/DX11_FINISH_NOTES.md)
— it is updated at every step boundary and is the source of truth for
where the work stands.
