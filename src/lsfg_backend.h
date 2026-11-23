// Phase 1 backend: LSFG inference on the HUD-less frame pair.
//
// The LS 3.2.1 native blob (Lossless.dll, 202 RCDATA resources) yields
// the DXBC compute family ids 255-279; the lsfg-vk v2.0.0-dev graph
// provides theschedule semantics. The backend runs that graph directly
// on the caller's D3D12 device, no Vulkan interop (docs/PHASE1.md).
#pragma once

#include "backends.h"

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
    uint64_t m_fidx = 0;           // real frames fed into the ring
    uint64_t m_calls = 0;
    uint64_t m_generations = 0;
    uint64_t m_fallbacks = 0;
};
} // namespace mfg
