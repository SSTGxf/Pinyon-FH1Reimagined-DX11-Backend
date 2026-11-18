# Memexport — the prime suspect (2026-11-18)

## What happened today

- **0.9.1 full-game build SUCCEEDED** (06:01 AM PC time):
  `pinyon_shift.exe` 65,656,320 B, `rexgpu-fh1.dll` 14,447,616 B,
  `rexruntime.dll` 13,905,408 B. The `cli11` configure block is cleared.
- **D3D11 LAUNCHED on the GTX 570 at 60 FPS — a first.** The DX11 build
  was installed into the launcher location (new exe/DLLs in the install,
  both configs say `d3d11` + GTX adapter, stock files backed up). The
  runtime shows DirectX 11 + FH1 Native Executor. The launcher shows
  DirectX 11 + GPU selector, and the setting now saves (a marker-hash
  mismatch was the cause; fixed).
- **In-game, the world is still not there.** First in-game run: white
  screen, then past the licence screen — a road visible as a strip at the
  bottom, everything above the line white/blank: no geometry, no 3D, no
  untiles. A separate test: world gone, sky + HUD only, Corrado black box
  still present. Sustained free roam is NOT confirmed.
- A later launch crashed; a privacy-safe diagnostic zip was prepared and
  is being analysed.

## The prime defect — memexport draws are dropped

Verified in the ported 0.9.1 tree
(`thirdparty/shiftglue-sdk/src/graphics/d3d11/command_processor.cpp`,
lines 1916–1922): the command processor deliberately DROPS every draw
whose vertex OR pixel shader writes memexport, because D3D11 FL 11_0 has
no vertex-shader UAVs.

- Line 1878: `memexport_used_vertex = vertex_shader->memexport_eM_written() != 0`
- Line 1903: pixel equivalent
- Line 1904: `memexport_used = vertex || pixel`
- Line 1919: `trace_tile_path("memexport_not_supported")`
- Line 1920: `++draw_counters_.dropped_memexport`

The executor skips memexport draws too:
`fh1_native_executor.cpp:789` and `:2688` both `if (draw.memexport)` —
PrepareTargets and NativeDrawIssued both skip them. **There is NO Step C
memexport transfer handling in the executor at all** (the
command-processor comment claims Step C owns transfers — it does not
exist).

## Correction — the translator DOES emit the memexport stores

An earlier carry-prompt claim said "no emitted code takes those temps and
writes them to a UAV / the store step is missing." **That was wrong**,
caught because Copilot was told not to take the prompt's word for it.

- `dxbc_translator.cpp:1058–1062` calls `ExportToMemory(...)`
- `dxbc_translator.cpp:1866–1889` calls `ExportToMemory(export_eM)` in
  `ProcessAllocInstruction`
- `dxbc_translator_memexport.cpp:620–758` emits raw stores/atomics
  (`OpAtomicAnd` 694, `OpAtomicOr` 700/722/729)

The translator DOES emit the memexport stores. **The defect is the
missing CARRIER** (no FL 11_0-legal way to get the data to a writable
stage), not a missing store. Do not repeat the "missing store" claim.

The destination was never the problem: a mirror UAV exists at
`shared_memory_upload_device.cpp:477–505` (`GetHostBufferUAV()` — cached
RAW buffer UAV, `D3D11_BUFFER_UAV_FLAG_RAW`, `DXGI_FORMAT_R32_TYPELESS`,
over `host_buffer_`, the host mirror).

## Routes considered — two dead, one open

- **Route B (pixel-shader carry) is DEAD.** It cannot preserve one
  ordered export per vertex, because pixel-shader executions depend on
  pixel coverage.
- **Route A (stream output + compute replay) is NOT YET DEFENSIBLE.**
  There is no stream-output support in the D3D11 backend (zero mentions
  of StreamOutput / SOSetTargets / StreamOut in `src/graphics/d3d11/*.cpp`),
  no proven bound for repeated/looped export events, no verified
  geometry-shader pass-through carrying every vertex-shader event, and no
  ordered replay/readback lifecycle. Geometry shaders exist
  (`command_list_immediate_sink.cpp:273`, `command_processor.cpp:4379–4385`,
  `fh1_native_executor.cpp:297,364`) but a GS cannot hold UAVs at FL 11_0
  either. `host_draw_vertex_count` does not prove event count or capacity,
  and compute work-item ordering cannot be assumed.
- **Third option: CPU-side shader interpretation.** It avoids
  vertex-stage UAVs entirely, but must preserve execution order and memory
  writes. Grounding, verified in the 0.9.1 tree:
  - `interpreter.cpp` (48,408 B), `ShaderInterpreter::Execute()` at line
    25, namespace `rex::graphics`. **ZERO memexport mentions** in the
    file.
  - `interpreter.h`: the constructor takes `const RegisterFile&` +
    `const memory::Memory&` — **`memory_` is CONST, so the interpreter
    cannot write guest memory through it**; the sink must do the writing.
    The `ExportSink` interface exposes `AllocExport(AllocType, size)` and
    `Export(ExportRegister, const float* value, uint32_t mask)`, with
    separate `kExportAddress` and `kExportData0..4` callbacks. The sink is
    called at `interpreter.cpp:220–223` (AllocExport) and `:840–860`
    (Export).
  - Only caller in the tree: `fh1_depth_overwrite.cpp:77` — its `Derive()`
    gates on `CanInterpretShader`, loops indices (≤12), sets
    `temp_registers()[0]` to the vertex index, calls `Execute()`, and its
    `PositionSink` captures `kVSPosition` exports. This is the exact
    pattern a memexport route would follow (sink captures address+data
    and WRITES it instead of reading position).
  - `CanInterpretShader()` returns FALSE if the shader uses
    texture-fetch instruction results — that limits which shaders the CPU
    route can take.

