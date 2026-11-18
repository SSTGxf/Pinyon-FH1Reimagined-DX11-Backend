# D3D11 Texture Sampling/Binding Audit — Flat Untextured 3D World

**Date:** 2026-11-16. **Source:** `E:\PinyonShift\work\.local\rexglue\` (canonical `E:\PinyonShift\work`). Read-only audit; no source or config modified.

## Verdict

**Most likely cause: the 3D draws' texture fetches resolve to null or never-uploaded bindful SRVs, so the pixel shaders' texture samples contribute nothing and the output is the flat diffuse/vertex color.**

The 3D pixel shaders sample textures through bindful SRVs (`t1+`) supplied by `GetActiveTextureBindfulSRV`. That function returns `nullptr` — and the texture cache leaves textures empty — on several named failure paths. The single most code-grounded sub-mechanism is the **upload rejection**: texture data whose guest pages were GPU-written (resolved/render-to-texture data) is refused by the CPU load path unless the resolve-guest-refresh lands the mirror bytes first, and that refresh path is known-incomplete (the refresh fix is unpushed). The cache code itself names the 3D scene as the most visible victim of exactly this failure. The HUD's textures are plain disc-loaded data in CPU memory, so they upload fine — which is why the HUD is textured and the world is flat.

**Honest tension (do not skip):** a null or never-uploaded SRV "reads as zeros" per the header (`include/rex/graphics/d3d11/texture_cache.h:44-46`). A zero texture sample multiplied by the diffuse color predicts **black**, but the photos show **bright** flat yellow/blue/white. So the null/empty-SRV mechanism explains "no texture detail" but does not by itself explain the bright colors. The bright colors are consistent with the texture term being effectively a constant ~1.0 (white) or the shader passing diffuse through unmodulated — e.g. a bound-but-uniform texture, a white border color with out-of-range UVs, or a shader that adds rather than multiplies. Which of these is true is exactly what the recommended diagnostic below resolves. I do not assert the color arithmetic; it needs the dump.

## 1. The traced path: 3D draw → texture sample

All paths below are in `src/graphics/d3d11/command_processor.cpp` unless noted.

1. **`IssueDraw`** (`command_processor.cpp:1735`) is the Phase 3 draw path for every draw, HUD and 3D alike. There is **no separate 3D texture path** — HUD and world go through the same function, the same `BindDrawRenderTargets`, the same `ConfigurePipeline`, the same `RequestTextures`, and the same `UpdateBindings`. The only thing that distinguishes HUD from world is the viewport test in the HUD-hide gate.
2. **HUD-hide gate** (`command_processor.cpp:1899-1908`): skips a draw only when `fh1_debug_hide_hud` is on **and** `normalized_color_mask != 0` **and** `viewport_info.xy_extent[0] < 1280 * draw_resolution_scale_x`. A 3D world draw has a 1280-wide viewport, so it **passes** the gate and reaches the texture-bind path. Depth-only passes (`normalized_color_mask == 0`) also pass. The gate therefore cannot starve the 3D draws of textures — it only removes HUD draws before binding.
3. **Texture request** (`command_processor.cpp:2000`): after the pipeline is configured, `texture_cache_->RequestTextures(used_texture_mask)` runs, where `used_texture_mask` comes from `GetUsedTextureMaskAfterTranslation()` on the vertex + pixel shaders (`command_processor.cpp:1976-1977`). This is where the cache creates/uploads the textures the draw is about to sample.
4. **Binding** — `UpdateBindings` (`command_processor.cpp:1605`), called from `IssueDraw` at `command_processor.cpp:2812`:
   - `t0` is the **shared-memory mirror** SRV, bound for both stages (`command_processor.cpp:1690`: `PSSetShaderResources(0, 1, &shared_memory_srv)`). This is the in-shader fetch path (vertex fetches / guest-memory reads).
   - Textures `t1+` come from the texture cache, per shader binding (`command_processor.cpp:1702`): `srvs[i] = texture_cache_->GetActiveTextureBindfulSRV(texture_bindings[i])`, then `PSSetShaderResources(1, ...)` (`command_processor.cpp:1707`) / `VSSetShaderResources(1, ...)` for the vertex stage.
   - Samplers come from `texture_cache_->GetSamplerDesc(...)` (`command_processor.cpp:1714`) hashed and passed through `GetSamplerState` (`command_processor.cpp:1716`), then `PSSetSamplers(0, ...)` (`command_processor.cpp:1722`) / `VSSetSamplers(0, ...)`.
5. **Shader side:** the DXBC translator emits pixel-shader texture fetches as **bindful SRV samples**. `FindOrAddTextureBinding` (`src/graphics/pipeline/shader/dxbc_translator_fetch.cpp:461-510`) allocates a `bindful_srv_index` when `!bindless_resources_used_` (`dxbc_translator_fetch.cpp:483-484`), and the fetch emits `OpLOD`/`Sample` against `T(bindful_srv_index, SRVMainRegister::kBindfulTexturesStart + texture_binding_index)` (`dxbc_translator_fetch.cpp:1361-1386`, `1805-1809`). D3D11 runs with `bindless_resources_used=false` (`src/graphics/d3d11/pipeline_cache.cpp:49`), so **D3D11 3D pixel shaders sample textures exclusively through these bindful SRVs**. If the SRV is null or its texture is empty, the sample contributes nothing.

## 2. Every place a texture bind can silently fail

### 2a. No valid binding — `GetActiveTextureBindfulSRV` returns `nullptr`

`src/graphics/d3d11/texture_cache.cpp:1009-1129`. Returns `nullptr` when:

- **No valid binding** (`texture_cache.cpp:1065`): `GetValidTextureBinding(fetch_constant_index)` returns null when `binding.key.is_valid` is false (`include/rex/graphics/pipeline/texture/cache.h:659-662`). An invalid key comes from a fetch constant that fails validation (bad format, zero size, unsupported dimension). The diagnostic log in the same function says it plainly: `"no valid binding (the fetch cannot be hosted)"` (`texture_cache.cpp:1052-1057`).
- **Dimension mismatch** (`texture_cache.cpp:1065`): `AreDimensionsCompatible` (`include/rex/graphics/d3d11/texture_cache.h`, `AreDimensionsCompatible`) — a `k2D` fetch accepts `k1D`/`k2DOrStacked`/`k3D` resources, but a `k3DOrStacked` fetch requires a `k3D` resource and a `kCube` fetch requires a `kCube` resource. A wrong-dimension resource yields a null SRV.
- **Sign mismatch** (`texture_cache.cpp:1082-1098`): if the shader binding is signed but the fetch has no signed components (or vice versa), `texture` stays null and the function returns `nullptr`.
- **Variant SRV creation failure** (`texture_cache.cpp:1106-1112`): if the signed/stacked variant view can't be created, returns `nullptr`.

In all of these, the header documents the consequence (`texture_cache.h:44-46`): **"nullptr for an unbound slot — reads as zeros, like the Direct3D 12 null descriptors."**

### 2b. Texture never created — `CreateTexture` returns `nullptr`

`src/graphics/d3d11/texture_cache.cpp:442-520`. Returns `nullptr` (no texture at all) when:

- The format has no D3D11 resource format or no CPU converter (`texture_cache.cpp:445-448`): logs `"rejected by the D3D11 CPU texture load path (format conversion unsupported)"` (`texture_cache.cpp:441`). Affected formats include the 4:2:2 video formats (`k_Cr_Y1_Cb_Y0_REP`, `k_Y1_Cr_Y0_Cb_REP`), `k_10_11_11` / `k_11_11_10`, the 32-bit integer formats (`k_32`, `k_32_32`, `k_32_32_32_32`), `k_32_AS_8`, `k_6_5_5`, `k_8_B`, and the EDRAM formats (`k_16_16_EDRAM`, `k_16_16_16_16_EDRAM`, `k_8_8_8_8_A`).
- **Decompression needed** (`texture_cache.cpp:449-453`): DXT-to-RGBA8 and 4:2:2 unchroma are "not on the CPU load path yet" — a block-compressed texture whose width/height is not a multiple of the block size is rejected.
- **`key.scaled_resolve`** (`texture_cache.cpp:454-458`): resolution-scaled resolves are deferred (Phase 4 work) — returns `nullptr`.
- `CreateTexture3D` / `CreateTexture2D` fails (`texture_cache.cpp:481`, `500`), or `CreateShaderResourceView` fails (`texture_cache.cpp:512`).

**Important scope limit:** the mainstream 3D formats **are** supported with converters — `k_DXT1`→`BC1_UNORM`, `k_DXT2_3`→`BC2_UNORM`, `k_DXT4_5`→`BC3_UNORM`, `k_8_8_8_8`→`R8G8B8A8`, `k_5_6_5`, `k_1_5_5_5`, `k_4_4_4_4` (all with `ConvertRunCopy` or an explicit R/B-swap converter; `texture_cache.cpp:94-217`). So format rejection cannot explain the *whole* world being flat — only individual textures in exotic formats.

### 2c. Texture created but data never uploaded — `LoadTextureDataFromResidentMemoryImpl` returns `false`

`src/graphics/d3d11/texture_cache.cpp:530+`. This is the strongest code-grounded failure for the 3D scene:

- Data is read from guest memory via `shared_memory().CopyCpuRange` (`texture_cache.cpp:630-645`). If any page in the range was GPU-written, `CopyCpuRange` refuses it and the load is **rejected** (`return false`), with a log only when `fh1_swap_fetch_trace` is on (`texture_cache.cpp:596-613`, `"FH1 texture load rejected"`).
- The comment at `texture_cache.cpp:604-610` states the consequence directly: *"Without this the range's pages stay GPU-written, CopyCpuRange below refuses them, and every texture the guest resolved and then samples — the 3D scene, most visibly — would never be uploaded."*
- The protection is the resolve-guest-refresh: *"Before a CPU-side untiling read of a range the GPU wrote … land the mirror's bytes for it in guest memory"* (`texture_cache.cpp:611`), gated on `fh1_resolve_guest_refresh`, which **defaults to true** (`src/graphics/flags.cpp:360`). So the refresh is *supposed* to run — but the refresh fix itself is **unpushed** (prior context: "Refresh fix UNPUSHED"), meaning the refresh path is known-incomplete. A refresh that silently fails (or lands the wrong range) leaves the pages GPU-written and the load rejected, and the host texture stays at whatever it held — **empty on first use** — while the guest thinks it just sampled the range.
- Why the HUD is unaffected: HUD textures are plain disc-loaded data in CPU memory, never GPU-written, so `CopyCpuRange` accepts them. The 3D scene is the one that samples resolved/render-to-texture data.

### 2d. Sampler failure

`GetSamplerDesc` (`texture_cache.cpp:1131-1240`) builds a complete `D3D11_SAMPLER_DESC` from the fetch constant (filters, clamp modes with `NormalizeClampMode`, border color including `k_ABGR_White` → `(1,1,1,1)` at `texture_cache.cpp:1229-1234`, anisotropy, mip bias). `GetSamplerState` (`command_processor.cpp:1589-1604`) caches by desc hash and, on `CreateSamplerState` failure, logs `"D3D11: failed to create a sampler state"` and returns `nullptr` — which `UpdateBindings` then stores into the sampler array, so a **null sampler is bound**. There is no evidence in the source that sampler creation is failing, and FL 11_0 supports 16 samplers per stage (the D3D11 per-stage limit), so a sampler-side failure is possible but unevidenced. Note: a null sampler does not by itself produce flat colors; it is listed here for completeness.

### 2e. No default-white fallback texture exists

Searched `texture_cache.cpp`, `command_processor.cpp`, and `texture_cache.h` for a default/white/1x1/dummy/placeholder texture: **none exists** in the D3D11 cache. The only "white" is the sampler **border color** (`k_ABGR_White`, `texture_cache.cpp:1229`), which is returned only when the fetch's clamp mode uses a border *and* the sampled UV is out of range. There is no code path that substitutes a white texture when a texture is missing — a missing texture yields a **null SRV (reads as zeros)**, not a white one.

## 3. Is there a 3D-specific texture path that could be broken while the HUD path works?

**No.** Both HUD and 3D world draws go through the identical `IssueDraw` → `BindDrawRenderTargets` → `ConfigurePipeline` → `RequestTextures` → `UpdateBindings` sequence (§1). The HUD-hide gate is the only code that treats them differently, and it only *skips* HUD draws — it does not alter binding. The difference is therefore in **the textures themselves** (their source, format, and page state), not in a separate 3D binding path:

- HUD textures: small, simple 2D `k_8_8_8_8` (RGBA8) disc-loaded textures in CPU memory → created, uploaded, and bound without hitting any rejection path.
- 3D world textures: a mix of disc-loaded albedo (DXT/RGBA8, mostly supported) **and** resolved/render-to-texture data (GPU-written pages → §2c), plus any exotic formats (§2b). The 3D draws are the ones exposed to the upload-rejection path.

## 4. Reconciling with Rank 3 (dropped guest fetch R/B swizzle)

Rank 3 (from the 2026-11-15 search) is that D3D11 drops the guest fetch's red/blue swizzle — D3D11.0 SRVs have no component-mapping field, so the fetch-level channel swizzle is not applied. The format-level swizzle *is* present in the host table (`GetHostFormatSwizzle`, `texture_cache.cpp:396-398`, returning `kHostFormats[format].swizzle`), but the **fetch-level** R/B selection is what D3D11 drops.

**Rank 3 does NOT explain flat-untextured.** A swizzle error permutes color channels (R↔B → blue tint). It changes *which* color a texel has; it does not remove texture detail. A blue-tinted world would still show roads, trees, and panel lines — just in the wrong hue. The observed symptom is the *absence* of texture detail (flat solid colors), which requires the texture sample to contribute nothing or a constant. So:

- **Blue tint = Rank 3 (swizzle), a color error. Still open, separate.**
- **Flat untextured = bind/upload failure (§2), a data error. This audit's subject.**

The two are independent defects that happen to coexist in the same frame. Fixing the swizzle will not restore texture detail, and restoring texture uploads will not fix the blue tint.

## 5. Ranked causes

### 1st — Texture data never uploaded: GPU-written (resolved) pages rejected, resolve-guest-refresh not landing the data
- **Evidence:** `LoadTextureDataFromResidentMemoryImpl` refuses GPU-written pages (`texture_cache.cpp:630-645`); the comment explicitly names the 3D scene as the most visible victim (`texture_cache.cpp:604-610`); the refresh that prevents it is gated on `fh1_resolve_guest_refresh` (default true, `flags.cpp:360`) but the refresh fix is **unpushed**, so the refresh path is known-incomplete; a rejected load leaves the texture empty on first use while the guest thinks it sampled the range. The HUD's CPU-memory textures are unaffected, matching the observed HUD-textured / world-flat split.
- **Caveat:** most world *albedo* textures are disc-loaded (CPU memory), not GPU-written, so this alone may not flatten the *entire* world unless the world's sampled textures are predominantly resolved data or the refresh failure is broader. Also the empty-texture → zeros → black tension (§Verdict). This is the top candidate because it is the only failure the code itself flags as hitting the 3D scene, but it is not proven.

### 2nd — Bindful SRV resolves to null: the 3D fetches cannot be hosted (invalid key / dimension / sign mismatch)
- **Evidence:** `GetActiveTextureBindfulSRV` returns `nullptr` for an invalid binding, incompatible dimensions, or a sign mismatch (`texture_cache.cpp:1065`, `1097-1098`, `1111-1112`); the header documents null → reads as zeros (`texture_cache.h:44-46`); the function itself logs `"no valid binding (the fetch cannot be hosted)"` for frame-sized fetches (`texture_cache.cpp:1052-1057`). This is a per-fetch failure, so it explains individual untextured surfaces; for it to explain the *whole* world, the 3D fetches would have to fail validation systematically (e.g. a dimension/sign convention the 3D shaders use that the cache rejects). Same black-vs-bright tension as #1.
- **Why 2nd, not 1st:** the bind mechanism itself works (the HUD binds fine through the same function), so a systematic bind failure would need a systematic property of the 3D fetches, which is less directly evidenced than the upload-rejection path that the code explicitly calls out for the 3D scene.

### 3rd — Format rejection in `CreateTexture`, or a null sampler
- **Evidence:** `CreateTexture` returns `nullptr` for formats with no converter (`texture_cache.cpp:445-448`) and for DXT decompression cases (`texture_cache.cpp:449-453`); `GetSamplerState` returns `nullptr` on `CreateSamplerState` failure (`command_processor.cpp:1598-1600`).
- **Why 3rd:** the mainstream 3D formats (DXT1/3/5, RGBA8, 565, 4444) **are** supported with converters (`texture_cache.cpp:94-217`), so format rejection cannot flatten the whole world — only exotic-format textures. Sampler creation failure is a real code path but has no supporting evidence (no error in the logs cited, and the desc is straightforward). These are real but narrow failure modes, not a systematic world-wide cause.

## 6. Recommended diagnostic (single cheapest — do not implement, just name it)

**Set `fh1_debug_texture_dump` to a directory and run one gameplay frame, then read the `FH1 srv bind` log lines together with the dumped textures.**

- The cvar is a string (a directory path) defined at `src/graphics/flags.cpp:281`.
- `GetActiveTextureBindfulSRV` already logs, per frame-sized fetch, either the bound key/texture/SRV (`"FH1 srv bind: swap=… slot=… base=… fetch=…x… fmt=… tiled=… -> key fmt=… …x… texture=… srv=…"`) or `"… -> no valid binding (the fetch cannot be hosted)"` (`texture_cache.cpp:1016-1060`). It is deduplicated (max 40 logs) so one frame is enough.
- The dump (`FindTexture2DForGuestBase` / `FindAnyTexture2DForGuestBase`, `texture_cache.h:71-88`) shows the actual texels the cache holds for a guest base address — i.e. what a shader sampling that address reads.

**How it distinguishes the three causes:**
- Log says **"no valid binding"** for the 3D draws' fetches → cause #2 (bind rejection); check the fetch's format/dimension/sign in the same line.
- Log says a texture **is bound (`texture=1 srv=1`)** but the dumped texture is **empty (all zeros) or uniform** → cause #1 (upload rejection / never-uploaded data).
- Log says bound **and** the dumped texture is **non-empty with real detail**, yet the screen is still flat → the failure is downstream of the cache (shader/UV/sampler), not a bind/upload failure — which rules out all three ranked causes and points at the shader or interpolator path instead.

## 7. What could NOT be verified from the source

- **The exact pixel-shader arithmetic** (whether the 3D shaders multiply by, add, or otherwise combine the texture sample with the diffuse/vertex color). This is what determines whether a zero-reading SRV renders black or leaves the flat diffuse color visible, and therefore whether the bright flat colors are consistent with cause #1/#2 or require a uniform-texture / white-border explanation. It needs a disassembly of a 3D pixel shader (DXBC), not the cache code.
- **Whether `RefreshResolvedGuestRange` actually lands the mirror bytes at runtime.** The cvar defaults to true, but the refresh fix is unpushed; whether the refresh succeeds or silently fails can only be settled by the diagnostic (or by reading `RefreshResolvedGuestRange`'s return path and the `fh1_swap_fetch_trace` `"FH1 texture load rejected"` lines).
- **Whether any 3D texture is in fact bound-but-uniform (single mip / 1x1 / stale recycled data).** No code path creates a uniform texture deliberately; this would only be visible in the dump.
- **Sampler creation success at runtime.** `GetSamplerState` logs on failure, but no runtime log was examined in this audit.

## 8. Files consulted (all under `E:\PinyonShift\work\.local\rexglue\`)

- `src/graphics/d3d11/command_processor.cpp` — `IssueDraw` (:1735), HUD gate (:1899-1908), `RequestTextures` (:2000), `UpdateBindings` (:1605) incl. t0 mirror (:1690), t1+ SRVs (:1702-1707), samplers (:1714-1722), `GetSamplerState` (:1589-1604), `UpdateBindings` call site (:2812).
- `src/graphics/d3d11/texture_cache.cpp` — host format table (:60-230), `GetHostFormatSwizzle` (:396-398), `CreateTexture` (:442-520) incl. rejections (:441, :445-458, :481, :500, :512), `LoadTextureDataFromResidentMemoryImpl` (:530+) incl. GPU-written rejection (:604-645) and refresh gate (:611), `GetActiveTextureBindfulSRV` (:1009-1129) incl. null returns (:1065, :1097-1098, :1111-1112) and `"no valid binding"` diagnostic (:1016-1060), `GetSamplerDesc` (:1131-1240) incl. white border (:1229-1234).
- `include/rex/graphics/d3d11/texture_cache.h` — null-SRV "reads as zeros" (:44-46), `FindTexture2DForGuestBase` / `FindAnyTexture2DForGuestBase` (:71-88), `AreDimensionsCompatible` (:~121-137).
- `include/rex/graphics/pipeline/texture/cache.h` — `GetValidTextureBinding` (:659-662), `RequestTextures` (:130).
- `src/graphics/d3d11/pipeline_cache.cpp:49` — `bindless_resources_used=false` for D3D11.
- `src/graphics/pipeline/shader/dxbc_translator_fetch.cpp` — `FindOrAddTextureBinding` bindful allocation (:461-510, :483-484), bindful sample emission (:1361-1386, :1805-1809).
- `src/graphics/flags.cpp` — `fh1_debug_hide_hud` (:33), `fh1_debug_texture_dump` (:281), `fh1_resolve_guest_refresh` default true (:360).
