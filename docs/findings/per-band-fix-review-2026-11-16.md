# Per-band tracking fix — code review package

**Date:** 2026-11-16
**For:** Seth (engineering specification + code review; no regression testing until he approves)
**Status:** Implemented by Copilot, built, one runtime run captured. Not yet reviewed, not yet regression-tested.

## Summary

**Defect (confirmed, Case B):** `NativeDrawIssued` recorded only `max(band)` per surface, so a surface drawn by both band 256 and band 512 remembered only 512. `EndUntiledTiling` then issued only the band-512 copy-down, and rows 256–512 were never copied down — the black middle.

**Fix:** per-surface band bitmask. Each distinct band a surface is drawn or cleared in sets its own bit; `EndUntiledTiling` issues one copy-down per recorded band, lowest first. One file changed only: `src/graphics/d3d11/fh1_native_executor.cpp`.

**Runtime evidence:** band-256 copy-downs went from **0** (three prior runs) to **1,998** in the fix run, alongside 2,298 band-512 copy-downs. Execution of the missing copy-down path is confirmed. Whether the copied content is correct is **not yet established** — see "Copy-down correctness" below. Do not call the rendering fixed on counts alone.

---

## Build identity

| Item | Value |
|---|---|
| File changed | `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\fh1_native_executor.cpp` |
| Backup of original | `fh1_native_executor.cpp.musebak-perband-20261116` (same directory) |
| `rexgpu-fh1.dll` | 11,343,360 bytes, rebuilt 2026-11-16 5:54:57 PM |
| `pinyon_shift.exe` | 61,851,136 bytes, rebuilt 2026-11-16 5:55:21 PM |
| Build | `CMAKE_BUILD_PARALLEL_LEVEL=4` via `E:\PinyonShift\work\.local\muse_build_game.bat`, exit 0, 3 pre-existing warnings, no errors |
| Fix in binary | Confirmed — `UntileBandBit` present in the deployed source (3 occurrences: helper + 2 record sites); executor object rebuilt 5:54:13 PM, DLL 5:54:57 PM, exe 5:55:21 PM |
| Test run | PID 23096, launched 6:03:50 PM PC time, GTX 570, `--gpu_backend=d3d11 --d3d11_adapter=GTX --fh1_debug_tile_trace_swap=1500 --fh1_debug_composite_dump=E:/PinyonShift/work/.local/compdump --fh1_debug_composite_draws=1500` |

**Git:** `E:\PinyonShift\work` is **not** a git repository. The rexglue checkout at `E:\PinyonShift\work\.local\rexglue` **is** a git repo at HEAD `79d3e6fae4b79e00def48a595c80f0eaa47fd2b6`, but its working tree is **not clean** (many pre-existing modified files across the tree, e.g. `CMakeLists.txt`, `include/rex/graphics/command_processor.h`, `include/rex/graphics/flags.h`, and `M src/graphics/vulkan/fh1_native_executor.cpp`). Critically, the entire `src/graphics/d3d11/` directory is **untracked** in that repo (`?? src/graphics/d3d11/`), so there is no git diff for this fix. The diff below is the authoritative one: unified diff of the backed-up original against the fixed file, covering every changed hunk. Use the DLL/exe mtimes + sizes above as the build identifier for the tested binary.

---

## Complete diff

Unified diff of `fh1_native_executor.cpp.musebak-perband-20261116` (before) vs `fh1_native_executor.cpp` (after). One file, five hunks, nothing else changed.

