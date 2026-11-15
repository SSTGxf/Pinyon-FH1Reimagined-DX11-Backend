# Render bug — update (2026-11-15)

New top suspect, new instrumentation, and a wrong-worktree detour.
Updated 2026-11-15 ~02:20 CST. No rendering fix yet.
Full history: [render-bug-status-2026-11-14.md](render-bug-status-2026-11-14.md).

## The bug (unchanged)

Open-world driving in Forza Horizon 1 does not render the world. Flat blue
with a black top half and a thin band of terrain. Roads and mountains
duplicate. The Viper renders blue instead of yellow/black. Menus,
cutscenes, automated driving, and the 2D HUD all render fine. Reproduces
identically on the RX 5700 XT and the GTX 570 — backend logic, not
Fermi-specific.

## New top suspect: resolve-time clears are not implemented in D3D11

Claude (Sonnet 5.5), asked for everything that could fix this, put this
first — and it fits the evidence better than anything so far.

- **What the other backends do:** D3D12 (`fh1_native_executor.cpp`
  ~1840–1900) and Vulkan end their resolve path with a clear — if
  `ClearingColor()` then clear (same for depth). `ClearTargetSet`
  performs the clear and the cleared tiles are claimed.
- **What D3D11 does:** a case-insensitive grep for the clear-related
  symbols finds only the one-time clear at surface creation
  (`fh1_native_surfaces.cpp`). There is no resolve-time clear.
- **Why it fits:** on a tiled frame the guest clears EDRAM through the
  resolve between passes. Without that clear, tiles 2 and 3 draw over
  tile 1's stale colour and depth. That gives missing geometry, repeats,
  and a bad pattern — and it shows up in the EDRAM *before* the resolve,
  which matches the earlier finding that the bad pattern is already in
  the native EDRAM surface before the resolve happens.
- **It also explains the owner-split result.** The owner-split populated
  the 4x sources but did not change the picture. If the problem is a
  missing clear, splitting owners was fixing the wrong thing.

### How to check

Log `plan.Info.ClearingColor()` / `ClearingDepth()` on gameplay
resolves. If they are true, port the D3D12 clear path to D3D11.

### Constraint

D3D11.0 has no rect clear. Partial rects need `ClearView` (D3D11.1
runtime) or a scissored full-rect draw.

### Depth detail (from the same analysis)

Decode `RB_DEPTH_CLEAR` / 10 per format and clear on the original base.
Depth uses `RB_DEPTH_CLEAR`, 24 bits in → 8, and `R0x24` is halved into
(0,1) per the D3D12 comment.

## New instrumentation (implemented, in the canonical source)

### Minimap three-stage capture

Copilot (VS Code, via OmniRoute) implemented a minimap capture to find
which stage first shows the stripes — the cheapest repro for the bug,
per Claude's earlier proposal.

- New cvar `fh1_debug_minimap_capture_swap` (default frame 338).
  Captures one 320×192 FH1 minimap candidate at three stages:
  1. source render target,
  2. resolved guest bytes,
  3. texture-cache / SRV upload.
- Files: `flags.cpp` (+3), `flags.h` (+2), `command_processor.cpp` (+1),
  `command_processor.h` (+6), `texture_cache.h` (+1),
  `texture_cache.cpp` (implementation). All additions, no deletions.
- Log-only: does not modify render targets or the production resolve
  destination.
- Verified present in the canonical source
  (`E:/PinyonShift/work/.local/rexglue`) via SSH on 2026-11-15 ~00:06
  CST. The capture has not been run yet — no three-stage result.

### Clear-flag logging

Copilot added logging of `IsClearingColor()` / `IsClearingDepth()` in
`command_processor.cpp` (~lines 3584–3585), logged as 1/0. A compile
error (bitfield types) was fixed by casting to int (`? 1 : 0`). This is
a compile-only fix — it does not change runtime behaviour; it lets the
clear-flag instrumentation build. This is the logging that the
resolve-time-clear check above needs.

## Wrong worktree detour (resolved)

VS Code got switched to an agent worktree that has no D3D11 backend at
all:

- Worktree: `rexglue.worktrees/e-pinyonshift-work-local-rexglue-src-graphics-d3`
- Branch: `agents/e-pinyonshift-work-local-rexglue-src-graphics-d3`,
  HEAD `ef8eaa43` ("draw fhi's tiles in one pass by default on linux")
