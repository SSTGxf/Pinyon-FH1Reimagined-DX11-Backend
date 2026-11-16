# D3D12 / Vulkan vs D3D11 — FH1 three-band separation mapping

Date: 2026-11-15. Read-only source comparison of the canonical source
`E:\PinyonShift\work\.local\rexglue` (detached HEAD 79d3e6fa).
All paths below are relative to that root. Line numbers were verified live
via PowerShell `Select-String`/`Get-Content` on 2026-11-15.

## Headline findings

1. **D3D12 does NOT separate the bands.** There is no band-separation
   mechanism anywhere in the D3D12 backend: zero hits for `band`/`Band` in
   `src/graphics/d3d12/fh1_native_executor.cpp`,
   `src/graphics/d3d12/command_processor.cpp`,
   `include/rex/graphics/d3d12/fh1_native_executor.h`, and
   `include/rex/graphics/d3d12/command_processor.h`, and zero hits for
   `untile` in the D3D12 executor and command processor. D3D12 has the
   same structural defect as D3D11. **D3D12 is not a working reference
   for band separation.**
2. **The untile system is Vulkan-only.** The only working in-tree reference
   is Vulkan's untile path (`fh1_untile_predicated_tiling` /
   `fh1_untile_height`, `untile_band`, `EndUntiledTiling`).
3. **Copilot's offset=0 claim is refuted as a justification** (see §6).
   Its premise — a "512-logical-row source surface" — is wrong for the
   actual 4x scene key: the surface is 640×1024. A +512 row offset would
   NOT exceed it. What exceeds the surface is stacking all three bands
   (3×512 = 1536 folded rows > 1024), which is exactly why an
   expanded/untile-height surface is needed, not just an offset.
4. **The overwrite happens at draw time; the resolve offset alone cannot
   fix D3D11.** If all three bands still draw into rows 0–256, there is
   nothing distinct for any resolve offset to read. D3D11 needs the full
   stack: expanded surface + viewport/scissor shift at draw time +
   resolve row offset (+ copy-down for later EDRAM reads).

## (a) Per-band mapping table

Guest row ranges are established by experiment (Baseline + Test A + Test B,
2026-11-15): Band 1 = top 0–256 (sky), Band 2 = middle 256–512 (world),
Band 3 = bottom 512–720 (cockpit). Bin selects: 3 → pass 0 (Band 1),
0xC → pass 1 (Band 2), 0x30 → pass 2 (Band 3)
(`TilingPassOfSelect()`, `include/rex/graphics/command_processor.h:518-524`).

### Vulkan (the working reference)

| Band | Guest row range | Host row range | Viewport offset | Scissor rect | Resolve source range | Destination range |
|---|---|---|---|---|---|---|
| 1 | 0–256 | 0–256 (screen rows of the 720-row expanded image) | `viewport.y += 0` (`untile_rows = -window_y_offset = 0`) | shifted by 0; first tile's scissor extended down to row 720 | guest EDRAM rect (0–256), shader reads `+ untile_band` = +0 | plan dest, unchanged by `untile_band` |
| 2 | 256–512 | 256–512 | `viewport.y += 256` | shifted by +256 | guest EDRAM rect, shader reads +256 | plan dest, unchanged |
| 3 | 512–720 | 512–720 | `viewport.y += 512` (trace: −208 in the 4x case) | shifted by +512 | guest EDRAM rect (0–208), shader reads +512 | plan dest, unchanged |

Sources: viewport/scissor shift `src/graphics/vulkan/command_processor.cpp:8909-8940`
(`UpdateDynamicState`); `untile_band = max(0, -window_y_offset)`
`src/graphics/vulkan/fh1_native_executor.cpp:2936-2939`; `sample_select |
(untile_band << 16)` at `:3107`, `:3147`; shader decode
`src/graphics/shaders/fh1_native_resolve_memory.cs.hlsl:237-238`
(`fh1_source_row_offset = FH1_SAMPLE_SELECT >> 16u`); expanded surface
`full_rows = max(surface.height, untile_height_)` at
`fh1_native_executor.cpp:778`, `:912`; `EndUntiledTiling` copy-down at
`:744-763`.

Important qualification: in normal untiled operation the **first tile draws
everything** over the whole 0–720 height and later-tile draws are skipped
(`src/graphics/vulkan/command_processor.cpp:5766-5776`, `:6351-6358`).
So the per-band host rows above are where each band's *resolve* reads, and
where a later tile's draw *would* land if not skipped — not three separate
per-band draw passes in the normal path.

