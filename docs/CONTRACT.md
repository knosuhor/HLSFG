# OptiScaler custom `dlssg` provider contract

Evidence below is from OptiScaler master at commit `91a1256` (Nov 22, 2026).
Re-check these paths when rebasing; OptiScaler moves fast.

## Loading and discovery

- Folder: `OptiScaler/custom/<name>/dlssg/`. The dll can have any name; if
  several dlls exist the standard name is used, which is `nvngx_dlssg.dll`.
  Source: `OptiScaler/misc/CustomProviders.h` (`KindInfo infos[]`, `DllPath`).
- Selection: `FGNvngxReplacement=<name>` in `OptiScaler.ini` (same mechanism
  ANR FG and nr-record use).
- `OptiPluginInit` (see `plugin_sdk/optiscaler_plugin.h`) is called by
  `PluginApi::InitPlugin` from `CustomProviders::Load`, **before** the NGX
  exports are resolved (`Nvngx_Custom::LoadLibraries`), so settings read in
  `OptiPluginInit` are visible to `PopulateParameters_Impl` on the same load.
- Settings persist in the `[Plugin.<name>]` INI section, where `<name>` is the
  **folder** name, not the dll name.
- `ERROR_DLL_INIT_FAILED` on load is treated as "the dll refused to load".

## NGX exports exacted from the dll

Resolved as `GetProcAddress(dll, "NVSDK_NGX_" + name)` in
`NgxFeatureModule::LoadExports` (`OptiScaler/proxies/Nvngx_Proxy.h`):

- D3D12: `D3D12_Init`, `D3D12_Init_Ext`, `D3D12_Shutdown`, `D3D12_Shutdown1`,
  `D3D12_GetScratchBufferSize`, `D3D12_CreateFeature`, `D3D12_ReleaseFeature`,
  `D3D12_GetFeatureRequirements`, `D3D12_EvaluateFeature`,
  `D3D12_PopulateParameters_Impl`
- Vulkan: `VULKAN_Init`, `VULKAN_Init_Ext`, `VULKAN_Init_Ext2`,
  `VULKAN_Shutdown`, `VULKAN_Shutdown1`, `VULKAN_GetScratchBufferSize`,
  `VULKAN_CreateFeature`, `VULKAN_CreateFeature1`, `VULKAN_ReleaseFeature`,
  `VULKAN_GetFeatureRequirements`, `VULKAN_EvaluateFeature`,
  `VULKAN_PopulateParameters_Impl`

Availability is inferred from null exports: `D3D12_Init != nullptr` means
D3D12 available, `VULKAN_Init != nullptr` means Vulkan available
(`Nvngx_DllProxy::isDx12Available/isVulkanAvailable`). A stub export that
returns `NVSDK_NGX_Result_FAIL_FeatureNotSupported` still counts as
available, so phase 0 keeps `VULKAN_EvaluateFeature` failing at runtime
while `VULKAN_PopulateParameters_Impl` still reports parameters.

Note the feature-dll `Shutdown1` signature takes **no** `OutRemaining`
(only the NGX runtime `nvngx.dll` variant does). See `PFN_D3D12_Shutdown1`
in `proxies/Nvngx_Proxy.h`.

## Parameters that decide whether the provider is used

`QueryReportedParameters()` in `OptiScaler/proxies/Nvngx_DllProxy.cpp` calls
our `PopulateParameters_Impl` immediately after load:

- `DLSSG.MultiFrameCountMax` (int, > 0) becomes `getMaxFakeFramesCount()`
- `FrameGeneration.Available` (int) == 0 makes OptiScaler **fall back**
  and ignore the provider
- `FrameInterpolation.GhostbusterVersionMajor/Minor` optionally reported;
  only for DLSS Enabler-style antighosting passthrough. Absent: DispatchFlags
  stay on OptiScaler defaults.

## What flows through Evaluate

`Nvngx_DllProxy::D3D12_EvaluateFeature` forwards the caller's params and adds:

- `DLSSG.ShowDebug`, `DLSSG.DispatchFlags` (`SetDispatchFlags`)
- `DLSSG.Depth` (a converted depth copy OptiScaler manages itself)

We log the union (game/OptiScaler-provided) per evaluate; see the probe list
in `src/backends.cpp`. Phase 1 consumes the HUD-less color buffer from this
set; depth and motion vectors are ignored by design (image interpolation).

## Result codes

