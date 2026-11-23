# mfg-provider

Custom **frame generation provider skeleton for OptiScaler** implementing the
`nvngx_dlssg` replacement contract (`custom/<name>/dlssg`). Goal of the wider
project: run image-based multi-frame generation (lsfg-vk / MAKO FG class
models) **under the game HUD**, receiving the HUD-less frame OptiScaler hands
to FG providers, instead of interpolating the final presented frame where HUD
artifacts are worst.

Status: **Phase 0** - the contract layer with a passthrough backend.
See `docs/PHASES.md` for the phase plan and reuse map.

## What phase 0 does and does not

- Loads into games that already have native DLSS FG through OptiScaler,
  answers `Init`/`CreateFeature`/`EvaluateFeature`/`PopulateParameters_Impl`
  correctly, and logs every FG input the game and OptiScaler pass.
- Produces **no frames**: the game renders real frames only. This is the
  safe validation state (same visible behavior as recording mode in nr-record).
- Adds an "MFG Provider" tab to the OptiScaler menu (backend selector,
  2x-4x multiplier, log toggle), persisted in `[Plugin.<name>]`.
- D3D12 only; Vulkan surface is present as correct stubs until phase 2.

## Install into a game (Windows or Proton)

1. Build (or take the CI artifact) -> `nvngx_dlssg.dll`.
2. In the game's OptiScaler folder:
   `custom/MFG/dlssg/nvngx_dlssg.dll`
3. In `OptiScaler.ini`:
   ```ini
   FGNvngxReplacement=MFG
   ```
4. In game, enable the DLSS/NVIDIA frame generation option.
5. Linux/SteamOS launch options (as with any OptiScaler dll):
   `WINEDLLOVERRIDES="dxgi=n,b" %command%`
   On Windows this step does not apply: the game loads OptiScaler's local
   `dxgi.dll` naturally, no launch options needed.
6. Log lands in `custom/MFG/dlssg/mfg-provider.log`.

Smoke test order: a Streamline 2 game with native DLSS FG first
(that input path has full HUD-less + UI recomposition support), then wider.

## Build

MSVC via CMake (VS 2022):

```bat
cmake -B build && cmake --build build --config Release
```

GitHub Actions workflow included (`.github/workflows/build.yml`); the build
job dumps the export table, which is the phase 0 acceptance check: 22
`NVSDK_NGX_*` exports plus `OptiPluginInit`.

## Layout

```
src/ngx_exports.cpp    NGX surface: 22 exports, signatures per NgxFeatureModule::LoadExports
src/plugin.cpp         OptiPluginInit: settings tab, [Plugin.<name>] persistence
src/backends.{h,cpp}   IFGBackend + Passthrough (phase 0) + Blend (stub)
src/log.cpp            mfg-provider.log next to the dll
docs/CONTRACT.md       the exact OptiScaler contract with source references
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
