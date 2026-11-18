# D3D11 backend build-quality audit — Forza Horizon 1 on Pinyon

**Date:** 2026-11-17
**Scope:** the whole D3D11 backend in the actual working tree on the PC
(`E:\PinyonShift\work\.local\rexglue`), not commit 602dab0.
**Method:** read-only source review. No build, no game run, no file modified on
the PC. Source fetched read-only to `~/workspace/dx11-backend-audit-scratch/src/`
(90 live files, 54,809 lines). Fetched copies hash-verified against the PC
(`command_processor.cpp` SHA256 `9d53077c…`, `fh1_native_executor.cpp`
`47b792f0…` — identical at fetch time; the Draw #23 footprint-vs probe was in
flight on the PC during this audit, so the live `command_processor.cpp`
already contains it).
**Audience:** Claude, the reviewer. This report is the audit; no patch and no
code changes are included.

## 0. Headline verdict

This is a well-engineered backend with unusually good instrumentation and
generally careful error handling — and three real problems Claude should look
at first:

1. **`WaitForGpu()` has a broken contract** (`src/graphics/d3d11/command_processor.cpp:6363-6381`):
   it returns `true` even when the GPU query fails (device removed), it spins
   forever with no timeout, and 9 of its 12 call sites ignore the return
   value. This is the single most consequential correctness risk in the
   backend.
2. **`IssueDraw` is a ~2,140-line single function** (`command_processor.cpp:1735-3874`)
   and `IssueCopy` is ~1,860 lines (`:4406-6265`). The entire per-draw
   pipeline lives in one function, on the single CPU decoder thread that is
   already the known bottleneck.
3. **Diagnostic accumulation is large and partly hardcoded:** 51 distinct
   `fh1_debug_*` cvars, several default-on traces, and absolute paths baked
   into the backend (`E:/PinyonShift/work/.local/...`). The probes themselves
   are cleanly written and properly gated — the risk is the size of the
   debug surface and the hardcoded paths, not the probe code.

The local changes (per-band bitmask fix + all 2026-11-16 diagnostic probes)
are clean: purely additive (0 lines removed in every diff I checked), well
commented, gated, and budgeted. The per-band fix itself is narrow and
correct. Details in §5.

One prior claim is **not borne out** in this tree: "SwapChain::Present calls
WaitForGpu() without checking its failure return." The presenter contains
zero `WaitForGpu` references, and `Present`'s HRESULT **is** checked
(`src/ui/d3d11/d3d11_presenter.cpp:1190-1201`). The WaitForGpu problem is in
the command processor, not the presenter.

## 1. File inventory

90 live files, 54,809 lines. Sizes are lines / bytes from the fetched copies.

### D3D11 executor + command processor — `src/graphics/d3d11/` (17 .cpp)

| File | Lines | Bytes | Responsibility |
|---|---|---|---|
| `command_processor.cpp` | 7,023 | 360,742 | Draw/resolve/copy/swap path, all probes |
| `fh1_native_executor.cpp` | 2,886 | 135,382 | EDRAM surfaces, untile, copy-down, transfers, clears |
| `texture_cache.cpp` | 1,358 | 67,634 | CPU texture load, untiling, SRV cache |
| `pipeline_cache.cpp` | 901 | 41,353 | Shader/pipeline cache, precompiled pack |
| `command_list_immediate_sink.cpp` | 531 | 23,898 | Tape replay sink |
| `fh1_native_resolve.cpp` | 467 | 22,282 | EDRAM resolve compute shaders |
| `shared_memory_upload_device.cpp` | 563 | 21,545 | GPU mirror uploads / readbacks |
| `fh1_native_surfaces.cpp` | 252 | 10,968 | EDRAM surface table |
| `primitive_processor.cpp` | 184 | 7,188 | Index/vertex processing |
| `command_list.cpp` | 110 | 3,197 | Command tape |
| `host_render_config.cpp` | 214 | 7,986 | Host render config (scale, formats) |
| `resource_state_tracker.cpp` | 113 | 3,437 | Resource state tracking |
| `render_target_cache.cpp` | 59 | 1,720 | Render-target cache |
| `shader.cpp` | 111 | 4,268 | D3D11 shader stage creation |
| `shared_memory.cpp` | 84 | 2,739 | Shared-memory wrapper |
| `graphics_system.cpp` | 40 | 1,288 | D3D11 graphics system glue |
| `submission_queue.cpp` | 22 | 707 | Submission/fence bookkeeping (**stub**, §3.12) |

Headers in `include/rex/graphics/d3d11/`: `command_processor.h` (407 lines),
`fh1_native_executor.h` (350), `command_list.h` (342), `pipeline_cache.h`
(289), `fh1_native_surfaces.h` (153), `fh1_native_resolve.h` (132),
`texture_cache.h` (199), `shared_memory_upload_device.h` (143),
`primitive_processor.h` (103), `shader.h` (75), `host_render_config.h` (86),
`resource_state_tracker.h` (82), `render_target_cache.h` (56),
`shared_memory.h` (51), `submission_queue.h` (41), `graphics_system.h` (48),
`shared_memory_upload.h` (54).