`NVSDK_NGX_Result_Success = 0x1`, `NVSDK_NGX_Result_Fail = 0xBAD00000`,
`FAIL|1` = FeatureNotSupported (`external/nvngx_dlss_sdk/nvsdk_ngx_defs.h`).

## hs: right and wrong ways to do no harm

- `CreateFeature` must only accept `NVSDK_NGX_Feature_FrameGeneration` (11).
- Returning `Success` from `EvaluateFeature` while producing nothing keeps
  the pipeline healthy, but under `FGOutput=dlssg` it means **black FG slots**:
  every presentation slot of the group flows through the provider, so without
  output copies the FG path shows nothing (measured, see below). The Blend
  backend exists for exactly that: copy the frame through the outputs while
  backend work continues.

## Measured: OptiScaler 10.0.0-dev (b7b07d2) + StreamlineSample, DX12, 2026-11-23

First end-to-end run of the dll against the real contract, on NVIDIA RTX
3070 / driver 610.88 / Windows 11. Loading, init, create, evaluate and the
plugin tab all behaved as the source analysis predicted; the numbers below
are what the sources could not tell us.

### Version floor

- OptiScaler **0.9.5-final has none of this**: no `CustomProviders.h`, no
  `OptiPluginInit`, no `FGNvngxReplacement` key, and its `FGOutput` enum has
  no `dlssg`. The contract needs **master** (measured on 10.0.0-dev, commit
  b7b07d2; custom providers per `misc/CustomProviders.h`).
- With 0.9.5-final the dll never loads and the ini is silently ignored:
  `FrameGen.FGInput`/`FGOutput` echo, nothing else happens.

### Deployment facts (FGOutput=dlssg, SL DLSSG input)

- `FGInput=dlssg` + `FGOutput=dlssg` + `FGNvngxReplacement=MFG` is the
  minimal working ini setup (all keys in `[FrameGen]`).
- The game must run against OptiScaler's own copy of Streamline: a full
  `streamline/` folder (sl.interposer + plugins + NGX dlls) next to the dll.
  `CheckForFGStatus` hard-requires it and silently disables FGOutput at swap
  chain creation when it is missing; the game then shows errors and may
  freeze with FG on.
- `DLSSG.MultiFrameCountMax` reported at `PopulateParameters_Impl` shows up
  as "max fake frames: 2" in the OptiScaler log; dynamic MFG also works
  ("Dynamic MFG is supported").

### What flows through Evaluate (measured)

- Keys present every call: `DLSSG.Backbuffer`, `DLSSG.HUDLess`,
  `DLSSG.Depth`, `DLSSG.MVecs`, `DLSSG.OutputReal`,
  `DLSSG.OutputInterpolated`, `DLSSG.BackbufferFrameID`, `DLSSG.Reset`,
  `DLSSG.ShowDebug`, `DLSSG.DispatchFlags`, `DLSSG.IsRecording`.
- The classic NGX upscaler keys (`Color`, `Color.HUDLess`, `Depth`,
  `MotionVectors`, `FPS.Latency`) are **absent** on this surface: phase 1
  consumes `DLSSG.HUDLess` (fallback `DLSSG.Backbuffer`), not
  `Color.HUDLess`.
- **Burst mode**: one Evaluate per real frame carries every output of the
  frame group at once. `DLSSG.MultiFrameIndex` is readable only through the
  generic `void*` getter (the typed integer Get fails); in burst mode its
  value is 0, so 0/absent means "fill all outputs". Per-slot mode (index
  1..N, one Evaluate per presented slot, `OutputReal` on slot 1 and
  `OutputInterpolated` on the rest) is what FlipMeter-style callers use; the
  Blend backend keeps both paths behind a `burst` flag (`src/backends.cpp`).
- Resource states work with the same assumptions OptiScaler's own FFX
  provider makes for these parameters: backbuffer = compute read, HUD-less
  = copy dest, outputs = unordered access. The Blend backend's transition
  barriers follow that.

### Observed behavior

- Passthrough: game configurable, engine healthy, but FG slots stay black
  while the window is focused (OptiScaler suspends FG on focus loss, which
  is why the image reappears when the window is unfocused).
- Blend: image visible, "mfg x2" in the OptiScaler panel, output copies at
  the full presented rate, process responsive over multi-thousand-frame
  runs. Expected phase-0.5 artifact: UI drawn over interpolated slots
  flickers, because interpolated slots carry the HUD-less copy and UI
  recomposition over generated slots belongs to OptiScaler
  (UIComposition / DrawUIOverFG), not to the provider.
