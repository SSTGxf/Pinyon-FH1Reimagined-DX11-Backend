# Launcher GPU selector + set-graphics script fix (2026-11-18)

What is in this folder and where each file belongs:

- `MainWindow.xaml` — WPF launcher window. Goes to
  `launcher/PinyonShift.Launcher/MainWindow.xaml` in the Pinyon 0.9.1 source.
  Adds a "GPU (DirectX 11)" ComboBox (GTX 570 / RX 5700 XT / Auto) under the
  Graphics API picker, and a Direct3D 11 entry in the Graphics API picker.
- `MainWindow.xaml.cs` — WPF launcher code-behind. Goes to
  `launcher/PinyonShift.Launcher/MainWindow.xaml.cs` in the Pinyon 0.9.1 source.
  Passes `-D3d11Adapter <tag>` to set-graphics-experiment.ps1, adds
  `d3d11_adapter` to GraphicsSettings, and restores the GPU pick on load.
- `set-graphics-experiment.ps1` — graphics settings script. Goes to
  `tools/set-graphics-experiment.ps1` in the installed Pinyon 0.9.1 source
  (`%LocalAppData%\PinyonShift\source\0.1.1\tools\`). Accepts `d3d11` in the
  GraphicsApi ValidateSet (was vulkan-only, which rejected the save), writes
  `gpu_backend` from the chosen API instead of forcing vulkan, and adds a
  `D3d11Adapter` parameter that writes `d3d11_adapter` (the d3d11_adapter cvar
  substring, e.g. "GTX" for the GeForce GTX 570) to pinyon_shift.toml.

These are launcher-side changes only. They do not add the D3D11 backend to the
game; the game still needs the ported D3D11 backend (see the repo README and
the testkit DLLs in testkit-dlls-20261118).
