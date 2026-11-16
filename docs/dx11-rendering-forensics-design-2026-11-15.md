# D3D11 Rendering Forensics System — Design

**Project:** Pinyon (static recompilation of Forza Horizon 1, Xbox 360) — custom D3D11 backend
**Target:** NVIDIA GTX 570 (Fermi), Feature Level 11_0, driver 391.35
**Repo:** https://github.com/assrammerfr/Pinyon-FH1Reimagined-DX11-Backend (branch `main`)
**Date:** 2026-11-15
**Status:** DESIGN ONLY. No rendering behavior is changed by this document. No code is written here.

## 0. Purpose and scope

### 0.1 What this system must explain

For any final pixel or screen region, the system must be able to answer:

- Which guest asset/resource was used.
- Which draw call consumed it.
- Where geometry was placed.
- Which viewport and scissor were active.
- Which render target received the output.
- What previous contents were overwritten.
- What other geometry overlapped it.
- Which blend / depth / stencil states were active.
- Which resolve operation processed it.
- What final pixel values were presented.

### 0.2 Defects under investigation

The system exists to explain these concrete, observed defects — not hypothetical ones:

| # | Defect | Current state of knowledge (2026-11-15) |
|---|--------|------------------------------------------|
| 1 | Incorrect scene composition (wrong objects in wrong bands) | Band-overwrite bug FIXED by the untile stack (expanded surface, viewport/scissor shift, `untile_band << 16`, `EndUntiledTiling` copy-down). Composition defects that remain are separate. |
| 2 | Overlapping geometry / duplicated roads and mountains | Was the band-overwrite symptom. If it recurs, the forensics system must show which draw wrote which band rows. |
| 3 | Missing scenery (middle of the screen blank in free-roam) | Was the band-overwrite symptom (Band 2 content stomped). If it recurs, the system must show whether Band 2 drew at all and where its output went. |
| 4 | Black rectangles (big black block on menu → gameplay exit) | Leading theory: stale untile band data persists across the menu→gameplay transition when `EndUntiledTiling` does not run and/or no clear is issued. Unconfirmed. The system must record the transition: which surfaces were live, which `untile_bands_` entries existed, whether a copy-down ran, whether a clear ran. |
| 5 | Blue regions / blue tint that slowly fades as surfaces populate | Observed 2026-11-15 ~20:48 CST: colour returned gradually during gameplay. Suggests stale/wrong surface data correcting frame by frame, not (only) a static fetch-swizzle error. The system must show per-frame surface population (nonzero share per band) and the fetch swizzle state at each swap. |
| 6 | Incorrect texture placement | Composite passes sample frame-sized textures; a wrong base/pitch/tiling lands the texture in the wrong place. The system must record every sampled texture's guest base, size, format, and the draw's viewport/scissor. |
| 7 | Resolve corruption | Two `D3D11 resolve planning failed` errors appear in every run; call sites unidentified (Copilot Task 1 outstanding). The system must give every resolve-planning failure a stable call-site ID. |

### 0.3 Hard rules for this project

1. **Do not change rendering behavior.** No change to the normal renderer, band geometry, resolve behavior, or color formats as part of this instrumentation project.
2. **Diagnostics are isolated.** When every forensics cvar is off, the normal frame must be bit-identical to the frame without the system compiled in (modulo the hook call sites themselves, which must be no-ops when disabled).
3. **No unbounded logging.** Every log stream has a cap (record count, byte count, or frame window). A capture that would exceed its cap truncates and says so in its summary record.
4. **No per-draw GPU readbacks in ordinary operation.** Readbacks are allowed only inside an explicitly gated single-frame capture, and only for the surfaces the capture names.
5. **No large permanent allocations.** The recorder is a bounded ring buffer; its size is a cvar with a default that fits the 16 GB machine (see §7).
6. **GTX 570 / FL 11_0 only.** No feature above D3D11.0. No compute-shader feature beyond SM 5.0. No UAV counters that assume FL 11_1.
7. **Implement in isolated stages, with confirmation after each stage** (§8). A stage that fails verification does not block the stages that do not depend on it.

### 0.4 Source snapshot this design is grounded in

