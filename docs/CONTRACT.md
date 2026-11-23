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
- Returning `Success` from `EvaluateFeature` while producing nothing is the
  phase 0 no-op: the game renders its real frames only (same visible behavior
  as nr-record capture mode).
