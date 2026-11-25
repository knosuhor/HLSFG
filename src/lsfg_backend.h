// Phase 1 backend: LSFG inference on the HUD-less frame pair.
//
// The LS 3.2.1 native blob (Lossless.dll, 202 RCDATA resources) yields
// the DXBC compute family ids 255-279; the lsfg-vk v2.0.0-dev graph
// provides theschedule semantics. The backend runs that graph directly
// on the caller's D3D12 device, no Vulkan interop (docs/PHASE1.md).
#pragma once

#include "backends.h"

#include "invert_copy.h"
#include "ui_composite.h"
#include "lsfg/pipeline.h"
#include "lsfg/shader_bank.h"

#include <cstdint>

namespace mfg
{
class LsfgBackend final : public IFGBackend
{
  public:
    const char* Name() const override { return "LSFG (phase 1)"; }
    NVSDK_NGX_Result InitD3D12(ID3D12Device* device) override;
    NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                   NVSDK_NGX_Parameter* params) override;
    void Shutdown() override;

  private:
    bool EnsurePipeline(uint32_t width, uint32_t height, uint32_t slices,
        DXGI_FORMAT sourceFormat);

    ID3D12Device* m_device = nullptr;
    lsfg::ShaderBank m_bank;
    lsfg::Pipeline m_pipeline;
    bool m_bankReady = false;      // shaders extracted + reflected
    bool m_pipelineReady = false;  // graph built for this resolution
    bool m_pipelineFailed = false; // do not retry this session
    bool m_formatWarned = false;
    bool m_rgb10Logged = false;   // R10G10B10A2 acceptance logged once
    uint64_t m_fidx = 0;           // real frames fed into the ring
    uint64_t m_calls = 0;
    uint64_t m_generations = 0;
    uint64_t m_fallbacks = 0;
    UiComposite m_ui;             // hudless UI recomposition pass
    bool m_uiFailed = false;      // do not retry this session

    // hudless polarity: some games tag HUDLessColor complemented
    // (255 - color bytes, measured in Cyberpunk 2077). The ring copy
    // negates such input so the graph and the UI delta see true colors.
    InvertCopy m_invert;
    bool m_invertFailed = false;
    int m_invMode = -1;            // -1 unknown, else g_settings.hudlessInvert
    bool m_invResolved = false;    // polarity decided for this session
    bool m_invActive = false;      // negate the hudless input
    // auto-detect probe: small ROI copies of bb + hudless into readback,
    // drained a few evaluates later once the copy has certainly finished
    Microsoft::WRL::ComPtr<ID3D12Resource> m_invRb[2];
    bool m_invRbQueued = false;
    uint64_t m_invQueuedCall = 0;
    uint32_t m_invRoiW = 0, m_invRoiH = 0;
    size_t m_invPitch = 0, m_invSize = 0;
    uint32_t m_invSamples = 0;
    double m_invDirect = 0.0;
    double m_invInverted = 0.0;

    // Garbage hudless detection (same ROI readback instrument): an
    // input that does not track the backbuffer (static white dummy in
    // Cyberpunk 2077, frozen frame in Control) is dropped for the rest
    // of the session and the pipeline interpolates the raw backbuffer.
    bool m_startupProbed = false;  // 3 startup samples drained + verdict
    bool m_hudlessBroken = false;  // session verdict: never use hudless
    uint64_t m_lastProbeCall = 0;  // periodic probe spacing bookkeeping
    uint32_t m_probeCount = 0;     // periodic samples for log cadence
    double m_startMeanB = 0.0;     // startup accumulators (3 samples)
    double m_startMeanH = 0.0;
    double m_startVarB = 0.0;
    double m_startVarH = 0.0;
    double m_periodicBb = -1.0;    // previous periodic sample means
    double m_periodicHud = -1.0;   // (-1 = no sample yet)

    bool InvProbe(ID3D12GraphicsCommandList* cmd, ID3D12Resource* bb,
        ID3D12Resource* hudless);
    void InvDrain();

    void CompositeUi(ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* ringSlot, ID3D12Resource* backbuffer,
        ID3D12Resource* interp, const D3D12_RESOURCE_DESC& interpDesc,
        uint32_t mode);

    bool m_temporalLogged = false;  // first temporal composite logged
};
} // namespace mfg