```diff
--- a/src/graphics/d3d11/fh1_native_executor.cpp	(before: fh1_native_executor.cpp.musebak-perband-20261116)
+++ b/src/graphics/d3d11/fh1_native_executor.cpp	(after: fixed)
@@ -122,6 +122,24 @@
 constexpr uint32_t kEdramTileBytes32bpp =
     xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples * 4;
 
+// Untile bands are 256-row tiles: within the 720-row untile height the band
+// offsets are 0, 256 and 512. untile_bands_ stores one bit per band index
+// (band / kUntileBandRows) for each surface, so a surface remembers every
+// distinct band it was drawn or cleared in instead of only the highest one.
+// The old max(band) record dropped every band below the highest, so those
+// rows were never copied down: band-256 draws happened, but only the band-512
+// copy-down was ever issued (the black middle). Bit 0 is the band at offset 0,
+// which needs no copy-down (Fh1D3D11CopyDownRows returns 0 for it) and is
+// never recorded.
+constexpr uint32_t kUntileBandRows = 256;
+
+// Bit for one untile band in a per-surface band mask. Repeated draws in the
+// same band set the same bit, so each band is copied down exactly once.
+inline uint32_t UntileBandBit(uint32_t band) { return 1u << (band / kUntileBandRows); }
+
+// Row offset of the band stored in one bit of a per-surface band mask.
+inline uint32_t UntileBandFromBit(uint32_t bit) { return bit * kUntileBandRows; }
+
 // Transfer shader variants: the source's data, and what the destination writes.
 // These are the FH1_SOURCE_* / FH1_DEST_KIND defines of fh1_native_transfer.ps.hlsl.
 constexpr uint32_t kTransferSourceColor = 0;
@@ -2581,8 +2599,7 @@
         (clear_value & 0xFF) != 0 || (surface->stencil_nonzero && !full_surface);
   }
   if (untile_band) {
-    uint32_t& band = untile_bands_[key.Pack()];
-    band = std::max(band, untile_band);
+    untile_bands_[key.Pack()] |= UntileBandBit(untile_band);
   }
   return true;
 }
@@ -2603,8 +2620,7 @@
     }
     for (uint32_t i = 0; i < std::size(pending_surfaces_); ++i) {
       if (!(pending_used_bits_ & (1u << i)) || !pending_surfaces_[i]) continue;
-      uint32_t& band = untile_bands_[pending_surfaces_[i]->key.Pack()];
-      band = std::max(band, untile_band);
+      untile_bands_[pending_surfaces_[i]->key.Pack()] |= UntileBandBit(untile_band);
     }
   }
 }
@@ -2630,7 +2646,7 @@
     context->PSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
     context->CSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
   };
-  for (const auto& [packed, band] : untile_bands_) {
+  for (const auto& [packed, bands] : untile_bands_) {
     D3D11Fh1NativeSurface* surface = config_.surfaces->FindSurface(packed);
     if (!surface || !surface->texture) {
       REXGPU_ERROR("D3D11 FH1 native executor: copy-down surface 0x{:08X} is unavailable",
@@ -2638,58 +2654,72 @@
       succeeded = false;
       continue;
     }
-    const uint32_t copy_rows = Fh1D3D11CopyDownRows(config_.untile_height, band);
-    if (!copy_rows) {
-      completed.push_back(packed);
-      continue;
-    }
-    if (band > surface->image_height || copy_rows > surface->image_height - band) {
-      REXGPU_ERROR("D3D11 FH1 native executor: copy-down rows {}..{} exceed {} (key={})",
-                   band, band + copy_rows, surface->image_height, surface->key.Describe());
-      succeeded = false;
-      continue;
-    }
-    if (!config_.surfaces->EnsureCopyDownSurface(*surface)) {
-      succeeded = false;
-      continue;
-    }
+    // One copy-down per distinct band the surface was drawn in, lowest band
+    // first. The band mask deduplicates repeated draws in the same band, so
+    // each band is copied down exactly once. Bands run in ascending order so
+    // the last copy-down is the highest band, leaving its rows at EDRAM row 0
+    // for the resolve that follows the flush - the same state the old
+    // single-band copy-down produced.
+    bool surface_done = true;
+    for (uint32_t bit = 0; bands >> bit; ++bit) {
+      if (!(bands & (1u << bit))) continue;
+      const uint32_t band = UntileBandFromBit(bit);
+      const uint32_t copy_rows = Fh1D3D11CopyDownRows(config_.untile_height, band);
+      if (!copy_rows) {
+        continue;
+      }
+      if (band > surface->image_height || copy_rows > surface->image_height - band) {
+        REXGPU_ERROR("D3D11 FH1 native executor: copy-down rows {}..{} exceed {} (key={})",
+                     band, band + copy_rows, surface->image_height, surface->key.Describe());
+        succeeded = false;
+        surface_done = false;
+        break;
+      }
+      if (!config_.surfaces->EnsureCopyDownSurface(*surface)) {
+        succeeded = false;
+        surface_done = false;
+        break;
+      }
 
-    ID3D11RenderTargetView* no_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
-    context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
-    unbind_srvs();
-    context->CopyResource(surface->copy_down_texture, surface->texture);
-    context->OMSetBlendState(nullptr, nullptr,
-                             Fh1D3D11SampleMask(surface->key, surface->samples));
-
-    D3D11Fh1NativeSurface copy_down = *surface;
-    copy_down.texture = surface->copy_down_texture;
-    copy_down.rtv = surface->copy_down_rtv;
-    copy_down.uint_rtv = surface->copy_down_uint_rtv;
-    copy_down.dsv = surface->copy_down_dsv;
-    const RECT rect = {0, 0, LONG(surface->width), LONG(copy_rows)};
-    TileGeometry geometry;
-    if (!GetTileGeometry(*surface, geometry)) {
-      REXGPU_ERROR("D3D11 FH1 native executor: copy-down geometry unavailable for {}",
-                   surface->key.Describe());
-      succeeded = false;
-      continue;
-    }
-    const uint32_t tile_rows = (copy_rows + geometry.pixel_height - 1) / geometry.pixel_height;
-    const uint32_t tile_count = geometry.columns * tile_rows;
-    if (!TransferRects(surface->key, copy_down, packed, &rect, 1, tile_count,
-                       surface->stencil_nonzero, band)) {
-      succeeded = false;
-      continue;
-    }
+      ID3D11RenderTargetView* no_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
+      context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
+      unbind_srvs();
+      context->CopyResource(surface->copy_down_texture, surface->texture);
+      context->OMSetBlendState(nullptr, nullptr,
+                               Fh1D3D11SampleMask(surface->key, surface->samples));
+
+      D3D11Fh1NativeSurface copy_down = *surface;
+      copy_down.texture = surface->copy_down_texture;
+      copy_down.rtv = surface->copy_down_rtv;
+      copy_down.uint_rtv = surface->copy_down_uint_rtv;
+      copy_down.dsv = surface->copy_down_dsv;
+      const RECT rect = {0, 0, LONG(surface->width), LONG(copy_rows)};
+      TileGeometry geometry;
+      if (!GetTileGeometry(*surface, geometry)) {
+        REXGPU_ERROR("D3D11 FH1 native executor: copy-down geometry unavailable for {}",
+                     surface->key.Describe());
+        succeeded = false;
+        surface_done = false;
+        break;
+      }
+      const uint32_t tile_rows = (copy_rows + geometry.pixel_height - 1) / geometry.pixel_height;
+      const uint32_t tile_count = geometry.columns * tile_rows;
+      if (!TransferRects(surface->key, copy_down, packed, &rect, 1, tile_count,
+                         surface->stencil_nonzero, band)) {
+        succeeded = false;
+        surface_done = false;
+        break;
+      }
 
-    unbind_srvs();
-    ID3D11UnorderedAccessView* no_uav = nullptr;
-    context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
-    context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
-    context->CopyResource(surface->texture, surface->copy_down_texture);
-    completed.push_back(packed);
-    REXGPU_INFO("D3D11 FH1 untile copy-down: key={} band={} rows=0..{} samples={}",
-                surface->key.Describe(), band, copy_rows, surface->samples);
+      unbind_srvs();
+      ID3D11UnorderedAccessView* no_uav = nullptr;
+      context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
+      context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
+      context->CopyResource(surface->texture, surface->copy_down_texture);
+      REXGPU_INFO("D3D11 FH1 untile copy-down: key={} band={} rows=0..{} samples={}",
+                  surface->key.Describe(), band, copy_rows, surface->samples);
+    }
+    if (surface_done) completed.push_back(packed);
   }
   for (uint32_t packed : completed) untile_bands_.erase(packed);
   return succeeded;
```