All file:line references are to the **canonical PC source** at `E:\PinyonShift\work\.local\rexglue\`, as of **2026-11-15 ~20:10 PM PC** (the build that produced the working untile binary), unless marked otherwise.

Path abbreviations used throughout:

| Abbrev | Path (relative to `E:\PinyonShift\work\.local\rexglue\`) |
|--------|----------------------------------------------------------|
| CP | `src/graphics/d3d11/command_processor.cpp` (6,030 lines) |
| CPH | `include/rex/graphics/d3d11/command_processor.h` |
| EX | `src/graphics/d3d11/fh1_native_executor.cpp` (2,708 lines) |
| EXH | `include/rex/graphics/d3d11/fh1_native_executor.h` |
| RS | `src/graphics/d3d11/fh1_native_resolve.cpp` (467 lines) |
| RSH | `include/rex/graphics/d3d11/fh1_native_resolve.h` |
| SF | `src/graphics/d3d11/fh1_native_surfaces.cpp` (243 lines) |
| SFH | `include/rex/graphics/d3d11/fh1_native_surfaces.h` (153 lines) |
| TC | `src/graphics/d3d11/texture_cache.cpp` (1,272 lines) |
| CL | `src/graphics/d3d11/command_list.cpp` (110 lines) |
| CLS | `src/graphics/d3d11/command_list_immediate_sink.cpp` (531 lines) |
| CLH | `include/rex/graphics/d3d11/command_list.h` (342 lines) |
| RST | `src/graphics/d3d11/resource_state_tracker.cpp` (113 lines) |
| RTC | `src/graphics/d3d11/render_target_cache.cpp` (59 lines) |
| SM | `src/graphics/d3d11/shared_memory.cpp` (84 lines) |
| SMU | `src/graphics/d3d11/shared_memory_upload_device.cpp` (563 lines) |
| PR | `src/ui/d3d11/d3d11_presenter.cpp` (1,490 lines) |
| GO | `src/ui/shaders/d3d11/guest_output.hlsl` (311 lines) |
| SCP | `src/graphics/command_processor.cpp` (shared core, 4,012 lines) |
| FER | `src/graphics/fh1_edram_resolve.cpp` (shared core, 187 lines) |
| RA | `include/rex/graphics/render_audit.h` (817 lines) |
| FL | `src/graphics/flags.cpp` / `include/rex/graphics/flags.h` |

### 0.5 Architecture facts the design relies on (verified in source)

- **Shared core + API-specific execution.** The command processor is split: the shared core (`SCP`) decodes guest PM4 packets (`ExecutePacket`, SCP:2161; Type 3 dispatch, SCP:2277; `XE_SWAP`, SCP:2490) and calls into the D3D11 backend (`D3D11CommandProcessor`, CP). The resolve plan is shared (`Fh1PlanResolve`, FER); the resolve execution is D3D11-specific (`IssueCopy`, CP:3614; `D3D11Fh1NativeResolve::ResolveToMemory`, RS:281).
- **The FH1 native executor owns the render targets.** `D3D11RenderTargetCache::CreateRenderTarget` returns `nullptr` — it is a skeleton with no consumers (RTC). All EDRAM surfaces live in `D3D11Fh1NativeSurfaces` (SF) and are driven by `D3D11Fh1NativeExecutor` (EX). There is no generic render-target cache to hook; the executor's surface registry is the render-target source of truth.
- **Three-band tiling with untile on D3D11.** The scene is drawn in three predicated bands (guest `PA_SC_WINDOW_OFFSET.window_y_offset` = 0, −256, −512 → bands 0/256/512). `kFh1D3D11UntileHeight = 720` (SFH:61). Surfaces are expanded to `Fh1D3D11SurfaceImageHeight(key, scale, untile_height)` = `min(max(period_height, untile_height), max_height)` (SFH:89-94), stored in `D3D11Fh1NativeSurface::image_height` (SFH:42; set at SF:132-138). Viewport and scissor are shifted by `-window_y_offset` at draw time in `UpdateFixedFunctionState` (CP:1133; viewport shift CP:1153-1160; scissor shift CP:1207-1221). The resolve passes `source_row_offset = untile_band` and ORs it into the sample select as `(untile_band << 16)` (CP:4138-4140). `EndUntiledTiling` copies bands back down at the next band-0 resolve (CP:3639-3641; EX:2589).
- **Untile band state is a map keyed by packed surface key.** `untile_bands_` (`std::map<uint32_t packed_key, uint32_t band>`) is updated in `NativeDrawIssued` (EX:2567; recorded at EX:2560-2584) and consumed/erased in `EndUntiledTiling` (EX:2589-2671). It is cleared in `Initialize`/`Shutdown` (EX:542, EX:594). **This map is the single most important state for defect #4 (black block on menu exit): if an entry survives a transition without a copy-down, or is erased without a copy-down, the surface holds stale band data.**
- **The resolve is a compute dispatch, not a hardware resolve.** `D3D11Fh1NativeResolve::ResolveToMemory` (RS:281) dispatches an SM 5.0 compute shader into the shared-memory mirror (a 512 MB raw UAV) or a staging buffer. The plan comes from `Fh1PlanResolve` (FER), which reads the guest registers and the resolve rectangle from a vertex fetch (`Fh1ResolveRectangle`, FER).
- **Presentation is a CPU readback + presenter blit.** At each swap the front buffer extent is read back from the GPU mirror into guest memory (`fh1_d3d11_resolve_readback`, CP `IssueSwap` CP:5395), then `D3D11Presenter::PaintAndPresentImpl` (PR:844) paints the swap texture through `guest_output.hlsl` (GO) and calls `swap_chain->Present` (PR:1190). There is no separate present-device path in the current build (the present-device code was reverted 2026-11-13).
- **Two execution paths exist: immediate and recorded (tape).** The immediate path issues D3D11 calls directly from `IssueDraw`/`IssueCopy`. The recorded path (`fh1_d3d11_command_lists`, off by default) records packets into `D3D11CommandList` (CL) and replays them through `ImmediateCommandSink::Execute` (CLS:38). **Every hook must cover both paths, or state explicitly which path it covers.** The tape packet types are the `D3D11Command` variant (CLH:230-231): barrier, UAV barrier, unbind RTs, unbind graphics SRV, dispatch, copy buffer, copy resource, copy subresource, upload buffer, upload texture, clear RT, clear depth-stencil, resolve subresource, draw.
- **A JSONL audit already exists and must be extended, not replaced.** `rex::graphics::render_audit` (RA, header-only, 817 lines) writes sampled per-frame records and a per-pass trace. See §1.3.

---

## 1. Existing instrumentation inventory

This section is the audit the implementer must not redo. Every item below was verified in the 2026-11-15 source. Where a cvar is defined in `FL` (flags.cpp) the definition line is given; where it is defined locally in `CP` that is noted.

### 1.1 Cvars — trace and diagnostic (all under the "GPU" cvar group)

**Frame / tile / band traces**

| Cvar | Default | Defined | What it captures | Gating / limits |
|------|---------|---------|------------------|-----------------|
| `fh1_debug_tile_trace_swap` | 338 | FL | Per-draw tile window/viewport/scissor state + 4x source-row hashes before resolves, on one presented frame | Single frame id; 0 disables |
| `fh1_debug_band2_trace_swap` | 0 | FL | Band 2 packets and draw processing for one PM4 `XE_SWAP` frame | Single frame id; 0 disables |
| `fh1_debug_band_suppress` | 0 | FL | Suppresses draw passes by bit: bit 0 = pass 0 (bin select 3), bit 1 = pass 1 (0xC), bit 2 = pass 2 (0x30). Copy/resolve/clear packets NOT suppressed | Bitmask |
| `d3d11_band_trace` | false | CP:69 | Untile resolve + viewport/scissor band diagnostics: per-resolve `frame, band, rect, key, clear_color, clear_depth` (CP:3648) and per-draw viewport/scissor band line (CP:1216) | Boolean; log only |
| `pinyon_draw_trace` | false | FL | Logs the world draws' viewport and scissor as they change | Boolean |
| `pinyon_transition_log` | false | FL | One line per presented frame in `E:/PinyonShift/work/.local/transition.log`: `frame=<n> wide=<m>` where `wide` = draws using the 8192-wide world viewport (RA `EndFrame`) | Boolean; appends one line/frame, unbounded file (see gap G7) |
| `pinyon_audit` | false | FL | One JSONL render-audit record per 300 frames (RA `kSampleInterval = 300`): bound RT sample counts, resolve sample counts, draw tally, error tally | Sampled 1/300 frames |
| `pinyon_audit_passes` | false | FL | Per-pass trace: each EDRAM tile claim with owner before/after, and each resolve with rectangle, source occupancy, destination write coverage per 16-row band | `pinyon_audit_passes_from` / `_to` frame window; **hard cap 400 records, then truncates** (RA) |
| `pinyon_audit_passes_from` / `_to` | 0 / 0 | FL | Frame window for the per-pass trace (0,0 records nothing) | Frame range |
| `pinyon_msaa_probe` | false | FL | MSAA source provenance for the 4x resolves of the captured frame: format support bits, the three-resource readback chain and its validity | Single captured frame |

**Resolve traces**

| Cvar | Default | Defined | What it captures | Gating / limits |
|------|---------|---------|------------------|-----------------|
| `pinyon_resolve_trace` | false | FL | Captures the next three large D3D11 FH1 resolves: owner rectangles, source-row hashes, resolve completion, guest-memory mirror | 3 resolves, then stops |
| `fh1_debug_resolve_trace` | 0 | FL | From this presented-frame index on, logs every resolve's destination (surface key, rectangle, destination base, pitch, info word, memory extent, source surface) | Up to 4,000 lines |
| `fh1_debug_resolve_store_probe` | false | FL | Runs the probe build of the native resolve shader, which stores `0xDEADBEEF` at offset 0 of its bound UAV before touching guest data | Probe build only |
| `fh1_debug_resolve_store_probe_from` | 0 | FL | Starts the per-resolve mirror before/after hash probe at this presented-frame index, up to 160 probes | 160 probes |
| `fh1_debug_resolve_small_uav` | false | FL | Binds the resolve destination to a dedicated 4 MB raw UAV instead of the 512 MB mirror | Boolean |
| `fh1_debug_resolve_force_pack` | −1 | FL | Rewrites the native resolve's destination pack (0–7) before it is sent to the shader | −1 = off |
| `fh1_debug_resolve_force_endian` | −1 | FL | Rewrites the resolve destination's endianness bits (3:5) | −1 = off |
| `fh1_debug_resolve_cb_capture` | false | FL | Snapshots the resolve constant buffer as the GPU saw it after the first two resolve dispatches, logs it beside the CPU-packed values | First 2 dispatches |
| `fh1_debug_resolve_cb_update` | false | FL | Uploads resolve constants with `UpdateSubresource` on a DEFAULT buffer instead of Map/WRITE_DISCARD | Boolean |
| `fh1_debug_resolve_bind_check` | false | FL | After a resolve's binds and before its dispatch, reads the bound SRVs, UAV, and constant buffer back from the context and logs whether each request survived (catches silently dropped binds) | Boolean; also implemented on the recorded path in CLS `CheckBindings` |
| `fh1_debug_resolve_direct_mirror` | false | FL | Dispatches resolves straight into the 512 MB mirror UAV instead of the staging buffer | Boolean |
| `fh1_debug_resolve_window_mb` | 0 | FL | Binds the resolve destination as a raw UAV window of N MiB at a fixed mirror offset (0x08000000) | 0 = whole-range window |
| `fh1_debug_resolve_alias_base` | "" | FL | A guest base (hex) that a 640x360 8_8_8_8 texture fetch samples; a resolve whose destination starts at the base but does not reproduce that fetch's own writer is skipped and logged | String; measured to stall the title before free-roam — a dissection probe, not a fix |
| `fh1_debug_resolve_alias_bytes` | 921600 | FL | Protected length for the alias base (640x360x4) | Bytes |
| `fh1_debug_resolve_alias_mode` | 0 | FL | Which base writers the alias guard covers (0 all, 1 depth only, 2 colour only) | Enum |
| `fh1_debug_resolve_alias_from_swap` | 0 | FL | Starts the alias guard at this presented-frame index | Frame id |
| `fh1_debug_resolve_owner_census` | 0 | FL | For up to N front-buffer resolves, logs the EDRAM tile owners under the copy's rectangle (unowned/mixed counts, owner histogram) beside the by-owner result | Count |
| `fh1_debug_frontbuffer_draws` | 0 | FL | Logs up to N draws that bind the front-buffer family (base_tiles != 0, pitch_tiles 16, 1x) with key, viewport, scissor, primitive, vertex counts, and sampled textures | Count; `_from` gates the start frame |
| `fh1_debug_frontbuffer_draws_from` | 0 | FL | Start frame for the above | Frame id |
| `fh1_resolve_swap_rb` | false | FL | Resolves colour surfaces with red and blue exchanged (guest swap bit inverted for 3D content only, never presentation) | Boolean — **this is a behaviour change, not a trace** |
| `fh1_resolve_guest_refresh` | true | FL | Before a texture upload reads a guest range the resolve path wrote into the GPU mirror, lands that range in guest memory | Experimental; measured too slow for free-roam (4.75 Hz vs 5 Hz floor) |

**Swap / fetch / present traces**

| Cvar | Default | Defined | What it captures | Gating / limits |
|------|---------|---------|------------------|-----------------|
| `fh1_swap_fetch_trace` | false | FL | Per-swap: swap packet front-buffer pointer + texture fetch constant 0 (VdSwap's front-buffer description) vs the last native resolve destination (guest addresses, pitch, format) | Boolean; requires restart |
| `fh1_d3d11_resolve_readback` | true | FL | At each swap, pulls the front-buffer extent from the GPU mirror back into guest memory | Boolean; costs one GPU-mirror readback per swap (production behaviour, not a trace) |

**Surface / texture / composite dumps**

| Cvar | Default | Defined | What it captures | Gating / limits |
|------|---------|---------|------------------|-----------------|
| `fh1_debug_surface_dump` | "" | FL | At `fh1_debug_surface_dump_swap`, writes every native EDRAM surface the executor holds as PPM + stats line (size, samples, nonzero share, distinct colours, mean), plus the EDRAM tile ownership histogram, once | One swap; directory path |
| `fh1_debug_surface_dump_swap` | 300 | FL | Presented-frame index for the surface dump | Frame id |
| `fh1_debug_dump_guest` | "" | FL | With the surface dump, also writes named guest ranges (addr+length hex) from guest memory and from the GPU mirror as .bin + linear 32-bit PPM, with word statistics | Range list |
| `fh1_debug_texture_dump` | "" | FL | With `fh1_debug_composite_draws`, at each world-rectangle draw the largest frame-sized sampled texture is read back from the host texture and written as PPM | Directory path |
| `fh1_debug_guest_probe` | "" | FL | At each world-half composite draw (1280x360 rectangle sampling the scene target), writes the guest bytes of every sampled texture ≥320x192 from guest memory and from the GPU mirror, with fetch base/size/format/tiled/endian | Directory path |
| `fh1_debug_presented_pair` | "" | FL | Once, at `fh1_debug_presented_pair_swap`, writes two PPMs from one call at that swap: the composite the presenting resolve sampled + the untiled swap texture the presenter paints, each with a per-60-row nonzero/mean band profile | One swap (count cvar allows more) |
| `fh1_debug_presented_pair_swap` / `_count` | 0 / 1 | FL | Swap index and count for the presented pair | Frame id / count |
| `fh1_debug_composite_draws` | 0 | FL | From this presented-frame index on, logs every draw that writes the 1280-wide 1x surface family the front buffer is composed into (viewport, scissor, each sampled texture with guest base/size/format), up to 400 lines | 400 lines |
| `fh1_debug_composite_dump` | "" | FL | With composite draws, writes the front-buffer-family render target as PPM before each of the first 12 draws that write it at the traced swap | 12 draws |
| `fh1_debug_composite_dump_world` | "" | FL | Same as above but starts at the first draw sampling a texture ≥640x360 (the 3D scene) | 12 draws |
| `fh1_debug_composite_world_only` | false | FL | With composite draws, logs/dumps only draws sampling a frame-sized (≥640x360) texture | Boolean |
| `fh1_debug_vertex_dump` | 0 | FL | For N traced draws sampling a frame-sized texture, logs each vertex the shader fetches (fetch base + draw index × binding stride + attribute offset) with positions mapped through the viewport, in pixels | Count |
| `fh1_debug_post_clip` | 0 | FL | With vertex dump, logs the post pass's own vertices (up to 48) with pixel positions | Count |
| `fh1_debug_clear_draw` | 0 | FL | With composite dumps, clears the front-buffer target to magenta before every traced draw from this one on, so each dump shows a single draw's footprint | **Destroys the frame — a probe only** |
| `fh1_debug_c2_probe` | "" | FL | For the first N draws writing a frame-sized (≥1280 wide) surface at/after a start frame, reads the surface twice before the draw (stability control, must report 0) and once after, and logs the pixels the draw changed (share, bounding box, change per 64-pixel column and 60-pixel row). First two probed draws also written as before/control/after PPMs | Default 24 draws; **each is a full surface readback** |
| `fh1_debug_c2_probe_from` / `_draws` | 0 / 24 | FL | Start frame and draw count for the c2 probe | Frame id / count |
| `fh1_debug_minimap_capture_swap` | 338 | FL | Captures one 320x192 FH1 minimap candidate at source, resolved guest bytes, and sampled texture stages on one presented frame | Single frame id; **not yet run** |

**Transfer / executor / command-list diagnostics**

| Cvar | Default | Defined | What it captures | Gating / limits |
|------|---------|---------|------------------|-----------------|
| `fh1_debug_transfer_off` | false | FL | Executor still claims EDRAM tiles and counts what it would transfer, but performs no transfer (bisects ownership vs transfer defects) | Boolean — behaviour change |
| `fh1_debug_transfer_unbatched` | false | FL | One rectangle per dispatch instead of one dispatch per destination/source pair | Boolean — behaviour change |
| `fh1_debug_transfer_direct` | false | FL | Depth-destination transfers read the previous owner's surface directly, no words buffer / compute dispatch | Boolean — behaviour change |
| `fh1_d3d11_command_lists` | false | FL | Records the frame and swap passes into deferred command lists and replays them on the immediate context (Phase 3 two-list recorder) | Boolean; requires restart |
| `fh1_d3d11_command_list_validate` | false | FL | With command lists, validates every recorded resource transition and packet binding, logs the first mismatch | Boolean |
| `fh1_d3d11_command_list_ab` | false | FL | With command lists, runs every recorded resolve a second time through the immediate path on the same source and compares the two dispatches' staging output bit-for-bit | **Doubles resolve work** |
| `fh1_debug_disable_msaa` | false | FL | Forces RB_SURFACE_INFO MSAA to 1x for FH1 | Boolean; requires restart |
| `fh1_d3d11_shader_pack` | false | FL | Uses the precompiled D3D11 shader pack instead of translating guest shaders at runtime | Boolean; requires restart |
| `dump_shaders` | "" | FL | Dumps translated shaders to a path | Path |
| `gpu_debug_markers` | false | FL | Inserts debug markers for PIX/RenderDoc; auto-enabled when RenderDoc is detected | Boolean |
| `fh1_native_gpu_profile` | false | FL | Measures executor GPU time per phase (transfers, resolves, clears) with timestamp queries + CPU time per phase, reported with periodic stats | Boolean |
| `fh1_native_executor` | 1 | FL | Runs the FH1 native executor for draw targets (0 off, 1 on, 2 reserved) | Enum; requires restart |

### 1.2 Existing log files and their formats

| File | Writer | Format | Notes |
|------|--------|--------|-------|
| `E:/PinyonShift/work/.local/preview/logs/runtime.log` (and `runtime.N.log`) | spdlog (`REXGPU_INFO` / `REXGPU_ERROR` / `REXLOG_*`) | Timestamped text, `[info] [gpu] [t<thread>] <message>` | **Rotates at ~5 MB**; exclusively locked while the game runs — pull only after the game closes. All trace cvars above write here. |
| `resolve-trace.log` | `pinyon_resolve_trace` / resolve trace writer (CP) | Text | Readable while running. Captures 3 large resolves. |
| `E:/PinyonShift/work/.local/audit/<backend>-<utc>.jsonl` | RA `WriteRecordLocked` | One JSON object per sampled frame (every 300 frames): `frame, backend, adapter, surface, draws, clear, rt[], resolves[], errors[]` | Only when `pinyon_audit` is on; `BeginRun` opens on first use. |
| `E:/PinyonShift/work/.local/audit/<backend>-passes-<utc>.jsonl` | RA `NotePassClaim` / `NotePassResolve` / `NoteProbe` | One JSON object per claim/resolve/probe: `ev, frame, pass, draw_id, rect, tiles, owner_before, owner_after, window_offset, viewport, scissor, …` | Only inside `pinyon_audit_passes_from`–`_to`; **hard cap 400 records, then `truncated: true`** in the capture summary. |
| `E:/PinyonShift/work/.local/transition.log` | RA `EndFrame` (when `pinyon_transition_log`) | One line per presented frame: `frame=<n> wide=<m>` | **Unbounded append** — the only existing log without a cap. |
| PPM / .bin dumps | Various `fh1_debug_*_dump` cvars | PPM images + .bin guest ranges | Written to caller-named directories; one-shot or first-N. |

### 1.3 The existing audit core (what to build on)

`rex::graphics::render_audit` (RA, header-only, no build-system change, no fmt) already provides:

- **Frame sampling:** `BeginRun` / `SetContext` / `EnsureStarted` / `EndFrame` (RA). `EndFrame` increments the frame counter, writes the transition line if enabled, and emits a JSONL record every 300 frames (`kSampleInterval`), then resets the per-frame tallies so they cannot grow without bound.
- **Per-pass trace:** `NotePassClaim` (RA) — rectangle, tile range, owner before/after, per-row flag, plus the draw geometry captured at the draw that set it (`PassGeo`: window offset, viewport, scissor, draw id). `NotePassResolve` (RA) — kind, rect, source key/dims/samples/sample_select, dest base/pitch/info (with pack/endian/swap/unscaled decoded), source-evidence string, raw/hw/dst row tallies. `NoteDrawSubmission` (RA) — increments the draw id. `SetDrawGeo` (RA) — captures window offset, viewport, scissor; counts "wide" (≥8000 px viewport) draws. `SetFrameWindow` / `PassWindowActive` (RA) — gates expensive per-resolve measurement to frames that will actually be recorded.
- **Provenance probes:** `NoteProbe` + `ProbeInfo` (RA) — a record whose fields are only meaningful together with a `verdict` (`invalid_measurement` / `probe_invalid` means no field below may be read as evidence). Carries format/support/HRESULT/copy-stage/sample-row/max-channel fields. This is the established pattern for "a zero is not evidence" — the forensics system must follow it.
- **Error counting:** `NoteError` (RA) — counts failures by site + code instead of burying them in the log.
- **Capture summary:** `WriteCaptureSummary` / `EndPassRun` (RA) — emits a final `capture` record with `frame_from, frame_to, records, draws_submitted, complete, truncated`.

**Design rule: the forensics recorder extends this file's conventions.** New event types go into the same `render_audit` namespace (or a sibling `render_forensics` namespace in the same header), use the same JSONL writer style, the same frame-window gating (`SetFrameWindow` / `PassWindowActive`), the same record cap + `truncated` summary, and the same "verdict" discipline for probe records. Do not invent a second logging framework.

### 1.4 Gaps — what does NOT exist today

| # | Gap | Why it matters (which defect it blocks) |
|---|-----|------------------------------------------|
| G1 | **No unified per-event recorder.** Traces are ~40 separate cvars, each with its own format, gating, and file. There is no single frame-scoped event stream with sequential event IDs that ties a draw to its resolve to its present. | All defects. Answering "what produced this pixel" today requires correlating 3+ log files by hand. |
| G2 | **No stable resource IDs and no provenance chain.** `TextureKey` (TC) and `Fh1SurfaceKey` (SFH) exist as cache/surface keys, but no event records the mapping guest address → cache key → host resource → bind → draw → resolve → present. A host texture ID is never linked to the game asset it came from. | Defects #5, #6, #7. |
| G3 | **No resource lifecycle events.** Surface creation (`GetOrCreateSurface`, SF:69) and texture creation (`CreateTexture`, TC:444) are not logged as events; destruction is not logged at all. A surface that is reused across a menu→gameplay transition cannot be distinguished from one that was recreated. | Defect #4 (the black block). |
| G4 | **No per-draw state capture.** No trace records the blend state, depth-stencil state, rasterizer state, or shader identity for a draw. `UpdateBindings` (CP:1603) binds shaders and textures, but the binding is never recorded as an event. | Defects #1, #2, #6. |
| G5 | **No resolve-planning failure call-site IDs.** The two `D3D11 resolve planning failed` errors per run are logged without a stable site ID. The planned RF001–RF008 instrumentation (ChatGPT plan, Copilot Task 1) was never implemented. | Defect #7. |
| G6 | **No pixel/region history.** The closest tool is `fh1_debug_c2_probe` (before/control/after readbacks on ≤24 draws), which is a probe, not a history: it shows what changed, not the ordered sequence of operations that could have changed it, and it costs a full surface readback per probed draw. | All defects. |
| G7 | **No visual diagnostic modes on D3D11.** No wireframe, no draw-call ID visualization, no overdraw heatmap, no depth visualization, no band-ownership visualization. (Vulkan has `vulkan_tessellation_wireframe`; D3D11 has no equivalent cvar.) | Defects #1, #2, #3. |
| G8 | **No menu↔gameplay transition marker.** `transition.log` counts wide draws per frame but does not mark when the game enters/exits the menu, which surfaces were live at the transition, or whether `EndUntiledTiling` ran. | Defect #4. |
| G9 | **No present-side linkage.** `DumpPresentedPairOnce` (CP:5727) is a one-shot PPM pair. There is no per-frame record linking the presented swap texture back to the resolve that produced it and the draws that fed it. | Defects #5, #6. |
| G10 | **No event-volume control.** The audit samples 1/300 frames and the pass trace caps at 400 records, but there is no way to capture exactly one full frame's complete event stream (every draw, resolve, clear, copy, bind) with a known byte budget. | All defects — the forensics system itself must provide this. |
| G11 | **Unbounded `transition.log`.** The only existing log without a cap; it appends one line per presented frame for the whole session. | Operational (disk fill on long sessions). |
| G12 | **No shader-identity logging.** The translated guest shader (vertex/pixel/compute) is never identified in any trace. Two draws that look identical in the log may run different shaders. | Defects #1, #6. |

---

## 2. Exact source locations for event hooks

Conventions for this section:

- A hook is a **single call** to the recorder (`Forensics::Record(...)`) placed at the stated location, gated on the recorder being active for the current frame (see §3.5). It reads state that is already in scope at that point; it does not compute anything expensive and does not call the GPU.
- "Immediate path" = the direct D3D11 calls in `CP`. "Recorded path" = the tape (`CL`/`CLS`). A hook listed for both paths must be placed in both, or in the one shared function that feeds both. Where only one path is listed, the other path is explicitly out of scope for that event type in Stage 1 and must be added in the stage that covers it.
- Line numbers are the function starts (verified). Inner statement lines are given where they matter (e.g. the exact `DrawIndexed` call). All lines are the fresh 2026-11-15 PC source unless noted.

### 2.1 Draw events

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-DRAW-1 — guest draw entry** | CP:1733 `D3D11CommandProcessor::IssueDraw` | At function entry, after the `SubmitFrameTape("draw")` call (CP:1747) and the band-range check (CP:1750-1759). In scope: `primitive_type`, `index_count`, `current_untile_band`, `register_file_` (all guest registers), `presented_output_frames_`. This is the one place that sees the guest draw before any host work. Record the event here; record the host outcome at H-DRAW-2. |
| **H-DRAW-2 — host draw issued (immediate path)** | CP:2941 `context->Draw(...)` and CP:2996 `context->DrawIndexed(...)` | Inside the primitive-processing branch of `IssueDraw` (the same function, later in the body). These are the only two immediate-path draw calls. In scope: `primitive_processing_result.host_draw_vertex_count`, the bound pipeline (from `RecordDrawPacket`, CP:3358), the render targets bound by `BindDrawRenderTargets` (CP:1002). The recorder call goes immediately before each of the two calls so the event's `issued` flag is set only if the call is reached; the HRESULT is not available from `Draw`/`DrawIndexed` (they return void) — record `hr = S_OK` with `hr_source = "void-return"`. |
| **H-DRAW-3 — host draw issued (recorded path)** | CLS:333 `context_->DrawIndexed(...)` and CLS:335 `context_->Draw(...)` | Inside `ImmediateCommandSink::Draw` (CLS). In scope: the `D3D11DrawPacket` (CLH:194) — vertex count, index count, primitive, pipeline. This is the replay side; the record side is H-DRAW-4. |
| **H-DRAW-4 — draw packet recorded (recorded path)** | CP:3358 `D3D11CommandProcessor::RecordDrawPacket` | At entry, after the packet is populated. In scope: the `D3D11PipelineCache::D3D11Pipeline&` (shaders, blend/depth/rasterizer state objects), the packet's vertex/index counts. Record the same `event_id` that H-DRAW-3 will later reference (see §3.4 on correlating record and replay). |
| **H-DRAW-5 — draw geometry (viewport/scissor/band)** | CP:1133 `D3D11CommandProcessor::UpdateFixedFunctionState` | Alongside the existing `SetDrawGeo` call (CP:1140) and the `d3d11_band_trace` log (CP:1216). In scope after the untile shift: the final `viewport` (CP:1153-1160, `TopLeftY` already shifted), the final `scissor_rect` (CP:1207-1221), `current_untile_band` (CP:1216). This hook does not create its own event; it **attaches geometry to the draw event opened by H-DRAW-1** (the draw event is still open at this point in the call sequence — verify in Stage 1 that `UpdateFixedFunctionState` is always called between H-DRAW-1 and H-DRAW-2 for every draw; if any draw path skips it, that path gets its own geometry attach point and a `geo_source` field marks which one). |
| **H-DRAW-6 — draw submission counter** | CP:2943 `render_audit::NoteDrawSubmission()` | Already exists. The forensics recorder reuses this as the draw's sequential id source: the recorder's per-frame draw sequence number is the same counter. Do not add a second counter. |
| **H-DRAW-7 — shader + texture bindings** | CP:1603 `D3D11CommandProcessor::UpdateBindings` | At exit, after all shader resources are bound. In scope: `vertex_shader` / `pixel_shader` (`D3D11Shader*`, with their translated-shader identity — see §3.3 for the shader id rule), the bound SRVs (up to the fetch-constant count), the sampler states. Record as a `bind` event linked to the open draw event (same `event_id`, `op = bind`). |
| **H-DRAW-8 — constant-buffer updates** | CP:1466 `UpdateConstantBuffer` and CP:1502 `FlushDeferredConstantBuffers` | At exit of each. In scope: the binding (slot, buffer, byte range). Record as a `bind` event with `detail = "cbuffer"`. The existing `fh1_debug_resolve_cb_capture` (FL) covers only the resolve constant buffer; this hook covers draw constant buffers, which no trace covers today. |

### 2.2 Clear events

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-CLEAR-1 — resolve clear (executor)** | EX:2365 `D3D11Fh1NativeExecutor::ResolveClear` | At entry, after the band-range check (EX:2372-2374). In scope: `key` (`Fh1SurfaceKey` — packed key, pitch tiles, msaa, format), `x0,y0,x1,y1` (guest rows), `clear_value` / `clear_value_lo`, `untile_band`, `clear_depth`, `clear_stencil`. The physical rows are `top + untile_band` (EX:2398-2399) — record both the guest rect and the physical rect. This is the hook that answers "did a clear run on menu exit, and on which rows". |
| **H-CLEAR-2 — clear RT (recorded path)** | CLS, `D3D11ClearRenderTargetPacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `context_->ClearRenderTargetView`. In scope: the packet (CLH:149) — RTV pointer, colour. |
| **H-CLEAR-3 — clear depth-stencil (recorded path)** | CLS, `D3D11ClearDepthStencilPacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `context_->ClearDepthStencilView`. In scope: the packet (CLH:154) — DSV pointer, flags, depth, stencil. |
| **H-CLEAR-4 — clear state creation** | EX:2009 `EnsureClearResources` and EX:2108 `GetClearDepthStencilState` | At exit of `EnsureClearResources` (records the clear shaders/resources created, once per run) and at entry of `GetClearDepthStencilState` (records which depth-stencil state a clear used: `clear_depth`, `clear_stencil`). Low frequency; record as `resource` events, not per-draw events. |

### 2.3 Resolve events

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-RES-1 — resolve plan (shared, backend-neutral)** | FER `Fh1PlanResolve` | At each return point. In scope: the full `Fh1ResolvePlan` — `x0,y0,x1,y1`, `info` (copy dest info word, `copy_dest_extent_start/length`), `dest_base`, `dest_pitch`, `dest_info` (pack, endian, swap, exp bias), `sample_select`, `msaa`, `pitch_tiles`, `resolve_key`, `kind`, `skip` reason string, `failure` enum. **This is the hook that fixes gap G5:** every `return false` (planning failure) records an event with a stable `site` id. The site ids are assigned in Stage 1 (see §8, Stage 1) and are the RF001–RF008 ids from the ChatGPT plan: RF001 = fetch type, RF002 = fetch size, RF003 = fetch memory, RF004 = empty rectangle, RF005 = resolve info, RF006 = dest format, RF007 = dest array, RF008 = dest endian / source format. The exact mapping from `Fh1ResolvePlanFailure` / `skip` string to RF id is fixed in Stage 1 and frozen thereafter. |
| **H-RES-2 — resolve entry (D3D11)** | CP:3614 `D3D11CommandProcessor::IssueCopy` | At entry, after the `untile_band` computation (CP:3628-3636) and the `EndUntiledTiling` call (CP:3639-3641), and after `Fh1PlanResolve` returns. In scope: `untile_band`, `plan` (the full plan from H-RES-1 — link by `plan_id`, not by copying the plan twice), `presented_output_frames_`, and whether the copy-down ran (`copy_down_ran` bool — this is the single most important field for defect #4). The existing `d3d11_band_trace` log (CP:3648) already emits `frame, band, rect, key, clear_color, clear_depth` here; the forensics event carries the same fields plus the plan link and the copy-down flag. |
| **H-RES-3 — resolve batch recorded** | CP:3090 `D3D11CommandProcessor::RecordResolveBatch` | At entry. In scope: `plan`, `source_rect` (`D3D11Fh1NativeResolve::SourceRect` — surface, rect, rows), `resolve_sample_select` (which already carries `(source_row_offset << 16)`). Record the per-source-rect batch as its own event (a resolve with multiple source rects produces one event per rect, all sharing the parent `resolve_event_id`). |
| **H-RES-4 — resolve dispatched (compute)** | RS:281 `D3D11Fh1NativeResolve::ResolveToMemory` | At entry (after the `Skip` check, RS:119) and at exit with the return value. In scope: `source` (SourceRect), `resolve_key`, the constant buffer (via `GetConstantsBuffer`, RS:137), the dispatch dimensions (group counts). The existing `NoteResolve` call (CP:4316) and `NotePassResolve` call (CP:4847) are in the caller; H-RES-4 records the dispatch itself. If the dispatch is skipped, record `op = resolve_skip` with the `Skip` reason string — a skipped resolve is evidence, not an absence of evidence. |
| **H-RES-5 — resolve staging copy** | CP:3208 `D3D11CommandProcessor::RecordResolveStagingCopy` | At entry. In scope: `host_offset`, `length`. This is the staging-buffer → mirror copy; record it as a `copy` event with `detail = "resolve_staging"`. |
| **H-RES-6 — resolve readback / guest refresh** | CP:5277 `D3D11CommandProcessor::RefreshResolvedGuestRange` | At entry and at exit with the return value. In scope: `start`, `length`, and whether `SubmitFrameTape("resolved range refresh")` (CP:5228) and `WaitForGpu()` (CP:5231) succeeded. This is the readback that lands resolved data in guest memory for the texture cache to sample. |
| **H-RES-7 — resolve subresource (recorded path)** | CLS, `D3D11ResolveSubresourcePacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `context_->ResolveSubresource`. In scope: the packet (CLH:161) — source/destination, subresources, format. (Note: this is the hardware `ResolveSubresource` path used by the recorded tape, distinct from the compute-shader resolve in H-RES-4. Record both; do not conflate them.) |