### Presenter + provider — `src/ui/d3d11/` (3 .cpp)

| File | Lines | Bytes | Responsibility |
|---|---|---|---|
| `d3d11_presenter.cpp` | 1,490 | 67,357 | Guest-output blit, Present, frame dumps |
| `d3d11_provider.cpp` | 379 | 14,768 | Device/adapter creation, present device |
| `d3d11_immediate_drawer.cpp` | 72 | 2,700 | Immediate-mode drawer |

Headers in `include/rex/ui/d3d11/`: `d3d11_presenter.h` (229),
`d3d11_provider.h` (85), `d3d11_immediate_drawer.h` (57), `d3d11_api.h` (32).

### Shared pipeline, D3D11-relevant — `src/graphics/` + `src/graphics/pipeline/shader/`

| File | Lines | Bytes | Responsibility |
|---|---|---|---|
| `command_processor.cpp` (shared) | 4,085 | 171,313 | Packet decode, GPU thread, record split |
| `dxbc_translator.cpp` | 3,501 | 164,073 | Guest shader → DXBC translation |
| `dxbc_translator_om.cpp` | 3,089 | 155,256 | Output-merger translation |
| `dxbc_translator_fetch.cpp` | 2,018 | 100,266 | Texture-fetch translation |
| `primitive_processor.cpp` (shared) | 1,594 | 81,118 | Primitive processing |
| `translator.cpp` | 1,412 | 59,874 | Shader translator core |
| `interpreter.cpp` | 1,135 | 48,408 | Shader interpreter |
| `dxbc_geometry_shaders.cpp` | 1,099 | 57,049 | Geometry-shader generation |
| `dxbc_translator_alu.cpp` | 965 | 45,980 | ALU translation |
| `dxbc_translator_memexport.cpp` | 785 | 31,939 | Memexport translation |
| `translator_disasm.cpp` | 507 | 15,024 | Ucode disassembly |
| `shader.cpp` (pipeline) | 251 | 9,666 | Shader analysis |
| `dxbc.cpp` | 52 | — | DXBC container helpers |
| `flags.cpp` | 450 | 33,452 | All GPU cvars (83 `REXCVAR_DEFINE`) |
| `d3d_list_compiler.cpp` | 907 | 36,686 | D3D list compiler |
| `d3d_shader_cache.cpp` | 263 | 10,559 | Shader cache (shared) |
| `d3d_draw_path.cpp` | 554 | 20,224 | D3D draw path (shared) |
| `d3d_census_decoder.cpp` | 258 | 7,967 | Census decoder |
| `fh1_edram_resolve.cpp` | 187 | 7,946 | Shared resolve plan (`Fh1PlanResolve`) |
| `fh1_frame_dump.cpp` | 326 | 12,715 | Frame dumping |
| `fh1_depth_overwrite.cpp` | 189 | 8,442 | Depth overwrite |
| `graphics_system.cpp` (shared) | 566 | 22,652 | GPU thread, vsync worker, presenter setup |
| `shared_memory.cpp` (shared) | 903 | 37,633 | Guest shared memory |
| `registers.cpp` / `register_file.cpp` | 32 / 59 | 1,062 / 1,816 | Register file |
| `xenos.cpp` | 214 | 8,117 | Xenos helpers |
| `cube_lut.cpp` | 105 | 3,588 | Cube LUT |

Headers in `include/rex/graphics/`: `xenos.h` (1,648 lines), `registers.h`
(965), `command_processor.h` (765), `shared_memory.h` (315),
`fh1_edram_tiles.h` (223), `primitive_processor.h` (836),
`render_audit.h` (837), `render_forensics.h` (303), `flags.h` (176),
`fh1_edram_resolve.h` (77), `fh1_edram_surfaces.h` (87),
`fh1_executor_counters.h` (41), `fh1_frame_dump.h` (107),
`fh1_depth_overwrite.h` (76), `graphics_system.h` (160),
`register_file.h` (91), `pipeline_util.h`, `video_mode_util.h`,
`render_audit.h`, `render_forensics.h`. Shader headers in
`include/rex/graphics/pipeline/shader/`: `shader.h` (1,111),
`translator.h` (154), `interpreter.h` (135), `dxbc_translator.h` (1,174),
`dxbc.h` (88), `dxbc_geometry_shaders.h` (60).

### Layering

The layering is sound: shared core (packet decode, resolve plan, shader
analysis, EDRAM model in `fh1_edram_resolve.cpp` / `fh1_edram_tiles.h`) +
D3D11-specific executors (draw, resolve dispatch, surfaces, presenter).
The D3D11 backend mirrors the D3D12 executor's resolve constant packing
(`fh1_native_resolve.cpp` header comment) — intentional parity, not
duplication. One layering gap: the `gpu_backend` cvar is **not defined
anywhere in this tree** (not in `flags.cpp`); backend selection lives in the
app layer outside rexglue, so the backend's own flags file does not show the
override behaviour that caused the Vulkan/D3D11 mixup on 2026-11-16.

