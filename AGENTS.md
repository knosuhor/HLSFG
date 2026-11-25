# AGENTS.md

Working notes for anyone (human or agent) touching this repository. Read
before changing code. Keep this file current when a stage changes.

## Project goal (do not lose this)

HLSFG must stay **game-agnostic**. The target is compatibility parity with
other OptiScaler nvngx replacements: everywhere FSR3 / ANR work under
OptiScaler, HLSFG should work too. Measuring in one game is fine; tuning
constants, thresholds or heuristics for one game is not. Every per-game
observation should be generalized into either (a) contract handling,
(b) a host-behavior detector with a principled measurement, or (c) a
documented, opt-in experiment.

## Current stage (Nov 2026)

Phase 1 is live end-to-end: the LSFG backend (LS 3.2.1 shader bank +
lsfg-vk graph semantics, D3D12-native) generates real intermediate frames
in Cyberpunk 2077 and Control at 4K with the DLSS-G present contract.

Validated on real hosts:

- Cyberpunk 2077: FG works; the game's Streamline hudless tag is a static
  near-white dummy (content gap ~0.9, variance 0) - the sanity detector
  breaks it within the first ~10 evaluates and the session interpolates
  the raw backbuffer.
- Control: R10G10B10A2 path runs (typed UAV stores); hudless behaves as a
  frozen frame - caught by the freshness verdict. Diagnostic blacklist only
  until the detector is re-validated there.
- NVIDIA Streamline sample: contract validation host for phase 0/0.5.

The visible behavior users care about: motion is interpolated (no judder
from duplicated slots), no UI strobe from the hudless delta, HUD slightly
warps during camera motion when no usable hudless exists (accepted for
now, see backlog).

## Architecture map

```
src/ngx_exports.cpp   22 NVSDK exports + init/feature surface
src/plugin.cpp        OptiScaler plugin tab, ini persistence [Plugin.HLSFG]
src/backends.*        IFGBackend dispatch, Passthrough + Blend backends
src/lsfg_backend.*    the real backend: ring buffer of source frames,
                      LSFG graph dispatch, OutputReal contract, hudless
                      sanity probes, UI composite modes
src/lsfg/pipeline.cpp lsfg-vk v2.0.0-dev graph on ID3D12Device
src/lsfg/shader_bank  extraction of the 202 RT_RCDATA resources from the
                      user's Lossless.dll (runtime, never vendored)
src/ui_composite.*    one compute pass, two modes (see below)
src/invert_copy.*     optional byte-polarity inversion for HUDLessColor
src/mfg_state.*       settings + shared runtime state (g_hudlessHealth)
```

### Contract invariants (learned the hard way, do not regress)

- Present order is `[OutputInterpolated, OutputReal]`; OutputReal must hold
  the CURRENT real frame, otherwise every second presented slot shifts
  back in time and motion visibly oscillates.
- Burst mode in OptiScaler = one Evaluate per real frame carrying all
  outputs (MultiFrameIndex missing or 0). Non-burst (per-slot calls) keeps
  the blend fallback.
- Assumed resource states at Evaluate: backbuffer
  NON_PIXEL_SHADER_RESOURCE, hudless COPY_DEST, ring/interp UAV.
  Every transition we add must restore these exactly.
- TYPELESS inputs: views use the concrete sibling (UNORM) format; copies
  within a format family use typeless casting. Ring keeps the raw source
  family and family-mismatched sources fall back instead of risking device
  removal.
- Output name must stay `nvngx_dlssg.dll` and payloads/params use the
  `DLSSG.*` keys; the provider folder name is free (HLSFG).

### HUD handling, three layers (in priority order)

