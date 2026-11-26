# AGENTS.md

Working notes for anyone (human or agent) touching this repository. Read
before changing code. Keep this file current when a stage changes.

## Project goal (do not lose this)

HLSFG must stay **game-agnostic**. The target is compatibility parity with
other OptiScaler nvngx replacements: everywhere FSR3 / ANR work under
OptiScaler, HLSFG should work too. Measuring in one game is fine; tuning
constants, thresholds or heuristics for one game is not. Per the 5.2
tagging contract, never conclude that a game's Streamline data is
incomplete or garbage: if other providers work there and the game
supports DLSS-G, the integration is possible and the fix belongs on our
side. Every per-game observation should be generalized into either
(a) contract handling, (b) principled input handling, or (c) a
documented, opt-in experiment.

## Current stage (Nov 2026)

The LSFG backend (LS 3.2.1 shader bank + lsfg-vk graph semantics,
D3D12-native) generates real intermediate frames in Cyberpunk 2077 and
Control at 4K with the DLSS-G present contract. The backend selection
(phase 0 passthrough / phase 0.5 blend) was removed on 2026-11-26:
LSFG is the only backend, the old phase backends are deleted.

Validated on real hosts:

- Cyberpunk 2077: FG works. CONFIRMED 2026-11-25/26 (preview-verified):
  the hudless tag is healthy in gameplay; the "HUDLess" preview mode
  shows the game image 1:1 without HUD. Flat near-white tagged content
  on pre-menu intro/logo screens is contract-consistent host behavior
  (hosts are supposed to null tags outside gameplay and often keep
  flat/stale content tagged instead, see the contract reference below).
  Evaluate/DLSSG runs a few seconds on those screens, goes idle at the
  menu, and re-activates near the end of level loading.
- Control: R10G10B10A2 path runs (typed UAV stores). CONFIRMED 2026-11-26
  (user-verified in-game): the hudless tag is healthy in gameplay, same
  pattern as Cyberpunk; flat white/black tagged content appears only on
  menus/cutscenes/intro-like screens. The earlier "hudless frozen frame"
  diagnosis is RETRACTED, same non-gameplay phase artifact.
- NVIDIA Streamline sample: contract validation host.

The visible behavior users care about: motion is interpolated (no judder
from duplicated slots), no UI strobe from the hudless delta, sharp HUD
via composite mode 0 whenever the host tags a hudless.

### Input policy (2026-11-26 directive, do not regress)

Never issue verdicts that a game's Streamline integration is incomplete
or delivers garbage data. If other providers work under OptiScaler in a
game that officially supports DLSS-G, our integration is possible; any
observed anomaly is OUR contract handling to fix, not the host's data.
Concretely:

- The tagged HUDLessColor is trusted whenever present. The old garbage/
  stale "sanity verdicts" (startup content + periodic freshness probes)
  were REMOVED; there is no hudless drop path anymore.
- Flat/stale hudless content during intro/menu/cutscene phases is
  contract-consistent (guide 5.2: hosts should null tags outside
  gameplay; many do not) and must never be treated as a data problem.
- The hudless byte-polarity (invert) feature was REMOVED on 2026-11-26
  (evening). "Complemented storage" was a phantom: it came from the
  same flawed ROI vote reading intro-phase content (flat white hudless
  vs dark intro backbuffer), and the raw-preview verification proved the
  tag is stored direct in both hosts. After the verdict removal the vote
  misfired on its own (log 22:39: "L1 direct=0.89 inverted=0.11 ->
  invert ON" during intro), which would have fed a negated hudless to
  the graph for a whole session. The tagged input is now used byte-exact
  as delivered; no readback probes remain.
- The tab's Input preview dropdown (Off / HUDLess / UI Alpha / UI Color
  and Alpha) writes the raw input to both outputs for live inspection;
  absent inputs log once and fall through to the normal pipeline.

## Streamline DLSS-G contract reference

Distilled from NVIDIA Streamline `docs/ProgrammingGuideDLSS_G.md`
(v2.14.16). This is the authoritative contract our nvngx provider is
standing in for; consult it before inventing heuristics. Facts below are
from the guide, with HLSFG implications noted.

### Verified nvngx parameter names (2026-11-26)