---

## Updated `NativeDrawIssued` (full function)

`src/graphics/d3d11/fh1_native_executor.cpp`, lines 2607–2626 (fixed file).

```cpp
void D3D11Fh1NativeExecutor::NativeDrawIssued(const Fh1DrawInfo& draw, uint32_t untile_band) {
  if (!initialized_) {
    return;
  }
  if (draw.memexport) {
    return;
  }
  ++draws_;
  if (untile_band) {
    if (untile_band >= config_.untile_height) {
      REXGPU_ERROR("D3D11 FH1 native executor: draw band {} exceeds untile height {}",
                   untile_band, config_.untile_height);
      return;
    }
    for (uint32_t i = 0; i < std::size(pending_surfaces_); ++i) {
      if (!(pending_used_bits_ & (1u << i)) || !pending_surfaces_[i]) continue;
      untile_bands_[pending_surfaces_[i]->key.Pack()] |= UntileBandBit(untile_band);
    }
  }
}
```

The clear path in `ResolveClear` (line 2602) changed the same way — the map's value is now a bitmask, so the clear record had to change too:

```cpp
  if (untile_band) {
    untile_bands_[key.Pack()] |= UntileBandBit(untile_band);
  }
```

---

## Updated `EndUntiledTiling` (full function)

`src/graphics/d3d11/fh1_native_executor.cpp`, lines 2628–2726 (fixed file).

