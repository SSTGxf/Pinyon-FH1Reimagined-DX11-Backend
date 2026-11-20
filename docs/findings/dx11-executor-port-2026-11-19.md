# DX11 executor port + upstream remake — 2026-11-19

## The upstream remake works (the render bug is solved upstream)

Upstream Pinyon lead nerijs (arcanite24 / Brandon G. Neri) built a D3D11
backend as a port of his own D3D12 backend (bindful path, no ROV / tiled
resources / bindless, runtime shader translation). It renders the FULL
Forza Horizon 1 world correctly in DX11 — The Gauntlet, road, terrain,
trees, mountains, sky, car, HUD, minimap — the first correct full-world
DX11 render. Merged into upstream dev as commit
`7404c62a6359634442d16339f281dd87fa80d297` (`feat/d3d11`), with
`docs/D3D11_BACKLOG.md`, bumping shiftglue-sdk to
`0c2187cbdc1857029550f510f4d3d5bee36a57a6`.

Seth swapped that SDK into his 0.9.1 tree, built it, installed it, and
tested it on the GTX 570 (2026-11-19 evening). **The game works properly
in D3D11 on the GTX 570**: title screen 60 FPS, full world renders
correctly in free roam (road, terrain, trees, sky, HUD), no white/missing
world. FPS: ~30 idle/title, **avg 10 while driving** (the old build was
4–5). Correctness is solved; performance is the remaining job.

**Adapter index is session-dependent.** In the SSH session, DXGI
enumerates the GTX 570 as index 0; on the interactive desktop, index 0 is
the RX 5700 XT and index 1 is the GTX 570. The live setting is
`d3d11_adapter = 1`. The stock 0.9.4 launcher only offers Vulkan in its
UI (the D3D11 picker was Copilot's custom add-on), so launch via
`pinyon.py` with a scheduled task on the interactive desktop, wrapped in
PowerShell Bypass (the shader-prep script is blocked by the default
Restricted execution policy otherwise).

**Upstream performance (backlog).** RTX 4080 (FL 11_0 only): DX11 11.1 ms
vs Vulkan 8.4 ms median (~32% gap, not "only 2.7 ms"). Intel UHD 620:
DX11 151 ms vs Vulkan 85 ms; GeForce 930MX: 80 vs 66 ms. Replays fr1400
and race4800 bit-exact vs Vulkan goldens. The backlog has no dedicated
performance (P-series) items. Direct3D 11 is a fallback for GPUs without
Vulkan 1.3, not a faster path.

**What nerijs took from Seth's package:** the SM 5.0 translator targeting
and the D3D11 pitfall notes (guest-memory mirror as raw view, staging
readback). He did NOT take the backend files ("do not build"), the
immediate drawer ("drops every draw"), the memexport handling, or the runs
as evidence ("drawn=0"). He promised Seth contributor credit on the main
Pinyon repo when it is live.

## Memexport — exonerated (2026-11-19)

FH1 issues **ZERO memexport draws**. Census over 28,560 frames
(`renderer.md`), and a combined free-roam run (3,207 frames, 2.29M draws)
also returned memexport 0. The memexport drop was never the render
blocker. The README's "prime suspect" framing is superseded. Do not build
the CPU memexport route to fix the render bug.

## The executor port — upstream's model into Seth's backend

Seth's direction: remake his DX11 backend following nerijs's approach,
using upstream `feat/d3d11` as the reference. The first step was porting
upstream's `Fh1NativeExecutor` surface-ownership model into Seth's
original D: tree backend, so his own backend renders correctly. The
Copilot prompt is `docs/dx11-executor-port-copilot-prompt.md`.

Copilot (run by Seth, 2026-11-19 evening) changed two files:

- `src/graphics/d3d11/fh1_native_executor.cpp` — added the PrepareTargets
  signature cache (target keys, ownership generation, used bits, stencil
  state, claim rectangle, cache-hit accounting), restored bound surfaces
  on cache hits without repeating claims/transfers, and fixed the
  shared-depth/color-base slot de-duplication to match upstream.
- `include/rex/graphics/d3d11/fh1_native_executor.h` — added the
  persistent cache state and `PrepareSignature` struct.

It also fixed a real compile issue: `FindSurface` needs a packed key
(`pending_keys_[i].Pack()`).

### Verification (checked against source, 2026-11-19)

- **Build SUCCEEDED.** `BUILD_EXIT=0`; `rexgpu-fh1.dll` 11,517,952 B,
  SHA256 `CFD2861B750B16E2133A2A48C751A631E625726D58ABD8678A0A3A750031E93D`.
- **Signature cache is in place.** `PrepareSignature` struct, `last_prepare_`,
  `prepare_cache_hits_` incremented and reported in the periodic executor
  stats. Diff vs the repo version is exactly Copilot's changes (cpp +64
  lines, h +15 lines).