### D3D12 (no separation — same defect as D3D11)

| Band | Guest row range | Host row range | Viewport offset | Scissor rect | Resolve source range | Destination range |
|---|---|---|---|---|---|---|
| 1 | 0–256 | 0–256 (same surface, same rows) | shared `GetHostViewportInfo` folds window offset into `ndc_offset` once; D3D12 copies verbatim, no band shift | shared `GetScissor`, no band shift | same rect for all bands; `plan.sample_select` passed verbatim, offset 0 | plan dest |
| 2 | 256–512 | 0–256 (same rows — overwrites Band 1) | same, no band shift | same, no band shift | same rect, offset 0 | plan dest (stepped) |
| 3 | 512–720 | 0–208 (same rows) | same, no band shift | same, no band shift | same rect, offset 0 | plan dest (stepped) |

Sources: `src/graphics/util/draw.cpp:387-391` (viewport offset),
`:607-612` (scissor); D3D12 forwards at
`src/graphics/d3d12/command_processor.cpp:2538`, `:2548-2553`,
`:3543-3558` (viewport), `:2570-2578`, `:3561-3566` (scissor);
one surface per key `src/graphics/d3d12/fh1_native_executor.cpp:499-508`;
resolve passes `plan.sample_select` verbatim at `:1785-1786`,
`:1816-1817`; 64-bpp halving confirmed at `:1079-1082`, `:669-672`.

### D3D11 (no separation — the bug)

| Band | Guest row range | Host row range | Viewport offset | Scissor rect | Resolve source range | Destination range |
|---|---|---|---|---|---|---|
| 1 | 0–256 | 0–256 in the 640×1024 (4x) surface | shared `GetHostViewportInfo`; D3D11 copies verbatim (`command_processor.cpp:1138-1141`), no band shift | shared `GetScissor`; D3D11 copies verbatim (`:1186-1189`), no band shift | same rect for all bands (`:3761-3762`), `source_row_offset` hardcoded 0 (`:4051-4053`) | plan dest (base) |
| 2 | 256–512 | 0–256 (same rows — overwrites Band 1) | same, no band shift | same, no band shift | same rect, offset 0 | plan dest (base + 1,310,720 B) |
| 3 | 512–720 | 0–208 (same rows) | same, no band shift | same, no band shift | same rect, offset 0 | plan dest (base + 2,621,440 B) |

Sources: viewport `src/graphics/d3d11/command_processor.cpp:1138-1141`,
scissor `:1186-1189`; claim `command_processor.cpp:1859-1865` →
`BindDrawRenderTargets` → `ClaimTargetTiles`
(`src/graphics/d3d11/fh1_native_executor.cpp:756-791`); resolve
`command_processor.cpp:3761-3762`; offset hardcode `:4051-4053`;
trace writer hardcodes 0 at `:4096-4104`, `:4118-4128`.

## 1. PA_SC_WINDOW_OFFSET → viewport/scissor

The offset is read and applied **once, in shared code**, by all three
backends. The difference is what Vulkan does *on top of it*.

- Shared read (all backends): `src/graphics/util/draw.cpp:387-391`
  (`GetHostViewportInfo`): if `vtx_window_offset_enable`, the window
  offset is folded into `offset_add_xy` once, then used in exactly one of
  two mutually exclusive ways (clip-enabled `:444-469`, clip-disabled
  `:408-424`), so the net vertex shift is applied once. Scissor:
  `draw.cpp:607-612` (`GetScissor`): if not `window_offset_disable`,
  the offset is added to the scissor corners. The offset is applied once
  to vertex positions and once to the independent scissor rectangle —
  the two move together, so their intersection moves by the offset exactly
  once. It is **not** double-applied to vertices.
- D3D11: reads the register only for logging/audit
  (`src/graphics/d3d11/command_processor.cpp:1130-1133`, `:1722`, `:1869`,
  `:3594`, `:4090`); copies the shared result verbatim into
  `D3D11_VIEWPORT` (`:1138-1141`) and `D3D11_RECT` (`:1186-1189`).
  **D3D11 adds no arithmetic of its own.**