## 2. Findings ranked by severity

### HIGH

#### 2.1 `WaitForGpu()` returns true on device failure, spins unbounded, and most callers ignore it

`src/graphics/d3d11/command_processor.cpp:6363-6381`:

```cpp
bool D3D11CommandProcessor::WaitForGpu() {
  ...
  context->End(gpu_wait_query_.Get());
  // Spin rather than Map, which only covers a subresource, not the queue.
  while (context->GetData(gpu_wait_query_.Get(), nullptr, 0, 0) == S_FALSE) {
    std::this_thread::yield();
  }
  return true;
}
```

Three defects in one function:

- **Wrong return on failure (line 6378):** the loop exits on any `GetData`
  result other than `S_FALSE` — including `DXGI_ERROR_DEVICE_REMOVED`,
  `DXGI_ERROR_DEVICE_RESET`, and other failure HRESULTs — and then returns
  `true`. A caller that checks the return is told the GPU finished when the
  device is actually gone.
- **No timeout:** if the query never completes (hung GPU), this spins
  forever on the calling thread. There is no deadline, no device-removed
  check inside the loop.
- **Most callers ignore the return:** of 12 call sites, only 3 use the
  result — `:4902` (`if (!WaitForGpu())`), `:6308` (`if (!WaitForGpu())`),
  `:5844` (assigned to `trace_gpu_completed`). The other 9 discard it:
  `:2433`, `:5238`, `:5676`, `:5757`, `:5776`, `:5962`, `:6482`, `:6740`,
  `:6822`.

Contrast with the presenter's bounded poll
(`src/ui/d3d11/d3d11_presenter.cpp:263-276`): 250 spins × 2 ms sleep
(500 ms cap), `poll != S_FALSE` recorded as failure status 3, and the
caller refuses to proceed (`:277-281`, `copy_not_complete`). That is the
pattern `WaitForGpu` should follow. Recommended shape: capture the `GetData`
HRESULT, return `false` on any failure, add a deadline, and check the return
at every call site.

Checked and **not** found: the presenter contains zero `WaitForGpu`
references, and the only `Present` call site
(`d3d11_presenter.cpp:1190`) checks its HRESULT — device removed/reset →
`PaintResult::kGpuLostExternally` (`:1191-1196`), other failures →
`kNotPresented` (`:1197-1201`). The prior "Present calls WaitForGpu
unchecked" claim does not match this tree.

#### 2.2 `IssueDraw` is a ~2,140-line single function; `IssueCopy` is ~1,860 lines

- `IssueDraw`: `src/graphics/d3d11/command_processor.cpp:1735-3874`
  (2,140 lines). It contains the entire per-draw pipeline: tape submission,
  band computation, shader analysis and translation, fixed-function state,
  system constants, vertex-buffer residency, bindings, the c2 probe,
  composite/texture/guest/vertex dumps, the draw call, and
  `NativeDrawIssued` bookkeeping.
- `IssueCopy`: `command_processor.cpp:4406-6265` (1,860 lines).
- `IssueDraw`'s own parameters are dead: `:1738-1740` explicitly voids
  `primitive_type`, `index_count`, and `major_mode_explicit` — the function
  reads member state instead. The signature promises inputs it ignores.

Why this matters beyond style: the known performance bottleneck is the
single CPU decoder thread, and this function *is* the per-draw work on that
thread. A 2,140-line function with ~15 early-return paths, nested probe
blocks, and interleaved diagnostic code is where a future regression will
hide, and where the per-draw cost is hardest to profile or split. The
`DrawRecordScope` RAII guard (`:3367-3390`, restores `recording_draw_` and
flushes deferred constant buffers on every return path) is good, but it is
papering over the fact that the function has that many return paths.

#### 2.3 Per-draw heap allocations in `UpdateBindings`

`src/graphics/d3d11/command_processor.cpp:1700` and `:1712`, inside
`UpdateBindings` (called once per draw, `:3385`):

```cpp
std::vector<ID3D11ShaderResourceView*> srvs(texture_bindings.size());
...
std::vector<ID3D11SamplerState*> samplers(sampler_bindings.size());
```

Two heap allocations per shader stage (up to four per draw), plus per
sampler binding: a `D3D11_SAMPLER_DESC` copy, an `XXH3_64bits` hash, and a
linear scan of `sampler_cache_` in `GetSamplerState` (`:1589-1601`).
`AnalyzeShaderUcode` at least early-outs when already analysed
(`src/graphics/d3d11/pipeline_cache.cpp:171-177`), so translation is not
redone per draw — but the binding vectors are. On the CPU-bound decoder
thread this is the cheapest per-draw win available: a small stack array or
a reused scratch buffer. Severity is moderate (the vectors are small), but
it fires on every draw of every frame.

