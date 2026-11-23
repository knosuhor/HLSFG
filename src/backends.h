// FG backend interface. This is the seam that makes phases 1 and 2 cheap:
// the NGX surface and OptiScaler plumbing stay untouched, only the class
// behind IFGBackend changes (Phase 1: lsfg-vk inference, Phase 2: MAKO FG).
//
// Design rule for backends (per the project's goal):
//   - Input of interest: the HUD-less color frame passed to Evaluate
//     (DLSSG.HUDLess, falling back to DLSSG.Backbuffer).
//   - Depth / motion vectors / jitter / exposure: available on the params
//     object and deliberately UNUSED by MG-style backends.
//   - A backend never touches present/pacing: that stays in OptiScaler's FG
//     framework. Backends only fill the DLSSG.Output* slot they are asked to.
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgi.h>

#include "ngx/nvsdk_ngx_defs.h"
#include "ngx/nvsdk_ngx_params.h"

namespace mfg
{
class LsfgBackend; // phase 1: defined in lsfg_backend.h

class IFGBackend
{
  public:
    virtual ~IFGBackend() = default;

    virtual const char* Name() const = 0;
    virtual NVSDK_NGX_Result InitD3D12(ID3D12Device* device) = 0;
    // One call per presented FG slot: slot 1 is the real frame
    // (DLSSG.MultiFrameIndex == 1), slots 2..N are generated. Phase 0
    // semantics: observe, never produce. Phase 0.5: copy the frame through
    // the output chain. In both cases always answer Success so the game keeps
    // rendering at a healthy rate.
    virtual NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                           NVSDK_NGX_Parameter* params) = 0;
    virtual void Shutdown() = 0;
};

// Phase 0: contract validator. Logs the FG inputs OptiScaler and the game
// hand over, counts slots, produces nothing (FG slots stay black under
// FGOutput=dlssg; use Blend when the image must stay visible).
class PassthroughBackend final : public IFGBackend
{
  public:
    const char* Name() const override { return "Passthrough (phase 0)"; }
    NVSDK_NGX_Result InitD3D12(ID3D12Device* device) override;
    NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                   NVSDK_NGX_Parameter* params) override;
    void Shutdown() override;

  private:
    ID3D12Device* m_device = nullptr;
    uint64_t m_realFrames = 0;
};

// Phase 0.5: output-chain validator. Repeats the frame OptiScaler hands
// over through the FG outputs: real slot from DLSSG.Backbuffer, generated
// slots from DLSSG.HUDLess when present. Proves the provider owns the whole
// output chain before any real interpolation exists.
class BlendBackend final : public IFGBackend
{
  public:
    const char* Name() const override { return "Blend (phase 0.5)"; }
    NVSDK_NGX_Result InitD3D12(ID3D12Device* device) override;
    NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                   NVSDK_NGX_Parameter* params) override;
    void Shutdown() override;

  private:
    ID3D12Device* m_device = nullptr;
    uint64_t m_calls = 0;
    uint64_t m_copies = 0;
    uint64_t m_missedOutputs = 0;
};

const char* BackendName(int backend);
IFGBackend* ActiveBackend();
} // namespace mfg