```cpp
bool D3D11Fh1NativeExecutor::EndUntiledTiling() {
  if (!initialized_ || untile_bands_.empty()) return true;

  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  if (!context) {
    REXGPU_ERROR("D3D11 FH1 native executor: no immediate context for untile copy-down");
    return false;
  }
  ScopedD3D11ClearState saved_state(context);
  std::vector<uint32_t> completed;
  bool succeeded = true;
  std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
      no_srvs = {};
  auto unbind_srvs = [&]() {
    context->VSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
    context->HSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
    context->DSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
    context->GSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
    context->PSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
    context->CSSetShaderResources(0, UINT(no_srvs.size()), no_srvs.data());
  };
  for (const auto& [packed, bands] : untile_bands_) {
    D3D11Fh1NativeSurface* surface = config_.surfaces->FindSurface(packed);
    if (!surface || !surface->texture) {
      REXGPU_ERROR("D3D11 FH1 native executor: copy-down surface 0x{:08X} is unavailable",
                   packed);
      succeeded = false;
      continue;
    }
    // One copy-down per distinct band the surface was drawn in, lowest band
    // first. The band mask deduplicates repeated draws in the same band, so
    // each band is copied down exactly once. Bands run in ascending order so
    // the last copy-down is the highest band, leaving its rows at EDRAM row 0
    // for the resolve that follows the flush - the same state the old
    // single-band copy-down produced.
    bool surface_done = true;
    for (uint32_t bit = 0; bands >> bit; ++bit) {
      if (!(bands & (1u << bit))) continue;
      const uint32_t band = UntileBandFromBit(bit);
      const uint32_t copy_rows = Fh1D3D11CopyDownRows(config_.untile_height, band);
      if (!copy_rows) {
        continue;
      }
      if (band > surface->image_height || copy_rows > surface->image_height - band) {
        REXGPU_ERROR("D3D11 FH1 native executor: copy-down rows {}..{} exceed {} (key={})",
                     band, band + copy_rows, surface->image_height, surface->key.Describe());
        succeeded = false;
        surface_done = false;
        break;
      }
      if (!config_.surfaces->EnsureCopyDownSurface(*surface)) {
        succeeded = false;
        surface_done = false;
        break;
      }

      ID3D11RenderTargetView* no_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
      context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
      unbind_srvs();
      context->CopyResource(surface->copy_down_texture, surface->texture);
      context->OMSetBlendState(nullptr, nullptr,
                               Fh1D3D11SampleMask(surface->key, surface->samples));

      D3D11Fh1NativeSurface copy_down = *surface;
      copy_down.texture = surface->copy_down_texture;
      copy_down.rtv = surface->copy_down_rtv;
      copy_down.uint_rtv = surface->copy_down_uint_rtv;
      copy_down.dsv = surface->copy_down_dsv;
      const RECT rect = {0, 0, LONG(surface->width), LONG(copy_rows)};
      TileGeometry geometry;
      if (!GetTileGeometry(*surface, geometry)) {
        REXGPU_ERROR("D3D11 FH1 native executor: copy-down geometry unavailable for {}",
                     surface->key.Describe());
        succeeded = false;
        surface_done = false;
        break;
      }
      const uint32_t tile_rows = (copy_rows + geometry.pixel_height - 1) / geometry.pixel_height;
      const uint32_t tile_count = geometry.columns * tile_rows;
      if (!TransferRects(surface->key, copy_down, packed, &rect, 1, tile_count,
                         surface->stencil_nonzero, band)) {
        succeeded = false;
        surface_done = false;
        break;
      }

      unbind_srvs();
      ID3D11UnorderedAccessView* no_uav = nullptr;
      context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
      context->OMSetRenderTargets(UINT(std::size(no_targets)), no_targets, nullptr);
      context->CopyResource(surface->texture, surface->copy_down_texture);
      REXGPU_INFO("D3D11 FH1 untile copy-down: key={} band={} rows=0..{} samples={}",
                  surface->key.Describe(), band, copy_rows, surface->samples);
    }
    if (surface_done) completed.push_back(packed);
  }
  for (uint32_t packed : completed) untile_bands_.erase(packed);
  return succeeded;
}
```

