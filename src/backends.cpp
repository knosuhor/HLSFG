#include "backends.h"

#include "log.h"
#include "mfg_state.h"

#include <mutex>

namespace mfg
{
namespace
{
std::mutex g_backendMutex;
PassthroughBackend g_passthrough;
BlendBackend g_blend;

// Keys probed on every Evaluate to characterize what the game + OptiScaler
// hand us. Extend this list from real logs; that inventory is the input
// definition phase 1 backends will consume (HUD-less color above all).
const char* const g_probeKeys[] = {
    // color / HUD-less inputs (the ones the project actually needs)
    "Color", "Color.HUDLess", "Color.OptimalProcessOrder",
    // inputs MG-style backends deliberately ignore, logged to prove presence
    "Depth", "DLSSG.Depth", "MotionVectors", "MV",
    // pacing / flags
    "FPS.Latency", "DLSSG.Flags", "DLSSG.ShowDebug", "DLSSG.DispatchFlags",
    "Reset", "FrameGeneration.Enabled",
};

void Probe(NVSDK_NGX_Parameter* params, const char* key)
{
    void* resource = nullptr;
    if (params->Get(key, &resource) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-24s resource=%p", key, resource);
        return;
    }

    unsigned long long integer = 0;
    if (params->Get(key, &integer) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-24s int=%llu", key, integer);
        return;
    }

    float value = 0.0f;
    if (params->Get(key, &value) == NVSDK_NGX_Result_Success)
    {
        MFG_LOG_INFO("  %-24s float=%.6f", key, value);
        return;
    }

    MFG_LOG_INFO("  %-24s (absent)", key);
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
    MFG_LOG_WARN("BlendBackend selected but not implemented yet, behaving as passthrough");
    return NVSDK_NGX_Result_Success;
}

const char* BackendName(int backend)
{
    switch (backend)
    {
    case static_cast<int>(BackendKind::Blend): return "blend";
    default: return "passthrough";
    }
}

IFGBackend* ActiveBackend()
{
    return g_settings.backend.load() == static_cast<int>(BackendKind::Blend) ? &g_blend : &g_passthrough;
}
} // namespace mfg