- No `src/graphics/d3d11/`, no `D3D11CommandProcessor`, CMakeLists has
  Vulkan and D3D12 only.

Copilot correctly refused to continue there and reported that the D3D11
implementation is in the other checkout. The earlier "build failure"
(stale cache, GCC-style `-Wall -Wextra` flags passed to MSVC) was a
symptom of building the wrong tree, not a problem with the D3D11 code.

The canonical checkout is `E:/PinyonShift/work/.local/rexglue` at
detached HEAD `79d3e6fa`. It has the D3D11 backend, the minimap capture,
and the clear-flag logging. Build with
`E:/PinyonShift/work/.local/muse_build_game.bat`, not VS Code's CMake.

## Build status (unconfirmed)

A build was run and the log (`muse_build_game.log`) inspected. It had a
compile error (the bitfield cast above); Copilot fixed it. The Copilot
chat then hit a context limit (below) before the rebuild result was
confirmed. **The latest build is not confirmed green.** Do not assume it
builds until verified.

## Tooling note: OmniRoute + Copilot context limit

VS Code Copilot now routes through OmniRoute (local AI gateway,
`http://localhost:20128`). `auto/best-coding` routes to
`mai-code-1.1-flash` (128K input limit); `auto/coding-pro` also routes
there. GPT 6.1 Sol (872K window) is the top model by usage on the
dashboard but had a transient Codex transport error
("app-server transport is not configured").

The Copilot chat accumulated ~215K tokens and exceeded
mai-code-1.1-flash's 128K input limit (HTTP 400). "Try Again" does not
help — the request is oversized. Fix: switch to a bigger-window model
(GPT 6.1 Sol, 872K) or start a new chat to reset context. Context
persists within a chat and only resets on a new chat.

## Other unverified leads (from Claude's analysis)

Each has a test; none verified yet. Kept as backup if the clear does
not pan out.

- **Tile passes:** window offset not applied (log
  `vtx_window_offset_enable`, `window_offset_disable`, viewport /
  scissor / claim rows per draw); predicated draws wrongly skipped or
  run (`bin_select`); draws vs resolve ordering (tape path
  `RecordResolveBatch` vs immediate `ResolveToMemory`, stamp sequence
  numbers); don't mix tiled and untiled (row-offset idea is Vulkan's
  untiled model); surface height too small to hold tile rows (check
  `fh1D3D11SurfaceHeight` for 4x/2x); ownership transfers between
  passes overwriting fresh tile data.
- **Resolve:** C12 is `2_10_10_10_FLOAT_AS_16_16_16_16` — check the
  16F→7e3 conversion and the packed dest format (could be the edge
  garbage); `dest_base - copy_dest_extent_start` math on the staging
  path; typed-UAV limits on Fermi, endian/swap bits, band heights of
  256/256/208 landing contiguously.
- **Texture cache / composite:** the "skip refresh when no new resolve"
  optimize (`c7d86e2`) and the stutter-fix path leaving a stale texture
  — toggle them off.

## Next steps (updated)

1. **Confirm the build is green** (rebuild after the bitfield-cast fix).
2. **Run the game and check the clear flags** in the log:
   `ClearingColor()` / `ClearingDepth()` on gameplay resolves.
3. **If the flags are true, port the D3D12 clear path** (`ClearTargetSet`)
   to D3D11, respecting the D3D11.0 no-rect-clear constraint.
4. **Run the minimap capture** (`fh1_debug_minimap_capture_swap`,
   frame 338) and report which of the three stages first shows the
   stripes. This confirms whether the corruption is upstream (source)
   or at the resolve.
5. If the clear does not fix it, work through the backup leads above,
   starting with window offset and the "skip refresh" optimize.

## Config (at time of writing)

- `gpu_backend="d3d11"`, `d3d11_adapter="5700 XT"` (renderer on the
  5700 XT; fix there, then verify back on the GTX 570).
- `pinyon_audit=true`, `pinyon_audit_passes=true`,
  `pinyon_msaa_probe=true`, `pinyon_dump_frames=true`.
- MSAA restored to 4x default; 1x diagnostic remains opt-in
  (`flags.cpp` line 19, `command_processor.cpp` line 860).
- Owner-split is backed out of the build. Commit `bda840e` on this
  repo still contains it; the repo is out of sync with the build on
  that point (re-sync decision pending).
- FPS: 8.0–17.7 in free-roam (workload streaming, single CPU decoder
  thread bottleneck, not a code regression). 30 FPS at 720p is the
  realistic target.
