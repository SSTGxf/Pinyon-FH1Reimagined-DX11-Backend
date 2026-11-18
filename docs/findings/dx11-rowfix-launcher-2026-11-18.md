# DX11 rowfix + launcher D3D11/GPU selector — 2026-11-18

## source_row_offset fix (D3D11 backend)

`src/graphics/d3d11/command_processor.cpp`, `IssueCopy()`:

```cpp
// before
const uint32_t source_row_offset = untile_band;
// after
// The plan and owner rectangles already use absolute screen rows. The
// shader's extra source-row translation is therefore zero; using the
// reset window offset here would misaddress a banded source.
constexpr uint32_t source_row_offset = 0;
```

Background: `untile_band` is derived from `PA_SC_WINDOW_OFFSET.window_y_offset`,
which reads 0 at resolve time, so `source_row_offset` was stale (0) — and
`Fh1D3D11UntileBand` returns the row offset (0/256/512), not the band index,
while `plan.y0` already holds the band's real rows (512 for band 2). The fix
stops the double-count. The same fix is in the 0.9.1 port (Copilot).

## First test result: world gone (unproven)

The D: game was rebuilt with the fix (BUILD_EXIT=0) and launched. In the
car-preview view the world did not render — flat blue sky, the Corrado
black-box artifact, HUD only. Seth: "World is now gone." The fix did NOT
bring the world back in that view; it looks worse than the pre-fix flat grey.
Caveat: that was the car-preview/garage view, not confirmed free roam. No
free-roam retest happened yet, and no edram3d/resolve probe verdict was
extracted from that run. If free roam also shows no world, revert this fix
(restore `command_processor.cpp.musebak-rowfix-20261118` on the PC) and
retest — the pre-fix state was flat-grey-but-present.

## Launcher: DirectX 11 + GPU selector

See `launcher-gpu-selector/` in this repo. The installed WPF launcher
(PinyonShift-Launcher) now shows DirectX 11 in the Graphics API picker and
a "GPU (DirectX 11)" dropdown (GTX 570 / RX 5700 XT / Auto), and
`set-graphics-experiment.ps1` accepts `d3d11` and a `D3d11Adapter` parameter
that writes `d3d11_adapter` (the `d3d11_adapter` cvar substring) to
`pinyon_shift.toml`. Saved and verified: `graphics_api: d3d11`,
`d3d11_adapter: GTX`.

Note: the installed 0.9.1 game is still the stock build, so it has no D3D11
backend in its DLL yet — the setting saves, but the game cannot run D3D11
until the 0.9.1 full-game build (with the ported backend) lands. The DLL
swap shortcut does not work (stock exe + kit runtime + stock DLL are a
matched set; the SDK-built DLL needs its own runtime).

## Test-kit DLLs (not in this repo)

The built DLLs are binary and too large for this repo's text-only push, so
they are NOT committed here. They are collected on the PC in
`D:\PinyonShift\work\testkit-dlls-20261118\`:

- `rexgpu-fh1-d11-rowfix.dll` — D: build with this rowfix (11,511,296 B)
- `rexgpu-fh1-091-port.dll` — 0.9.1 SDK build with the ported backend (3,578,880 B)
- `PinyonShiftLauncher-wpf.dll` / `.exe` — rebuilt WPF launcher with D3D11 + GPU selector
- `PinyonShiftLauncher-desktop.dll` / `.exe` — rebuilt Desktop (Avalonia) launcher
