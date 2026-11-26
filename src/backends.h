// FG backend interface. This is the seam that makes a future backend
// (e.g. MAKO FG) cheap: the NGX surface and OptiScaler plumbing stay
// untouched, only the class behind IFGBackend changes.
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
class LsfgBackend; // the backend: defined in lsfg_backend.h

class IFGBackend
{
  public:
    virtual ~IFGBackend() = default;

    virtual const char* Name() const = 0;
    virtual NVSDK_NGX_Result InitD3D12(ID3D12Device* device) = 0;
    // One Evaluate per burst group (OptiScaler's burst mode carries all
    // outputs in a single call) or per presented FG slot in the legacy
    // per-slot mode. Always answer Success so the game keeps rendering
    // at a healthy rate.
    virtual NVSDK_NGX_Result EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
                                           NVSDK_NGX_Parameter* params) = 0;
    virtual void Shutdown() = 0;
};

// The single backend instance (LSFG today).
IFGBackend* ActiveBackend();
} // namespace mfg
