# Phase plan: from contract validation to MAKO

One architecture, three phases. The seam is `IFGBackend`
(`src/backends.h`); the NGX surface, OptiScaler plumbing, settings UI and
logging never change between phases.

| Phase | Backend | Ships | Purpose |
|-------|---------|-------|---------|
| 0 | Passthrough | this skeleton | validate contract end to end; inventory the real FG inputs per game |
| 0.5 | Blend | copy/blend of HUD-less frame | prove ownership of the FG output path (present/pace) before any inference |
| 1 | LSFG | lsfg-vk GPL inference (loader + translated shaders) | first real MG frame generation below the HUD |
| 2 | MAKO FG | reimplementation of the open-weight model under CC BY-NC, or author-supported runtime | legally clean default; A/B against phase 1 |

## Reuse map (phase 0 -> 1/2)

Everything below is written exactly once, in phase 0, and inherited:

- NGX surface: 22 exports, signatures, result codes (`src/ngx_exports.cpp`)
- OptiScaler integration: folder layout, `FGNvngxReplacement`, plugin tab,
  INI persistence, log file (`src/plugin.cpp`, `docs/CONTRACT.md`)
- Feature handle lifecycle, thread safety, shutdown paths
- Input inventory: which keys a real game passes at Evaluate
  (`g_probeKeys` in `src/backends.cpp`, extend from real logs)
- Validation: nr-record replays (same recorded inputs replayed through any
  provider dll) plus game smoke tests under Proton

Only `Interpolate(frameA, frameB, t)` changes per phase.

## Phase 1 notes (LSFG)

- Reuse the lsfg-vk loader (PE parse + DXBC->SPIR-V via statically linked
  DXVK subset) as a library; GPL-3 both sides, no conflict with OptiScaler.
- Distribution: **never ship Lossless Scaling resources**. Runtime reads from
  the user's own LS install; process-local translation only (lsfg-vk pattern).
- MAKO FG's own screen-overlay/HUD stage is skipped entirely: in this
  architecture HUD is composited by OptiScaler per output frame.
- Maintenance risk: LS version churn changes resource offsets; expect the
  lsfg-vk compat treadmill.

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