- D3D12: same — caches the register and calls the shared helper
  (`src/graphics/d3d12/command_processor.cpp:2538`, `:2548-2553`), then
  copies into `D3D12_VIEWPORT` (`:3543-3558`) and the scissor rect
  (`:3561-3566`). No second application.
- Vulkan: after the shared computation, `UpdateDynamicState`
  (`src/graphics/vulkan/command_processor.cpp:8889`) reads the register
  again and applies the **untile shift**: `:8914-8918`
  (`untile_rows = -pa_sc_window_offset.window_y_offset; viewport.y +=
  float(untile_rows * scale)`), and `:8929-8940` (scissor shifted by the
  same amount; for the first tile the scissor is extended down to row 720).
  `SetViewport`/`SetScissor` (`:5627`, `:5643`) are pure pass-throughs,
  so there is no double-shift. The same register also gates draw skipping
  (`:5769-5771`, `:6351-6352`).

Net effect for band separation: the shared offset moves where pixels land
in guest/EDRAM space, but every band still renders into the one surface
keyed by its EDRAM base. Only Vulkan *undoes* the offset on the host side
(shift down by `-window_y_offset`), placing each band at its screen rows.

## 2. Render-target organization

- **Surface identity has no band component** (all backends).
  `Fh1SurfaceKey { base_tiles, pitch_tiles, msaa, is_depth, format }`
  (`include/rex/graphics/fh1_edram_surfaces.h:15-25`). All three bands
  share the same base, so they share one surface.
- **D3D11:** one EDRAM-sized texture per key; no expanded surface; no
  per-band surface. `Fh1D3D11SurfaceHeight`
  (`include/rex/graphics/d3d11/fh1_native_surfaces.h:73-76`) delegates to
  shared `Fh1SurfaceHeight` (`include/rex/graphics/fh1_edram_surfaces.h:76-84`):
  `tile_rows = ceil(2048 / pitch_tiles)`, height =
  `tile_rows * (16 >> msaa_y_log2)`, clamped to host limits. Registry:
  `std::unordered_map<uint32_t, unique_ptr<D3D11Fh1NativeSurface>> surfaces_`
  (`fh1_native_surfaces.h:118`); `GetOrCreateSurface`
  (`src/graphics/d3d11/fh1_native_surfaces.cpp:69-76`, `:81`) sizes
  `desc.Height = surface->height * scale`. The key packs only
  `base/pitch/msaa/is_64bpp/is_depth/format`
  (`src/graphics/d3d11/fh1_native_resolve.cpp:65-69`) — no band/row field.
  Greps for `untile|expanded|full` in `fh1_edram_surfaces.h` and
  `tiling|band|untile` in `fh1_native_surfaces.cpp` return nothing.
- **Measured size of the 4x scene surface:** 640×1024 (resolve trace:
  `source_key=0/16/4x/c2 source_size=640x1024`; 1x surfaces 1280×2048).
  Arithmetic agrees: pitch_tiles=16 → tile_rows=128 → height =
  128 × (16>>1) = 1024; width = 16 × (80>>1) = 640. The EDRAM
  addressing-period height in pixel rows for the 1280-wide 4x scene is
  256 (Vulkan `SurfaceHeight`, `fh1_native_executor.cpp:632-636`) — the
  rows all three bands collide in.
- **D3D12:** identical structure — one `TEXTURE2D` per key
  (`src/graphics/d3d12/fh1_native_executor.cpp:499-508`), height from the
  same shared `Fh1SurfaceHeight`. No `image_height`, no `untile_height_`,
  no growth path. Guest→host mapping is pure resolution scale (`HostRect`,
  `include/rex/graphics/d3d12/fh1_native_executor.h:346-349`).
- **Vulkan:** one `VkImage` per surface key, but **expanded**:
  `GetOrCreateSurface` (`src/graphics/vulkan/fh1_native_executor.cpp:766-782`):
  `surface.height = SurfaceHeight(...)` (EDRAM period, e.g. 256),
  `full_rows = max(surface.height, untile_height_)` (`:778`),
  image created at `image_height` (`:830`). `image_height` vs `height`
  is documented in `include/rex/graphics/vulkan/fh1_native_executor.h:243-250`:
  "The EDRAM addressing period's height; the image may be taller
  (fh1_untile_predicated_tiling), its rows past the period host-only."
  `EnsureSurfaceRows` (`:911-919`) grows `image_height` toward `full_rows`
  in 128-row steps. Guest EDRAM rows 0..`height` are console-addressable;
  host rows past `height` (up to 720) are host-only "screen rows" where
  later bands live. Nothing allocates a separate image per band.
  `untile_height_` comes from cvars: `fh1_untile_predicated_tiling`
  (`src/graphics/vulkan/command_processor.cpp:83-87`, default true only on
  Android/Mac — **off on Windows**) and `fh1_untile_height` (`:98-100`,
  default **720**); wired at `:2549-2556`, stored at
  `fh1_native_executor.cpp:419`.