---

## Tracking declaration and lifecycle

**Declaration** — `include/rex/graphics/d3d11/fh1_native_executor.h`, line 274, inside `class D3D11Fh1NativeExecutor` (class at line 105):

```cpp
  std::unordered_map<uint32_t, uint32_t> untile_bands_;
```

The type did **not** change: the map still maps packed surface key → `uint32_t`, but the value is now a **bitmask** (one bit per band index, bit = band / 256) instead of a max band. No header change was needed. A related accessor sits at line 160:

```cpp
  bool has_untile_bands() const { return !untile_bands_.empty(); }
```

**Helpers** (new, file-local in the anonymous namespace, `fh1_native_executor.cpp` lines 122–141):

```cpp
constexpr uint32_t kUntileBandRows = 256;
inline uint32_t UntileBandBit(uint32_t band) { return 1u << (band / kUntileBandRows); }
inline uint32_t UntileBandFromBit(uint32_t bit) { return bit * kUntileBandRows; }
```

**Lifecycle:**

| Stage | Where | Code |
|---|---|---|
| Initialized (declaration default) | `fh1_native_executor.h:274` | `std::unordered_map<uint32_t, uint32_t> untile_bands_;` (default-constructed empty map) |
| Cleared on init | `fh1_native_executor.cpp:560`, in `Initialize()` (line 535) | `untile_bands_.clear();` |
| Cleared on shutdown | `fh1_native_executor.cpp:612`, in `Shutdown()` (line 569) | `untile_bands_.clear();` |
| Updated — draw path | `fh1_native_executor.cpp:2623`, in `NativeDrawIssued` | `untile_bands_[pending_surfaces_[i]->key.Pack()] \|= UntileBandBit(untile_band);` (for each pending surface; guarded by `untile_band != 0` and `untile_band < untile_height`) |
| Updated — clear path | `fh1_native_executor.cpp:2602`, in `ResolveClear` | `untile_bands_[key.Pack()] \|= UntileBandBit(untile_band);` (guarded by `untile_band != 0`) |
| Consumed | `fh1_native_executor.cpp:2649`, in `EndUntiledTiling` | `for (const auto& [packed, bands] : untile_bands_)` — one copy-down per set bit, ascending |
| Cleared (per surface) | `fh1_native_executor.cpp:2724`, in `EndUntiledTiling` | `for (uint32_t packed : completed) untile_bands_.erase(packed);` (only surfaces whose bands all copied down successfully) |

**Row math** — `Fh1D3D11CopyDownRows` is defined in `include/rex/graphics/d3d11/fh1_native_surfaces.h:101` (unchanged by this fix):

```cpp
inline uint32_t Fh1D3D11CopyDownRows(uint32_t untile_height, uint32_t band) {
  return band && band < untile_height ? std::min(untile_height - band, band) : 0u;
}
```

With `untile_height = 720`: band 0 → 0 rows (no copy-down, by design); band 256 → `min(464, 256)` = **256 rows**; band 512 → `min(208, 512)` = **208 rows**. These match the log lines exactly (`band=256 rows=0..256`, `band=512 rows=0..208`).

---

## Runtime evidence

