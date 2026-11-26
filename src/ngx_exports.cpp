// NGX surface exports. Exactly the functions OptiScaler resolves with
// GetProcAddress("NVSDK_NGX_" + name) for a custom dlssg provider:
// proxies/Nvngx_Proxy.h, NgxFeatureModule::LoadExports().
//
// Every call is logged, the contract is answered correctly
// (Success where a no-op is valid), but no frame is ever generated. Games run
// at their base frame rate while we validate load, init, create and evaluate.

#include "backends.h"
#include "log.h"
#include "mfg_state.h"
#include "vk_shims.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ngx/nvsdk_ngx_defs.h"
#include "ngx/nvsdk_ngx_params.h"

// Copied 1:1 from nvsdk_ngx.h (third NGX SDK header; not fetched by CI to keep
// vendoring minimal). Same pointer type OptiScaler resolves in
// NgxFeatureModule::LoadExports, so the export signature matches the real SDK.
typedef void (NVSDK_CONV* PFN_NVSDK_NGX_ProgressCallback)(float InCurrentProgress,
                                                          bool& OutShouldCancel);

namespace
{
struct MfgFeature
{
    NVSDK_NGX_Handle handle {};
};

std::mutex g_featureMutex;
std::vector<std::unique_ptr<MfgFeature>> g_features;
uint32_t g_nextId = 1;

// Set once, at first use; exported surface and plugin enter here.
void InitLogging()
{
    static bool initialised = false;
    if (initialised)
        return;
    initialised = true;

    mfg::log::InitFromAddress(reinterpret_cast<const void*>(&InitLogging));
    MFG_LOG_INFO("HLSFG provider loaded (lsfg), multiplier=%d",
                 mfg::g_settings.multiplier.load());
}

MfgFeature* FindFeature(const NVSDK_NGX_Handle* handle)
{
    if (handle == nullptr)
        return nullptr;
    std::scoped_lock lock(g_featureMutex);
    for (auto& f : g_features)
        if (&f->handle == handle)
            return f.get();
    return nullptr;
}

void Populate(NVSDK_NGX_Parameter* parameters)
{
    // The values OptiScaler reads right after load
    // (proxies/Nvngx_DllProxy.cpp, QueryReportedParameters):
    //   DLSSG.MultiFrameCountMax > 0  -> getMaxFakeFramesCount()
    //   FrameGeneration.Available      -> reportedAvailable (0 falls back!)
    const int multiplier = mfg::g_settings.multiplier.load();
    parameters->Set("DLSSG.MultiFrameCountMax", multiplier);
    parameters->Set("FrameGeneration.Available", 1);

    // Not reported: no DispatchFlags support, so OptiScaler keeps DLSSG.ShowDebug
    // and DLSSG.DispatchFlags on its own defaults.
}
} // namespace

