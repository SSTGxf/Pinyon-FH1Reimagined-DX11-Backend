# Copilot Prompt: Port Upstream Fh1NativeExecutor into Seth's D3D11 Backend

## Goal
Port the upstream Pinyon Shift D3D11 `Fh1NativeExecutor` surface-ownership model into Seth's original D3D11 backend (D: tree) so Seth's backend renders the full Forza Horizon 1 world correctly on the GTX 570.

## Background / Why
- Upstream (nerijs, arcanite24) built a D3D11 backend as a port of his own D3D12 backend (bindful path). It renders the FULL world correctly (The Gauntlet, road, terrain, trees, mountains, sky, HUD, minimap) — the first correct full-world DX11 render. Merged into upstream dev as commit 7404c62a, SDK at 0c2187cb.
- Seth's original D3D11 backend does NOT render the world correctly (road as a strip at the bottom with everything above white/blank, or world gone with sky + HUD only). The render path is the problem, not the shaders or textures.
- The fix is the executor: how EDRAM surfaces are claimed, owned, transferred, and bound per draw. Upstream's executor model is what makes it render.
- This is ALSO the optimization: upstream's executor skips redundant target-prep work when a draw repeats the last one's targets (signature cache), so porting it cuts per-draw CPU cost as well as fixing the render.

## Source (upstream, KNOWN GOOD)
- SDK at commit 0c2187cb, in the 0.9.1 tree: `C:\Users\seths\Downloads\pinyon-shift-0.9.1\pinyon-shift-0.9.1\thirdparty\shiftglue-sdk\`
- Executor: `src\graphics\d3d11\fh1_native_executor.cpp` (112,351 B, ~2401 lines) + header `include\rex\graphics\d3d11\fh1_native_executor.h`
- Also read as needed for interfaces: `src\graphics\d3d11\command_processor.cpp` (how IssueDraw calls PrepareTargets/BindTargets), `src\graphics\d3d11\shared_memory.cpp`, `src\graphics\d3d11\texture_cache.cpp`.
- Upstream backend shape for reference: `src\graphics\d3d11\` has 8 files (command_processor.cpp 85,375 B, dxbc_shader_model_5_0.cpp, fh1_native_executor.cpp, graphics_system.cpp, pipeline_cache.cpp, primitive_processor.cpp, shared_memory.cpp, texture_cache.cpp).

## Target (Seth's backend, TO FIX)
- Live tree: `D:\PinyonShift\work`
- D3D11 code: `D:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\` (executor: `fh1_native_executor.cpp` + headers in `.local\rexglue\include\rex\graphics\d3d11\`)
- Presenter: `D:\PinyonShift\work\src\ui\d3d11\` — do NOT touch the presenter in this build. That's a separate build (R/B-swizzle + viewport fit).
- Build the D: tree with: `D:\PinyonShift\work\.local\muse_build_game.bat`

## What to port (the executor model)
1. **PrepareTargets signature cache**: Upstream computes a `PrepareSignature` (generation, used_bits, length_tiles, stencil_written, surface keys) and returns early if `signature == last_prepare_` (and the draw is not a depth-overwrite draw). Port this fast path so repeated draws with the same targets skip the redundant prep work. This is both a correctness and a performance win.
2. **Surface ownership + claims**: Upstream's model of EDRAM surface ownership — how surfaces are created (GetOrCreateSurface), how ownership is claimed per draw (tile ownership), and how transfers move data between the guest EDRAM and host surfaces. Port the ownership/claim/transfer logic, adapting to Seth's existing surface-key and tile structures where they already match.
3. **BindTargets flow**: How upstream binds the depth + color targets for the draw (bound bits, formats, width/height), including the depth-overwrite rectangle-claim path.
4. **Preserve Seth's existing fixes — do NOT regress these** (they are verified and must stay):
   - Per-band fix (per-surface bitmask in the executor).
   - Row fix: `Fh1D3D11UntileBand` returns the ROW OFFSET (0/256/512); `source_row_offset = 0` (plan.y0 is already the band's real rows).
   - Counter fix (split counters `dropped_memexport_vertex` / `dropped_memexport_pixel`; summary prints the dropped counters, not the never-incremented `skipped_memexport`).
   - Resolve-invalidation fix: after the resolve staging copy succeeds, call `shared_memory_->RangeWrittenByGpu(...)` so cached textures over the resolved range are invalidated (the D3D11 resolve path never called it; D3D12 does).
   - Three-band tiling is real (rows 0–256 / 256–512 / 512–720). Band 0 copy-down is a NO-OP by design.
5. **Do NOT port**: memexport handling (FH1 issues ZERO memexport draws — census over 28,560 frames; it was never the blocker), the immediate drawer, or the presenter (separate build).

## Constraints
- **Verify every claim against source. Do not take my word for it.** Read the upstream executor and Seth's executor before changing anything. If a claim in this prompt doesn't match the source, follow the source and say so in the report.
- **Backups**: Before modifying any file, copy it to `<file>.musebak-executor-port-20261119`.
- **Minimal, surgical changes**: Port the model, don't rewrite Seth's backend wholesale. Keep his interfaces where they work; adapt upstream's logic to them. If an interface must change, say why and keep the change minimal.
- **Build**: After changes, build the D: tree with `D:\PinyonShift\work\.local\muse_build_game.bat`. The build must succeed with no new errors. Use `CMAKE_BUILD_PARALLEL_LEVEL=4` (clang can OOM with more). The build output is `D:\PinyonShift\work\out\build\win-amd64-release\`.
- **Do NOT install or launch the game yourself.** Build only. Seth will launch and test on the GTX 570.

## Verification (for Seth to run after the build)
- Launch via `D:\PinyonShift\work\.local\run_game.cmd` (or the launcher with D3D11 + GTX 570).
- Confirm: full world renders in free roam (road, terrain, trees, mountains, sky, HUD, minimap) — not a road strip, not sky-only.
- FPS is a secondary check (the upstream build does ~10 FPS avg while driving on the GTX 570; the point of this build is correctness first).

## Key facts (verify against source)
- Upstream command_processor.cpp is 85,375 B; Seth's is ~388,219 B. Upstream is a port of D3D12; Seth's is from-scratch. Do not assume the same structure — read both.
- Upstream `d3d11_adapter` is an INT32 index; Seth's old backend used a name substring ("GTX"). The executor port does not change adapter selection.
- The upstream backend has CPU timing (`fh1_native_gpu_profile`) that logs prepare_targets/transfers/bind_targets/resolves ms/frame every 600 frames — useful for verifying the signature cache is skipping work.
- Fh1SurfaceKey Describe(): `base_tiles/pitch_tiles/msaa/format` — EDRAM colour `0/32/4x/c3` (c3 = colour FORMAT 3, NOT slot 3), composite `0/16/1x/c2`, EDRAM depth `1024/32/4x/d1`.
- The upstream backend is a port of nerijs's own D3D12 backend (bindful path, no ROV/tiled resources/bindless). It translates shaders at runtime.
- Seth's backend was built around what the GTX 570 handles best, then he would tune CPU-to-GPU overhead. Keep that design stance: don't add work the 570 can't afford.

## Report back
- Build success/failure and the full build log path.
- Every file changed (with backup suffix) and a one-line summary of what changed in each.
- Any claim in this prompt that didn't match source, and what the source actually says.
- Whether the signature-cache fast path is in place and where.
- Anything you could not port and why.
