// FG backend interface. This is the seam that makes phases 1 and 2 cheap:
// the NGX surface and OptiScaler plumbing stay untouched, only the class
// behind IFGBackend changes (Phase 1: lsfg-vk inference, Phase 2: MAKO FG).
//
// Design rule for backends (per the project's goal):
//   - Input of interest: the HUD-less color frame passed to Evaluate.
//   - Depth / motion vectors / jitter / exposure: available on the params
//     object and deliberately UNUSED by MG-style backends.
//   - A backend never touches present/pacing: that stays in OptiScaler's FG framework.
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgi.h>

#include "ngx/nvsdk_ngx_defs.h"

namespace mfg
{
class IFGBackend
{
  public:
    virtual ~IFGBackend() = default;

    virtual const char* Name() const = 0;
    virtual NVSDK_NGX_Result InitD3D12(ID3D12Device* device) = 0;
    // One call per rendered real frame. Phase 0 semantics: observe, never
    // produce. Returns Success so the game keeps rendering at its base rate.
    virtual NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                           NVSDK_NGX_Parameter* params) = 0;
    virtual void Shutdown() = 0;
};

// Phase 0: contract validator. Logs the FG inputs OptiScaler and the game hand
// over, counts real frames, produces nothing (game shows only real frames).
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

// Phase 0.5 target: copy/blend the HUD-less frame N times through the FG
// output path, proving ownership of the output chain before any real
// interpolation happens. Deliberately unimplemented in this skeleton.
class BlendBackend final : public IFGBackend
{
  public:
    const char* Name() const override { return "Blend (planned stub)"; }
    NVSDK_NGX_Result InitD3D12(ID3D12Device* device) override;
    NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* /*cmdList*/,
                                   NVSDK_NGX_Parameter* /*params*/) override
    {
        return NVSDK_NGX_Result_Success;
    }
    void Shutdown() override {}

  private:
    ID3D12Device* m_device = nullptr;
};

const char* BackendName(int backend);
IFGBackend* ActiveBackend();
} // namespace mfg