//
// D3D12
//

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_Init(unsigned long long InApplicationId, const wchar_t* InApplicationDataPath,
                     ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
                     NVSDK_NGX_Version InSDKVersion)
{
    InitLogging();
    MFG_LOG_INFO("D3D12_Init appid=%llu datapath=%ls device=%p sdk=0x%04x", InApplicationId,
                 InApplicationDataPath ? InApplicationDataPath : L"(null)", static_cast<void*>(InDevice),
                 static_cast<unsigned>(InSDKVersion));
    return mfg::ActiveBackend()->InitD3D12(InDevice);
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_Init_Ext(unsigned long long InApplicationId, const wchar_t* InApplicationDataPath,
                         ID3D12Device* InDevice, NVSDK_NGX_Version InSDKVersion,
                         const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    InitLogging();
    MFG_LOG_INFO("D3D12_Init_Ext appid=%llu device=%p sdk=0x%04x", InApplicationId,
                 static_cast<void*>(InDevice), static_cast<unsigned>(InSDKVersion));
    return mfg::ActiveBackend()->InitD3D12(InDevice);
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown()
{
    MFG_LOG_INFO("D3D12_Shutdown");
    {
        std::scoped_lock lock(g_featureMutex);
        g_features.clear();
    }
    mfg::ActiveBackend()->Shutdown();
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* InDevice)
{
    MFG_LOG_INFO("D3D12_Shutdown1 device=%p", static_cast<void*>(InDevice));
    {
        std::scoped_lock lock(g_featureMutex);
        g_features.clear();
    }
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId, const NVSDK_NGX_Parameter* InParameters,
                                     size_t* OutSizeInBytes)
{
    if (OutSizeInBytes != nullptr)
        *OutSizeInBytes = 0;
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,
                              NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle)
{
    InitLogging();
    MFG_LOG_INFO("D3D12_CreateFeature featureId=%u cmdList=%p", static_cast<unsigned>(InFeatureID),
                 static_cast<void*>(InCmdList));

    if (InFeatureID != NVSDK_NGX_Feature_FrameGeneration)
    {
        MFG_LOG_WARN("CreateFeature refused, feature %u is not FrameGeneration",
                     static_cast<unsigned>(InFeatureID));
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    if (OutHandle == nullptr)
        return NVSDK_NGX_Result_Fail;

    unsigned int width = 0, height = 0;
    InParameters->Get("InWidth", &width);
    InParameters->Get("InHeight", &height);
    MFG_LOG_INFO("  create params: %ux%u", width, height);

    std::scoped_lock lock(g_featureMutex);
    MfgFeature* feature = g_features.emplace_back(std::make_unique<MfgFeature>()).get();
    feature->handle.Id = g_nextId++;
    *OutHandle = &feature->handle;
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    MFG_LOG_INFO("D3D12_ReleaseFeature handle=%p", static_cast<void*>(InHandle));
    std::scoped_lock lock(g_featureMutex);
    std::erase_if(g_features, [InHandle](const auto& f) { return &f->handle == InHandle; });
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter* Adapter,
                                       const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
                                       NVSDK_NGX_FeatureRequirement* OutSupported)
{
    MFG_LOG_INFO("D3D12_GetFeatureRequirements featureId=%u",
                 FeatureDiscoveryInfo ? static_cast<unsigned>(FeatureDiscoveryInfo->FeatureID) : 0u);
    if (OutSupported == nullptr)
        return NVSDK_NGX_Result_Fail;

    *OutSupported = {};
    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
    OutSupported->MinHWArchitecture = 0;
    strcpy_s(OutSupported->MinOSVersion, "10.0.19041.0");
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle,
                                const NVSDK_NGX_Parameter* InParameters,
                                PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (FindFeature(InFeatureHandle) == nullptr)
    {
        MFG_LOG_WARN("EvaluateD3D12 with unknown handle %p", static_cast<const void*>(InFeatureHandle));
        return NVSDK_NGX_Result_Fail;
    }
    return mfg::ActiveBackend()->EvaluateD3D12(InCmdList, const_cast<NVSDK_NGX_Parameter*>(InParameters));
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    InitLogging();
    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;
    Populate(InParameters);
    return NVSDK_NGX_Result_Success;
}

//
// Vulkan (correctly reported as unavailable; OptiScaler only treats
// VULKAN_Init != null as "vulkan available", and stubs are fine)
//

namespace
{
template <typename... Args>
NVSDK_NGX_Result VkUnavailable(const char* name, Args...)
{
    MFG_LOG_INFO("%s: no Vulkan support", name);
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}
} // namespace

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_Init(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                      PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NVSDK_NGX_FeatureCommonInfo*,
                      NVSDK_NGX_Version)
{
    return VkUnavailable("VULKAN_Init");
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_Init_Ext(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                          NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*)
{
    return VkUnavailable("VULKAN_Init_Ext");
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_Init_Ext2(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                           PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
                           const NVSDK_NGX_FeatureCommonInfo*)
{
    return VkUnavailable("VULKAN_Init_Ext2");
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown()
{
    return VkUnavailable("VULKAN_Shutdown");
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown1(VkDevice)
{
    return VkUnavailable("VULKAN_Shutdown1");
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_GetScratchBufferSize(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                               NVSDK_NGX_Handle**)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_CreateFeature1(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                NVSDK_NGX_Handle**)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle*)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_GetFeatureRequirements(const VkInstance, const VkPhysicalDevice,
                                        const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_EvaluateFeature(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                 PFN_NVSDK_NGX_ProgressCallback)
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;
    Populate(InParameters);
    return NVSDK_NGX_Result_Success;
}