- **Seth's fixes are intact.** Row fix (`source_row_offset`, 16 matches),
  per-band / `ClaimRect`, split memexport counters
  (`dropped_memexport_vertex` / `dropped_memexport_pixel` in
  `command_processor.cpp`).
- **Resolve invalidation is real.** The resolve path calls
  `TextureCache::MarkRangeAsResolved`, whose implementation
  (`pipeline/texture/cache.cpp:443`) calls
  `shared_memory().RangeWrittenByGpu(...)`. Copilot's claim on this was
  correct.
- **Seth's executor already had the ownership machinery.** Per-surface
  ownership, `ClaimRect`, batched transfer, per-band tiling, and
  `BindTargets` were already present — so the port was mainly the
  signature cache, not the whole ownership model.
- **Gaps (Copilot's own report, confirmed).** Upstream CPU timing/profile
  fields were NOT ported, so this build has no prepare/bind/resolve
  ms-per-frame breakdown — only `prepare_cache_hits`. The upstream
  `Fh1DepthOverwrite` interface was NOT introduced; Seth's scissor /
  rectangle claims remain.

### The ported backend was never in-game tested (DLL swap fails)

The ported D: tree DLL (`CFD2861B`) is **not ABI-compatible with the
installed runtime**. Swapping it into the launcher install gives
"Failed to load GPU plugin 'fh1'". The ported backend cannot be tested by
DLL swap alone; it needs its matching exe/runtime (the D: tree's own
`run_game.cmd`), not a DLL swap. The working upstream DLL (`0B5A8F44…`)
was restored (hash-verified) so the game launches again.

## Launcher — DX11 + GPU selection, and the set-graphics fix

Seth had Copilot rebuild the launcher with DX11 and GPU selection
(scoped to the launcher + required files only). The rebuilt launcher shows
Graphics API: Direct3D 11 and GPU (DirectX 11): NVIDIA GeForce GTX 570.

It errored on load: "Settings could not be loaded:
`C:\source\0.1.1\tools\set-graphics-experiment.ps1`: A parameter cannot
be found that matches parameter name 'D3d11Adapter'." The install
location's `set-graphics-experiment.ps1` was the stock upstream script
(schema 30, Vulkan-only): no `D3d11Adapter` parameter, `GraphicsApi`
only allowed `'vulkan'`, and Apply forced `gpu_backend` back to
`'vulkan'` on every save.

Fixed (backup `.musebak-set-graphics-d3d11-20261119`):

1. Added a `D3d11Adapter` parameter (`[int]`, range −2..64; −1 = first
   FL 11_0 adapter, −2 = WARP).
2. `GraphicsApi` now accepts `'vulkan'` and `'d3d11'`.
3. `Get-SettingsResult` reports the backend actually in the file
   (instead of hardcoding `'vulkan'`) and reports `d3d11_adapter`.
4. Apply no longer forces `gpu_backend` to `'vulkan'`; a `GraphicsApi`
   passed on the command line wins, otherwise the backend already in the
   file is kept.
5. Apply writes `d3d11_adapter` when passed.

Verified: Get and Apply both return `graphics_api: d3d11`,
`d3d11_adapter: 1`; the config keeps `gpu_backend = "d3d11"` and
`d3d11_adapter = 1` after a save. The launcher's Save button works.

The fixed script is `launcher-gpu-selector/set-graphics-experiment.ps1`
in this repo. Note it also carries the newer upstream SPIR-V shader
settings (`spirv_specialize_texture_signs`, `spirv_fast_pixel_math`,
`spirv_implicit_lod_2d`) that the repo's older copy lacked.

## Performance — where FPS gains come from

The frame is ~100 ms at 10 FPS; 30 FPS needs ~33 ms. The time goes to:
GPU rendering (most likely the bottleneck on the 2010 GTX 570 at 720p
4x MSAA), CPU per-draw work (mostly already cached upstream), transfers
/ uploads, and the cross-adapter present copy (render on the 570,
present on the 5700 XT). CPU tweaks alone will not get to 30 FPS; the
timing data (`fh1_native_gpu_profile`, upstream only) decides where the
real gains are. The executor signature cache cuts redundant target-prep
CPU work, but the big gains need the GPU-side picture.
