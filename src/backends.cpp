#include "backends.h"

#include "log.h"
#include "lsfg_backend.h"
#include "mfg_state.h"

#include <mutex>

namespace mfg
{
namespace
{
std::mutex g_backendMutex;
PassthroughBackend g_passthrough;
BlendBackend g_blend;
LsfgBackend g_lsfg;

// Keys probed on every Evaluate to characterize what the game + OptiScaler
// hand us. Measured against OptiScaler 10.0.0-dev (custom NVNGX DLSSG
// providers): the FG Nvngx contract talks in DLSSG.* keys, the classic NGX
// upscaler keys (Color, Depth, MotionVectors) are logged to prove they are
// NOT part of this surface. Extend this list from real logs; that inventory
// is the input definition phase 1 backends will consume.
const char* const g_probeKeys[] = {
    // slot identity
    "DLSSG.MultiFrameIndex", "DLSSG.MultiFrameCount",
    // inputs
    "DLSSG.Backbuffer", "DLSSG.HUDLess", "DLSSG.Depth", "DLSSG.MVecs",
    // outputs (the chain phase 0.5 proves ownership of)
    "DLSSG.OutputReal", "DLSSG.OutputInterpolated",
    // legacy NGX keys, kept to show they stay unused on this surface
    "Color", "Color.HUDLess", "Depth", "MotionVectors",
    // control / pacing
    "DLSSG.Reset", "DLSSG.BackbufferFrameID", "DLSSG.IsRecording",
    "DLSSG.Flags", "DLSSG.ShowDebug", "DLSSG.DispatchFlags",
};

void Probe(NVSDK_NGX_Parameter* params, const char* key)
{
    void* resource = nullptr;
    if (params->Get(key, &resource) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-28s resource=%p", key, resource);
        return;
    }

    unsigned long long integer = 0;
    if (params->Get(key, &integer) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-28s int=%llu", key, integer);
        return;
    }

    float value = 0.0f;
    if (params->Get(key, &value) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-28s float=%.6f", key, value);
        return;
    }

    MFG_LOG_INFO("  %-28s (absent)", key);
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* key)
{
    void* resource = nullptr;
    if (params->Get(key, &resource) == NVSDK_NGX_Result_Success)
        return static_cast<ID3D12Resource*>(resource);
    return nullptr;
}

unsigned long long GetInteger(NVSDK_NGX_Parameter* params, const char* key, unsigned long long fallback)
{
    unsigned long long value = 0;
    if (params->Get(key, &value) == NVSDK_NGX_Result_Success)
        return value;
    return fallback;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

// CopyResource with the state assumptions OptiScaler's own FFX provider
// makes about the same parameters: outputs arrive in unordered access,
// backbuffer in compute read, HUD-less in copy dest.
void CopyFrame(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dst, ID3D12Resource* src,
               D3D12_RESOURCE_STATES srcState)
{
    const D3D12_RESOURCE_BARRIER pre[] = {
        Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList->ResourceBarrier(2, pre);

    cmdList->CopyResource(dst, src);

    const D3D12_RESOURCE_BARRIER post[] = {
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState),
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(2, post);
}
} // namespace

NVSDK_NGX_Result PassthroughBackend::InitD3D12(ID3D12Device* device)
{
    m_device = device;
    MFG_LOG_INFO("PassthroughBackend init, device=%p", static_cast<void*>(device));
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result PassthroughBackend::EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                                   NVSDK_NGX_Parameter* params)
{
    ++m_realFrames;

    // Log the first frames verbosely (they confirm the contract end to end),
    // then only every 300th, so a long session does not produce gigabytes.
    const bool verbose = m_realFrames <= 5 || (m_realFrames % 300) == 0;
    if (verbose)
    {
        MFG_LOG_INFO("EvaluateD3D12 real frame #%llu cmdList=%p", m_realFrames,
                     static_cast<void*>(cmdList));
        for (const char* key : g_probeKeys)
            Probe(params, key);
    }

    // Observe only: with FGOutput=dlssg every FG presentation slot passes
    // through here, so a passthrough that writes nothing shows black FG
    // slots. Use the Blend backend when the image must stay visible.
    return NVSDK_NGX_Result_Success;
}

void PassthroughBackend::Shutdown()
{
    MFG_LOG_INFO("PassthroughBackend shutdown after %llu real frames", m_realFrames);
    m_device = nullptr;
}

NVSDK_NGX_Result BlendBackend::InitD3D12(ID3D12Device* device)
{
    m_device = device;
    MFG_LOG_INFO("BlendBackend init (phase 0.5), device=%p", static_cast<void*>(device));
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result BlendBackend::EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                              NVSDK_NGX_Parameter* params)
{
    ++m_calls;

    ID3D12Resource* backbuffer = GetResource(params, "DLSSG.Backbuffer");
    ID3D12Resource* hudless = GetResource(params, "DLSSG.HUDLess");
    ID3D12Resource* outputReal = GetResource(params, "DLSSG.OutputReal");
    ID3D12Resource* outputInterpolated = GetResource(params, "DLSSG.OutputInterpolated");

    // Slot identity arrives through the generic pointer getter: measured
    // against OptiScaler 10.0.0-dev the int-typed Get fails while the void*
    // Get hands the value over in the pointer bits. MultiFrameIndex of 0 (or a
    // missing key) means burst mode: one Evaluate per real frame carries every
    // output of the group at once instead of one call per presented slot.
    void* rawIndex = nullptr;
    const bool hasIndex =
        params->Get("DLSSG.MultiFrameIndex", &rawIndex) == NVSDK_NGX_Result_Success && rawIndex != nullptr;
    const unsigned long long frameIndex = hasIndex ? (unsigned long long)(uintptr_t)rawIndex : 1;
    const unsigned long long frameCount = GetInteger(params, "DLSSG.MultiFrameCount", 1);
    const bool burst = !hasIndex || frameIndex == 0;

    const bool verbose = m_calls <= 5 || (m_calls % 300) == 0;
    if (verbose)
    {
        MFG_LOG_INFO("EvaluateD3D12 blend call #%llu frameIndex=%llu/%llu cmdList=%p", m_calls,
                     frameIndex, frameCount, static_cast<void*>(cmdList));
        for (const char* key : g_probeKeys)
            Probe(params, key);
    }

    // Phase 0.5 semantics: repeat the current frame through the FG output
    // chain. Real slots get the backbuffer (HUD included), interpolated
    // slots repeat the HUD-less color when the game + OptiScaler provide one
    // and fall back to the backbuffer otherwise.

    // Burst mode: one Evaluate per real frame with every output of the group
    // present, OptiScaler then presents them in order. Fill both.
    if (burst)
    {
        bool copied = false;
        ID3D12Resource* interpSource = hudless != nullptr ? hudless : backbuffer;

        if (outputReal != nullptr && backbuffer != nullptr && cmdList != nullptr)
        {
            CopyFrame(cmdList, outputReal, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ++m_copies;
            copied = true;
        }
        if (outputInterpolated != nullptr && interpSource != nullptr && cmdList != nullptr)
        {
            CopyFrame(cmdList, outputInterpolated, interpSource,
                      hudless != nullptr ? D3D12_RESOURCE_STATE_COPY_DEST
                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ++m_copies;
            copied = true;
        }

        if (!copied)
            ++m_missedOutputs;

        return NVSDK_NGX_Result_Success;
    }

    bool copied = false;
    if (frameIndex == 1)
    {
        if (outputReal != nullptr && backbuffer != nullptr && cmdList != nullptr)
        {
            CopyFrame(cmdList, outputReal, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ++m_copies;
            copied = true;
        }
    }
    else
    {
        ID3D12Resource* source = hudless != nullptr ? hudless : backbuffer;
        if (outputInterpolated != nullptr && source != nullptr && cmdList != nullptr)
        {
            CopyFrame(cmdList, outputInterpolated, source,
                      hudless != nullptr ? D3D12_RESOURCE_STATE_COPY_DEST
                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ++m_copies;
            copied = true;
        }
    }

    if (!copied)
        ++m_missedOutputs;

    return NVSDK_NGX_Result_Success;
}

void BlendBackend::Shutdown()
{
    MFG_LOG_INFO("BlendBackend shutdown after %llu evaluate calls, %llu output copies, %llu slots without an output",
                 m_calls, m_copies, m_missedOutputs);
    m_device = nullptr;
}

const char* BackendName(int backend)
{
    switch (backend)
    {
    case static_cast<int>(BackendKind::Blend): return "blend";
    case static_cast<int>(BackendKind::Lsfg): return "lsfg";
    default: return "passthrough";
    }
}

IFGBackend* ActiveBackend()
{
    switch (g_settings.backend.load())
    {
    case static_cast<int>(BackendKind::Blend):
        return static_cast<IFGBackend*>(&g_blend);
    case static_cast<int>(BackendKind::Lsfg):
        return static_cast<IFGBackend*>(&g_lsfg);
    default:
        return static_cast<IFGBackend*>(&g_passthrough);
    }
}
} // namespace mfg
