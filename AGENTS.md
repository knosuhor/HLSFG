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

- Cyberpunk 2077: FG works. CORRECTED 2026-11-25 (preview-verified): the
  hudless tag is HEALTHY in gameplay; the raw "HUDLess preview" combo
  shows the game image 1:1 without HUD. The old "near-white dummy" is
  a PHASE artifact: on pre-menu intro/logo screens the tagged hudless is a
  flat near-white image (content gap ~0.9, variance 0). Evaluate/DLSSG runs
  a few seconds on those screens, goes idle at the menu, and re-activates
  near the end of level loading with a real hudless. The detector's
  startup probes sample the intro phase and issue a sticky phase-blind
  "garbage" verdict that discards a healthy hudless for the whole session
  (known detector bug, top backlog item). This is contract-consistent
  host behavior, see "Streamline DLSS-G contract reference" below: hosts
  are supposed to null tags outside gameplay and often do not, leaving
  flat/stale content tagged instead.
- Control: R10G10B10A2 path runs (typed UAV stores). CONFIRMED 2026-11-26
  (user-verified in-game): the hudless tag is HEALTHY in gameplay, same
  pattern as Cyberpunk. Flat white/black tagged content appears only on
  menus/cutscenes/intro-like screens. The earlier "hudless frozen frame"
  diagnosis is RETRACTED, same phase artifact as in Cyberpunk. The
  R10G10B10A2 ROI probe path is still unaudited (10-bit readback), so the
  detector is likely silent there; do not read that silence as health.
- NVIDIA Streamline sample: contract validation host for phase 0/0.5.

The visible behavior users care about: motion is interpolated (no judder
from duplicated slots), no UI strobe from the hudless delta, HUD slightly
warps during camera motion only while the hudless is dropped; in Cyberpunk
that drop was the detector's intro false positive, not a broken tag. Once
the detector is phase-aware, healthy-tag hosts should show a sharp HUD.

## Streamline DLSS-G contract reference

Distilled from NVIDIA Streamline `docs/ProgrammingGuideDLSS_G.md`
(v2.14.16). This is the authoritative contract our nvngx provider is
standing in for; consult it before inventing heuristics. Facts below are
from the guide, with HLSFG implications noted.

### Required inputs and their semantics (guide 5.0/5.1)

- Required per frame: backbuffer (auto via SL proxy swap chain), depth,
  motion vectors. Hudless + a UI buffer are optional but "critical for
  best image quality" when interpolated UI would distort.
- **Hudless**: scene color BEFORE any UI/HUD, same color space and
  post-processing as the backbuffer, full viewable scene. Size must match
  the backbuffer (when no explicit extents are provided).
- **UI Alpha** (preferred, single channel) OR **UI Color and Alpha**
  (4ch). If both are tagged, SL uses UI Alpha. Requirements:
  - 0.0 for pixels with no UI, non-zero where UI exists;
  - must respect the standard blending formula
    `Final.RGB = UI.RGB + (1 - UI.A) * Hudless.RGB`;
  - when UI color is provided, RGB must be PRE-MULTIPLIED by alpha.
  - HLSFG: this formula is the principled basis of composite mode 0;
    `(bb - hudless)` recovers the additive UI layer without any UI tag.
- With Hudless + UI tagged, hosts can enable **UI recomposition**
  (`enableUserInterfaceRecomposition`): HUD and scene are interpolated
  separately and composited afterwards. Slight perf/memory cost. This is
  the official version of what our composite modes approximate.
- **Tags must be nulled outside gameplay**: the guide is explicit that
  when resource validity cannot be guaranteed (loading, paused, in menus,
  video cutscenes) hosts must "set all tags to null pointers to avoid
  stability or image quality issues". HLSFG implication: flat/stale
  tagged content on intro/menu/cutscene screens is EXPECTED host
  non-compliance, not a broken tag. Never diagnose garbage hudless from
  non-gameplay phases.
- Guide also recommends DLSS-G be turned OFF (`eOff`) in fullscreen menus,
  on leaderboard-like overlays, during window/resolution changes, and
  halving motion blur magnitude while FG is on.
- Fullscreen menu detection exists (`eEnableFullscreenMenuDetection`),
  but only when UI Color and Alpha is tagged; imperfect, manual off still
  preferred.

### Buffer formats, extents, subrects (guide 5.1/5.2/10.0/11.0)

- Extent resolution or resource size of Hudless/UI buffers must exactly
  match the backbuffer.
- **Subrect**: FG can run on a rectangle of the final color buffer; the
  backbuffer tag carries only the subrect info (resource pointer may be
  NULL). Region outside the subrect is copied as-is to generated frames.
  Maps to the nvngx `DLSSG.<res>SubrectBaseX/Y/Width/Height` params.
- **Dynamic resolution**: mvec/depth extents may change per frame; the
  final color buffer (or its subrect) must stay fixed size. There is a
  fixed processing resolution (`dynamicResWidth/Height`) for mvec/depth.
- **HDR**: use R10G10B10A2 / HDR10/BT.2100. FP16 + scRGB is NOT supported
  (cost). For UI Color and Alpha, alpha channel needs precision, so
  R10G10B10A2 is not suitable FOR THE UI TAG (but is fine as
  backbuffer/hudless HDR format).