### MEDIUM

#### 2.4 Diagnostic surface: 51 `fh1_debug_*` cvars, default-on traces, hardcoded paths

`src/graphics/flags.cpp` defines **51 distinct `fh1_debug_*` cvars**
(74 mentions). Most default off, but three traces are default-on at
specific swaps: `fh1_debug_tile_trace_swap` defaults to 338 (`flags.cpp:23`),
`fh1_debug_minimap_capture_swap` defaults to 338 (`:37`), and
`fh1_debug_surface_dump_swap` defaults to 300 (`:253`). Every run therefore
emits tile-trace logging at swap 338 unless the user overrides it.

Absolute paths are baked into the backend (all diagnostic-only, but not
configurable):

- `src/graphics/d3d11/command_processor.cpp:1181` — `draw-trace.log`
  (`E:/PinyonShift/work/.local/draw-trace.log`)
- `command_processor.cpp:4966,4973` — `resolve-trace.log`
- `src/ui/d3d11/d3d11_presenter.cpp:1156,1176` — `frames/frame_%06u.bmp`,
  `frames/guest_%06u.bmp` (gated on `pinyon_dump_frames`, `:1145`)
- `include/rex/graphics/render_audit.h:206,430` — `audit/<backend>-<utc>.jsonl`,
  `audit/<backend>-passes-<utc>.jsonl`; `:362-374` — `transition.log`
  (with rotate-to-`.1` logic in a header)
- `include/rex/graphics/render_forensics.h:146` — `audit/d3d11-forensics-<stamp>.jsonl`
- `src/graphics/d3d11/command_processor.cpp:6390` — the audit context
  hardcodes the adapter string `"GTX 570"`:
  `render_audit::SetContext("d3d11", "GTX 570", 1280, 720)`

None of these break anything while the paths exist, but the backend cannot
be run from another checkout or by another user without editing source, and
the audit/forensics headers contain filesystem side effects (file creation,
rotation) inline in header code.

#### 2.5 `fh1_resolve_guest_refresh`: default contradicts its description

`src/graphics/flags.cpp:394`:

```cpp
REXCVAR_DEFINE_BOOL(fh1_resolve_guest_refresh, true, "GPU",
                    "EXPERIMENTAL, off by default: before a texture upload reads ...")
```

Defined `true`, described as "off by default". One of the two is wrong;
given the description calls it EXPERIMENTAL and the flag changes texture
upload behaviour (landing GPU-written ranges into guest memory before a
load), this is worth resolving explicitly rather than leaving ambiguous.

#### 2.6 `-Wno-unused-variable` on the whole `rexgpu-fh1` target

`src/graphics/CMakeLists.txt:218`:

```cmake
target_compile_options(${rexgpu_fh1_target} PRIVATE -Wno-unused-variable)
```

The global flags are `-Wall -Wextra` (`rexglue/CMakeLists.txt:274-278`),
which is good — but this target-local suppression turns off a warning class
that catches real mistakes (a computed-but-unused value that should have
been used, a dead store). It is applied to all three `rexgpu-fh1*` targets.

#### 2.7 `REX_HAS_FIDELITYFX_SDK=1` is an ABI-sensitive define set in the plugin build

`src/graphics/CMakeLists.txt:221-224`:

```cmake
# presenter.h's class layout depends on REX_HAS_FIDELITYFX_SDK, which rexui
# gives its users; the plugin does not link rexui, so it must set it too
# to see the Presenter the runtime built.
target_compile_definitions(${rexgpu_fh1_target} PRIVATE REX_HAS_FIDELITYFX_SDK=1)
```

The comment acknowledges the hazard: `Presenter`'s class layout depends on
this macro, and the plugin must define it identically to the runtime or the
two sides disagree on the object's layout (ODR violation, silent memory
corruption). It is correct as written, but it is a landmine — a single
divergent define breaks the presenter ABI with no diagnostic.

#### 2.8 Raw owning pointers in the pipeline cache + manual `delete` in `Shutdown`

`src/graphics/d3d11/pipeline_cache.cpp:135,141`:

```cpp
void D3D11PipelineCache::Shutdown() {
  for (auto& it : pipelines_) {
    delete it.second;
  }
  pipelines_.clear();
  ...
  for (auto& it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
```

`pipelines_` and `shaders_` hold raw owning pointers. This is safe only if
`Shutdown` runs after the GPU thread has stopped and nothing else holds a
pipeline pointer. The same file has the graphics-side backend's lock,
`translation_request_lock_` (`:259`, guarding translation requests), so the
authors are aware of the threading — but the ownership is still manual where
`unique_ptr` would make it structural. (The presenter has its own mailbox
lock, `d3d11_presenter.cpp:895,1213`.) The provider has the same pattern:
raw COM members with manual `Release()` in the destructor
(`src/ui/d3d11/d3d11_provider.cpp:100-135`) — correct because members are
`nullptr`-initialised (`d3d11_provider.h:65-68`), but fragile.

