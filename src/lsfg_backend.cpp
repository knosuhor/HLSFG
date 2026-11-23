#include "lsfg_backend.h"

#include "log.h"
#include "mfg_state.h"

#include "lsfg/dll_extract.h"

#include <cstring>
#include <filesystem>

namespace mfg
{
namespace
{
// Lossless.dll lookup order: explicit override first, then the two
// Steam install roots (measured default install locations).
const char* const kDllCandidates[] = {
    "C:/Program Files (x86)/Steam/steamapps/common/Lossless Scaling/Lossless.dll",
    "C:/Program Files/Steam/steamapps/common/Lossless Scaling/Lossless.dll",
};

bool IsSupportedFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* key)
{
    void* resource = nullptr;
    if (params->Get(key, &resource) == NVSDK_NGX_Result_Success)
        return static_cast<ID3D12Resource*>(resource);
    return nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

void CopyFrame(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dst,
        ID3D12Resource* src, D3D12_RESOURCE_STATES srcState)
{
    const D3D12_RESOURCE_BARRIER pre[] = {
        Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList->ResourceBarrier(2, pre);
    cmdList->CopyResource(dst, src);
    const D3D12_RESOURCE_BARRIER post[] = {
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState),
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(2, post);
}
} // namespace

NVSDK_NGX_Result LsfgBackend::InitD3D12(ID3D12Device* device)
{
    if (m_bankReady)
        return NVSDK_NGX_Result_Success;

    MFG_LOG_INFO("LsfgBackend init (phase 1), device=%p",
        static_cast<void*>(device));
    m_device = device;

    // locate Lossless.dll
    std::filesystem::path dll;
    char overridePath[MAX_PATH] = {};
    if (GetEnvironmentVariableA("MFG_LS_DLL", overridePath, MAX_PATH) > 0)
        dll = std::filesystem::path(overridePath);
    else
    {
        for (const char* candidate : kDllCandidates)
        {
            if (std::filesystem::exists(candidate))
            {
                dll = std::filesystem::path(candidate);
                break;
            }
        }
    }
    if (dll.empty())
    {
        MFG_LOG_ERROR("lsfg: Lossless.dll not found (set MFG_LS_DLL or "
            "install Lossless Scaling via Steam)");
        m_pipelineFailed = true;
        return NVSDK_NGX_Result_Success; // degrade to fallback copies
    }
    MFG_LOG_INFO("lsfg: using %s", dll.string().c_str());

    try
    {
        const auto resources = lsfg::ExtractRcData(dll);
        MFG_LOG_INFO("lsfg: %zu RT_RCDATA resources", resources.size());
        if (!m_bank.Load(device, resources))
        {
            MFG_LOG_ERROR("lsfg: shader bank failed: %s", m_bank.m_error.c_str());
            m_pipelineFailed = true;
            return NVSDK_NGX_Result_Success;
        }
        m_bankReady = true;
    }
    catch (const std::exception& e)
    {
        MFG_LOG_ERROR("lsfg: extraction failed: %s", e.what());
        m_pipelineFailed = true;
        return NVSDK_NGX_Result_Success;
    }

    return NVSDK_NGX_Result_Success;
}

bool LsfgBackend::EnsurePipeline(uint32_t width, uint32_t height,
        uint32_t slices, DXGI_FORMAT sourceFormat)
{
    if (m_pipelineReady || m_pipelineFailed || !m_bankReady)
        return m_pipelineReady;
    if (width == 0 || height == 0 || slices == 0)
        return false;

    const uint32_t flow = static_cast<uint32_t>(g_settings.lsFlow.load());
    if (!m_pipeline.Init(m_device, m_bank, {width, height}, flow, slices,
            sourceFormat))
    {
        m_pipelineFailed = true;
        return false;
    }
    m_pipelineReady = true;
    return true;
}
NVSDK_NGX_Result LsfgBackend::EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
        NVSDK_NGX_Parameter* params)
{
    ++m_calls;

    ID3D12Resource* backbuffer = GetResource(params, "DLSSG.Backbuffer");
    ID3D12Resource* hudless = GetResource(params, "DLSSG.HUDLess");
    ID3D12Resource* outputReal = GetResource(params, "DLSSG.OutputReal");
    ID3D12Resource* outputInterpolated =
        GetResource(params, "DLSSG.OutputInterpolated");

    // slot identity: pointer-bits contract measured against OptiScaler
    void* rawIndex = nullptr;
    const bool hasIndex =
        params->Get("DLSSG.MultiFrameIndex", &rawIndex) ==
            NVSDK_NGX_Result_Success && rawIndex != nullptr;
    const unsigned long long frameIndex =
        hasIndex ? (unsigned long long)(uintptr_t)rawIndex : 1;
    const bool burst = !hasIndex || frameIndex == 0;

    ID3D12Resource* source = hudless != nullptr ? hudless : backbuffer;
    const D3D12_RESOURCE_STATES sourceState = hudless != nullptr
        ? D3D12_RESOURCE_STATE_COPY_DEST
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // TEMP instrumentation: slot identity + resource pointers per call
    void* rawCount = nullptr;
    const bool hasCount =
        params->Get("DLSSG.MultiFrameCount", &rawCount) ==
            NVSDK_NGX_Result_Success && rawCount != nullptr;
    if (m_calls <= 12 || (m_calls % 500) == 0)
        MFG_LOG_INFO("lsfg: call#%llu mfiHas=%d mfi=%llu | mfcHas=%d mfc=%llu | hudless=%p bb=%p real=%p interp=%p",
            m_calls, (int) hasIndex, frameIndex, (int) hasCount,
            hasCount ? (unsigned long long)(uintptr_t) rawCount : 0ULL,
            (void*) hudless, (void*) backbuffer, (void*) outputReal,
            (void*) outputInterpolated);

    // Blend-style fill used whenever the graph cannot run (first frame,
    // unsupported format, one-Evaluate-per-slot mode, init failure).
    auto fallback = [&]()
    {
        ++m_fallbacks;
        if (outputReal != nullptr && backbuffer != nullptr)
            CopyFrame(cmdList, outputReal, backbuffer,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (outputInterpolated != nullptr && source != nullptr)
            CopyFrame(cmdList, outputInterpolated, source, sourceState);
    };

    if (!burst || source == nullptr || cmdList == nullptr ||
        outputInterpolated == nullptr)
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    // resolution + slice contract for the graph
    const D3D12_RESOURCE_DESC srcDesc = source->GetDesc();
    const D3D12_RESOURCE_DESC interpDesc = outputInterpolated->GetDesc();
    if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        interpDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }
    if (!IsSupportedFormat(srcDesc.Format))
    {
        if (!m_formatWarned)
        {
            MFG_LOG_INFO("lsfg: source format %u not supported yet, "
                "staying on blend fallback", srcDesc.Format);
            m_formatWarned = true;
        }
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    uint32_t slices = static_cast<uint32_t>(interpDesc.DepthOrArraySize);
    if (slices == 0)
        slices = 1;

    if (m_calls <= 3)
        MFG_LOG_INFO("lsfg: eval#%llu fidx=%llu src fmt=%u flags=0x%x | interp fmt=%u flags=0x%x depth=%u",
            m_calls, m_fidx, static_cast<unsigned>(srcDesc.Format),
            static_cast<unsigned>(srcDesc.Flags), static_cast<unsigned>(interpDesc.Format),
            static_cast<unsigned>(interpDesc.Flags), slices);

    if (!EnsurePipeline(static_cast<uint32_t>(srcDesc.Width),
            static_cast<uint32_t>(srcDesc.Height), slices, srcDesc.Format))
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    // ring: store the incoming frame into slot (fidx & 1)
    ID3D12Resource* ring = m_pipeline.Source(
        static_cast<uint32_t>(m_fidx & 1));
    {
        const D3D12_RESOURCE_BARRIER pre[] = {
            Transition(ring, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmdList->ResourceBarrier(2, pre);
        cmdList->CopyResource(ring, source);
        const D3D12_RESOURCE_BARRIER post[] = {
            Transition(ring, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState),
        };
        cmdList->ResourceBarrier(2, post);
    }

    if (m_fidx == 0)
    {
        // first real frame: zero the transient images, blend the group
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=clears");
        m_pipeline.RecordClears(cmdList);
        fallback();
    }
    else
    {
        // the pair (F_{fidx-1}, F_fidx) now sits in the ring: generate
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=graph fidx=%llu", m_fidx);
        m_pipeline.RecordGraph(cmdList, m_fidx, outputInterpolated, slices);
        ++m_generations;
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=graph-done");
        if (outputReal != nullptr && backbuffer != nullptr)
            CopyFrame(cmdList, outputReal, backbuffer,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    ++m_fidx;
    return NVSDK_NGX_Result_Success;
}

void LsfgBackend::Shutdown()
{
    MFG_LOG_INFO("LsfgBackend shutdown after %llu evaluate calls, "
        "%llu generated groups, %llu blend fallbacks",
        m_calls, m_generations, m_fallbacks);
    m_fidx = 0;
    m_pipelineReady = false;
    m_device = nullptr;
}
} // namespace mfg