## Counter bug — every memexport log reading so far was meaningless

Verified in the live 0.9.1 tree:

- `command_processor.cpp:1920` increments `dropped_memexport`, but the
  per-frame summary (lines 6997–7026) prints `skipped_memexport`.
- `skipped_memexport` appears ONLY at `cpp:6997` (print gate), `cpp:7026`
  (the print), and `command_processor.h:65` (declaration) — **it is NEVER
  incremented**.
- `dropped_memexport` appears ONLY at `cpp:1920` and `h:66` — **never
  printed**.
- The print gate also omits `dropped_memexport`, so a memexport-only
  frame may print no summary at all.

**Every `skipped[memexport=N]` log reading so far was always 0 and
meaningless.** The existing logs contain no `memexport_not_supported`
traces, no vertex/pixel split, and no interpreter timing. The
vertex-vs-pixel drop split and the CPU cost cannot be measured from
current logs — a separately established measurement path is required
before the CPU route can be justified.

## Phase 1 fix — implemented in the 0.9.1 tree (BUILD prompt)

Copilot implemented the counter fix from the BUILD prompt
(`dx11-memexport-build-copilot-prompt.md`, Phase 1) in the 0.9.1 tree.
Exact diff against
`command_processor.cpp.musebak-memexport-build-20261118` (26 lines):

- New cvar (line ~84): `REXCVAR_DEFINE_BOOL(d3d11_memexport_drop_trace,
  true, "GPU", "Log D3D11 memexport draw drops with shader hashes and
  primitive type")` — per-draw drop trace, default ON.
- At the drop: `if (memexport_used_vertex)
  ++draw_counters_.dropped_memexport_vertex;` and the pixel equivalent,
  plus a gated per-draw line: `FH1 memexport drop: stage={} prim={}
  vs={:016X} ps={:016X}` (stage `vertex` / `pixel` / `both`).
- Summary gate (7013–7014): now uses `dropped_memexport` plus the two
  split counters, instead of `skipped_memexport`.
- Summary format: `memexport={} memexport_vertex={} memexport_pixel={}`
  (replacing `skipped[memexport={} ...]`), printing
  `dropped_memexport`, `dropped_memexport_vertex`,
  `dropped_memexport_pixel`.

The split counters were added to `DrawCounters`
(`command_processor.h`, after `h:66`); the `skipped_memexport` field was
NOT deleted. Backups: `.musebak-memexport-build-20261118`.

**Scope note: this fix is in the 0.9.1 tree only
(`thirdparty/shiftglue-sdk/...`). The D: tree — the source this repo
tracks at `src/graphics/d3d11/` — does NOT yet have it, and still prints
the dead `skipped_memexport` counter. The same fix must be applied to
the D: tree before it is in this repo's source.**

## Translator change — flagged, NOT verified

In the same 0.9.1 tree, `dxbc_translator_memexport.cpp` changed (7 lines
vs `.musebak-fullbackend-20261118`): the early return that skipped
memexport emission for vertex shaders at SM 5.0 was REMOVED:

```cpp
// (removed) Direct3D 11 feature level 11_0 does not permit UAV
// declarations or UAV writes in vertex shaders.  Retain
// vertex-shader rasterization output and omit only the unsupported
// export sequence for SM 5.0 D3D11 lowering.
if (target_sm_50_ && is_vertex_shader()) {
  return;
}
```

This is NOT part of Phase 1 and its correctness is NOT verified. A
vertex shader containing UAV stores would fail D3D11 shader creation at
FL 11_0, so removing the guard risks turning a clean draw-drop into a
shader-creation failure. **Do not treat it as a fix. It needs review
against the translator's call sites before it ships.**

## What is NOT in this commit

- The 0.9.1 tree source (`thirdparty/shiftglue-sdk/...`) is a different
  tree from the D: source this repo tracks. Its `command_processor.cpp`
  (379,076 B + Phase 1) is NOT the file at `src/graphics/d3d11/`
  (377,267 B). The port source is not pushed here; only the findings and
  the Phase 1 diff above.
- The built DLLs are binary and are NOT committed. They are collected on
  the PC in `D:\PinyonShift\work\dx11-testkit-dlls-20261118.zip`
  (13.9 MB, 6 DLLs).
- The D: free-roam test of the rowfix build is still outstanding.
- The resolve constant-buffer + shader trace (ChatGPT's question: is
  `plan.y0` the source origin, making offset 0 correct, or destination
  only, needing 512) is still open.

## Next, in order

1. Run the instrumented 0.9.1 build to free roam and measure the real
   vertex/pixel drop split from ALL rotated logs (the BUILD prompt,
   Phase 1 §2.3).
2. Build the CPU route (BUILD prompt Phase 2): memexport sink pairing
   `kExportAddress` / `kExportData0..4` in order, a writable write path,
   a call site in the command processor, GPU still draws the geometry,
   drop branch replaced only after verification, interpreter timing
   measured.
3. After the memexport fix: re-measure the resolve/composite path — fix
   the resolve probe gate (rect check instead of `untile_band == 2`) and
   measure staging vs mirror in free roam. Do not assume the memexport
   fix alone renders the world.
4. Apply the Phase 1 counter fix to the D: tree so it lands in this
   repo's source.
5. Review the translator early-return removal before it ships.