Test run: PID 23096, launched 6:03:50 PM PC time, GTX 570, D3D11, fix build. Logs read from `E:\PinyonShift\work\.local\preview\logs\` after the game closed (no process running at read time). Counts are of `D3D11 FH1 untile copy-down:` log lines.

**Important counting note:** `runtime.2.log` contains the tail of the *prior* run (PID 10332, GPU thread `t25548`) as well as the start of the fix run (GPU thread `t3288`), because logs rotate by size (~5 MB), not by session. The clean fix-run totals below are filtered to thread `t3288` only. All `band=256` lines in these files belong to the fix run — the prior run had zero.

### Copy-down counts

| Log file | Fix run (t3288) band=256 | Fix run (t3288) band=512 | Prior run (t25548) band=512 |
|---|---|---|---|
| `runtime.log` | 466 | 524 | 0 |
| `runtime.1.log` | 1,120 | 1,122 | 0 |
| `runtime.2.log` | 412 | 652 | 512 |
| **Fix-run total** | **1,998** | **2,298** | — |
| Raw file total (all threads) | 1,998 | 2,810 | 512 |

### Before / after

| Band | Before fix | After fix (fix run, t3288) |
|---|---|---|
| 256 | 0 (three runs: crash-log 932, GTX 570 462, GTX 570 512 — all band-512 only) | **1,998** |
| 512 | Present in all runs | 2,298 |

Band-256 copy-downs went from zero in every prior run to 1,998 in the fix run. That is direct runtime evidence the missing copy-down path is restored.

### Tile trace at swap 1500

`FH1 tile path swap=1500` lines: **357**, all in `runtime.2.log`, all from the fix run (thread `t3288`, ~18:04:55 PC time). Sample lines:

```
[2026-11-16 18:04:55.711] [info] [gpu] [t3288] FH1 tile path swap=1500 stage=issue_enter pass=0 issued=1 window_y=0 mode=6 select=0000000080000003 mask=00000000FFFFFFFF
[2026-11-16 18:04:55.785] [info] [gpu] [t3288] FH1 tile path swap=1500 stage=issue_enter pass=1 issued=1 window_y=-256 mode=6 select=000000000000000C mask=00000000FFFFFFFF
[2026-11-16 18:04:55.952] [info] [gpu] [t3288] FH1 tile path swap=1500 stage=issue_enter pass=2 issued=1 window_y=-512 mode=6 select=0000000000000030 mask=00000000FFFFFFFF
```

Pass 0 = band 1 (rows 0–256), pass 1 = band 2 (rows 256–512), pass 2 = band 3 (rows 512–720). Band-256 draws occur (pass 1, `window_y=-256`) — this is what Case B predicted and what the fix addresses.

### Sample copy-down lines (fix run, thread t3288)

Band 256 (new — never appeared before this fix):

```
[2026-11-16 18:04:36.779] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=0/32/4x/c3 band=256 rows=0..256 samples=4
[2026-11-16 18:04:36.779] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=1024/32/4x/d1 band=256 rows=0..256 samples=4
[2026-11-16 18:04:36.846] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=0/32/4x/c3 band=256 rows=0..256 samples=4
```

Band 512 (present before and after; samples below are from the fix run):

```
[2026-11-16 18:04:36.779] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=0/32/4x/c3 band=512 rows=0..208 samples=4
[2026-11-16 18:04:36.846] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=1024/32/4x/d1 band=512 rows=0..208 samples=4
```

Both bands now copy down for the same surface keys (`0/32/4x/c3`, `1024/32/4x/d1`) — the same surfaces that previously only ever recorded 512.

### Composite dump lines

`FH1 composite dump` lines: **16**, all in `runtime.2.log`, all swap 1500, all from the fix run. Sample:

```
[2026-11-16 18:04:56.107] [info] [gpu] [t3288] FH1 composite dump: swap=1500 before draw#2 key=0/16/1x/c3 E:/PinyonShift/work/.local/compdump/c2-before-00.ppm [written] nonzero=0 colours=1
[2026-11-16 18:04:56.138] [info] [gpu] [t3288] FH1 composite dump: swap=1500 before draw#3 key=0/16/1x/c3 E:/PinyonShift/work/.local/compdump/c2-before-01.ppm [written] nonzero=0 colours=1
[2026-11-16 18:04:56.170] [info] [gpu] [t3288] FH1 composite dump: swap=1500 before draw#5 key=0/16/1x/c3 E:/PinyonShift/work/.local/compdump/c2-before-02.ppm [written] nonzero=0 colours=1
```

All 16 dump lines report `nonzero=0 colours=1`. See the correctness section — this matters.

---

## Copy-down correctness (execution confirmed, correctness still to verify)

Three-stage framing. Only stage (a) is confirmed. Do not call the rendering fixed on counts alone.

### (a) Copy-down execution: CONFIRMED

1,998 band-256 copy-down operations fired in the fix run (thread `t3288`), alongside 2,298 band-512 operations. Before the fix, band-256 copy-downs were zero in three runs. The missing execution path is restored.

### (b) Copy-down correctness: UNKNOWN from the available evidence

**1. Representative band-256 copy-down — source and destination coordinates.**

From `EndUntiledTiling` (`fh1_native_executor.cpp:2691–2701` in the fixed file), one band-256 copy-down does:

```cpp
context->CopyResource(surface->copy_down_texture, surface->texture);  // snapshot whole surface
// ...
const RECT rect = {0, 0, LONG(surface->width), LONG(copy_rows)};      // copy_rows = 256 for band 256
// ...
if (!TransferRects(surface->key, copy_down, packed, &rect, 1, tile_count,
                   surface->stencil_nonzero, band)) { ... }           // band = 256
