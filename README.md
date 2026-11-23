# mfg-provider

Custom **frame generation provider skeleton for OptiScaler** implementing the
`nvngx_dlssg` replacement contract (`custom/<name>/dlssg`). Goal of the wider
project: run image-based multi-frame generation (lsfg-vk / MAKO FG class
models) **under the game HUD**, receiving the HUD-less frame OptiScaler hands
to FG providers, instead of interpolating the final presented frame where HUD
artifacts are worst.

Status: **Phase 0.5** - contract layer validated end to end on a real SL2
game; Blend backend copies the frame through the FG output chain.
See `docs/PHASES.md` for the phase plan and `docs/CONTRACT.md` for the
measured contract.

## What it does and does not

- Loads into games that already have native DLSS FG through OptiScaler,
  answers `Init`/`CreateFeature`/`EvaluateFeature`/`PopulateParameters_Impl`
  correctly, and logs the `DLSSG.*` inputs the game and OptiScaler pass.
- **Passthrough backend**: observes only. Under `FGOutput=dlssg` every
  presentation slot flows through the provider, so without output copies the
  FG slots stay black - that is the measured phase 0 behavior, keep it for
  log/inventory runs.
- **Blend backend** (default for interactive use): copies the frame through
  `DLSSG.OutputReal` and `DLSSG.OutputInterpolated` every Evaluate. Visible
  repeated-frame output, healthy pacing, no interpolation yet.
- Adds an "MFG Provider" tab to the OptiScaler menu (backend selector,
  2x-4x multiplier, log toggle), persisted in `[Plugin.<name>]`.
- D3D12 only; Vulkan surface is present as correct stubs until phase 2.

## Requirements (measured, see docs/CONTRACT.md)

- **OptiScaler master** (v10.0.0-dev line). The released 0.9.5-final has no
  custom-provider support at all - the dll silently never loads.
- For `FGOutput=dlssg`: a full `streamline/` folder (SL 2 SDK: interposer,
  plugins, NGX dlls) next to the OptiScaler dll. Without it OptiScaler
  disables FGOutput at swap chain creation and the game errors with FG on.
- Game side: a Streamline 2 title with native DLSS FG, run through
  OptiScaler's `dxgi.dll`.

## Install into a game (Windows or Proton)

1. Build (or take the CI artifact) -> `nvngx_dlssg.dll`.
2. In the game's OptiScaler folder:
   `custom/MFG/dlssg/nvngx_dlssg.dll`
3. In `OptiScaler.ini`:
   ```ini
   [FrameGen]
   FGNvngxReplacement=MFG
   Enabled=true
   FGInput=dlssg
   FGOutput=dlssg

   [Plugin.MFG]
   Backend=1        ; 0 = passthrough (log only, black FG slots), 1 = blend
   Multiplier=2     ; reported as DLSSG.MultiFrameCountMax (2..4)
   ```
4. In game, enable the DLSS/NVIDIA frame generation option.
5. Linux/SteamOS launch options (as with any OptiScaler dll):
   `WINEDLLOVERRIDES="dxgi=n,b" %command%`
   On Windows this step does not apply: the game loads OptiScaler's local
   `dxgi.dll` naturally, no launch options needed.
6. Log lands in `custom/MFG/dlssg/mfg-provider.log`.

Smoke test used for the phase 0/0.5 validation: NVIDIA
Streamline_Sample (DX12, `-d3d12`) with OptiScaler master + SL 2.14.16,
RTX 3070, Windows 11.

## Build

MSVC via CMake (VS 2022):

```bat
cmake -B build && cmake --build build --config Release
```

The NGX SDK headers are fetched from OptiScaler's vendored copy (the CI
workflow shows the two URLs); the repository itself carries no NVIDIA
proprietary files.

GitHub Actions workflow included (`.github/workflows/build.yml`); the build
job dumps the export table, which is the phase 0 acceptance check: 22
`NVSDK_NGX_*` exports plus `OptiPluginInit`.

## Layout

```
src/ngx_exports.cpp    NGX surface: 22 exports, signatures per NgxFeatureModule::LoadExports
src/plugin.cpp         OptiPluginInit: settings tab, [Plugin.<name>] persistence
src/backends.{h,cpp}   IFGBackend + Passthrough (phase 0) + Blend (phase 0.5)
src/log.cpp            mfg-provider.log next to the dll
docs/CONTRACT.md       the exact OptiScaler contract, source references + measured findings
docs/PHASES.md         phase 0 -> 0.5 -> 1 (lsfg) -> 2 (MAKO) plan
include/               optiscaler_plugin.h (MIT, from OptiScaler plugin_sdk) + NGX SDK defs
```

## Licensing notes (pre-publication checklist)

- Own code: GPL-3.0-or-later (matches OptiScaler, enables code sharing with
  lsfg-vk in phase 1). Decide before the first public release.
- `include/optiscaler_plugin.h` is MIT (OptiScaler plugin SDK) and vendored so
  the code builds against the exact API it was written for.
- NVIDIA NGX SDK headers (`nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`, SPDX:
  LicenseRef-NvidiaProprietary) are **not** vendored in this repository. The CI
  workflow fetches them from OptiScaler's own vendored copy at build time. Build
  locally the same way, so the repo carries no NVIDIA proprietary files.
- Phase 1 must not ship Lossless Scaling resources; phase 2 model weights are
  CC BY-NC 4.0 and stay a separate, user-fetched download.