#### 2.9 Provider: uninitialised member + unsynchronised lazy present-device creation

- `include/rex/ui/d3d11/d3d11_provider.h:82`: `GpuVendorID adapter_vendor_id_;`
  has no default initialiser. It is set in `Initialize()`
  (`d3d11_provider.cpp`), but `GetAdapterVendorID()` can read it before
  initialisation if called on a failed provider.
- `GetOrCreatePresentDevice(HWND)` (`d3d11_provider.cpp:130-...`) is a
  `const` method that lazily creates the present device by writing `mutable`
  members (`d3d11_provider.h:76-79`) with no lock. If two threads call it
  concurrently, both can create a present device and one leaks (the second
  `Detach()` overwrites the first). In practice it is called from the UI
  thread only — but nothing enforces that.

#### 2.10 Single-argument asserts vanish in Release builds

`include/rex/assert.h` (fetched for this check):

```cpp
#ifdef NDEBUG
#define rex_assert(expr) ((void)(false && (expr)))
#else
#define rex_assert(expr) assert(expr)
#endif
```

`assert_true(expr)` / `assert_not_zero(expr)` (one argument) expand to
`rex_assert` and are compiled out under `NDEBUG`. `dxbc_translator.cpp`
uses these 61 times (e.g. `:205`, `assert_true(count <= system_temp_count_current_)`).
The two-argument forms (`assert_true(expr, message)`) still abort via
`rex_assert_fail`. So in a Release build, most translator invariants are
unchecked. This is inherited Xenia behaviour, not a bug per se — but Claude
should know that the translator's internal checks are debug-only, and that
a translation failure in Release surfaces only through `is_valid`
(checked at `dxbc_translator.cpp:1243` and `command_processor.cpp:2041`).

### LOW

#### 2.11 Per-band fix loop: safe, but only because of guards elsewhere

`src/graphics/d3d11/fh1_native_executor.cpp:2745`:

```cpp
for (uint32_t bit = 0; bands >> bit; ++bit) {
```

If `bands` ever had bit 31 set, `bit` would reach 32 and `bands >> 32` is
undefined behaviour; likewise `UntileBandBit` (`:138`,
`1u << (band / kUntileBandRows)`) shifts by up to 31 only if `band < 8192`.
Both recording sites guard this: `ResolveClear` returns early when
`untile_band >= config_.untile_height` (`:2486-2490`), `NativeDrawIssued`
does the same (`:2690-2695`), and `IssueDraw` drops over-height bands
(`command_processor.cpp:1756-1761`). With `untile_height` = 720 the
maximum bit is 2. **Verified safe in practice** — noted only so a future
change to the band bound does not silently introduce UB. A cheap hardening
would be to mask the shift (`1u << (band / kUntileBandRows & 31)` is wrong;
better: assert or clamp the band at the recording sites).

#### 2.12 `swap_post_effect_desired_` is written from any thread without synchronisation

`include/rex/graphics/command_processor.h:545-546` — plain enum members.
`SetDesiredSwapPostEffect` (`src/graphics/command_processor.cpp:410-416`)
writes `swap_post_effect_desired_` from the calling thread (the cvar change
callback fires from whatever thread changes the cvar,
`src/graphics/graphics_system.cpp:201`) and reads it via
`GetDesiredSwapPostEffect()`. The *actual* value is correctly marshalled
onto the GPU thread with `CallInThread` (`:415`). The `desired_` write is
benign in practice (single-writer, idempotent enum store) but is technically
a data race; an `std::atomic` would make it structural.

#### 2.13 Unsupported texture formats fail silently after the first log

`src/graphics/d3d11/texture_cache.cpp:447-451`: `CreateTexture` returns
`nullptr` when the host format has no DXGI format or no converter. The
rejection is logged once per format (`LogUnsupportedFormatOnce`, `:436-443`,
bitmask `unsupported_format_mask_logged_`) and the rejected-load log is
capped at 60 (`:592-596`). Correct behaviour — but after the first log, a
texture that samples nothing (unbound SRV samples `(0,0,0,1)` in D3D11)
fails silently at runtime. This is the known flat-world failure mode shape:
the mechanism is fine, the observability after the first occurrence is not.

#### 2.14 `D3D11SubmissionQueue` is a stub whose `Reset()` promises a wait it does not perform

`src/graphics/d3d11/submission_queue.cpp` is 22 lines; `Reset()` is an
empty body. The header (`include/rex/graphics/d3d11/submission_queue.h`)
says "Drops queued bookkeeping and waits for any outstanding submitted
work" and "Phase 3 M0 reserves the type and its context; M1 adds the replay,
query and deferred-release machinery." It is only constructed and reset
(`command_processor.cpp:737,795`), never used for real submission tracking
— a known M0 placeholder, not a live bug. The trap is the comment: if
anyone later calls `Reset()` expecting the documented wait, they get none.

#### 2.15 `dxbc_translator.cpp` has no HRESULT checks and no `return false`

