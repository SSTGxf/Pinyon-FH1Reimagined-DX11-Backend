# HUD-hide diagnostic — setup note (2026-11-16)

Diagnostic-only, reversible, default OFF. Goal: tell whether the black-half
defect (presented-2850.ppm: rows 0–360 black, rows 360–720 populated) belongs
to the 3D pipeline or only appears when the HUD is composited.

## (a) How the HUD is rendered, and where the gate is

There is NO separate UI pass and NO pre-existing HUD toggle. The guest
draws the HUD as part of the normal draw stream, but it is distinguishable:

- The HUD renders to its own 1280-wide EDRAM surfaces with SMALL viewports
  (320x192, 160x96, 80x48) — mostly thousands of line-list glyphs
  (text), plus textured quads (minimap, speedometer).
- The 3D world uses a 1280-wide viewport: the two world halves are
  1280x360, the post pass is 1280x720.
- Evidence: `command_processor.cpp` c2-probe comment (~line 2826–2839):
  "The HUD's thousands of line-list glyphs would otherwise spend the
  budget" / "The HUD's own 1280-wide surfaces get 320x192, 160x96 and
  80x48 viewports" / "the two 1280x360 world halves and the 1280x720
  post pass."
- The post pass (1280x720) composites the world halves + HUD surfaces
  onto the front buffer. Hiding the HUD surface draws leaves those
  surfaces blank, so the post pass composites a blank HUD.

Gate (single place, D3D11 only, no shared-core change):

- `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\command_processor.cpp`
  - `REXCVAR_DECLARE(bool, fh1_debug_hide_hud);` — line 46.
  - Gate in `D3D11CommandProcessor::IssueDraw`, lines 1899–1908, right
    after `viewport_info` is computed and BEFORE `BindDrawRenderTargets`
    (so a skipped HUD draw never binds a render target and never touches
    ownership/clears/copies):
    - Condition: `fh1_debug_hide_hud` is true AND the draw writes color
      (`normalized_color_mask != 0`, spares depth-only shadow passes)
      AND `viewport_info.xy_extent[0] < 1280 * draw_resolution_scale_x`
      (narrower than the full frame = HUD).
    - Action: `++draw_counters_.dropped_other; return true;`
      (successful no-op, not an error).
- Cvar definition:
  - `E:\PinyonShift\work\.local\rexglue\src\graphics\flags.cpp`, line 33:
    `REXCVAR_DEFINE_BOOL(fh1_debug_hide_hud, false, "GPU", ...)`
    with `.lifecycle(kHotReload)`.

NOT touched: resolve, present, surface allocation, band/untile stack,
present-device path, 3D draw submission. The only draws skipped are
small-viewport color draws.

## (b) Cvar / config used, and its default

- Cvar: `fh1_debug_hide_hud`, bool, default `false` (HUD renders normally).
- Set in the preview config, which the game reads at launch.

## (c) Exact config change made

File: `E:\PinyonShift\work\.local\preview\config\pinyon_shift.toml`
Appended at the end (2026-11-16):

```
# Diagnostic (2026-11-16): hide all HUD/2D overlay draws, 3D world only.
# Revert: set fh1_debug_hide_hud = false
fh1_debug_hide_hud = true
```

No other config lines were changed. The audit/trace cvars that were on
(`pinyon_audit`, `fh1_swap_fetch_trace`, `fh1_debug_tile_trace_swap`,
etc.) were left as they were.

## (d) How to turn it OFF (revert)

In `E:\PinyonShift\work\.local\preview\config\pinyon_shift.toml`, change:

```
fh1_debug_hide_hud = true
```

to:

```
fh1_debug_hide_hud = false
```

Then relaunch the game (config is read at launch). To remove the
diagnostic entirely, delete those three appended lines. No rebuild is
needed to toggle it — the cvar defaults to false and the gate is a
no-op when false. (A rebuild is only needed if the code itself is
removed.)

## (e) What to look for when you run it

Launch via the scheduled task "PinyonShift Test Run" (session 1 — the
only way you see it). Drive into gameplay and look at the frame:

- **Black half GONE (full 720 rows render, only the 3D world, no
  speedometer/minimap/text):** the corruption belongs to the HUD
  compositing path. The 3D pipeline is fine; the defect appears when
  the HUD is composited onto the front buffer.
- **Black half STILL THERE (upper half still black, world only in the
  lower half, no HUD visible):** the corruption is in the 3D pipeline
  itself (the world halves / resolve), not the HUD. The HUD is
  exonerated.
- **Sanity check that the gate is working:** the speedometer, minimap,
  and all HUD text/icons must be GONE. If any HUD element is still
  visible, the gate is not catching that element's draws — note which
  one and report it (it would use a full-width viewport or a different
  path).
- The 3D world (car, terrain, roads, sky, cockpit) should still render
  exactly as before, just without the HUD on top.

## (f) Build verification

- Build: `E:\PinyonShift\work\.local\muse_build_game.bat` via
  `CMAKE_BUILD_PARALLEL_LEVEL=4`, run 2026-11-16 ~00:05 CST.
- `E:\PinyonShift\work\.local\muse_build_game.log`: **BUILD_EXIT=0**.
  Compiled clean (only pre-existing unused-field warnings).
- `E:\PinyonShift\work\out\build\win-amd64-release\pinyon_shift.exe`:
  61,851,136 bytes, mtime 11/15/2026 11:07:11 PM (PC time) — NEW.
- `rexgpu-fh1.dll` (same dir + `rexglue-artifacts\`): 11,341,312 bytes,
  mtime 11/15/2026 11:07:06 PM (PC time) — NEW (grew from 11,311,616,
  consistent with the added code).
- DLL string search (on the built `rexgpu-fh1.dll`):
  `fh1_debug_hide_hud` = **1 hit** (cvar is in the binary);
  `fh1_debug_band_suppress` = 1 hit (sanity); `untile` = 27 hits.

## Status

Ready to run. The game was NOT launched (SSH launches run in session 0
— Seth hears it but does not see it). Launch via "PinyonShift Test Run"
when ready. Local copies of the two edited files:
`~/workspace/dx11-hud-hide/flags.cpp` and
`~/workspace/dx11-hud-hide/command_processor.cpp`.
