# HLSFG

Custom **frame generation provider for OptiScaler** implementing the
`nvngx_dlssg` replacement contract (`custom/<name>/dlssg`). HLSFG runs
Lossless Scaling's frame generation shaders directly on the game's D3D12
device and drives them through the DLSS-G input contract OptiScaler hands
to FG providers, with game-agnostic HUD handling layered on top.

The project goal is explicitly **not** single-game tuning: HLSFG should
work wherever other OptiScaler nvngx replacements (FSR3, ANR) work.

Status: **Phase 1** - end-to-end multi-frame generation validated in real
Streamline 2 titles (Cyberpunk 2077, Control). See `docs/CONTRACT.md` for the
measured contract and `docs/PHASES.md` for the phase plan.

## What it does

- **LSFG backend** (default in games): extracts the LS 3.2.1 shader bank from
  a locally installed `Lossless.dll` at runtime (nothing ships in this
  repository) and runs the lsfg-vk v2.0.0-dev flow/generate graph natively
  on the caller's D3D12 device. One Evaluate per real frame produces the
  interpolated slot for the `[OutputInterpolated, OutputReal]` present pair;
  `OutputReal` is always refreshed with the current real frame (intermediate
  frames never shift real slots back in time).
- **Blend / Passthrough backends** for contract validation and log runs.
- TYPELESS-safe pipeline: games that tag inputs `R8G8B8A8_TYPELESS` (or the
  B8G8R8A8 / R10G10B10A2 families) run the same graph via concrete-view
  casting; no silent fallbacks on format aliases.
- **Garbage-hudless detection**: HUD-less tags that do not track the
  backbuffer (a static white dummy in Cyberpunk 2077, a frozen frame in
  Control) are detected at runtime from small ROI readback probes
  (content gap + optical flatness, then frame-to-frame freshness) and
  dropped for the rest of the session; the interpolator then runs on the
  raw backbuffer, exactly like an untagged game.
- **HUD recomposition, two modes**:
  - *hudless delta* (default): with a healthy HUD-less capture the UI layer
    is `(backbuffer - hudless)`, detected per frame with a threshold and
    layered onto generated frames.
  - *temporal mask* (experimental, off by default): when no usable hudless
    exists, pixels that did not change between consecutive real frames are
    pinned to the current real frame instead of being interpolated. Being a
    per-pixel threshold without temporal state it can judder in low-contrast
    motion; it is opt-in (`TemporalUi`).
- **Plugin tab** in the OptiScaler menu: backend selector, flow scale
  (pinned or tracking the FrameGen multiplier), UI recomposition toggle,
  temporal mask toggle, HUDLess storage polarity, log toggle and a live
  hudless-health indicator, persisted in `[Plugin.HLSFG]`.

## Requirements

- **OptiScaler master** (custom provider support; released 0.9.5-final has
  none).
- For `FGOutput=dlssg`: full `streamline/` folder next to OptiScaler.
- A legally installed **Lossless Scaling** on the machine (the DLL is located
  in the default Steam install roots or can be pointed at explicitly); the
  202 `RT_RCDATA` shader resources are extracted at runtime.
- Game side: Streamline 2 title with native DLSS FG, run through
  OptiScaler's `dxgi.dll`. D3D12 only; the Vulkan surface is stubbed until
  phase 2.

## Install into a game

1. Build -> `nvngx_dlssg.dll` (the output name is part of the contract).
2. `<game OptiScaler folder>/custom/HLSFG/dlssg/nvngx_dlssg.dll`
3. `OptiScaler.ini`:
   ```ini
   [FrameGen]
   FGNvngxReplacement=HLSFG
   Enabled=true
   FGInput=dlssg
   FGOutput=dlssg

   [Plugin.HLSFG]
   Backend=2         ; 0 passthrough, 1 blend, 2 lsfg
   Flow=             ; empty = track FrameGen multiplier, else pin 1..4
   UiComposite=true
   ```
4. Enable the game's native DLSS frame generation option.
5. Log lands next to the dll as `hlsfg.log`.

## Build

MSVC via CMake (VS 2022):

```bat
cmake -B build && cmake --build build --config Release
```

NGX SDK headers are fetched from OptiScaler's vendored copy (see the CI
workflow). The repository carries no NVIDIA or Lossless Scaling files.

## Layout

```
src/ngx_exports.cpp    NGX surface: 22 exports + OptiPluginInit
src/plugin.cpp         settings tab, [Plugin.HLSFG] persistence
src/backends.{h,cpp}   IFGBackend + Passthrough + Blend
src/lsfg_backend.*     phase 1: LSFG graph on the caller's device, hudless
                       sanity detector, present-contract output handling
src/lsfg/              shader bank extraction + graph pipeline
src/ui_composite.*     hudless-delta and temporal-mask UI modes
src/invert_copy.*      HUDLessColor byte-polarity pass (auto/forced)
docs/CONTRACT.md       measured OptiScaler contract, source references
docs/PHASES.md         roadmap
```

## Known limitations (honest list)

- No usable hudless means the HUD is interpolated with the world: it can
  warp/smear during camera motion (same class of artifact as any FG without
  a UI layer). OptiScaler's own `FGDisableHudless` converges to the same
  state here: once the detector breaks a garbage hudless, the two paths are
  identical.
- `DLSSG.UI` / `UIAlpha` / subrect parameters are logged when present but
  not yet consumed (no SL2 host measured so far provides them).
- Subrects (`*SubrectBaseX/Y`, `*SubrectWidth/Height`) are not handled yet;
  titles presenting via subrects fall back to whole-frame paths.
- The temporal UI mask is experimental and disabled by default (see above).
- Vulkan surface is phase 2.

## Licensing notes

- Own code: GPL-3.0-or-later (matches OptiScaler, enables code sharing with
  lsfg-vk).
- `include/optiscaler_plugin.h` is MIT (OptiScaler plugin SDK), vendored.
- NGX SDK headers are fetched at build time from OptiScaler's vendored copy;
  not stored here.
- Lossless Scaling resources are **never** redistributed: the shader bank is
  extracted at runtime from the user's own installation.
