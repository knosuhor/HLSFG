// SPDX-License-Identifier: GPL-3.0-or-later
//
// LSFG shader bank: creates D3D12 root signatures + pipeline state objects
// for Lossless Scaling's packed DXBC compute shaders (raw RT_RCDATA ids
// 255-279, the lsfg-vk v2.0.0-dev shader family). Registers come from
// D3DReflect at runtime (d3dcompiler_47 loaded on demand), so a new LS
// version only needs a probe run, not code changes.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mfg::lsfg
{
// Special static samplers of the FG family (lsfg-vk semantics):
//  border black/never-compare, border white/never, edge/always-compare.
enum class SamplerKind : uint32_t
{
    BorderBlack = 0,
    BorderWhite = 1,
    EdgeCompare = 2,
};

// One reflected + compiled compute shader.
struct ShaderMeta
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    uint32_t srvCount = 0;   // t0..t(srvCount-1)
    uint32_t uavCount = 0;   // u0..u(uavCount-1)
    uint32_t samplerCount = 0;
    uint32_t cbSize = 0;     // reflected cbuffer size (32 or 48 bytes)
    bool hasCb = false;
    uint32_t threadsX = 1;
    uint32_t threadsY = 1;
    // Root parameter slots (-1 when unused), cb first, then srv/uav tables.
    int cbSlot = -1;
    int srvSlot = -1;
    int uavSlot = -1;
};

class ShaderBank
{
  public:
    // Builds pipelines for the whole FG family (ids 255-279) from the raw
    // RT_RCDATA resources extracted from Lossless.dll. Returns false and
    // fills m_error on failure.
    bool Load(ID3D12Device* device,
              const std::map<uint32_t, std::vector<uint8_t>>& resources);

    // nullptr when the id was never built.
    const ShaderMeta* Get(uint32_t id) const;

    // Static samplers this shader id binds (in s0.. order).
    static const std::vector<SamplerKind>& SamplersFor(uint32_t id);

    std::string m_error;

  private:
    std::map<uint32_t, ShaderMeta> m_shaders;
};
} // namespace mfg::lsfg