## 3. Resolve source-row selection

- **Shared planner carries no source-row offset.**
  `Fh1PlanResolve` (`src/graphics/fh1_edram_resolve.cpp:91-98`, `:180-183`):
  `plan.dest_base`, `plan.dest_pitch`,
  `plan.sample_select = copy_sample_select`, `plan.copy = true`. The rect
  comes from `Fh1ResolveRectangle` (`:40-46`: vertex bounds plus window
  offset when `vtx_window_offset_enable`), clamped to scissor (`:47-54`),
  aligned (`:55-59`).
- **The sample-select chain (all backends).** 3-bit field
  `copy_sample_select : 3` in `RB_COPY_CONTROL`
  (`include/rex/graphics/registers.h:857`); enum
  (`include/rex/graphics/xenos.h:988-996`); carried as `copy_sample_select
  : 3` in `ResolveCopyDestCoordinateInfo`
  (`include/rex/graphics/util/draw.h:405`); sanitized by
  `SanitizeCopySampleSelect` (`src/graphics/util/draw.cpp:753-792`);
  stored at `draw.cpp:989`; `plan.sample_select = uint32_t(...)` in
  `Fh1PlanResolve` (`src/graphics/fh1_edram_resolve.cpp:182`).
- **D3D11 passes it through with offset forced to 0.**
  `PlanResolveMemory` (`src/graphics/d3d11/fh1_native_resolve.cpp:268`)
  forwards it (`plan.constants[4] = sample_select`). The caller builds it
  at `src/graphics/d3d11/command_processor.cpp:4051-4053`:
  `const uint32_t source_row_offset = 0; const uint32_t
  resolve_sample_select = (plan.sample_select & 0xFFFFu) |
  (source_row_offset << 16);` with the comment at `:4047-4050`:
  "D3D11's normal EDRAM-sized surfaces rasterize each window-offset band
  at source Y=0; unlike Vulkan's optional untile-height surfaces, no extra
  row band is stored past the EDRAM surface. Keep the shader's upper-half
  row offset explicit and zero for this path." No code path can pass
  nonzero (grep for `source_row_offset|SourceRowOffset` in
  `fh1_native_resolve.cpp`: zero hits; in `command_processor.cpp` the
  only hits are the `= 0` definition, the pack, and trace format strings).
  The trace writer hardcodes 0 via that constant (`:4096-4104`,
  `:4118-4128`).
- **D3D12 passes it through verbatim.** Both resolve dispatch sites pass
  `plan.sample_select` with no band OR'd in
  (`src/graphics/d3d12/fh1_native_executor.cpp:1785-1786`, `:1816-1817`);
  inside `ResolveToMemory` (`:1524-1531`): `constants[4] = sample_select`.
  64-bpp: D3D12 halves tile width and doubles pitch
  (`:1079-1082` in `GetResolveSources`; also `TransferTiles` `:669-672`
  and the resolve-clear lambda `:1844-1847`). `SplitByOwner`
  (`include/rex/graphics/fh1_edram_tiles.h:146-152`) only splits the given
  rect by which surface owns each tile — it cannot separate bands, because
  all three bands' rects are the same rect.