Extracted as strings from the host-side bridge `sl.dlss_g.dll` in the
Cyberpunk install (NVIDIA's own Streamline-to-nvngx adapter), so these
are the parameter names the REAL DLSS-G consumes, not guesses:

- `DLSSG.HUDLess`, `DLSSG.Backbuffer`, `DLSSG.Depth`, `DLSSG.MVecs`,
  `DLSSG.OutputReal`, `DLSSG.OutputInterpolated`,
  `DLSSG.OutputDisableInterpolation` (all already handled).
- UI: `DLSSG.UI` (UI Color and Alpha) and `DLSSG.UIAlpha` (UI Alpha).
  There is NO "uiColorAlpha"-style key; UIColorAndAlpha is the SL
  buffer TYPE (kBufferTypeUIColorAndAlpha), which the bridge maps to
  `DLSSG.UI`. Related flags: `DLSSG.UserInterfaceRecompositionEnabled`,
  `DLSSG.MenuDetectionEnabled`.
- `DLSSG.NoWarp` (SL type kBufferTypeNoWarpMask) and
  `DLSSG.BidirectionalDistortionField` (+ LowPrecision Scale/Bias).
- Per-resource subrect keys exist for EVERY input:
  `<Input>SubrectBaseX/BaseY/Width/Height` for Backbuffer, HUDLess,
  UI, UIAlpha, Depth, MVecs, NoWarp, BidirectionalDistortionField.
- Camera constants are passed as plain DLSSG.* params (CameraFwd/Right/
  Up/Pos, ViewToClip matrices, FOV, Near/Far, JitterOffset...), i.e. the
  ASCD inputs live on this surface too when the host provides them.
- The bridge itself warns: "Neither kBufferTypeHUDLessColor,
  kBufferTypeUIAlpha, nor kBufferTypeUIColorAndAlpha tag provided",
  confirming the three UI-layer options.

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
src/backends.*        IFGBackend seam; the single LSFG instance lives here
src/lsfg_backend.*    the backend: ring buffer of source frames,
                      LSFG graph dispatch, OutputReal contract,
                      input preview, UI composite modes
src/lsfg/pipeline.cpp lsfg-vk v2.0.0-dev graph on ID3D12Device
src/lsfg/shader_bank  extraction of the 202 RT_RCDATA resources from the
                      user's Lossless.dll (runtime, never vendored)
src/ui_composite.*    one compute pass, five modes (see HUD handling)
src/mfg_state.*       settings (g_settings)
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

1. If a host provides `DLSSG.UI`/`UIAlpha` (none measured so far on
   D3D12; OptiScaler forwards these keys only on its Vulkan path today;
   probed and logged once per session at call #5): consume for exact
   recomposition. **Implemented** as composite modes 2/3/4 in
   ui_composite (guide formula `Final.RGB = UI.RGB + (1-UI.A) *
   Hudless.RGB`):
   - `DLSSG.UIAlpha` + hudless -> mode 3: UI.RGB is recovered from the
     tagged pair as `present - (1-A)*hudless`, then
     `out = present + (1-A)*(interp - hudless)` (exact UI layer added
     on top of the interpolated hudless).
   - `DLSSG.UIAlpha` without hudless -> mode 4: approximation
     `lerp(interp, present, A)` (UI read from the single channel .r).
   - `DLSSG.UI` (Color+Alpha, premultiplied) -> mode 2:
     `out = UI.rgb + (1-UI.a) * interp`.
   UI tags must match the interpolated extent (guide 5.1); a mismatched
   tag is treated as absent and logged once, never judged. Assumed
   state for the UI resource at Evaluate: NON_PIXEL_SHADER_RESOURCE
   (same as the backbuffer; we add no transition for it).
2. Tagged hudless (trusted as delivered, see the input policy): interpolate
   the hudless ring, composite mode 0:
   `out = interp + (backbuffer - hudless) * mask(threshold)`.
3. Untagged game: interpolate the raw backbuffer; composite mode 1
   (temporal UI mask) is **opt-in and experimental** (`TemporalUi`,
   default off) because a per-pixel threshold without temporal state
   judders in low-contrast motion. Do not turn it on by default without
   adding state/hysteresis (see backlog).

### Input preview (tab dropdown, diagnostic)

Off / HUDLess / UI Alpha / UI Color and Alpha. A selected input is copied
raw to both FG outputs, bypassing the graph; missing inputs log once
("preview: ... not provided") and the normal pipeline runs. This is how
the healthy hudless in both test hosts was confirmed live.

## Build / deploy / test loop

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build 'C:\Users\knosu\OneDrive\Documents\HLSFG\build' --config Release --target mfg-provider
Copy-Item .\build\Release\nvngx_dlssg.dll <game>\custom\HLSFG\dlssg\nvngx_dlssg.dll -Force
```

- Lossless.dll is picked up from the default Steam install roots.
- Test hosts: Cyberpunk 2077 (D:\SteamLibrary), Control. FG on through the
  game's own DLSS option; OptiScaler FrameGen tab multiplier overrides the
  provider's MultiFrameCountMax.
- Logs land next to the dll (`hlsfg.log`). Lines prefixed `lsfg: stage=`
  order the per-evaluate pipeline.
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
   global camera motion from the flow field. This is the path to sharp
   HUD in untagged hosts without judder. Do not ship stateless again.
2. **Subrect support** (`*SubrectBaseX/Y/Width/Height`): copy regions
   instead of whole frames where the host presents via subrects.
3. **R10G10B10A2 + HDR** color path audit (PQ10 profiles) and the
   BidirectionalDistortionField / NoWarp inputs.
4. Vulkan surface (later; MAKO FG behind the same seam).

## Housekeeping

- CI builds the dll and dumps the export table (22 NVSDK exports +
  OptiPluginInit) - keep that green.
- Push: `git -c credential.helper='!gh auth git-credential' push` (gh
  auth on the dev machine). Repo: github.com/knosuhor/HLSFG.
- Do not commit anything NVIDIA/LS proprietary; shaders extract at runtime.
- `_staging/` is scratch (ignored); keep diagnostic outputs there, not in
  the repo.