The 3,501-line translator contains zero `FAILED` checks and zero
`return false`. That is by design — it emits DXBC bytecode in memory and
never calls D3D — and failure is signalled only through `is_valid`
(checked in `PostTranslation`, `:1243`, and by the command processor,
`:2041`). Not a bug, but the failure surface is narrow: a translation
problem is visible only as `is_valid() == false`, with no reason attached
at the check sites.

#### 2.16 TODO/FIXME load: 78 markers, mostly inherited upstream

`grep -c "TODO|FIXME"` across the fetched tree: 78. Most are inherited
Xenia attributions (`TODO(benvanik)`, `TODO(Triang3l)`), e.g.
`src/graphics/command_processor.cpp:3426` — "Don't drop the draw call
completely if the vertex [shader is missing]" — which is a live behaviour
note for the D3D11 path. Worth a triage pass to separate upstream
aspirations from D3D11-specific gaps; none of the ones I sampled is a
currently-firing defect.

#### 2.17 Probe-specific notes (all low)

- The copydown probe's `CountNonzeroTexelsResolved`
  (`fh1_native_executor.cpp:527-602`) does a synchronous GPU readback
  (`ResolveSubresource` + staging copy + `Map`) inside `EndUntiledTiling`.
  It is capped at 24 readbacks per process (`:2710`) and gated on
  `fh1_debug_tile_trace_swap`, so it is bounded — but it is a full GPU
  stall in a hot path while active.
- The footprint-vs probe hardcodes swap 1500 and the #23 PS hash
  `0xA39E23E2FD1F8A8B` (`command_processor.cpp:2732,3104,3340`). Fine for a
  diagnostic; the values will go stale if the target frame changes.
- Probe budget counters are function-static (`c2_probe_draws`,
  `command_processor.cpp:3412`; `copydown_probe_readbacks`,
  `fh1_native_executor.cpp:2710`; `guest_probe_swaps`, `:2431`) — process
  lifetime, single-threaded use assumed. A second run in the same process
  does not re-probe.
- The c2 probe reads the surface twice for a stability control
  (`command_processor.cpp:3409-...`) — good methodology, and the comment
  explains why (Phase 10's pairs were indistinguishable from unstable
  readbacks).

## 3. What is good and well-built

- **Error handling is careful in most places.** `HRESULT` checks on every
  `Create*` call I sampled (provider, resolve, executor shader creation at
  `fh1_native_executor.cpp:1740,1771,1827,1857,1969,2119,2199,2207,2215,2297`);
  the resolve's `Skip(reason)` pattern with named reasons
  (`fh1_native_resolve.cpp:130-136`); pipeline creation `nullptr` checks
  (`pipeline_cache.cpp:845-860`); the precompiled shader pack is offered to
  the driver before adoption so a stale pack degrades to runtime translation
  instead of failing every draw (`pipeline_cache.cpp:294-321`, with a
  logged refusal count).
- **The OM-unbind fix is correct and well explained.** D3D11 silently drops
  an SRV bind when the resource is still bound as a render target; the
  resolve unbinds the output merger before binding its source
  (`fh1_native_resolve.cpp`, `ResolveToMemory`), with a comment describing
  the exact defect it fixed (first resolve read back NULL, source had
  content, destination got nothing). The same hazard is handled in
  `TransferRects` (`fh1_native_executor.cpp`) and for the shared-memory
  mirror (`command_processor.cpp:4622-4633`). This is the kind of D3D11
  subtlety that is easy to get wrong, and it is handled deliberately.
- **Empty-rect guard chain is intact.** `Fh1PlanResolve` returns false on
  empty rectangles (`src/graphics/fh1_edram_resolve.cpp:65,96`), `plan.copy`
  is set only on the success path (`:183`), the D3D11 caller skips when
  `!plan.copy`, and `TransferRects` validates every rectangle
  (`fh1_native_executor.cpp:1058-1079`, rejecting empty/out-of-bounds and
  counting skips). A zero-group `Dispatch` cannot be reached from these
  paths.
- **Shutdown ordering is deliberate and commented**
  (`command_processor.cpp:790-840`): tape/sink first (packets hold COM
  references), then resolve, executor, surfaces, pipeline cache, and so on.
- **RAII where it counts.** `DrawRecordScope` (`command_processor.cpp:3367-3390`)
  restores `recording_draw_` and flushes deferred constant buffers on every
  return path; `ScopedD3D11ClearState` saves/restores OM state; `ComPtr`
  throughout the resolve and executor; `DrawCounters` for the gate harness.
- **Provider.** Adapter selection by description substring with the full
  adapter list logged, feature level pinned to exactly 11_0 with a check
  (`d3d11_provider.cpp`), present-device search by monitor with a documented
  fallback chain.
- **Presenter.** `Present` HRESULT checked (`:1190-1201`), bounded query
  poll with an explicit failure status (`:263-281`), frame dump gated on
  `pinyon_dump_frames` (`:1145`).