### 2.4 Copy events (non-resolve copies)

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-COPY-1 — copy resource (recorded path)** | CLS, `D3D11CopyResourcePacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `context_->CopyResource`. In scope: the packet (CLH:118) — source/destination. |
| **H-COPY-2 — copy subresource (recorded path)** | CLS, `D3D11CopySubresourcePacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `CopySubresource`. In scope: the packet (CLH:123). |
| **H-COPY-3 — copy buffer (recorded path)** | CLS, `D3D11CopyBufferPacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `CopyBuffer`. In scope: the packet (CLH:110). |
| **H-COPY-4 — untile copy-down** | EX:2589 `D3D11Fh1NativeExecutor::EndUntiledTiling` | At entry (records the `untile_bands_` map contents: one entry per packed key with its band) and per surface inside the loop (EX:2610), after the copy for that surface. In scope: `packed` key, `band`, `copy_rows` (EX:2618, from `Fh1D3D11CopyDownRows`), the surface's `image_height`, success/failure per surface. The existing log line (EX:2668, `D3D11 FH1 untile copy-down: key=… band=… rows=0..… samples=…`) is the text form; the forensics event is the structured form. **If `EndUntiledTiling` is NOT called in a frame, no event is recorded — and that absence is itself the evidence for defect #4.** The per-frame summary (§3.5) therefore includes an explicit `copy_down_ran` boolean so "no copy-down" is distinguishable from "no data". |
| **H-COPY-5 — present-device copy** | PR:486 `D3D11Presenter::CopyGuestOutputToPresent` | At entry and exit. In scope: `mailbox_index`, the copy result. (Only reached when `PresentDeviceIsSeparate()`; in the current build this is normally false — record the event anyway so the path is visible if it is ever re-enabled.) |

### 2.5 Resource creation / destruction / upload / bind

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-RSC-1 — EDRAM surface created** | SF:69 `D3D11Fh1NativeSurfaces::GetOrCreateSurface` | At exit, only when a new surface was actually created (not when an existing one was found). In scope: `key` (`Fh1SurfaceKey`), the created `D3D11Fh1NativeSurface` — `image_height` (SF:132-138), texture desc (width, height, format, samples, bind flags), the assigned stable resource id (§3.3). |
| **H-RSC-2 — EDRAM surface destroyed** | SF:51 `D3D11Fh1NativeSurfaces::Shutdown` and the surface destructor (SF:47) | At each surface release. In scope: the packed key and the stable resource id, the frame it is destroyed on. (Verify in Stage 1 whether surfaces are ever individually destroyed outside `Shutdown`; if they are only destroyed at shutdown, record the shutdown as a single `resource_destroy` batch event with one entry per live surface.) |
| **H-RSC-3 — texture created** | TC:444 `D3D11TextureCache::CreateTexture` | At exit, on success. In scope: `key` (`TextureKey` — guest format, dims, mips, tiling, `scaled_resolve`), the host format (`kHostFormats`, TC), the created `D3D11Texture` (host texture desc), the assigned stable resource id. |
| **H-RSC-4 — texture uploaded (guest → host)** | TC:524 `D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl` | At entry and at exit with the return value. In scope: `key`, `guest_layout` (packed level, base address), `load_base` / `load_mips`, the byte range uploaded, and whether the upload read from guest memory or from the GPU mirror (the `fh1_resolve_guest_refresh` path). Record the guest base address — this is the field that ties a host texture back to the guest range a resolve wrote (§3.3). |
| **H-RSC-5 — texture upload recorded (tape)** | CP:3288 `D3D11CommandProcessor::RecordTextureUpload` | At entry. In scope: `destination` resource, the upload byte range. Recorded-path counterpart of H-RSC-4's host side. |
| **H-RSC-6 — upload buffer (tape)** | CLS, `D3D11UploadBufferPacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `UploadBuffer`. In scope: the packet (CLH:134) — destination buffer, offset, size. |
| **H-RSC-7 — upload texture (tape)** | CLS, `D3D11UploadTexturePacket` handler (inside `Execute`, CLS:38) | In the `std::visit` arm that calls `UploadTexture`. In scope: the packet (CLH:140). |
| **H-RSC-8 — shared-memory upload (guest → host buffer)** | SM:70 `D3D11SharedMemory::UploadRanges` | Per chunk inside the loop (not per call). In scope: `offset`, `chunk_bytes`, and the `UploadToHostBuffer` result. This is the CPU-side guest-memory upload that the texture cache and the resolve readback both depend on. Low frequency per call but the chunk loop is the right granularity. |
| **H-RSC-9 — swap texture requested** | TC:774 `D3D11TextureCache::RequestSwapTexture` | At exit. In scope: the returned SRV, `format_out`, and the guest extent from `GetSwapGuestExtent` (TC:753). This is the texture the presenter paints; it links the swap to the texture cache. |
| **H-RSC-10 — barrier / state transition (tape)** | RST `D3D11ResourceStateTracker::Transition` and `AppendBarrier` | At each successful `AppendBarrier`. In scope: resource pointer, `before` / `after` state. Record as a `barrier` event. Only on the recorded path (the immediate path has no explicit barriers). |
| **H-RSC-11 — shader created / translated** | `D3D11Shader` creation — the shader cache / translator (see note) | **Not verified in this audit.** The shader object (`D3D11Shader`, `shader.cpp` / `include/rex/graphics/d3d11/shader.h`) and the pipeline cache (`pipeline_cache.cpp`, 901 lines) were listed but their creation entry points were not read line-by-line. Stage 1 must locate the exact function that creates a translated shader and the function that creates a pipeline, and add hooks there. Do not guess the lines. The shader id rule (§3.3) depends on this. |