1. If a host provides `DLSSG.UI`/`UIAlpha` (none measured so far; probed
   and logged once per session at call #5): consume for exact recomposition.
   Not implemented yet.
2. Healthy hudless: interpolate the hudless ring, composite mode 0:
   `out = interp + (backbuffer - hudless) * mask(threshold)`.
3. Garbage hudless (detector verdict) or untagged game: interpolate the raw
   backbuffer; composite mode 1 (temporal UI mask) is **opt-in and
   experimental** (`TemporalUi`, default off) because a per-pixel threshold
   without temporal state judders in low-contrast motion. Do not turn it on
   by default without adding state/hysteresis (see backlog).

### Sanity detector (in lsfg_backend.cpp, rides the ROI readback probe)

- Startup, 3 samples: garbage when `|mean(bb)-mean(hud)| > 0.30` while the
  hudless ROI is optically flat (`var(hud) < 0.25*var(bb)`, `var(bb) > 100`).
  The flatness term protects hypothetical complemented-storage hosts, which
  would carry scene structure.
- Periodic, every ~300 calls: garbage when the backbuffer mean moved
  (>0.01) while the hudless mean stayed still (<25% of that), or the content
  verdict fires again; catches frozen-frame hosts (Control).
- Verdicts are sticky per session; once broken, HUDLess is nulled for the
  whole session and the tab shows the health state (g_hudlessHealth).

## Build / deploy / test loop

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build 'C:\Users\knosu\mfgtest\mfg-provider\build' --config Release --target mfg-provider
Copy-Item .\build\Release\nvngx_dlssg.dll <game>\custom\HLSFG\dlssg\nvngx_dlssg.dll -Force
```

- Lossless.dll is picked up from the default Steam install roots.
- Test hosts: Cyberpunk 2077 (D:\SteamLibrary), Control. FG on through the
  game's own DLSS option; OptiScaler FrameGen tab multiplier overrides the
  provider's MultiFrameCountMax.
- Logs land next to the dll (`hlsfg.log`). Lines prefixed `lsfg: stage=`
  order the per-evaluate pipeline; `hudless sanity:` is the detector.
- Frame dumps: `custom/HLSFG/dlssg/mfg-dump.txt` with content
  `<count> <dir>` enables ROI readback dumps for offline forensics
  (tools/dump_analyze). Remove the file for clean smoke tests.

## Conventions

- Log actionable facts through MFG_LOG_INFO with stable prefixes; per frame
  logs only at calls <= 3 or >= 500 spacing.
- New settings: atomic in mfg_state.h, persisted in plugin.cpp
  ([Plugin.HLSFG] ini), a tab control only when a user decision is real
  (not for internal debug knobs).
- No game names in shader/code comments as tuning justification: describe
  the measured host *behavior* class instead ("static flat tag", "frozen
  frame"), the game belongs in AGENTS/README observations.
- CRLF, no tabs in new code, comments explain *why* (measured behavior or
  contract), not what.

## Backlog (priority order)

1. **Temporal UI mask with state**: ping-pong mask texture + hysteresis
   (findings stay pinned until they really move), optionally gated by
   global camera motion from the flow field. This is the path to sharp HUD
   in garbage-hudless hosts without judder. Do not ship stateless again.
2. **DLSSG.UI / UIAlpha consumer**: exact recomposition when a host
   provides the UI layer; probe logging already in place.
3. **Subrect support** (`*SubrectBaseX/Y/Width/Height`): copy regions
   instead of whole frames where the host presents via subrects.
4. **R10G10B10A2 + HDR** color path audit (PQ10 profiles) and the
   BidirectionalDistortionField / NoWarp inputs.
5. Vulkan surface (phase 2, MAKO FG later).

## Housekeeping

- CI builds the dll and dumps the export table (22 NVSDK exports +
  OptiPluginInit) - keep that green.
- Push: `git -c credential.helper='!gh auth git-credential' push` (gh
  auth on the dev machine). Repo: github.com/knosuhor/HLSFG.
- Do not commit anything NVIDIA/LS proprietary; shaders extract at runtime.
- `_staging/` is scratch (ignored); keep diagnostic outputs there, not in
  the repo.