- **Threading model is explicit.** One GPU/decode thread owns the command
  processor (`src/graphics/graphics_system.cpp:190`, "spin up a thread to
  process all incoming ringbuffer packets"); presenter creation is marshalled
  to the UI thread (`:148`); `CallInThread` marshals cross-thread state
  changes (`command_processor.cpp:415`); the pipeline cache has the
  graphics-side lock (`translation_request_lock_`,
  `pipeline_cache.cpp:259`); the presenter mailbox has its own lock
  (`d3d11_presenter.cpp:895,1213`). The
  D3D11 code is largely lock-free *because* it is single-threaded by
  construction — that is a defensible design, and it is the reason the
  decoder-thread bottleneck is a single-thread problem, not a locking one.
- **Build.** Explicit source lists in `src/graphics/CMakeLists.txt:127-143`
  (no glob), `-Wall -Wextra` globally (`rexglue/CMakeLists.txt:274-278`),
  PCH configured for the target.

## 4. The local changes (per-band fix + diagnostic probes)

All verified by diff against the `.musebak-*` backups on the PC.

### Per-band bitmask fix — clean, narrow, correct

`src/graphics/d3d11/fh1_native_executor.cpp`:

- Helpers `:125-142` — `kUntileBandRows = 256`, `UntileBandBit`,
  `UntileBandFromBit` — with a comment explaining the old `max(band)`
  failure (the black middle) and why bit 0 needs no copy-down.
- Recording sites `:2678` (`ResolveClear`) and `:2699` (`NativeDrawIssued`)
  — `untile_bands_[key.Pack()] |= UntileBandBit(untile_band)` replaces
  `band = std::max(band, untile_band)`.
- Drain loop `:2745-...` — one copy-down per set bit, ascending, with the
  same bounds checks and error handling as the old code (error path changed
  from `continue` to `surface_done = false; break;` — the surface is still
  left out of `completed`, so it is retried next flush, same as before).

Both recording sites are guarded against out-of-range bands
(`ResolveClear :2486-2490`, `NativeDrawIssued :2690-2695`), and `IssueDraw`
drops over-height bands (`command_processor.cpp:1756-1761`), so the shift
operations in the fix cannot overflow in practice (§2.11). No header
change. The Vulkan executor's identical pattern was left alone, as intended.

### Diagnostic probes — cleanly additive, gated, budgeted

- `fh1_native_executor.cpp`: 2,731 lines (per-band backup) → 2,886 live
  (+155). The copydown probe (`CountNonzeroTexelsResolved`, `:527-602`,
  plus the three `probe_stage` call sites) is diagnostic-only: readbacks +
  log lines, a 24-readback process budget (`:2710`), gated on
  `fh1_debug_tile_trace_swap`. It never changes a copy-down's outcome.
- `command_processor.cpp`: 6,675 lines (routing-probe-v2 backup) → 7,023
  live (+348, **0 lines removed**). The 348 lines are the routing drop-log
  gating (+43) and the footprint-vs probe (+305). Earlier probe layers
  (follow-up, next-evidence, routing v1/v2) are all in the v2 backup.
- `flags.cpp` / `flags.h`: probe cvars added with descriptions; the
  `fh1_debug_*` family is now 51 cvars (§2.4).
- Every probe I checked is gated (cvar non-empty, swap match, hash match,
  or budget) and logs only, except the copydown/c2/guest probes which do
  synchronous GPU readbacks while active (§2.17). No probe changes
  rendering behaviour when its cvar is off. Backups exist for every change
  (`*.musebak-*-20261116` in the source dirs).

One caution for the future: the probe layers accumulate. The command
processor has grown by ~350 lines of probe code on top of the routing
probes, and the footprint probe hardcodes swap 1500 and a PS hash. When the
render bug is fixed, these should be removed or put behind a single
`fh1_diagnostics` gate rather than left as 51 separate cvars with hardcoded
targets.

## 5. What Claude should look at first

1. **`WaitForGpu` + its callers** — `command_processor.cpp:6363-6381`,
   call sites `:2433,4902,5238,5676,5757,5776,5844,5962,6308,6482,6740,6822`.
   Fix the return contract first (return `false` on any non-`S_OK` `GetData`
   result), add a deadline, then check the return at the 9 unchecked sites.
   Compare with the presenter's bounded poll (`d3d11_presenter.cpp:263-281`).
2. **`IssueDraw` (`:1735-3874`)** — read it in sections (shader analysis
   ~:1814-2070, bindings ~:1605-1733, probes ~:2124-3400, draw + bookkeeping
   ~:3860-3874). The question is whether it can be split without changing
   behaviour; at minimum the dead parameters (`:1738-1740`) should go.
3. **`UpdateBindings` allocations** (`:1700,1712`) and `GetSamplerState`
   (`:1589-1601`) — the cheapest per-draw win on the bottleneck thread.
4. **The debug surface** — `flags.cpp` (51 `fh1_debug_*` cvars), the
   hardcoded paths (§2.4), and the `fh1_resolve_guest_refresh` default /
   description mismatch (`flags.cpp:394`).
5. **The local changes** — already verified clean here; a skim of the
   per-band diff (`fh1_native_executor.cpp:125-142,2678,2699,2745`) and the
   probe diffs should be enough to confirm.
6. **Provider + pipeline-cache lifetime** — raw COM members
   (`d3d11_provider.cpp:100-135`), raw `delete` in `Shutdown`
   (`pipeline_cache.cpp:135,141`), the uninitialised
   `adapter_vendor_id_` (`d3d11_provider.h:82`), and the unsynchronised
   lazy present-device creation (`d3d11_provider.cpp:130+`).

## 6. Method and coverage

- **Read fully:** `d3d11_provider.cpp`, `fh1_native_resolve.cpp`,
  `submission_queue.cpp` (+ header), `rex/assert.h`, the CMakeLists
  excerpts, `flags.cpp` (cvar inventory), the per-band fix and all probe
  diffs.
- **Read in the sections that matter, sampled elsewhere:**
  `command_processor.cpp` (7,023 lines — draw path, resolve path, probes,
  `WaitForGpu`, shutdown, tape; the remainder sampled by targeted greps),
  `fh1_native_executor.cpp` (2,886 — per-band fix, copy-down,
  `TransferRects`, `ResolveClear`, `NativeDrawIssued`),
  `dxbc_translator.cpp` (3,501 — error handling, asserts, `is_valid`),
  `pipeline_cache.cpp`, `texture_cache.cpp`,
  `d3d11_presenter.cpp` (1,490 — Present path, query poll, frame dump),
  `graphics_system.cpp` (threading), `fh1_edram_resolve.cpp` (plan guards).
- **Not read in full:** `dxbc_translator_alu.cpp`,
  `dxbc_translator_fetch.cpp`, `dxbc_translator_memexport.cpp`,
  `dxbc_translator_om.cpp`, `dxbc_geometry_shaders.cpp`,
  `primitive_processor.cpp` (both), `shared_memory_upload_device.cpp`,
  `command_list.cpp`, `command_list_immediate_sink.cpp`, `d3d_draw_path.cpp`,
  `d3d_list_compiler.cpp`, `d3d_shader_cache.cpp`, `d3d_census_decoder.cpp`,
  `fh1_frame_dump.cpp`, `fh1_depth_overwrite.cpp`, `interpreter.cpp`,
  `translator.cpp`, `translator_disasm.cpp`, `shader.cpp` (both),
  `xenos.cpp`, `registers.cpp`, `register_file.cpp`, `cube_lut.cpp`,
  `shared_memory.cpp` (both), `host_render_config.cpp`,
  `resource_state_tracker.cpp`, `render_target_cache.cpp`,
  `d3d11_immediate_drawer.cpp`. Findings in those files come only from
  targeted greps (TODO counts, `WaitForGpu`/`Present`/`new`/`delete`
  sweeps, allocation greps). If Claude wants a second pass, those are the
  files to read.
- **Confirmed by diff:** per-band fix vs
  `fh1_native_executor.cpp.musebak-perband-20261116`; copydown probe (same
  diff, +155 lines total); routing probes vs
  `command_processor.cpp.musebak-routing-probe-v2-20261116` (+348 lines, 0
  removed); footprint-vs probe vs
  `command_processor.cpp.musebak-footprint-vs-20261116` (+305 lines, 0
  removed).
- **Checked and not found:** `WaitForGpu` in the presenter (0 references);
  an unchecked `Present` (its HRESULT is checked); an empty-rect path to
  `Dispatch` (guard chain intact, §3); a null-deref in the footprint probe
  (`vertex_shader` is null-checked at `command_processor.cpp:1815` before
  the probe sites at `:2779,3104,3216`); a `gpu_backend` cvar in this tree
  (it lives outside rexglue).

## 7. Severity summary

| Severity | Count | Items |
|---|---|---|
| High | 3 | §2.1 WaitForGpu contract; §2.2 IssueDraw/IssueCopy size; §2.3 per-draw allocations |
| Medium | 7 | §2.4 debug surface + hardcoded paths; §2.5 cvar default/description mismatch; §2.6 `-Wno-unused-variable`; §2.7 `REX_HAS_FIDELITYFX_SDK` ABI define; §2.8 raw owning pointers + manual delete; §2.9 provider uninitialised member + unsynchronised lazy init; §2.10 debug-only asserts |
| Low | 7 | §2.11 band-shift UB (guarded); §2.12 `swap_post_effect_desired_` race; §2.13 silent texture-format failure after first log; §2.14 submission-queue stub; §2.15 translator failure surface; §2.16 TODO load; §2.17 probe notes |

No patch is proposed in this report. The most valuable next step is §2.1
(`WaitForGpu`), which is a small, self-contained fix with a clear
before/after test (device-removed behaviour and the 9 unchecked call sites).