```

Matching log line:

```
[2026-11-16 18:04:36.779] [info] [gpu] [t3288] D3D11 FH1 untile copy-down: key=0/32/4x/c3 band=256 rows=0..256 samples=4
```

In plain terms, for this copy-down:
- **Source rows:** rows **256–512** of the surface (the band-256 rows, 256 rows tall, full 1280 width). The `band` parameter (256) is the source row offset into the snapshotted surface; `rect` is `{0, 0, width, 256}`.
- **Destination rows:** rows **0–256** (EDRAM row 0). The band's rows are copied down to the top of the surface so the resolve that follows the flush can read them — this is what the log's `rows=0..256` records.
- For comparison, a band-512 copy-down reads source rows **512–720** (208 rows) and writes destination rows **0–208** (`rows=0..208`).

The geometry is correct per the code and the log: the right band, the right row counts (256 and 208, matching `Fh1D3D11CopyDownRows` with untile height 720), the right surface keys. What the log lines cannot show is whether the source rows contained non-black pixels at copy time.

**2. Whether the source region contains non-black pixels: NOT ESTABLISHED by the composite dump.**

The composite dump fired (16 files, `c2-before-00.ppm` through `c2-before-15.ppm`, 1280×2048 P6, in `E:\PinyonShift\work\.local\compdump`). Both the most recent file (`c2-before-15.ppm`) and the first (`c2-before-00.ppm`) were measured directly (PIL/numpy, non-black = any channel > 0):

| File | Size | Non-black pixels overall | Rows 0–256 (band 1) | Rows 256–512 (band 2 / middle) | Rows 512–720 (band 3) | Distinct colours |
|---|---|---|---|---|---|---|
| `c2-before-15.ppm` | 1280×2048 | **0 / 2,621,440 (0.0000%)** | 0.0000% | **0.0000%** | 0.0000% | 1 — `(0,0,0)` |
| `c2-before-00.ppm` | 1280×2048 | **0 / 2,621,440 (0.0000%)** | 0.0000% | **0.0000%** | 0.0000% | 1 — `(0,0,0)` |

Both dumped images are **100% black**. This matches the dump log lines exactly (all 16 report `nonzero=0 colours=1`).

What this does and does not establish:
- The dumped surface (key `0/16/1x/c3`, the composite target) contains no image content at swap 1500 before the composite draws. It does **not** show the middle band as populated.
- The dump is of the composite target surface, **not** of the EDRAM surfaces the copy-downs write to (keys `0/32/4x/c3`, `1024/32/4x/d1`). So it cannot confirm or deny whether the copy-down source rows held non-black pixels. The source-region content question is therefore **unknown** from this evidence.
- The black composite is consistent with the separate, still-open texture/resolve issue (3D texture fetches resolving to null/never-uploaded SRVs), not proof that the copy-down wrote black pixels. Do not conflate the two.
- Only `c2-before-*` files exist (16); no `c2-after-*` files were written, so there is no "after composite draw" image to compare against.

### (c) Final presentation: UNKNOWN from the dump

The dumps are **"before"** images — the composite input surface before the composite draw — not the presented frame. They cannot establish what the screen showed. The on-screen photo from this run (taken ~1 minute after launch, around the time the trace fired) still showed the top black, but a single photo is not a measurement of the presented frame at swap 1500, so final presentation remains **unknown** and must be verified by the post-review test runs (Baseline / Test A / Test B) with attention to the actual middle-band image.

### Diagnostic note

`--fh1_debug_composite_draws=1500` is already in `run_game.cmd` (added before the fix run, alongside the existing `--fh1_debug_composite_dump` dir flag), so the next run will capture the composite at swap 1500 the same way. The empty dump directory in earlier runs was not evidence the trace failed — the dump block requires the `composite_draws` flag, which was previously unset (default 0).

---

## Review notes (carried over from the implementer)

- **Data structure:** the map is still `std::unordered_map<uint32_t, uint32_t> untile_bands_` (header line 274) — the value is now a **bitmask** instead of a max band, so **no header change was needed**. Bit = band / 256 (`kUntileBandRows = 256`, file-local; bands are 0/256/512 within the 720-row untile height, and both record sites already reject `untile_band >= untile_height`, so bits are only 0–2). Name-collision checked across the rexglue tree — none.
- **Deduplication:** recording is `mask |= UntileBandBit(band)`. Repeated draws in the same band set the same bit, so each band produces exactly one copy-down. Verified by code reading, not by running: mask bits are only set, never duplicated per draw.
- **Band-0 caveat:** band 0 is never recorded (both record sites guard on `untile_band != 0`) and `Fh1D3D11CopyDownRows(720, 0) = 0`, so "bands 0+256+512" yields the 256 and 512 copy-downs with band 0 as a no-op — exactly as before, by design, not a missing copy-down.
- **Ordering / ownership:** bands run ascending; the last copy-down is the highest band, leaving its rows at EDRAM row 0 for the resolve that follows the flush (`IssueCopy` calls `EndUntiledTiling` when a non-banded resolve arrives with bands pending) — the same end state the old single-band code produced. Surface ownership behavior is untouched; the per-band body (snapshot → `TransferRects` → copy-back, same rect/geometry/error paths, same log line) is verbatim, just executed once per recorded band. On a band failure the surface is left in the map for retry, as the old code did (it `continue`d without marking completed); a band with `copy_rows == 0` is skipped and the surface still completes, as before.
- **One review point worth Seth's attention:** each band's copy-down re-snapshots the texture (`CopyResource` into the scratch copy-down texture) and copies back, so a later band's transfer reads the texture including earlier bands' results. Band 512's source rows ([512,720)) are untouched by band 256's write (which only touches rows [0,256) of the main texture), so each band reads its own original rows. The final state after ascending copy-downs has the highest band's rows at EDRAM row 0, matching the old single-band behavior. The swap-1500 log after the next run will show whether `band=256` copy-down lines now appear alongside `band=512` — that is the direct verification. (This run's log already shows it: 1,998 band-256 lines alongside 2,298 band-512 lines.)
- **Regression behavior (by code reading, not yet tested):** band 256 only → one 256 copy-down (256 rows); band 512 only → one 512 copy-down (208 rows), byte-identical path to the old code; bands 256+512 → both copy-downs, ascending; repeated band-256 draws → one 256 copy-down. These still need the post-review test runs to verify.

---

## What was NOT changed

- Viewport mapping — untouched.
- Resolve planning (`Fh1PlanResolve`) — untouched.
- Shader code — untouched.
- `TransferRects` — untouched (same call, same arguments, now invoked once per recorded band).
- The presenter — untouched.
- All other D3D11 rendering paths — untouched.
- The Vulkan executor — left alone per scope, even though it has the identical `max(band)` pattern at its lines 3103–3104 and 3239–3240 (and `M src/graphics/vulkan/fh1_native_executor.cpp` appears in the rexglue working-tree status from earlier work, not from this fix).
- `run_game.cmd` — not changed by the fix itself. (The `--fh1_debug_composite_draws=1500` diagnostic flag was added separately before the fix run so the composite dump would fire; it is a launch flag, not a code change.)
- No config, no headers, no other source files. `std::max(band, …)` no longer appears anywhere in `fh1_native_executor.cpp` (asserted in the edit).

---

## What happens next (after Seth's review)

Seth reviews this package for missed bands, incorrect deduplication, ordering problems, and regressions. Only after he approves, run the regression tests:

| Test | Expected |
|---|---|
| Band 256 only | One 256 copy-down |
| Band 512 only | One 512 copy-down |
| Bands 256 + 512 | Both copy-downs |
| Bands 0 + 256 + 512 | All three copy-downs (band 0 as no-op by design) |
| Repeated Band 256 draws | One 256 copy-down |

Then rerun the actual FH1 scene: Baseline (all three bands visible), Test A (`0x05`: middle world visible), Test B (`0x02`: sky and cockpit visible). And verify the actual middle-band image — the counts alone do not prove the rendering is fixed.
