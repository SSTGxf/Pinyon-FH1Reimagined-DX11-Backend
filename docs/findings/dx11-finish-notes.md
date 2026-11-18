# DX11 Finish — running state (update at EVERY step boundary)

Source of truth prompt: C:\Users\seths\Downloads\Freebuff-Pinyon-DX11-Finish-Prompt.txt
Rexglue tree: F:\PinyonShift\work\.local\rexglue (project root F:\PinyonShift\work)
No other checkout. No concurrent actor. No d3d11 lock file.

## Current step
STEP A (Phase 3, gate: draws issue) — starting. A0 baseline re-verified below.

## Gate verdicts recorded so far
- Phase 1 gate: PASS — "d3d11-cleared-frames" (4,165 frames / 70 s, GTX 570 FL11_0, median 59.98 FPS).
- Phase 2 gate: PASS — "d3d11-texture-uploads" (4,122 frames / 70 s, 17000 textures / 6024290 KB, 0 failures).
- "d3d11-native-resolve-path" probe family: ran 4x today (17:51–18:08 UTC). Verdict of NEWEST run
  = Phase 4 entry baseline (to be read at Step B1; not yet read at Step A).
- GATE A/B/C/D/E: not run.

## True file inventory (verified 2026-10-21 from disk + git status)
include/rex/graphics/d3d11/: command_processor.h (3025), fh1_native_resolve.h (2888),
  fh1_native_surfaces.h (4115), graphics_system.h (1569), host_render_config.h (3404),
  primitive_processor.h (1631, NO-OP SKELETON), render_target_cache.h (2210), shared_memory.h (1590),
  shared_memory_upload.h (2323), shared_memory_upload_device.h (2543), texture_cache.h (6544).
  MISSING: shader.h, pipeline_cache.h, fh1_native_executor.h.
src/graphics/d3d11/: command_processor.cpp (15454; IssueCopy works, IssueSwap wired, IssueDraw drops
  draws), fh1_native_resolve.cpp (9259), fh1_native_surfaces.cpp (7768), graphics_system.cpp (1288),
  host_render_config.cpp (7986), primitive_processor.cpp (1501, NO-OP SKELETON),
  render_target_cache.cpp (1720), shared_memory.cpp (2739), shared_memory_upload_device.cpp (6543),
  texture_cache.cpp (42470, COMPLETE incl. GetActiveTextureBindfulSRV / GetSamplerDesc).
  MISSING: shader.cpp, pipeline_cache.cpp, fh1_native_executor.cpp.
Untracked additions relevant to port: src/graphics/pipeline/shader/dxbc_geometry_shaders.{h,cpp}
  (shared GS DXBC builder, "Shared by the DXBC-based backends (D3D11 and D3D12)"),
  src/graphics/shaders/fullscreen_cw.vs.hlsl, src/graphics/shaders/bytecode/d3d11_5_0/,
  src/ui/shaders/d3d11/, tests/unit/graphics/d3d11_*_test.cpp.
Modified tracked files (from earlier phases): src/graphics/CMakeLists.txt, conversion.{h,cpp},
  d3d12/pipeline_cache.{h,cpp} (shared-code changes; must stay backend-conditional), compile-fh1-native.sh.

## Key design facts established for the port (from disk)
- DxbcShaderTranslator(vendor_id, bindless_resources_used, edram_rov_used, ...) — D3D11 uses
  bindless_resources_used=false -> flat registers: b0=system constants, b1=float constants,
  b2=bool/loop, b3=fetch constants; t0=shared memory mirror ByteAddressBuffer, t1+=bindful textures
  (bindful_srv_index); s#=samplers (bindful index). (dxbc_translator.h SRVSpace/SRVMainRegister/
  CbufferRegister enums.)
- Shared-memory host buffer already has BindFlags SRV|UAV|INDEX_BUFFER (Phase 3 prepared).
- D3D12 templates for shape/semantics: src/graphics/d3d12/{shader.cpp, pipeline_cache.cpp,
  primitive_processor.cpp, command_processor.cpp, fh1_native_executor.cpp}.
- PrimitiveProcessor base contract: InitializeCommon(true, false, false, true, true, true) like D3D12
  (D3D11 FL11_0: full 32-bit indices; no fans/loops; quads->lists via GS; point/rect via translator).
- geometry_shaders: GeometryShaderKey + CreateDxbcGeometryShader() in
  include/rex/graphics/pipeline/shader/dxbc_geometry_shaders.h (identical DXBC for D3D11).
- Guest shader blobs: translator stamps SM 5.1; FL 11_0 rejects -> Gate A logs+counts rejected
  guest shader hashes; SM 5.0 retarget is Step D.

## Step log
- Step A0: re-verified inventory via git status + ls (2026-10-21 ~13:50 MDT). Todo list reset to
  prompt steps A-E.