- **Vulkan passes the band in the high bits.** `Resolve()`
  (`src/graphics/vulkan/fh1_native_executor.cpp:2936-2939`):
  `const uint32_t untile_band = untile_height_ ? uint32_t(std::max(0,
  -int32_t(regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset))) : 0u;`
  (comment `:2934-2935`: "Untiled tiling: a later tile's band is at its
  screen rows of the surfaces, past the EDRAM rows its window offset maps
  it to.") Band 1 → 0, Band 2 → 256, Band 3 → 512. Consumed at `:3107`
  and `:3147`: `plan.sample_select | (untile_band << 16)`. The band is
  also recorded per surface (`:3102-3105`: `untile_bands_[key] =
  max(band, untile_band)`), and clear rects are shifted the same way
  (`:3236-3240`).
- **The shader decodes it (shared by all backends).**
  `src/graphics/shaders/fh1_native_resolve_memory.cs.hlsl:237-238`:
  `fh1_source_row_offset = FH1_SAMPLE_SELECT >> 16u;` (upper half is
  preserved even under SPIR-V specialization, macro `:83-85`). Both load
  paths in `src/graphics/shaders/fh1_native_edram.hlsli` add it — `:333`
  (same-place path) and `:375` (EDRAM-relocation path). Comment at
  `:89-91`: "Untiled predicated tiling: a later tile's band is this many
  guest rows below the EDRAM rows of the source surface." Net effect: the
  resolve's source rectangle stays the guest EDRAM rect (rows 0–256), but
  the shader reads it shifted down by `untile_band` host rows. The
  destination is **not** shifted by `untile_band` anywhere in the Vulkan
  code read; each band's resolve writes to the guest dest range the plan
  computed.
- **Copy-down (Vulkan only).** `EndUntiledTiling()`
  (`src/graphics/vulkan/fh1_native_executor.cpp:744-763`): at the next
  band-0 resolve (`:2940`: `if (untile_height_ && !untile_band)
  EndUntiledTiling();`), copies each recorded band from its screen rows
  down to EDRAM rows 0 (`region.srcOffset = {0, band * scale_, 0}`,
  dst offset unset = 0, height `min(untile_height_ - band, band)`),
  restoring the console layout for later texture loads / EDRAM-addressed
  reads (`include/rex/graphics/vulkan/fh1_native_executor.h:80-83`).

## 4. Where band separation lives

**Vulkan: both paths, but the draw path is what prevents the overwrite.**
Three cooperating pieces: (a) the expanded surface (720 rows tall, so
distinct rows 256–511 and 512–719 exist to draw into); (b) the
viewport/scissor shift (a later tile's draw lands at its screen rows, not
at EDRAM rows 0–256); (c) later-tile draws are normally skipped entirely
(`src/graphics/vulkan/command_processor.cpp:5766-5776`, `:6351-6358` —
"the first tile draws every tile's objects over the whole surface height
and the later tiles only resolve their bands"), so nothing can overwrite
Band 1. The resolve offset is the read side: without it, resolves would
all read rows 0–256 (the D3D11 symptom). The copy-down is a restore, not
the separator or the reader.

**D3D12: nowhere — neither path.** Draw path: `PrepareTargets`
(`src/graphics/d3d12/fh1_native_executor.cpp:1251-1358`) derives keys from
`RB_SURFACE_INFO`/`RB_COLOR_INFO`/`RB_DEPTH_INFO` only; claims tiles
`[base, base+length)`; `EstimateMaxY` adds the window offset only as an
extent cap (`src/graphics/util/draw_extent_estimator.cpp:246-248`,
`:262-274`), not as a row remap. Resolve path: no band offset (§3).
The only band-aware thing is the shared predicated-packet filtering
(`bin_select_` / `TilingPassOfSelect()`), which decides *which draws run*
in each tiling pass — not *where they land*. The shared
`untile_select_all_` flag (`include/rex/graphics/command_processor.h:511-517`,
comment: "the Vulkan FH1 executor") defaults to `false` and is never set
by D3D12.

**D3D11: nowhere — neither path, and the overwrite originates at draw
time.** The per-draw claim (`src/graphics/d3d11/command_processor.cpp:1859-1865`,
viewport ∩ scissor, both window-offset-adjusted but band-blind) feeds
`BindDrawRenderTargets` → `PrepareTargets` → `ClaimTargetTiles`
(`src/graphics/d3d11/fh1_native_executor.cpp:756-791`):
`row_first = top / pixel_height`, `row_end = min((bottom + pixel_height
- 1) / pixel_height, rows)`, then `tiles_.ClaimRect(key.base_tiles, pitch,
packed_key, column_first, row_first, column_end, row_end)`, with
`GetTileGeometry` (`:202-217`): `pixel_height = 16 >> msaa_y_log2` (8 at
4x), `rows = surface.height / pixel_height`. Band identity never reaches
this code (`current_draw_tiling_pass_` is used in D3D11 only for trace
gating). Since all three bands replay the same registers (same
`RB_COLOR_INFO`/`RB_SURFACE_INFO`, hence same key/texture), each band's
claim re-assigns the same tile rows starting at `base_tiles + 0`, and
rasterization into the same viewport/scissor/RTV overwrites the previous
band's pixels. The resolve clear is equally band-blind
(`fh1_native_executor.cpp:2480-2485`: viewport = whole surface, scissor =
plan rect). The resolve then reads the same rect for every band (§3), so
even a correct draw-side separation would be unread unless the resolve
offset matched — and conversely, fixing only the resolve offset would read
rows the draws never wrote.

**Answer to the question "does the resolve offset even matter if the draws
already overwrote each other?": no — not on its own.** The resolve offset
is necessary but not sufficient. The overwrite is created at draw time;
the resolve offset can only read what the draws put in distinct rows.

## 5. Smallest D3D11 change that reproduces the reference behavior

Do not write the fix — identify it. The reference is Vulkan's untile path
only (D3D12 has nothing to port). The minimal reproduction is the three-part
stack, plus the copy-down:

1. **Expand the surface** — size the D3D11 scene surface to
   `max(period_height, untile_height)` rows (720 guest rows), i.e. add the
   equivalent of Vulkan's `image_height` alongside `height` in
   `include/rex/graphics/d3d11/fh1_native_surfaces.h` /
   `src/graphics/d3d11/fh1_native_surfaces.cpp` (`GetOrCreateSurface`,
   `:69-81`). Without this there are no distinct rows 256–719 to draw into.
2. **Shift viewport + scissor by `-window_y_offset` at draw time** — the
   equivalent of Vulkan `UpdateDynamicState`
   (`src/graphics/vulkan/command_processor.cpp:8909-8940`), applied in
   D3D11's `UpdateFixedFunctionState` / where
   `src/graphics/d3d11/command_processor.cpp:1138-1141` (viewport) and
   `:1186-1189` (scissor) copy the shared result — plus extending the
   first tile's scissor to the full 720 rows. This is the operation that
   actually prevents the overwrite: each band's draws land at its screen
   rows instead of EDRAM rows 0–256.
3. **Pass the band in the resolve** — the equivalent of Vulkan
   `:3107`/`:3147`: compute `untile_band = max(0, -window_y_offset)` at
   resolve time and OR `(untile_band << 16)` into the sample select that
   `src/graphics/d3d11/command_processor.cpp:4051-4053` builds (replacing
   the hardcoded `source_row_offset = 0`), so the shared shader reads
   each band's screen rows. The shader side already works
   (`fh1_native_resolve_memory.cs.hlsl:237-238`).
4. **Copy-down at the next band-0 resolve** — the equivalent of Vulkan
   `EndUntiledTiling()` (`fh1_native_executor.cpp:744-763`): copy each
   recorded band from its screen rows down to EDRAM rows 0, so later
   texture loads / EDRAM-addressed reads see the console layout.

Optional, not required for correctness: Vulkan's later-tile draw skip
(`command_processor.cpp:5766-5776`, `:6351-6358`) — an optimization (the
first tile draws everything once) rather than a separator. Without it,
each band draws its own objects at its own screen rows, which also works
provided (1) and (2) hold; it just draws everything three times.

The single most direct operation to change is the per-draw
destination-row placement — the claim at
`src/graphics/d3d11/fh1_native_executor.cpp:790-791`
(`tiles_.ClaimRect`), fed by the viewport∩scissor claim at
`src/graphics/d3d11/command_processor.cpp:1859-1865`. That is the first
and only place where guest pixels are assigned to EDRAM tile rows, and it
receives no band input. But changing the claim alone cannot work, and
changing the resolve offset alone cannot work — both halves must move
together, on a surface tall enough to hold all three bands.

## 6. Verdict on Copilot's offset=0 claim

Copilot's claim: "For this D3D11 path the offset is 0: the frame-2200
trace shows Band 3 rasterizing at viewport/scissor Y=0, height 208.
Unlike Vulkan's optional untile-height path, D3D11 uses EDRAM-sized
surfaces; adding 512 would exceed the 512-logical-row source surface."

- **(a) "512-logical-row source surface" — REFUTED for the actual key.**
  The 4x scene surface is **640×1024** (resolve trace
  `source_key=0/16/4x/c2 source_size=640x1024`; `Fh1SurfaceHeight`
  arithmetic: pitch_tiles=16 → tile_rows=128 → height = 128 × 8 = 1024).
  512 is what `Fh1SurfaceHeight` returns only for pitch_tiles=32, which
  is not the key in use. 512 does have a real meaning, derived: one
  1280×256 4x band is 1280×256×4 = 1,310,720 samples ÷ 1280
  samples/tile = 1024 tiles = 64 tile-rows at pitch 16 = **512 texture
  rows** when unfolded through pitch-16 addressing — i.e. 512 is *one
  band's footprint*, not the surface height.
- **(b) "Adding 512 would exceed the surface" — FALSE for the actual
  surface.** Rows 512..1023 exist in the 1024-row surface; a +512 offset
  for Band 2 would be in bounds. What exceeds the surface is stacking
  **all three** bands (3×512 = 1536 folded rows > 1024) — which is
  exactly why an expanded/untile-height surface is needed, not just an
  offset. Copilot's arithmetic is right only for the (wrong) 512-row
  premise.
- **(c) "The shader has a row-offset slot D3D11 leaves at zero" —
  CONFIRMED.** `fh1_native_edram.hlsli:91` declares it;
  `fh1_native_resolve_memory.cs.hlsl:237` sets it from bits 16+ of
  `FH1_SAMPLE_SELECT`; `src/graphics/d3d11/command_processor.cpp:4051-4053`
  hardcodes those bits to 0. (Verified independently as well as by the
  D3D11 analysis.)
- **(d) "No untile mechanism in D3D11" — CONFIRMED (as an absence).**
  Recursive grep for `untile` in `src/graphics/d3d11/` returns only
  comments (texture-cache CPU untile on load, `texture_cache.cpp:373,562,703`;
  "untiled swap texture", `command_processor.cpp:5691`; and the `:4048`
  comment contrasting with Vulkan) — no `untile_select_all_`, no
  untile-height surface, no expanded surface.
- **The "frame-2200 trace shows Band 3 at viewport/scissor Y=0, height
  208" piece could NOT be re-verified from source.** There are no
  `swap=2200` lines in the on-disk `resolve-trace.log`, and the tile trace
  is runtime data, not source. The source is *consistent* with Y=0 (D3D11
  applies no band shift), and on-disk resolve lines do show `window_y=0`,
  but the Band 3 / 208 numbers themselves are not source-verified.

**Overall verdict: REFUTED as a justification.** The offset is 0 in D3D11
only in the vacuous sense that all three bands draw at rows 0–256, so
there is nothing else for the resolve to read — but that is the bug
itself, not a reason the offset is correct. The claim confuses "the
resolve reads what's there" with "what's there is correct," and its
supporting arithmetic rests on a wrong surface height. The correct
statement is: *with D3D11's current draw path, no resolve offset can
produce the right picture, because the draws have already overwritten
each other; the offset becomes meaningful only once the draws are
separated onto distinct rows of an expanded surface, at which point it
must be 0 / 256 / 512 per band (Vulkan's `untile_band`), not 0 for all.*

## Caveats / not verified from source

- Whether D3D12 renders FH1 correctly at runtime despite the missing
  mechanism was not tested — the conclusion is from source analysis only.
  It is possible D3D12 takes some other path not covered by the searches
  (the searches covered `WindowOffset`, `window_offset`,
  `PA_SC_WINDOW_OFFSET`, `Viewport|Scissor`, `untile`, `full_rows`,
  `untile_height`, `SurfaceHeight`, `source_row`, `Resolve`,
  `64|bpp|pitch`, `TilingPass|bin_select`, `band|Band` across the D3D12
  executor, command processor, and headers, plus shared `fh1_edram_*`,
  `util/draw.cpp`, `util/draw_extent_estimator.cpp`, and the resolve
  shader files).
- Whether each band's resolve `dest_base` differs per band in Vulkan was
  not established: `untile_band` does not touch `dest_base`/`dest_pitch`;
  any per-band dest difference would come from `PlanCopy`/registers,
  which were not traced.
- The `untile_select_all_` predication-override site in Vulkan was not
  read in full (only the grep hit at `command_processor.cpp:2613`); the
  first-tile-draws-everything claim rests on the cvar comment (`:83-87`),
  the scissor extension (`:8935-8940`), and the two skip sites.
- Which branch of the resolve shader (`LayoutSamePlace`) a given scene
  resolve takes was not verified for D3D11.
- All findings are from the on-disk source at the time of reading; no
  runtime capture was run to confirm the mechanisms fire.
