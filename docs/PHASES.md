# Phase plan: from contract validation to MAKO

One architecture, three phases. The seam is `IFGBackend`
(`src/backends.h`); the NGX surface, OptiScaler plumbing, settings UI and
logging never change between phases.

| Phase | Backend | Ships | Purpose | Status |
|-------|---------|-------|---------|--------|
| 0 | Passthrough | `nvngx_dlssg.dll` | validate contract end to end; inventory the real FG inputs per game | done, measured 2026-11-23 on StreamlineSample + OptiScaler 10.0.0-dev |
| 0.5 | Blend | copy of the frame through `DLSSG.Output*` | prove ownership of the FG output path (present/pace) before any inference | done, visible image at "mfg x2"; UI-over-interpolated-slots flicker is the known open point |
| 1 | LSFG | lsfg-vk GPL inference (loader + translated shaders) | first real MG frame generation below the HUD | |
| 2 | MAKO FG | reimplementation of the open-weight model under CC BY-NC, or author-supported runtime | legally clean default; A/B against phase 1 | |

## Measured corrections to the plan (2026-11-23)

These come from the first live run; details and log excerpts in
`docs/CONTRACT.md`, section "Measured".

- Passthrough is **not** inert under `FGOutput=dlssg`: every presentation
  slot of the frame group goes through the provider, so writing nothing
  means black FG slots. Phase 0 therefore requires the Blend backend for
  anything the user is supposed to watch.
- Evaluate is called **once per real frame** with the whole output group
  (burst mode), not once per presented slot; the backend handles both
  (`burst` flag in `src/backends.cpp`).
- The input inventory is `DLSSG.*` keys (`DLSSG.HUDLess` above all), not
  the classic NGX upscaler keys; `g_probeKeys` was corrected accordingly.
- Phase 1 consumes `DLSSG.HUDLess` (fallback `DLSSG.Backbuffer`).

## Reuse map (phase 0 -> 1/2)

Everything below is written exactly once, in phase 0, and inherited:

- NGX surface: 22 exports, signatures, result codes (`src/ngx_exports.cpp`)
- OptiScaler integration: folder layout, `FGNvngxReplacement`, plugin tab,
  INI persistence, log file (`src/plugin.cpp`, `docs/CONTRACT.md`)
- Feature handle lifecycle, thread safety, shutdown paths
- Input inventory: which `DLSSG.*` keys a real game passes at Evaluate
  (`g_probeKeys` in `src/backends.cpp`, extend from real logs)
- Output-chain handling: CopyResource with transition barriers for the
  measured resource states; phase 1 replaces the copy with inference
  writing the same outputs (`BlendBackend::EvaluateD3D12` is the template)
- Validation: nr-record replays (same recorded inputs replayed through any
  provider dll) plus game smoke tests under Proton

Only `Interpolate(frameA, frameB, t)` changes per phase.

## Phase 1 notes (LSFG)

- Measured correction (LS 3.2.1, 2026-11-23): LS now ships **DXBC SM5
  compute shaders** in RT_RCDATA, not SPIR-V. The backend runs them
  natively on D3D12, so no Vulkan, no statically linked DXVK, no
  interop; the lsfg-vk loader is replaced by our own PE RCDATA parser
  and the execution graph is ported from lsfg-vk tag v2.0.0-dev (last
  GPL-3.0-or-later release, attribution in source). Current lsfg-vk
  (lsfg-vk.dev, post-migration 2.0) is CC BY-NC-ND and off limits.
  Id-to-shader table and probe findings: `docs/PHASE1.md`.
- Distribution: **never ship Lossless Scaling resources**. Runtime reads from
  the user's own LS install; process-local translation only (lsfg-vk pattern).
- MAKO FG's own screen-overlay/HUD stage is skipped entirely: in this
  architecture HUD is composited by OptiScaler per output frame.
- The live test confirmed OptiScaler does hand over the HUD-less frame
  (`DLSSG.HUDLess` populated every call) and also confirmed where its
  recomposition ends: UI drawn over interpolated slots flickers. Phase 1
  must verify OptiScaler's UIComposition/DrawUIOverFG stage closes that
  gap, or the backend composites UI itself from the real-frame copy.
- Maintenance risk: LS version churn. Measured: the 3.1-era resources
  were SPIR-V (lsfg-vk raw-id = base 49 scheme), 3.2.1 packs raw DXBC
  ids 101-302 with the FG family at 255-279. Support stays per LS
  version; `tools/lsfg_probe` (see `docs/PHASE1.md`) regenerates the
  binding tables against a new install before the backend is pointed
  at it.

## Phase 2 notes (MAKO FG)

- License: CC BY-NC 4.0, open weights (< 1 KB), **own runtime allowed** but
  file formats undocumented -> collaborate with the MAKO author instead of
  reverse engineering.
- The prebuilt MAKO runtime is a native Linux library; a Windows provider dll
  cannot load it, which is why the inference must be reimplemented as
  shaders inside the dll (D3D12 compute or Vulkan via wine-vulkan).
- MAKO targets 2x/3x/4x and HDR10; the `DLSSG.MultiFrameCountMax` report
  stays the single knob asking OptiScaler for the multiplier.

## How to publish safely

Phases 0 and 0.5 are contract work with no third-party IP: publish freely
(choose GPL-3.0-or-later to match OptiScaler). Phase 1 stays bring-your-own-
LS. If the project goes public, prefer debuting at phase 2 (MAKO), keeping
phase 1 as internal validation.