### 2.6 Present events

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-PRES-1 — swap entry** | CP:5395 `D3D11CommandProcessor::IssueSwap` | At entry. In scope: `frontbuffer_ptr`, `frontbuffer_width`, `frontbuffer_height`, `presented_output_frames_`. The existing swap-fetch trace (CP:5420+) already logs the fetch constant 0 vs the last resolve destination here; the forensics event carries the same fields in structured form plus the `frame_id`. This is also where the per-frame summary is emitted (§3.5) and where the recorder's frame boundary is. |
| **H-PRES-2 — guest output refresh** | PR:668 `D3D11Presenter::RefreshGuestOutputImpl` | At entry and exit. In scope: the refresh context (which mailbox, which guest texture). |
| **H-PRES-3 — guest output painted** | PR:682 `D3D11Presenter::PresentGuestOutputTexture` | At entry. In scope: the texture being painted, the effect pipeline (`BindEffectPipeline`, PR:158 — which effect: bilinear / bilinear-dither / CAS / FSR1 fallback). Record the effect — the presenter currently falls back to bilinear for CAS/FSR1 (known incomplete feature), and that fallback is invisible in every trace today. |
| **H-PRES-4 — swap chain present** | PR:1190 `paint_context.swap_chain->Present(vsync ? 1 : 0, 0)` | Immediately after the call, with the returned `present_result` HRESULT. This is the last host operation in the frame. Record `hr` and `vsync`. |
| **H-PRES-5 — frame end / audit** | CP:5395 `IssueSwap` (the `render_audit::EndFrame` call inside it) and RA `EndFrame` | Already exists. The forensics recorder hooks the same boundary: when `EndFrame` runs, the recorder finalises the frame's event list, writes the per-frame summary (§3.5), and — if the frame is inside the capture window — flushes the frame to the capture file. |

### 2.7 Transition events (menu ↔ gameplay)

| Hook | File:line | Placement detail |
|------|-----------|------------------|
| **H-TR-1 — swap / frame boundary with transition state** | EX:2517 `D3D11Fh1NativeExecutor::OnSwap` | At entry. In scope: `frame`, `frontbuffer_address`. This is the executor's per-swap hook — the natural place to snapshot transition-relevant state: the `untile_bands_` map size and contents, the live surface count, and whether the frontbuffer address changed since the last swap (a frontbuffer change is the observable signature of a menu↔gameplay transition). |
| **H-TR-2 — transition log** | RA `EndFrame` (`pinyon_transition_log` branch) | Already exists (writes `frame=<n> wide=<m>`). The forensics per-frame summary (§3.5) adds `frontbuffer_address`, `untile_bands_live`, `copy_down_ran`, and `surfaces_live` to the same line's structured equivalent. Do not change the existing `transition.log` format; the forensics summary is a separate record. |
| **H-TR-3 — surface dump trigger** | CP:5814 `D3D11CommandProcessor::DumpNativeSurfacesOnce` | At entry. This is the existing one-shot surface dump (`fh1_debug_surface_dump`). The forensics system does not replace it; Stage 4 adds a `surfaces_live` snapshot to the per-frame summary so the dump can be correlated with the event stream. |

### 2.8 What is deliberately NOT hooked (and why)

- **The guest packet decoder** (`SCP::ExecutePacket`, SCP:2161). Hooking every PM4 packet would multiply the event volume by an order of magnitude for information the D3D11 backend already summarises at `IssueDraw`/`IssueCopy`/`IssueSwap`. The only packet-level hook is the swap (`XE_SWAP`, SCP:2490), which is already the frame boundary.
- **Per-vertex / per-index data.** The vertex fetch path (`fh1_debug_vertex_dump`) is a separate probe. The forensics recorder records draw parameters, not vertex contents.
- **Per-pixel GPU data.** There is no hook that reads a pixel. Pixel values are obtained only through the §6 strategy (event-region overlap + gated readback + draw-ID re-render), never by adding a readback to the hot path.

---

## 3. Proposed event / resource data structures

### 3.1 Design principles

- **One event struct for the ring buffer, one JSON schema for the file.** The in-memory record is a fixed-size binary struct (no heap allocation per event). The file record is the same fields serialised as one JSON object per line (JSONL), following the `render_audit` writer style (RA). A binary file format is rejected: the existing tooling (and the humans reading it) already consume JSONL, and the volume (§7) does not justify a binary format.
- **Every event carries the frame and a sequence number.** There is no event without `frame_id` and `event_id`. An event that cannot be assigned a frame is recorded with `frame_id = 0` and a `frame_unknown = true` flag, and that flag is itself a defect to investigate.
- **IDs are stable within a run, meaningless across runs.** A resource id is a per-run monotonic counter. It is never persisted as if it identified the asset across runs. The asset is identified by the guest address + cache key (§3.3).
- **A host texture ID does NOT identify the original game asset.** This is a rule, not a caveat. The host texture is a cache entry created from a guest range at a moment in time. The same guest asset can produce several host textures (different mips, different tiling, re-upload after a resolve), and the same host texture slot can be reused for different guest ranges. Provenance is the chain (guest address → cache key → host resource id → bind → draw → resolve → present), never any single id in isolation. Any tool or report that prints a host texture id as "the asset" is wrong.
- **Optional fields are explicit.** An event field that was not captured is written as `null` in JSONL, never as 0. A zero in a field means "measured zero". This follows the `ProbeInfo`/`verdict` discipline already in RA: a zero must never be readable as "empty surface".

### 3.2 Event record (in-memory struct + JSONL schema)

```cpp
// include/rex/graphics/render_forensics.h  (new header; see Stage 1)
// Fixed-size, no heap allocation per event. ~128 bytes.

enum class ForensicsOp : uint8_t {
  kDraw = 0,          // H-DRAW-1/2/3/4
  kBind = 1,          // H-DRAW-7/8 (shader/texture/cbuffer binds)
  kClear = 2,         // H-CLEAR-1/2/3
  kResolve = 3,       // H-RES-2/3/4
  kResolveSkip = 4,   // H-RES-4 when skipped (Skip reason carried)
  kResolvePlanFail = 5, // H-RES-1 planning failure (RF001-RF008)
  kCopy = 6,            // H-COPY-1/2/3/5, H-RES-5 (staging copy)
  kCopyDown = 7,      // H-COPY-4 (untile copy-down, per surface)
  kResourceCreate = 8,  // H-RSC-1/3
  kResourceDestroy = 9, // H-RSC-2
  kUpload = 10,       // H-RSC-4/5/6/7/8
  kBarrier = 11,      // H-RSC-10
  kPresent = 12,      // H-PRES-1/2/3/4
  kFrameSummary = 13, // H-PRES-5 (one per frame, §3.5)
  kTransition = 14,   // H-TR-1 (per-swap transition snapshot)
};

struct ForensicsEvent {
  uint64_t frame_id;        // presented_output_frames_ at the time of the event
  uint64_t event_id;        // sequential within the frame, starting at 1
  uint64_t timestamp_ns;    // QueryPerformanceCounter, CPU time (not GPU time)
  ForensicsOp op;           // enum above
  uint32_t parent_event_id; // 0 = none; links a sub-event to its parent
                            // (e.g. per-rect batch -> resolve; per-surface
                            //  copy-down -> resolve; bind -> draw)
  // --- resources ---
  uint32_t dst_resource_id; // stable id of the render target / destination
  uint32_t src_resource_id; // stable id of the source surface / texture
  uint32_t tex_resource_ids[4]; // stable ids of textures bound by a draw
                            // (0 = slot unbound; more than 4 -> see tex_overflow)
  uint8_t  tex_count;       // how many of tex_resource_ids[] are valid
  uint8_t  tex_overflow;    // 1 if the draw bound more than 4 textures
  // --- guest side ---
  uint32_t guest_base;      // guest address (physical) of the resource's data
  uint32_t guest_extent;    // byte length of the guest range (0 = unknown)
  // --- geometry ---
  int32_t  rect[4];         // x0, y0, x1, y1 (guest rows for resolves;
                            // draw rect for clears; 0,0,0,0 if n/a)
  float    viewport[4];     // TopLeftX, TopLeftY, Width, Height (post-shift)
  int32_t  scissor[4];      // left, top, right, bottom (post-shift)
  int32_t  window_offset[2];// PA_SC_WINDOW_OFFSET x, y (guest, pre-shift)
  // --- shaders / state ---
  uint32_t vs_id;           // stable shader id, or 0 if none / not captured
  uint32_t ps_id;
  uint32_t cs_id;           // compute shader id (resolves)
  uint32_t blend_state_id;  // stable id of the D3D11 blend state object
  uint32_t depth_stencil_state_id;
  uint32_t rasterizer_state_id;
  uint8_t  primitive;       // xenos::PrimitiveType (draws only)
  // --- band / EDRAM ---
  uint32_t untile_band;     // band rows (0, 256, 512)
  uint32_t owner_before;    // EDRAM tile owner before this event (claims)
  uint32_t owner_after;     // EDRAM tile owner after this event
  uint32_t sample_select;   // resolve sample select, with (row_offset << 16)
  uint32_t source_row_offset; // = untile_band at resolve time
  // --- draw / dispatch params ---
  uint32_t index_count;
  uint32_t vertex_count;
  uint32_t instance_count;  // 1 if not instanced
  uint32_t group_count[3];  // compute dispatch dimensions (resolves)
  // --- resolve ---
  uint32_t dest_base;       // RB_COPY_DEST_BASE (guest)
  uint32_t dest_pitch;
  uint32_t dest_info;       // full info word (pack/endian/swap/exp bias)
  uint8_t  pack;            // dest_info & 7
  uint8_t  endian;          // (dest_info >> 3) & 7
  uint8_t  swap_rb;         // (dest_info >> 6) & 1
  uint8_t  copy_down_ran;   // 1 if EndUntiledTiling ran before this resolve
  // --- result ---
  int32_t  hr;              // HRESULT, or 0 if the call returns void
  uint8_t  hr_source;       // 0 = real HRESULT; 1 = void-return (assumed);
                            // 2 = not applicable
  char     detail[32];      // short tag: "draw", "resolve_batch",
                            // "copy_down", "skip:…", "RF003", …
};
```

JSONL schema (one object per line; field names match the struct; `null` where not captured):

```json
{"frame":2200,"event":1234,"ts_ns":123456789,"op":"draw","parent":0,
 "dst_rt":17,"src":0,"tex":[42,43,0,0],"tex_count":2,"tex_overflow":0,
 "guest_base":"0x1C4E1000","guest_extent":3768320,
 "rect":[0,256,1280,512],"viewport":[0,256,1280,256],"scissor":[0,256,1280,512],
 "window_offset":[0,-256],"vs":101,"ps":205,"cs":0,
 "blend":3,"depth_stencil":2,"rasterizer":1,"primitive":5,
 "untile_band":256,"owner_before":0,"owner_after":0,"sample_select":0,
 "source_row_offset":0,"index_count":1234,"vertex_count":0,"instance_count":1,
 "group_count":[0,0,0],"dest_base":0,"dest_pitch":0,"dest_info":0,
 "pack":0,"endian":0,"swap_rb":0,"copy_down_ran":0,"hr":0,"hr_source":1,
 "detail":"draw"}
```

Field rules:

- `frame` is `presented_output_frames_` (the same counter the existing traces use). It is a presented-frame counter, not a guest-frame counter; say so in every report.
- `event` restarts at 1 each frame. The pair (`frame`, `event`) is the unique key.
- `parent` links sub-events to the event that caused them. A resolve with three source rects is one `op: "resolve"` event (the parent) plus one `op: "resolve"` event per rect with `parent` set to the parent's `event`. A copy-down is one `op: "copy_down"` event per surface, with `parent` set to the resolve event that triggered it (or 0 if it ran at a frame boundary with no resolve in scope — that case must be recorded, because a copy-down with no triggering resolve is a transition anomaly).
- `tex` is capped at 4 entries. A draw binding more than 4 textures sets `tex_overflow: 1` and records the first 4; the full list is available from the `bind` event (H-DRAW-7), which carries the complete binding. The cap exists to keep the struct fixed-size; it is not a claim that draws bind at most 4 textures.
- `hr_source: 1` ("void-return") is mandatory on every draw event, because `ID3D11DeviceContext::Draw` / `DrawIndexed` return void. Any report that treats a draw's `hr: 0` as "the draw succeeded" is wrong; the draw's success is only observable through its effects (the next resolve / readback / present).
- `detail` is a short human-readable tag, not a free-form message. Long explanations go in the log, not in the event.

### 3.3 Resource provenance record

One record per host resource, created at the hook that creates it (H-RSC-1 / H-RSC-3) and updated at each event that reads or writes it. Stored in a bounded map in the recorder (not in the per-frame event list); the map is snapshotted into the capture file's header (§4).

```cpp
struct ForensicsResource {
  uint32_t resource_id;     // monotonic per run, starting at 1. Never reused
                            // within a run, even after destruction.
  uint8_t  kind;            // 0=texture, 1=surface(RT), 2=depth, 3=buffer,
                            // 4=uav, 5=swap texture
  uint32_t guest_base;      // guest physical address the resource was
                            // created from (0 = no guest backing)
  char     cache_key[64];   // TextureKey::Describe() or
                            // Fh1SurfaceKey::Describe() (truncated at 63)
  uint32_t width, height;
  uint32_t depth_or_array;  // depth for 3D, array size otherwise
  uint32_t guest_format;    // xenos format enum value
  uint32_t host_format;     // DXGI_FORMAT value
  uint32_t samples;         // MSAA sample count (1, 2, 4)
  uint32_t mip_count;
  uint32_t bind_flags;      // D3D11_BIND_* bitmask
  uint32_t usage;           // D3D11_USAGE value
  uint64_t created_frame;   // frame_id at creation
  uint64_t destroyed_frame; // 0 = still alive
  uint64_t last_writer_event; // (frame, event) of the last event that
  uint64_t last_reader_event; // wrote / read this resource, packed as
                              // frame<<32 | event. 0 = never.
  uint32_t write_count;     // number of events that wrote it
  uint32_t read_count;      // number of events that read it
};
```

Rules:

- **The resource id is assigned by the recorder, not by the backend.** The backend passes its own pointer/key; the recorder maps it to a stable id. Two different host objects never share an id. The same host object never gets a second id.
- **The cache key is the guest-side identity.** For textures it is the `TextureKey` (guest format, dims, tiling, mip range, `scaled_resolve`). For surfaces it is the `Fh1SurfaceKey` (base, pitch tiles, msaa, format). The cache key plus the guest base address is the closest thing to "the game asset" the system has — and even that is a cache entry, not the asset. The asset itself (the FH1 model/texture/level data) is identified only by the guest address range and the game's own data, which the backend does not know.
- **A host texture id does not identify the game asset.** Restated as a rule for every consumer of this data: if a report, a visualisation, or a log line prints a host texture id and calls it the asset, that report is wrong. Provenance is the chain. The chain is: guest range (base + extent) → cache key → host resource id → bind event → draw event → resolve event → present event. Every link is recorded; no link is inferred.
- **Reuse is explicit.** When `GetOrCreateSurface` (SF:69) returns an existing surface, the event records `reused: 1` and the existing resource id. When a texture cache entry is reused, same. A surface that survives a menu→gameplay transition keeps its resource id and its `created_frame` — that is how the forensics system distinguishes "reused across the transition" (same id, old `created_frame`) from "recreated at the transition" (new id, `created_frame` = transition frame). This distinction is the core of the defect #4 investigation.