- **Bidirectional Distortion Field**: only for strong post-process
  distortion (e.g. barrel). 4ch signed, >= 8 bits per channel (R10G10B10A2
  explicitly not allowed), half-res recommended; RG = undistorted UV
  offset, BA = distorted UV offset, same scale as mvecs. When tagged,
  mvec/depth must be undistorted, FinalColor distorted, and
  Blend(Hudless, UI) = FinalColor must hold.
- Resource lifecycle: prefer `eValidUntilPresent`; `eOnlyValidNow` /
  `eValidUntilEvaluate` waste VRAM (SL copies) and are only for inputs
  reused before present. SL holds references on eValidUntilPresent
  (D3D12 only, not Vulkan).

### Modes, cadence, and presentation (guide 6.0-6.4/13.0)

- Modes: eOff / eOn (fixed multiplier) / eDynamic (dynamic MFG) / eAuto
  (legacy; fixed multiplier + auto-disable on perf loss). Turning on
  requires an explicit slDLSSGSetOptions call; loading + tagging is not
  enough.
- **`numFramesToGenerate` = N means N INTERPOLATED frames per app frame,
  i.e. N+1 presented slots** (N=1 gives 2x, not "2 generated").
- Dynamic MFG: driver >= 595.41, D3D12 only, targets display refresh or
  `dynamicTargetFrameRate`; expects the Reflex frame limiter
  (`frameLimitUs`).
- eOff frees resources by default; `eRetainResourcesWhenOff` avoids
  re-enable stutter (menu toggles should use it, plus explicit
  slFreeResources for long-term off).
- SL presents asynchronously ("SL pacer"); the host's Present() is
  non-blocking. **The real frame is always presented; an interpolated
  frame can be dropped** if it lands too close to the previous present.
  Reported FPS can exceed 2x when CPU-bound because the host's Present no
  longer blocks (~1 ms saved per frame).
- Other present-side facts: swap chain should be recreated on every
  DLSS-G on/off toggle (proxy overhead otherwise); SL owns the frame
  latency waitable object unless the app requested the flag; Reflex
  (Streamline flavor) is REQUIRED and its present markers must carry the
  same frame index as the common constants; do not set constants twice
  per frame; report/query via slDLSSGGetState (`numFramesToGenerateMax`,
  `bIsDynamicMFGSupported`, invalid state => turn off).

### Scene changes (guide 20.0)

Auto Scene Change Detection runs on every frame pair, but only when the
per-frame camera right/up/forward vectors (common constants) form an
orthonormal 3x3 matrix; otherwise ASCD is disabled entirely. HLSFG note:
nvngx/OptiScaler hosts do not give us SL constants, so we have no ASCD
equivalent and cannot rely on a reset-flag analog; treat scene-change
detection as our own responsibility if ever needed.

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
  verdict fires again; catches genuinely frozen-frame hosts (no known host
  has shown one: both motivating observations, CP and Control, turned out
  to be intro/menu-phase artifacts, see above).
- Verdicts are sticky per session; once broken, HUDLess is nulled for the
  whole session and the tab shows the health state (g_hudlessHealth).
- KNOWN BUG (2026-11-25): the verdict is PHASE-BLIND. Startup probes run at
  session start, which in Cyberpunk is the intro/logo screens where the tag
  legitimately holds flat near-white content; the sticky verdict then
  discards the healthy gameplay hudless for the rest of the session. Fix
  (backlog #1): settle verdicts only under gameplay-like conditions (scene
  motion confirmed) and re-open the verdict after a null-hudless stretch
  when the tag returns.

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

1. **Phase-aware sanity detector**: do not settle sticky verdicts from probes
   taken during non-gameplay phases (intro/logos, menus, load screens);
   require confirmed scene motion before a verdict sticks, and re-open the
   verdict after null-hudless stretches when the tag returns. This un-drops
   the healthy Cyberpunk gameplay hudless and fixes the DisableHudless
   no-op the user observed.
2. **Temporal UI mask with state**: ping-pong mask texture + hysteresis
   (findings stay pinned until they really move), optionally gated by
   global camera motion from the flow field. This is the path to sharp HUD
   in garbage-hudless hosts without judder. Do not ship stateless again.
3. **DLSSG.UI / UIAlpha consumer**: exact recomposition when a host
   provides the UI layer; probe logging already in place.
4. **Subrect support** (`*SubrectBaseX/Y/Width/Height`): copy regions
   instead of whole frames where the host presents via subrects.
5. **R10G10B10A2 + HDR** color path audit (PQ10 profiles) and the
   BidirectionalDistortionField / NoWarp inputs.
6. Vulkan surface (phase 2, MAKO FG later).

## Housekeeping

- CI builds the dll and dumps the export table (22 NVSDK exports +
  OptiPluginInit) - keep that green.
- Push: `git -c credential.helper='!gh auth git-credential' push` (gh
  auth on the dev machine). Repo: github.com/knosuhor/HLSFG.
- Do not commit anything NVIDIA/LS proprietary; shaders extract at runtime.
- `_staging/` is scratch (ignored); keep diagnostic outputs there, not in
  the repo.