### 3.4 Correlating the two execution paths

The immediate path and the recorded (tape) path must produce events that can be joined:

- **Record time vs replay time.** When `fh1_d3d11_command_lists` is on, a draw is recorded once (H-DRAW-4, `RecordDrawPacket`, CP:3358) and replayed once (H-DRAW-3, CLS:333/335). The record event and the replay event share a `tape_packet_id` (the packet's index in the tape, assigned by `D3D11CommandList::Append`). The record event has `op: "draw"` with `detail: "draw_record"`; the replay event has `op: "draw"` with `detail: "draw_replay"` and the same `tape_packet_id`. A draw that is recorded but never replayed (tape overflow, `D3D11CommandList::overflow_`) is visible as a record event with no matching replay event.
- **The immediate path has no tape.** Its draw events have `tape_packet_id: null`. A capture must state which path was active (`command_lists_enabled_` at capture time, recorded in the capture header, §4).
- **SubmitFrameTape boundaries.** `SubmitFrameTape(reason)` (CP:3244) is the point where the tape is flushed to the immediate context. Record a `copy` event with `detail: "tape_submit:<reason>"` at each `SubmitFrameTape` call, carrying the reason string. The reasons already in the code are the vocabulary: `"draw"`, `"resolve batch overflow"`, `"texture upload overflow"`, `"draw packet space"`, `"draw packet overflow"`, `"resolve clear ordering"`, `"resolve dispatch"`, `"A/B tape mirror"`, `"resolve staging copy"`, `"resolved range refresh"`, `"swap"`, `"untile copy-down ordering"`. Do not invent new reason strings; use the ones the code already emits.

### 3.5 Recorder: frame scope, ring buffer, per-frame summary

- **Frame scope.** The recorder holds one frame's events in a bounded ring buffer. The frame boundary is `IssueSwap` (CP:5395) / `render_audit::EndFrame` (RA). When the frame ends, the recorder:
  1. Emits a `frame_summary` event (see below).
  2. If the frame is inside the capture window (§4), flushes the frame's events to the capture file.
  3. Resets the ring buffer for the next frame (the buffer is reused, not reallocated).
- **Ring buffer.** Fixed capacity, default 16,384 events (see §7 for the sizing). When the buffer fills mid-frame, the oldest events are overwritten and a `dropped_events` counter is incremented; the frame summary reports `dropped_events`, and the capture file's summary marks the frame `complete: false`. Overwriting is preferred over allocating: the recorder must never grow the buffer at runtime.
- **Gating.** The recorder is active only when the capture cvars (§4) select the current frame. When inactive, every hook is a single branch on a cached bool (set at frame start) and returns immediately. No event struct is filled, no string is formatted, no file is touched.
- **Per-frame summary event** (`op: "frame_summary"`, one per frame, always emitted when the recorder is active for that frame):

```json
{"frame":2200,"event":9999,"op":"frame_summary",
 "draws":3120,"clears":48,"resolves":212,"copies":96,"uploads":340,
 "copy_down_ran":true,"copy_down_surfaces":2,
 "untile_bands_live":2,"surfaces_live":14,"textures_live":210,
 "frontbuffer_address":"0x1CE2D000","frontbuffer_changed":false,
 "dropped_events":0,"complete":true,
 "resolve_plan_failures":{"RF003":1},
 "present_hr":0}
```

  - `copy_down_ran` is false when `EndUntiledTiling` was not called in the frame. This field is what makes "no copy-down" distinguishable from "no data" (defect #4).
  - `untile_bands_live` is the size of the `untile_bands_` map at frame end (read at H-TR-1, EX:2517 `OnSwap`).
  - `frontbuffer_changed` is true when the frontbuffer address differs from the previous frame's. A change here is the observable signature of a menu↔gameplay transition.
  - `resolve_plan_failures` is a map from RF site id to count for the frame (empty object when none).

---

## 4. Capture format and example event record

### 4.1 Format choice: JSONL, extending `render_audit`

- **File format: JSONL (one JSON object per line), UTF-8, LF line endings.** Same as the existing audit (RA). Rationale: the existing tooling and the humans debugging this backend already read JSONL; the per-frame volume (§7) is small enough that JSON parsing is not a bottleneck; and a text format lets a broken capture still be partially read (a corrupt line does not lose the file).
- **File location:** `E:/PinyonShift/work/.local/audit/<backend>-forensics-<utc>.jsonl`, next to the existing audit files. The path is fixed (not a cvar) so captures are always findable in the same place.
- **File structure, in order:**
  1. One `header` record (first line): backend, adapter, capture cvars, command-list path active or not, ring-buffer capacity, recorder version, capture window, and the resource-provenance snapshot (the `ForensicsResource` map as of capture start — see §3.3). The header is the only record that carries the resource map; events carry resource ids that resolve against it.
  2. One record per event, in `event_id` order, grouped by frame (frames in increasing order).
  3. One `capture_summary` record (last line): frames captured, events per frame, total events, dropped events, whether any frame was truncated, and the per-frame `complete` flags. Same shape as RA `WriteCaptureSummary`.
- **New cvars (all under the "GPU" group, all `kRequiresRestart` unless noted):**

| Cvar | Type / default | Meaning |
|------|---------------|---------|
| `fh1_forensics_capture` | bool / false | Master switch. When false, every hook is a no-op (single cached branch). |
| `fh1_forensics_from` | int32 / 0 | First presented-frame index to capture (0 = disabled). |
| `fh1_forensics_to` | int32 / 0 | Last presented-frame index to capture. `from == to == 0` captures nothing. A single-frame capture is `from == to == N`. |
| `fh1_forensics_event_mask` | int32 / -1 | Bitmask over `ForensicsOp` values (§3.2). −1 = all events. 0 = frame summaries only. Lets a capture exclude the highest-volume event type (draws) while keeping resolves and clears. |
| `fh1_forensics_ring_events` | int32 / 16384 | Ring-buffer capacity in events. Capped at 262,144 by the recorder (see §7). |
| `fh1_forensics_include_binds` | bool / false | Whether `bind` events (H-DRAW-7/8) are recorded. Off by default: binds are the highest-volume event type after draws, and they are only needed when the question is "which shader/texture was bound". |
| `fh1_forensics_include_uploads` | bool / false | Whether `upload` events (H-RSC-4/5/6/7/8) are recorded. Off by default. |

- **Capture window semantics (same as `pinyon_audit_passes`):** frames outside [`from`, `to`] are not recorded, and the expensive hooks (binds, uploads) are only evaluated for frames inside the window. The window is checked once at frame start (in `IssueSwap`, CP:5395) and cached for the frame, so per-event gating is a single bool test.
- **Truncation rule:** if a frame's events exceed the ring capacity, the oldest events are dropped, `dropped_events` is counted, and the frame's summary marks `complete: false`. The capture summary also marks the whole capture `complete: false`. There is no silent truncation.

### 4.2 Example: one Band 2 draw and its resolve

Real-shaped example, using values consistent with the measured Baseline run (frame ~2200; Band 2 = middle rows 256–512; 4x MSAA scene surface 640x1024; front buffer 1280x720; guest front-buffer base `0x1CE2D000` as seen in the swap trace). Field values that are illustrative rather than measured are marked with a comment in the prose below the record, not inside the JSON.

```json
{"kind":"header","backend":"d3d11","adapter":"GTX 570","recorder_version":1,"capture_from":2200,"capture_to":2200,"event_mask":-1,"command_lists":false,"ring_events":16384,"resources":[{"id":17,"kind":"surface","guest_base":"0x00000000","cache_key":"0/32/4x/c3","width":640,"height":1024,"depth_or_array":1,"guest_format":12,"host_format":10,"samples":4,"mip_count":1,"bind_flags":40,"usage":0,"created_frame":512,"destroyed_frame":0,"write_count":0,"read_count":0},{"id":42,"kind":"texture","guest_base":"0x1C4E1000","cache_key":"tex:640x360:8_8_8_8:tiled","width":640,"height":360,"depth_or_array":1,"guest_format":6,"host_format":28,"samples":1,"mip_count":1,"bind_flags":8,"usage":0,"created_frame":1800,"destroyed_frame":0,"write_count":0,"read_count":0}]}
{"frame":2200,"event":1,"ts_ns":881234567890,"op":"draw","parent":0,"dst_rt":17,"src":0,"tex":[42,0,0,0],"tex_count":1,"tex_overflow":0,"guest_base":"0x00000000","guest_extent":0,"rect":[0,256,1280,512],"viewport":[0.0,256.0,1280.0,256.0],"scissor":[0,256,1280,512],"window_offset":[0,-256],"vs":101,"ps":205,"cs":0,"blend":3,"depth_stencil":2,"rasterizer":1,"primitive":5,"untile_band":256,"owner_before":0,"owner_after":0,"sample_select":0,"source_row_offset":0,"index_count":1234,"vertex_count":0,"instance_count":1,"group_count":[0,0,0],"dest_base":0,"dest_pitch":0,"dest_info":0,"pack":0,"endian":0,"swap_rb":0,"copy_down_ran":0,"hr":0,"hr_source":1,"detail":"draw"}
{"frame":2200,"event":2,"ts_ns":881234568100,"op":"bind","parent":1,"dst_rt":17,"src":0,"tex":[42,0,0,0],"tex_count":1,"tex_overflow":0,"guest_base":"0x1C4E1000","guest_extent":921600,"rect":[0,0,0,0],"viewport":[0.0,256.0,1280.0,256.0],"scissor":[0,256,1280,512],"window_offset":[0,-256],"vs":101,"ps":205,"cs":0,"blend":3,"depth_stencil":2,"rasterizer":1,"primitive":0,"untile_band":256,"owner_before":0,"owner_after":0,"sample_select":0,"source_row_offset":0,"index_count":0,"vertex_count":0,"instance_count":0,"group_count":[0,0,0],"dest_base":0,"dest_pitch":0,"dest_info":0,"pack":0,"endian":0,"swap_rb":0,"copy_down_ran":0,"hr":0,"hr_source":2,"detail":"bind"}
{"frame":2200,"event":3,"ts_ns":881234570000,"op":"resolve","parent":0,"dst_rt":0,"src":17,"tex":[0,0,0,0],"tex_count":0,"tex_overflow":0,"guest_base":"0x1CE2D000","guest_extent":3768320,"rect":[0,0,1280,256],"viewport":[0.0,0.0,1280.0,256.0],"scissor":[0,0,1280,256],"window_offset":[0,0],"vs":0,"ps":0,"cs":301,"blend":0,"depth_stencil":0,"rasterizer":0,"primitive":0,"untile_band":0,"owner_before":0,"owner_after":0,"sample_select":3,"source_row_offset":0,"index_count":0,"vertex_count":0,"instance_count":0,"group_count":[80,16,1],"dest_base":484966400,"dest_pitch":5120,"dest_info":6,"pack":6,"endian":0,"swap_rb":0,"copy_down_ran":1,"hr":0,"hr_source":0,"detail":"resolve"}
{"frame":2200,"event":4,"ts_ns":881234570200,"op":"copy_down","parent":3,"dst_rt":17,"src":17,"tex":[0,0,0,0],"tex_count":0,"tex_overflow":0,"guest_base":"0x00000000","guest_extent":0,"rect":[0,0,1280,208],"viewport":[0.0,0.0,0.0,0.0],"scissor":[0,0,0,0],"window_offset":[0,0],"vs":0,"ps":0,"cs":0,"blend":0,"depth_stencil":0,"rasterizer":0,"primitive":0,"untile_band":512,"owner_before":0,"owner_after":0,"sample_select":0,"source_row_offset":0,"index_count":0,"vertex_count":0,"instance_count":0,"group_count":[0,0,0],"dest_base":0,"dest_pitch":0,"dest_info":0,"pack":0,"endian":0,"swap_rb":0,"copy_down_ran":0,"hr":0,"hr_source":0,"detail":"copy_down"}
{"frame":2200,"event":9999,"op":"frame_summary","draws":3120,"clears":48,"resolves":212,"copies":96,"uploads":340,"copy_down_ran":true,"copy_down_surfaces":2,"untile_bands_live":0,"surfaces_live":14,"textures_live":210,"frontbuffer_address":"0x1CE2D000","frontbuffer_changed":false,"dropped_events":0,"complete":true,"resolve_plan_failures":{},"present_hr":0}
{"kind":"capture_summary","frames":1,"events":9999,"dropped_events":0,"complete":true,"frames_truncated":[]}
```

Reading the example:

- **Event 1 (draw):** a Band 2 draw. `window_offset: [0,-256]` is the guest register; `viewport.TopLeftY = 256` and `scissor top = 256` are the post-shift host values (the untile shift, CP:1153-1221). `untile_band: 256` matches. `dst_rt: 17` is the expanded 640x1024 4x surface (resource 17 in the header, `cache_key: "0/32/4x/c3"`, `height: 1024` = the expanded surface, not 512). `tex: [42]` is the 640x360 texture the draw sampled, whose guest base `0x1C4E1000` is in the bind event. `hr_source: 1` records that the draw's HRESULT is void-return (assumed), per §3.2.
- **Event 2 (bind):** the same draw's bindings, linked by `parent: 1`. It carries the texture's guest base and extent (`0x1C4E1000`, 921,600 = 640x360x4) — the link from the host texture (id 42) back to the guest range a resolve wrote.
- **Event 3 (resolve):** a band-0 resolve. `sample_select: 3` is the plan's sample select with `(source_row_offset << 16) = (0 << 16)` — band 0, so `source_row_offset: 0`. A band-2 resolve would carry `sample_select: 3 | (256 << 16)` = 16,777,219. `dst_rt: 0` because the resolve's destination is the guest range (`dest_base: 484966400` = `0x1CE2D000`, the guest front buffer), not a host render target; `src: 17` is the source surface. `dest_info: 6` decodes to pack 6, endian 0, swap 0. `group_count: [80,16,1]` is the compute dispatch. `copy_down_ran: 1` — the copy-down ran before this band-0 resolve (the field that matters for defect #4). `cs: 301` is the resolve compute shader id.
- **Event 4 (copy_down):** the per-surface copy-down, linked to the resolve by `parent: 3`. `untile_band: 512` is the band that was copied down; `rect: [0,0,1280,208]` is the rows copied (208 = 720 − 512).
- **Frame summary:** `untile_bands_live: 0` at frame end (the map was drained by the copy-down — the expected steady state). If this field were non-zero at frame end, that would be the anomaly to investigate for defect #4.

Illustrative values (not measured, and the implementer must not treat them as ground truth): the exact `index_count` (1234), the shader ids (101/205/301), the state ids (1/2/3), the event counts in the frame summary (3120 draws, 212 resolves), and the `group_count` (80,16,1). The measured values the example is consistent with: Band 2 = 1,138 packets / 900 draws in the Baseline run; the 4x scene surface is 640x1024; the front buffer is 1280x720; the guest front-buffer base `0x1CE2D000` appears in the swap trace; band rows are 0–256 / 256–512 / 512–720.

---

## 5. Proposed visual debug modes

General rules for all modes:

- Every mode is gated by its own cvar (named `fh1_forensics_viz_*`, all under the "GPU" group). When all viz cvars are off, the mode code is not executed at all — no state is changed, no shader is swapped, no extra draw is issued.
- Modes are implemented as **overrides applied at the draw call**, not as modifications to the guest's shaders or to the scene. The override replaces the pipeline state the backend would otherwise bind (rasterizer state, pixel shader, or blend state) for the duration of the capture frame, then restores it. The guest's own shader bytecode is never modified.
- Modes that need a replacement pixel shader use a **small set of precompiled debug shaders** (HLSL, SM 5.0, compiled at backend startup when the first viz cvar is enabled — not at game startup, so the normal path pays nothing). The debug shaders are: (a) a solid-colour shader that takes its colour from a constant buffer; (b) a draw-id shader that outputs the draw's sequential id encoded as RGB; (c) an overdraw shader that outputs a fixed low alpha with additive blending (see §5.4); (d) a depth visualisation shader that samples a depth texture and outputs a colour ramp.
- Modes that need a readback (overdraw heatmap, depth viz, RT before/after) do the readback **only on the captured frame** and only for the named surface, through the existing staging-readback path (the same mechanism `fh1_debug_surface_dump` and the c2 probe use). They never read back every frame.
- **No mode changes the normal frame when disabled.** Verification for each mode (Stage 5) is: capture a frame with the mode off, capture the same scene with the mode on and then off again, and confirm the "off" frames are identical (same presented PPM hash as `fh1_debug_presented_pair` produces).

### 5.1 Wireframe rendering

- **What it shows:** the triangle structure of the scene — whether geometry exists in a region where the screen is blank, and whether overlapping geometry is actually overlapping triangles or a single triangle drawn wrong.
- **Implementation:** at `IssueDraw` (CP:1733), when `fh1_forensics_viz_wireframe` is on, replace the rasterizer state with a pre-created `D3D11_RASTERIZER_DESC` with `FillMode = D3D11_FILL_WIREFRAME` and the same cull mode / depth-clip settings as the draw's own rasterizer state. The replacement happens in `UpdateFixedFunctionState` (CP:1133), where the rasterizer state is already selected — that is the single place that sets `RSSetState` for draws, so the override is one branch there. The pixel shader is unchanged (so the wireframe is coloured by the guest shader, which is fine — the shape is what matters).
- **Isolation:** rasterizer state only; no shader, blend, or render-target change. Restored automatically because the next draw re-selects its own state through the same function.
- **Limitation:** the resolve is a compute dispatch, not a draw — wireframe does not apply to resolves. The mode answers "what geometry was submitted", not "what the resolve did with it".

### 5.2 Draw-call ID visualisation

- **What it shows:** which draw call produced each pixel. The screen is painted with one flat colour per draw (the draw's sequential id encoded as RGB). Two regions with the same colour came from the same draw; a region whose colour does not match any draw in the event stream came from a draw the recorder missed (a hook gap) or from a non-draw operation (a resolve, a copy, a clear).
- **Implementation:** at `IssueDraw` (CP:1733), when `fh1_forensics_viz_draw_id` is on, replace the pixel shader with the debug draw-id shader and set its constant buffer to the draw's `event_id` (the same id the recorder assigns). The vertex shader, viewport, scissor, and render targets are unchanged. The colour encoding is `R = (id & 0xFF) / 255`, `G = ((id >> 8) & 0xFF) / 255`, `B = ((id >> 16) & 0xFF) / 255` — up to 16.7M draws per frame, far above the observed ~3,000. The frame is then captured with the existing surface-dump path (`fh1_debug_surface_dump`, CP:5814) or the presented-pair path (CP:5727), and the colour at a pixel is decoded back to a draw id offline.
- **Isolation:** pixel shader + one constant buffer only. The draw still writes to the same render target with the same depth/stencil state, so depth testing and occlusion behave identically — only the colour changes. The mode is useless for colour debugging (it replaces the colour), which is why it is a separate mode from normal rendering and never on by default.
- **Limitation:** it shows the *last* draw that wrote each pixel (depth-tested), not all draws that touched it. For the full list, use the overdraw heatmap (§5.4) or the region-history query (§6).

### 5.3 Resource ID visualisation

- **What it shows:** which resource (render target or texture) a pixel's colour came from. Same mechanism as §5.2, but the constant buffer carries the bound texture's stable resource id (§3.3) instead of the draw id.
- **Implementation:** same hook point as §5.2 (`IssueDraw`, CP:1733), gated by `fh1_forensics_viz_resource_id`. The debug shader outputs the first bound texture's resource id encoded as RGB (same encoding). If the draw binds no texture, it outputs black (id 0) — black pixels are therefore "no texture bound", which is itself diagnostic (a draw with no texture that should have one).
- **Isolation:** same as §5.2.
- **Limitation:** only the first bound texture is shown (the `tex[0]` slot). A draw sampling several textures shows only the first; the full binding list is in the `bind` event (§2.1, H-DRAW-7).

### 5.4 Overdraw heatmap

- **What it shows:** how many times each pixel was written. Bright = many overlapping draws. This is the direct test for defect #2 (overlapping geometry): if a region is bright in the heatmap, the overlap is real (many draws); if it is dark, the "overlap" is a single draw's output being misplaced (a resolve/viewport error, not overdraw).
- **Implementation:** two parts. (a) At `IssueDraw` (CP:1733), when `fh1_forensics_viz_overdraw` is on, replace the pixel shader with the debug overdraw shader (outputs a constant colour with alpha 1/255) and replace the blend state with additive blending (`BlendEnable = TRUE`, `SrcBlend = ONE`, `DestBlend = ONE`, `RenderTargetWriteMask = ALL`). Each draw then adds 1/255 to the colour channels. (b) At frame end (in `IssueSwap`, CP:5395), read back the render target once through the staging path and write it as a PPM (same path as `fh1_debug_surface_dump`). The pixel value / 255 (per channel, clamped at 255 = 255+ overlapping draws) is the overdraw count. A separate UAV-counter implementation is **rejected**: FL 11_0 does not guarantee UAV counters on the render path, and the additive-blend method needs no new GPU feature.
- **Isolation:** pixel shader + blend state only, and only on the captured frame. The additive blend changes the frame's colours (that is the point of the mode), so the captured frame is not a normal frame — the mode is never on during normal play.
- **Cost:** one extra blend-state change per draw (negligible) + one readback at frame end (see §7). The 1/255 step means counts above 255 saturate; that is acceptable (255 overlapping draws in one pixel is already conclusive).

### 5.5 Depth-buffer visualisation

- **What it shows:** the depth values in the scene, as a colour ramp. Tests whether a missing region is missing because its geometry failed the depth test (depth is wrong) or because it was never drawn (depth is clear value).
- **Implementation:** the D3D11 depth-stencil view cannot be sampled directly. The mode copies the depth buffer to a shader-readable texture at frame end: `CopyResource` from the executor's depth surface into a `DXGI_FORMAT_R32_FLOAT` texture created for this purpose (the executor's depth is D24FS8; the copy goes through the existing resolve/readback staging path — the same mechanism the MSAA probe uses, `pinyon_msaa_probe`). The debug depth shader then samples that texture and outputs a ramp (near = red, far = blue, clear value = black). Gated by `fh1_forensics_viz_depth`. The copy is done once per captured frame, at `IssueSwap` (CP:5395), before the present.
- **Isolation:** one copy + one debug draw at frame end. No change to the depth buffer itself.
- **Limitation:** the copy shows the depth at frame end (after all draws), not the depth at the moment a particular draw ran. A draw that failed the depth test early and a draw that was later overwritten look the same. For per-draw depth, use the region-history query (§6) with the draw events.

### 5.6 Stencil inspection (where supported)

- **What it shows:** the stencil values the transfer passes use (the executor's EDRAM transfers use stencil bits to mark ownership — `GetTransferStencilState`, EX:1740).
- **Implementation:** same copy mechanism as §5.5, but the stencil byte is extracted (D24FS8 packs stencil in the high 8 bits of the 32-bit value; the debug shader masks and shifts). Gated by `fh1_forensics_viz_stencil`. The stencil values are small integers (ownership tags), so the shader outputs them as distinct flat colours (value → hue), not a ramp.
- **Limitation:** stencil is only meaningful on the surfaces the executor uses for transfers. On other surfaces the stencil is clear (0) and the mode shows black. The mode is an ownership diagnostic, not a general stencil viewer.

### 5.7 Texture and UV inspection

- **What it shows:** for a selected draw, the texture it sampled and the UVs it used — i.e. whether incorrect texture placement (defect #6) is a wrong texture, a wrong UV, or a wrong viewport.
- **Implementation:** two existing tools already cover most of this, and the mode wires them to the event stream rather than inventing a new one. (a) `fh1_debug_texture_dump` (FL) already writes the largest frame-sized sampled texture as a PPM at each world-rectangle draw. (b) `fh1_debug_vertex_dump` (FL) already logs each vertex the shader fetches with positions mapped through the viewport. The forensics addition is a cvar `fh1_forensics_viz_uv` that, when on, makes those two dumps fire for the draws named by the event stream (by `event_id` range) instead of by frame index — so the operator picks "draw 1234" from the capture and gets that draw's texture + vertices, rather than guessing a frame. No new GPU work; the dumps already exist.
- **Isolation:** the dumps are readbacks gated by the cvar; when off, nothing happens.

### 5.8 Band ownership visualisation

- **What it shows:** which EDRAM tiles belong to which owner, and which band each tile was written by — as an image. This is the direct visualisation of the tiling system: a correct frame shows three clean horizontal bands (sky / world / cockpit) with no overlap; a broken frame shows bands overlapping or a band missing.
- **Implementation:** CPU-side, no GPU work. At frame end (in `IssueSwap`, CP:5395), when `fh1_forensics_viz_band_owner` is on, the recorder reads the `Fh1EdramTiles` ownership map (the same map `NotePassClaim` already reads, EX:802) and the `untile_bands_` map (EX:2589), and writes a PPM where each 16-row band of the image is coloured by (owner id → hue, band → brightness). The image is 1280x720 (one pixel per guest pixel column, one row per 16 guest rows is too coarse — instead each output row is one guest row, and the colour is the owner of the tile containing that row's first column; the exact mapping is fixed in Stage 5 and documented there). The existing `fh1_debug_surface_dump` already writes the ownership histogram as text; this mode is its image form.
- **Isolation:** pure CPU + file write at frame end. No GPU state is touched.
- **Limitation:** the ownership map is per-tile, not per-pixel. Two objects in the same tile share a colour. The mode shows tile ownership, not pixel ownership.

### 5.9 Render-target contents before and after resolve

- **What it shows:** the render target's pixels immediately before a resolve runs and immediately after — i.e. whether the resolve changed the data, corrupted it, or left it alone. This is the direct test for defect #7 (resolve corruption).
- **Implementation:** at `IssueCopy` (CP:3614), when `fh1_forensics_viz_rt_before_after` is on and the resolve is one of the three large colour resolves (the same selection rule `pinyon_resolve_trace` uses — "the next three large D3D11 FH1 resolves"), the recorder reads back the source surface *before* the resolve dispatch (through the existing staging path) and again *after* `WaitForGpu()` confirms the dispatch completed, and writes both as PPMs with the resolve's `event_id` in the filename. The readback reuses the `ResolveToMemory` staging buffer (RS:281) — no new staging resource. The before/after pair is written only for the selected resolves (max 3 per capture, same cap as `pinyon_resolve_trace`).
- **Isolation:** two readbacks per selected resolve, only on the captured frame, only for the selected resolves. The readbacks are synchronised (`WaitForGpu`, CP:5374) so they do not overlap the dispatch — that serialisation is the cost (§7) and is why the mode is capped at 3 resolves.
- **Limitation:** the "before" readback is of the *source surface* (the EDRAM surface), and the "after" readback is of the *destination* (the guest-memory mirror). They are different resources in different formats; the comparison is therefore "did the destination receive the source's content", not a pixel-for-pixel diff of the same buffer. A byte-exact diff is only meaningful after the format conversion is accounted for, which the offline analysis must do (the capture records both formats in the event).

---

## 6. Pixel / region history strategy

### 6.1 The honest limitation (read this first)

**Exact per-pixel history — the ordered list of GPU operations that wrote a specific pixel, with the value each one wrote — cannot be obtained on D3D11 Feature Level 11_0 without replay or GPU instrumentation that this hardware does not provide.**

Reasons, stated plainly:

- D3D11.0 has no per-pixel shader-invocation history, no fragment-shader breakpoint, and no built-in "what wrote this pixel" query. PIX and RenderDoc provide pixel history on PC by *replaying* the frame's command stream with instrumentation, not by reading it off the live GPU.
- The backend's draws are issued to the immediate context and the results live in GPU memory. Once a later draw overwrites a pixel, the earlier value is gone. No API call recovers it from the live frame.
- The resolve is a compute dispatch that reads a surface and writes guest memory. Its output overwrites the mirror. The mirror holds the last write only.

**Therefore the system does NOT invent pixel history from draw-call metadata alone.** A list of draws whose viewport/scissor *could* have covered a pixel is not a history of that pixel. The system reports exactly what it can prove, and labels the rest as inference.

### 6.2 What the system CAN prove (the reliable alternative)

The system provides three layers, in order of reliability. Each layer states what it proves and what it does not.

**Layer 1 — Event-region overlap query (no GPU work, always available from a capture).**

Given a captured frame (JSONL) and a query rectangle (x0,y0,x1,y1 in guest 1280x720 pixels), the offline analysis (a Python script shipped with the system, Stage 6) returns, in `event_id` order, every event whose recorded rectangle *could* have modified the query rectangle:

- Draws: the draw's scissor rect (post-shift) intersected with the query rect is non-empty. The draw's viewport, render target, shaders, textures, and blend/depth state are listed. **Proves:** the draw was submitted, its state, and that its scissor covered the region. **Does not prove:** the draw actually wrote any pixel in the region (its geometry may not cover the region, its pixels may have failed the depth test, or it may have been fully occluded).
- Resolves: the resolve's rect intersected with the query rect is non-empty. **Proves:** the resolve read the source surface over that rect and wrote the destination range. **Does not prove:** the source surface held meaningful data over that rect (use the surface dump / nonzero share for that).
- Clears: the clear's rect intersected with the query rect is non-empty. **Proves:** the clear ran over that rect with that clear value. This is the strongest statement in the system — a clear unconditionally writes its rect.
- Copy-downs: the copy-down's rect intersected with the query rect is non-empty. **Proves:** the band rows were copied down.

The query output labels each event as `covers` (rect intersects) or `writes` (only clears and copy-downs are `writes`; draws and resolves are `covers`). The distinction is in the output, not hidden.

**Layer 2 — Before/after readback for a selected region (gated, single-frame, bounded).**

For a selected rectangle, the system extends the existing `fh1_debug_c2_probe` mechanism (FL) — which already does before/control/after readbacks on up to 24 draws — to a named rectangle:

- The operator names a rectangle (cvar `fh1_forensics_region` = `x0,y0,x1,y1`) and a frame (`fh1_forensics_from`/`_to`).
- At the captured frame, the recorder reads back the named surface region *before* the first draw that could touch it and *after* each draw that could touch it (up to a cap of 24 draws, same as the c2 probe), using the staging path.
- The output is, per draw: the share of pixels in the region that changed, the bounding box of the change, and the change per 64-pixel column and 60-pixel row (the same statistics the c2 probe already emits).
- **Proves:** which draws actually changed pixels in the region, and by how much. **Does not prove:** the value each draw wrote in isolation if two draws changed the same pixels in the same interval (the readback is per draw boundary, so consecutive draws are separated — but if a draw is skipped by the cap, the change is attributed to the next captured draw, and the output says so).

**Layer 3 — Draw-ID re-render (the actual per-pixel attribution, via replay).**

This is the only layer that gives a true per-pixel answer ("draw N wrote this pixel"), and it does so by re-rendering, not by inspecting the live frame:

- Capture a frame with the event stream (Layer 1) and with the tape recorded (`fh1_d3d11_command_lists` on, so the frame's draws exist as replayable packets).
- Offline (or in a second run with the same capture), replay the tape with the draw-ID pixel shader (§5.2) bound instead of the guest pixel shader. The resulting image's colour at each pixel decodes to the draw id that last wrote it.
- **Proves:** for each pixel, the last draw that wrote it (depth-tested). **Does not prove:** earlier draws that wrote the same pixel and were overwritten, or the values they wrote. For those, combine with Layer 2 (the per-draw change statistics).
- **Limitation:** replay requires the tape path, which is off by default and "still being wired" (FL). If the tape path cannot replay a frame faithfully, Layer 3 is unavailable for that frame and the system says so in the capture summary (`replay_available: false`). Layer 3 is therefore a Stage 6+ capability, not a Stage 1 capability.

### 6.3 What the system will NOT do

- It will not present a draw list as a pixel history. Every query output carries a `provenance` field per event: `covers`, `writes`, or `changed` (Layer 2), and a header stating the layer used.
- It will not attribute a pixel to a draw based on viewport/scissor alone. Viewport/scissor is a necessary condition, not a sufficient one.
- It will not read back the framebuffer per draw in normal operation. The Layer 2 readbacks are capped (24 draws, one region, one frame) and gated by cvars.

### 6.4 Using the strategy on the known defects

| Defect | Query | What the answer looks like |
|--------|-------|----------------------------|
| #4 black block on menu exit | Region = the black rectangle. Frames = the last menu frame and the first gameplay frame. | Layer 1: list every clear/copy-down/resolve whose rect intersects the block, in order, across the transition. If no clear and no copy-down intersect the block on the gameplay frame, and the block's rows still hold the menu frame's data (surface dump nonzero share), the stale-data theory is confirmed. If a clear *did* run over the block, the clear is the cause and the question becomes why nothing redrew it. |
| #5 blue tint fading | Region = a blue area (e.g. the sky). Frames = several consecutive gameplay frames. | Layer 1 + surface dump: per-frame nonzero share and mean colour of the scene surface per band. If the blue fades as the nonzero share rises, the tint is stale/partial surface data. If the nonzero share is already 100% and the colour is still blue, the tint is a format/swizzle/gamma issue (fetch swizzle or `guest_output.hlsl` gamma), and the swap-fetch trace (`fh1_swap_fetch_trace`) is the next evidence. |
| #6 incorrect texture placement | Region = the misplaced texture. | Layer 1: the draws covering the region, with their sampled textures (guest base, size, format) and viewports. Layer 2: which draw actually changed the region. If the texture's guest base matches a resolve's destination, the texture is the resolved scene (correct); if it matches a different range, the placement error is in the fetch or the upload. |
| #7 resolve corruption | Region = the corrupted area. | Layer 1: the resolves covering the region, with their `dest_info` (pack/endian/swap), `sample_select`, and `source_row_offset`. Layer 3 (§5.9): the before/after PPMs for the resolve. If the before image is correct and the after image is corrupt, the resolve dispatch is the corrupting step; if the before image is already corrupt, the corruption is upstream (a draw or a copy-down). |
| #2 overlapping geometry | Region = the overlap. | §5.4 overdraw heatmap (bright = real overlap) + Layer 1 draw list. If the heatmap is dark but the image shows overlap, the "overlap" is one draw's output in the wrong place (a band/viewport error), not multiple draws. |

---

## 7. Performance cost estimates

All estimates are for the GTX 570 / 16 GB machine, 1280x720, target 30 FPS. Measured baselines (2026-11-15): free-roam 8.0–17.7 FPS with no traces (single CPU decoder thread ~76% of frame — the bottleneck is the CPU decoder, not the GPU); ~4 FPS with all trace/audit cvars on. The forensics system is therefore designed to add as little CPU work as possible, because CPU time is the scarce resource.

### 7.1 Event volume (measured anchors)

| Quantity | Measured / observed | Source |
|----------|--------------------|--------|
| Band 2 packets in one frame | 1,138 | Baseline run, frame ~2200 |
| Band 2 D3D11 draws in one frame | 900 | Same run (1,132 predicate-accepted, 6 rejected) |
| Total scene draws per frame (3 bands + composite) | ~3,000–5,000 (estimate) | 900/band × 3 bands + menu/composite draws; the 17k/30k/44k figures from the tile analysis are per-band *object/packet* counts, not D3D11 draws — do not use them as the draw count |
| Resolves per frame | Not directly measured; order of 100–300 dispatch calls (owner-split batches multiply the count) | `pinyon_resolve_trace` captures "the next three large resolves" out of many; `fh1_debug_resolve_trace` caps at 4,000 lines/frame |
| Clears per frame | Order of 10–100 (resolve clears + executor clears) | Inferred from the resolve-clear path (EX:2365) being called per resolve batch |
| Texture uploads per frame | Order of 100–400 | Inferred from the texture-cache upload path (TC:524) and the composite's texture sampling |
| Tape packets per frame (if command lists on) | Same order as draws + dispatches + copies (one packet per operation) | `D3D11CommandList` (CL) |

### 7.2 Per-event cost

| Item | Cost | Basis |
|------|------|-------|
| One `ForensicsEvent` struct | 128 bytes (fixed, §3.2) | Struct size; no heap allocation per event |
| Filling one event (hook) | ~100–200 ns CPU | Field copies from values already in scope + one `QueryPerformanceCounter`. No string formatting, no file I/O, no GPU call in the hook. |
| One frame's events, draws+resolves+clears+copies only (mask excludes binds/uploads) | ~4,000–6,000 events × 128 B = **512–768 KB per frame** | Volume table above |
| One frame's events, all types (binds + uploads included) | ~8,000–12,000 events × 128 B = **1.0–1.5 MB per frame** | Binds roughly double the draw count; uploads add ~10% |
| Ring buffer, default 16,384 events | **2 MB** | 16,384 × 128 B. Fits easily; the cap of 262,144 events = 32 MB, which is the maximum the recorder will allocate and is only reached if the operator raises `fh1_forensics_ring_events`. |
| JSONL serialisation (capture flush, once per captured frame) | ~400–600 bytes per event on disk → **~2–3 MB per captured frame** (draws+resolves+clears+copies) or **~4–7 MB** (all types) | JSON field names dominate; the binary struct is 128 B but the JSONL form is ~4× larger. This is why captures are single-frame or short-window, not continuous. |
| Capture file for a 10-frame window | **~20–70 MB** | 10 × the per-frame figures above. Acceptable on E: (1,115 GB free). Not acceptable on C: (3.9 GB free as of 2026-11-15) — captures write to `E:/PinyonShift/work/.local/audit/` (RA already writes there). |

### 7.3 Per-feature cost (when enabled, on the captured frame only)

| Feature | CPU cost | GPU cost | Frame-time impact (estimate) | Notes |
|---------|----------|----------|------------------------------|-------|
| Recorder hooks, all types, capture window active | ~1–2 ms/frame | 0 | ~3–6% at 30 FPS target; ~1–2% at the current 8–17 FPS (CPU-bound) | 10,000 events × 150 ns = 1.5 ms. This is the main recurring cost and the reason the recorder is gated to the capture window. |
| Recorder hooks, capture window inactive | ~0 (one cached branch per hook) | 0 | Unmeasurable | The "off" state must be verified in Stage 1 (see §8). |
| Frame summary + JSONL flush | ~1–3 ms, once per captured frame | 0 | One-off per captured frame | File I/O only; the flush happens at the frame boundary (`IssueSwap`). |
| Resource provenance map (per-frame snapshot in header) | ~0.1 ms | 0 | Negligible | Only in the header, not per frame. Map size is bounded by live resources (~200–400 entries). |
| Wireframe (§5.1) | ~0 | ~0 (same draw count, different fill) | Negligible | Rasterizer state change only. |
| Draw-ID viz (§5.2) / resource-ID viz (§5.3) | ~0.05 ms (one constant-buffer update per draw) | ~0 | Negligible | One extra `UpdateSubresource` per draw for the id constant buffer. |
| Overdraw heatmap (§5.4) | ~0 | ~0 + one readback at frame end (~2–5 ms) | Readback only | The readback is a full 1280x720 staging copy + map; ~2–5 ms on the GTX 570 (measured readback cost for the front buffer is in the same range). |
| Depth viz (§5.5) / stencil (§5.6) | ~0 | One depth copy + one debug draw + one readback (~3–6 ms) | Readback + copy only | |
| Band ownership viz (§5.8) | ~1 ms CPU (map walk + PPM write) | 0 | ~1 ms | CPU only. |
| RT before/after (§5.9) | ~0 | Two readbacks per selected resolve (~4–10 ms total for 3 resolves) | The most expensive viz mode | Capped at 3 resolves per capture; each readback serialises on `WaitForGpu`. |
| Region history Layer 2 (§6.2) | ~0 | One readback per probed draw boundary, up to 24 draws (~50–120 ms for the whole capture) | **The capture runs at ~1 FPS while Layer 2 is active** | Same cost profile as the existing c2 probe, which is why it is capped at 24 draws and one region. This is a capture-time cost, not a gameplay cost. |
| Region history Layer 1 (§6.2) | 0 at capture time; ~10–50 ms offline (Python) | 0 | 0 | Pure offline analysis of the JSONL. |

### 7.4 What the system must NOT do (cost ceilings)

- **No continuous logging.** The recorder is off outside the capture window. A "log everything, all the time" mode is explicitly rejected: at ~500 KB/frame of events, continuous logging at 15 FPS would write ~7.5 MB/s and fill the 5 MB `runtime.log` rotation in under a second.
- **No per-draw readbacks.** The only readbacks are the gated, capped ones in §5.4/§5.5/§5.6/§5.9 and §6.2 Layer 2. A per-draw readback would serialise the GPU on every draw and drop the frame rate to <1 FPS (the c2 probe's own documentation notes the readback cost).
- **No unbounded files.** Every file the system writes has a cap: the JSONL capture is bounded by the frame window × ring capacity; the transition log's unbounded append (gap G11) is fixed by rotating it at 1 MB (Stage 1).
- **No permanent GPU allocations for diagnostics.** Debug shaders and the depth-copy texture are created lazily when the first viz cvar is enabled and destroyed when the last one is disabled. The staging buffers are the existing ones (`ResolveToMemory` staging, the surface-dump staging) — no new large staging resource.

### 7.5 Interaction with the existing 4 FPS trace overhead

The ~4 FPS observed with all trace/audit cvars on is dominated by the *existing* traces (per-resolve `WaitForGpu` + staging readbacks in the resolve trace, the audit's per-pass measurement, and the tile trace's per-draw logging to `runtime.log` via spdlog). The forensics recorder is designed to be cheaper than those traces (no `WaitForGpu`, no readbacks, no spdlog formatting in the hook — only a struct fill). Expected outcome: a forensics capture (draws+resolves+clears+copies, no binds/uploads, no viz) should cost **~1–2 ms/frame** over the no-trace baseline, versus the ~100+ ms/frame the full existing trace set costs. This must be measured in Stage 2 (see §8) and the measurement recorded; if the recorder costs more than 5 ms/frame, the hook implementation is wrong and must be fixed before proceeding.

---

## 8. Staged implementation plan

Each stage is independently verifiable and ends with a confirmation step. A stage is not "done" until its confirmation passes. Stages are ordered so that each one only depends on the stages before it. **No stage modifies the normal renderer, band geometry, resolve behavior, or color formats.**

### Stage 0 — Groundwork and cvar scaffolding (no hooks)

**Work:**
- Create `include/rex/graphics/render_forensics.h` (new header, same style as RA: header-only where possible, no fmt, no build-system change for the header itself).
- Add the §4.1 cvars (`fh1_forensics_capture`, `_from`, `_to`, `_event_mask`, `_ring_events`, `_include_binds`, `_include_uploads`) to `FL` (flags.cpp/h), all `kRequiresRestart` except the master switch (hot-reload is acceptable for the master switch only if the recorder's active flag is re-read at frame start — decide in implementation and document the choice).
- Add the RF001–RF008 site-id mapping (§2.3, H-RES-1) as a frozen table in `FER` (or in the new header) mapping each `Fh1ResolvePlanFailure` value and each `skip` string to its RF id. This table is the only new code in the shared core in Stage 0.
- Fix gap G11: rotate `transition.log` at 1 MB (in RA `EndFrame`, before appending: if the file exceeds 1 MB, rename to `transition.log.1` and start a new one).

**Verification:**
- Build green (`CMAKE_BUILD_PARALLEL_LEVEL=4`, the `muse_build_game.bat` path — the PC has 16 GB RAM and the default ninja parallelism OOMs; see the build notes in the repo history).
- Launch, set `fh1_forensics_capture = true` with `from = to = 0` (captures nothing). Confirm: no forensics file is created, `runtime.log` shows no forensics lines, and the frame rate is unchanged from the no-forensics baseline (measure over 60 s of free-roam; record the FPS).
- Confirm `transition.log` rotates (temporarily lower the threshold in a test build, or artificially grow the file — do not ship a test-only threshold).

**Deliverable:** header skeleton + cvars + RF table + log rotation. No hooks yet.

### Stage 1 — Recorder core + resolve-plan failure IDs (G5)

**Work:**
- Implement the recorder: `ForensicsEvent` struct (§3.2), the bounded ring buffer (§3.5), the frame-boundary finalisation in `IssueSwap` (CP:5395), the per-frame summary event, and the JSONL writer (header + events + capture summary, §4.1).
- Implement the resource-provenance map (§3.3) with the stable-id assignment (monotonic counter, never reused in a run).
- Place hooks H-RES-1 (FER `Fh1PlanResolve`, all return points, with RF ids) and H-RES-2 (CP:3614 `IssueCopy`, entry). These two hooks alone answer defect #7 (the two resolve-planning failures per run now carry RF ids).
- Place hooks H-RSC-1 (SF:69 surface create) and H-RSC-3 (TC:444 texture create) so the provenance map is populated.
- Locate the shader/pipeline creation entry points (gap H-RSC-11) and record their exact file:line in the design doc as an addendum — do not guess. If they cannot be located in this stage, record that explicitly and carry the item to Stage 3.

**Verification:**
- Build green. Launch with a single-frame capture (`from = to = 2200`, event mask = resolves + resolve-plan-failures only). Drive to free-roam, capture frame 2200, close the game.
- Confirm: the capture file exists at `E:/PinyonShift/work/.local/audit/d3d11-forensics-<utc>.jsonl`; it has a header, resolve events, a frame summary, and a capture summary; every event has `frame` and `event`; the two resolve-planning failures (if they occur in that frame) carry RF ids.
- Confirm the "off" state: with `fh1_forensics_capture = false`, no file is created and the FPS matches the Stage 0 baseline within measurement noise (±1 FPS over 60 s).
- Confirm the ring-buffer cap: set `fh1_forensics_ring_events = 100` and capture a frame; confirm `dropped_events > 0` and `complete: false` in the summary (the cap must be observable, not just code).

**Deliverable:** working recorder + resolve hooks + provenance map + RF ids.

### Stage 2 — Draw, clear, and copy hooks (G1, G4, G8)

**Work:**
- Place hooks H-DRAW-1 (CP:1733), H-DRAW-2 (CP:2941/:2996), H-DRAW-5 (CP:1133 geometry attach), H-DRAW-6 (reuse `NoteDrawSubmission`, CP:2943), H-CLEAR-1 (EX:2365), H-COPY-4 (EX:2589), H-TR-1 (EX:2517 `OnSwap`).
- Place H-DRAW-7 (CP:1603 bindings) and H-DRAW-8 (CP:1466/:1502 cbuffers) behind `fh1_forensics_include_binds` (off by default).
- Emit the `copy_down_ran`, `untile_bands_live`, `surfaces_live`, `frontbuffer_address`, and `frontbuffer_changed` fields in the frame summary (§3.5).
- Measure the recorder's frame-time cost (see §7.5): capture a frame with draws+resolves+clears+copies only, and record the ms/frame overhead vs the no-forensics baseline. If it exceeds 5 ms/frame, stop and fix the hooks before proceeding.

**Verification:**
- Single-frame capture of a free-roam frame. Confirm: draws carry viewport/scissor (post-shift) and `untile_band`; clears carry guest rect + physical rect + band; copy-down events exist when a copy-down ran; the frame summary's `copy_down_ran` matches the log's `D3D11 FH1 untile copy-down` lines for the same frame; `untile_bands_live` is 0 at frame end in steady-state gameplay.
- **Defect #4 test:** capture the menu frame and the first gameplay frame after exiting the menu (set the window to cover the transition). Confirm the capture shows, for the transition: whether `EndUntiledTiling` ran, which `untile_bands_` entries existed, whether a clear ran over the black-block region, and whether the surfaces were reused (same resource id) or recreated (new id). This capture is the deliverable that answers defect #4.
- Confirm the cost measurement is recorded in the design doc as an addendum (actual ms/frame, not the estimate).

**Deliverable:** full draw/clear/copy/transition event stream + the defect #4 transition capture.

### Stage 3 — Upload, bind, present, and tape-path hooks (G2, G9, G12)

**Work:**
- Place hooks H-RSC-4 (TC:524), H-RSC-5 (CP:3288), H-RSC-8 (SM:70), H-RSC-9 (TC:774), H-PRES-1 (CP:5395), H-PRES-2 (PR:668), H-PRES-3 (PR:682), H-PRES-4 (PR:1190), H-PRES-5 (CP:5395 `EndFrame` boundary).
- Place the recorded-path hooks H-DRAW-3 (CLS:333/335), H-DRAW-4 (CP:3358), H-CLEAR-2/3 (CLS), H-RES-7 (CLS), H-COPY-1/2/3 (CLS), H-RSC-6/7 (CLS), H-RSC-10 (RST) — all gated on `fh1_d3d11_command_lists` being on, and all carrying `tape_packet_id` (§3.4).
- Add the shader-id rule: once the Stage 1 addendum locates the shader creation points, assign stable shader ids and record them in `vs_id` / `ps_id` / `cs_id`. If the shader creation points could not be located, the shader id fields stay 0 with `hr_source` semantics (0 = "not captured", documented) — do not fabricate shader ids.
- Wire the `bind` event's full texture list (H-DRAW-7) so a draw binding more than 4 textures is fully recorded in the bind event even though the draw event caps at 4 (§3.2).

**Verification:**
- Single-frame capture with binds and uploads on. Confirm: every draw has a matching bind event (same `event_id` as parent); every sampled texture's guest base appears in the bind event; the present events link the swap texture (H-RSC-9) to the presenting resolve (H-RES-2) by guest base address.
- Tape-path verification (only if `fh1_d3d11_command_lists` can be enabled without breaking the game): record a frame, confirm every record event has a matching replay event with the same `tape_packet_id`, and confirm a tape overflow (if it occurs) shows as a record event with no replay event. If the tape path cannot be enabled, record that explicitly and mark the tape hooks as "placed but unverified" — do not claim they work.
- Confirm the present effect is recorded (H-PRES-3): the capture shows which effect pipeline was bound (bilinear vs bilinear-dither vs CAS/FSR1 fallback).

**Deliverable:** complete event stream on both execution paths + present linkage + shader ids (or an explicit "not captured" statement).

### Stage 4 — Region-history query tool (Layer 1 + Layer 2) (G6)

**Work:**
- Write the offline query tool (Python, shipped as `tools/forensics_query.py` in the repo — or in `~/workspace/` if the repo is not the right home; decide in implementation and document it). Input: a capture JSONL + a query rectangle (+ optional frame range). Output: the ordered event list (§6.2 Layer 1) with `covers`/`writes` labels.
- Implement Layer 2 (region readback) by extending the existing `fh1_debug_c2_probe` mechanism to a named rectangle (`fh1_forensics_region` cvar, `x0,y0,x1,y1`), capped at 24 draws and one region per capture, reusing the c2 probe's before/control/after statistics (share changed, bounding box, per-64-column / per-60-row change).
- Write the §6.4 worked examples as a test: run the Layer 1 query for the black-block region across a menu→gameplay transition and for a blue region across consecutive gameplay frames, and record the outputs as the reference answers.

**Verification:**
- Run the Layer 1 query on a Stage 2 capture. Confirm: the output lists events in `event_id` order, each labelled `covers` or `writes`, and a draw whose scissor does not intersect the query rect does not appear.
- Run Layer 2 on a single frame with a small region. Confirm: the per-draw change statistics are produced, the stability control (two reads before the draw) reports 0, and the frame rate during the capture is understood (~1 FPS is expected and acceptable for a capture).
- Confirm the worked examples (§6.4) produce the reference answers and are stored alongside the capture files.

**Deliverable:** query tool + Layer 2 region probe + worked examples for defects #4 and #5.

### Stage 5 — Visual debug modes (G7)

**Work:**
- Implement the modes in §5, in this order (each is independently verifiable, so the order is by value, not by dependency): (1) band ownership viz (§5.8, CPU-only, cheapest), (2) wireframe (§5.1), (3) draw-ID viz (§5.2), (4) resource-ID viz (§5.3), (5) overdraw heatmap (§5.4), (6) depth viz (§5.5), (7) stencil (§5.6), (8) texture/UV (§5.7, wiring existing dumps to event ids), (9) RT before/after (§5.9, most expensive).
- Create the debug shaders (§5, preamble) as precompiled HLSL, compiled lazily on first viz use.
- Each mode gets its `fh1_forensics_viz_*` cvar.

**Verification (per mode):**
- Capture a frame with the mode off, then the same scene with the mode on, then off again. Confirm the two "off" frames are identical (same presented PPM hash via `fh1_debug_presented_pair`).
- Confirm the mode's output answers its question on a known case: wireframe shows triangles in a blank region (or proves there are none); draw-ID viz colours decode to draw ids present in the capture's event stream; overdraw heatmap brightness correlates with the Layer 1 draw count for the same region; band ownership viz shows three clean bands in a working frame.
- Confirm no mode is on by default and no mode changes the frame when off.

**Deliverable:** all nine modes, each verified in isolation.

### Stage 6 — Draw-ID re-render (Layer 3) and documentation

**Work:**
- Implement Layer 3 (§6.2): replay a captured tape with the draw-ID shader (§5.2) bound, producing the per-pixel draw-id image. Gate on the tape path being replayable; if it is not, the capture summary records `replay_available: false` and Layer 3 is documented as unavailable for that capture.
- Write the operator documentation: how to capture a frame, how to run a region query, how to read a capture, how to use each viz mode, and the §6.4 worked examples as the tutorial.
- Record the actual measured costs (§7.5) as an addendum to this design doc.

**Verification:**
- Replay a captured frame and confirm the draw-id image decodes to draw ids that exist in the capture's event stream, and that a pixel's decoded draw id matches the last draw covering that pixel in the Layer 1 query.
- Confirm the documentation lets a reader who did not write the system capture a frame and answer "what produced this pixel" for a named region without asking the author.

**Deliverable:** Layer 3 (or an explicit "unavailable" statement with the reason) + operator docs + measured costs.

### Stage 7 — Hardening and handover

**Work:**
- Remove or gate any Stage 0–6 debug output that is not part of the design (no stray `REXGPU_INFO` in hooks; all hook output goes through the recorder).
- Run the full verification suite: build green; single-frame capture; defect #4 transition capture; defect #5 multi-frame capture; all viz modes off-by-default check; ring-buffer cap check; "off" state FPS check.
- Push the implementation to the repo (same push flow as the backend source: `github push-files`, text files only, excluding generated artefacts) with a status update in the repo README.

**Verification:**
- The suite passes end to end. The design doc is updated with the measured costs and any deviations from the design (with reasons).
- Seth (or the operator) confirms the system answers the defect #4 question from a real capture.

**Deliverable:** hardened system + passing suite + repo push + updated design doc.

---

## Appendix A — File map (hook locations at a glance)

| File | Hooks | Notes |
|------|-------|-------|
| CP `src/graphics/d3d11/command_processor.cpp` | H-DRAW-1/2/4/5/6/7/8, H-RES-2/3/5, H-RSC-5, H-PRES-1/5, H-CLEAR (via `ResolveClear` call sites), H-COPY (via `SubmitFrameTape`) | The main hook file. 6,030 lines. |
| EX `src/graphics/d3d11/fh1_native_executor.cpp` | H-CLEAR-1/4, H-COPY-4, H-TR-1, H-RES (via `NotePassClaim`/`NotePassResolve` call sites) | Owns EDRAM surfaces and the untile map. |
| RS `src/graphics/d3d11/fh1_native_resolve.cpp` | H-RES-4 | The compute resolve dispatch. |
| SF `src/graphics/d3d11/fh1_native_surfaces.cpp` | H-RSC-1/2 | Surface registry. |
| TC `src/graphics/d3d11/texture_cache.cpp` | H-RSC-3/4/9 | Texture cache. |
| CL `src/graphics/d3d11/command_list.cpp` | (tape infrastructure; no hooks, but `tape_packet_id` comes from here) | |
| CLS `src/graphics/d3d11/command_list_immediate_sink.cpp` | H-DRAW-3, H-CLEAR-2/3, H-RES-7, H-COPY-1/2/3, H-RSC-6/7 | Recorded-path replay side. |
| CLH `include/rex/graphics/d3d11/command_list.h` | (packet struct definitions; the `D3D11Command` variant) | |
| RST `src/graphics/d3d11/resource_state_tracker.cpp` | H-RSC-10 | Recorded-path barriers only. |
| RTC `src/graphics/d3d11/render_target_cache.cpp` | (none — skeleton, returns nullptr) | Do not hook; the executor owns RTs. |
| SM `src/graphics/d3d11/shared_memory.cpp` | H-RSC-8 | Guest → host buffer uploads. |
| PR `src/ui/d3d11/d3d11_presenter.cpp` | H-PRES-2/3/4, H-COPY-5 | Presentation. |
| GO `src/ui/shaders/d3d11/guest_output.hlsl` | (none — read to understand the present gamma/HDR logic; the shader is not modified) | Bilinear + dither; the gamma question (defect #5) lives here. |
| SCP `src/graphics/command_processor.cpp` (shared) | (none — packet decode only; the swap is the frame boundary) | |
| FER `src/graphics/fh1_edram_resolve.cpp` (shared) | H-RES-1 | The resolve planner; backend-neutral. |
| RA `include/rex/graphics/render_audit.h` | (extend — new `render_forensics` additions go here or in a sibling header) | The existing audit; extend, don't replace. |
| FL `src/graphics/flags.cpp` / `flags.h` | (new `fh1_forensics_*` cvars) | |

## Appendix B — Open questions (for the implementer to resolve in Stage 1)

1. **Shader creation entry points (H-RSC-11).** The exact functions that create a translated `D3D11Shader` and a `D3D11PipelineCache::D3D11Pipeline` were not read line-by-line in this audit. Stage 1 must locate them. Until then, shader ids are 0 ("not captured").
2. **Surface destruction.** Whether EDRAM surfaces are ever individually destroyed outside `Shutdown` (SF:51) is unverified. Stage 1 must confirm; if they are only destroyed at shutdown, H-RSC-2 is a batch event.
3. **Draw-geometry attach ordering.** Stage 1 must verify that `UpdateFixedFunctionState` (CP:1133) is always called between `IssueDraw` entry (CP:1733) and the host draw call (CP:2941/:2996) for every draw. If any draw path skips it, that path needs its own geometry attach point.
4. **Tape-path replay fidelity.** Whether the recorded tape (`fh1_d3d11_command_lists`) replays a frame faithfully enough for Layer 3 is unverified — the cvar's own description says the recording path is "still being wired". Stage 3 must test it; if it is not faithful, Layer 3 is marked unavailable.
5. **Capture-file home.** The JSONL capture is specified at `E:/PinyonShift/work/.local/audit/`. If the repo prefers the capture tool (`tools/forensics_query.py`) to live elsewhere, Stage 4 decides and documents it.
6. **The two resolve-planning failures.** The RF ids are assigned in Stage 1, but the *call sites* that produce the two per-run failures are still unknown. The RF ids identify the failure *kind*, not the call site. If Stage 1 shows both failures share one RF id, a follow-up is needed to distinguish them by caller (add a `caller` field to the `resolve_plan_fail` event).

## Appendix C — Sources consulted for this design

- Canonical PC source, `E:\PinyonShift\work\.local\rexglue\`, as of 2026-11-15 ~20:10 PM PC (D3D11 backend + shared core + presenter + `guest_output.hlsl` + `render_audit.h`).
- `~/workspace/fh1-d3d11-band-testkit.zip` (28 files, 2026-11-15 17:42 CST snapshot) for the shared core and as a cross-check.
- `~/workspace/dx11-render-bug-search-2026-11-15.md` (ranked causes, incl. the untile/Rank 1 and fetch-swizzle/Rank 3 findings).
- `~/workspace/dx11-d3d12-vulkan-band-mapping-2026-11-15.md` (Vulkan untile mechanism, D3D12 not a reference).
- `~/workspace/dx11-chatgpt-implementation-plan-2026-11-15.md` (RF001–RF008 plan, Copilot Task 1).
- Runtime evidence: `~/workspace/runtime-20261115-2138.log` (5,447 lines; untile copy-down lines present; zero "resolve planning failed" lines in that run — the failures are intermittent per run, not per frame).
- Claude audit share (2026-11-15): confirmed the four band fixes, the `SubmitFrameTape`/`WaitForGpu` defects, the band-0 scissor issue, and the black-block stale-band mechanism.

*End of design.*
