// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_bank.h"

#include "dll_extract.h"

#include "../log.h"

#include <d3d12shader.h>
#include <d3dcompiler.h>

#include <array>
#include <cstring>

namespace mfg::lsfg
{
namespace
{
// d3dcompiler_47.dll is loaded on demand so the provider never adds a
// hard dependency inside a game process; D3DReflect needs the HLSL
// reflection interfaces and D3D12SerializeRootSignature comes from
// d3d12.lib (inbox).
using FnReflect = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
FnReflect g_fnReflect = nullptr;
bool LoadCompiler()
{
    if (g_fnReflect != nullptr)
        return true;
    const HMODULE hc = LoadLibraryA("d3dcompiler_47.dll");
    if (hc == nullptr)
        return false;
    g_fnReflect = reinterpret_cast<FnReflect>(GetProcAddress(hc, "D3DReflect"));
    return g_fnReflect != nullptr;
}

// Static sampler table per shader id (lsfg-vk v2.0.0-dev semantics,
// validated against the measured LS 3.2.1 binding table).
const std::vector<SamplerKind> kSamplers[] = {
    /* 255 mipmaps  */ {SamplerKind::BorderBlack},
    /* 256 generate */ {SamplerKind::BorderBlack, SamplerKind::EdgeCompare},
    /* 257 gamma0   */ {SamplerKind::BorderWhite, SamplerKind::EdgeCompare},
    /* 258 delta5   */ {SamplerKind::BorderWhite, SamplerKind::EdgeCompare},
    /* 259 gamma1a  */ {SamplerKind::BorderBlack},
    /* 260 gamma1b  */ {SamplerKind::BorderBlack},
    /* 261 gamma1c  */ {SamplerKind::BorderBlack},
    /* 262 gammaT   */ {SamplerKind::BorderBlack, SamplerKind::EdgeCompare},
    /* 263 delta1a  */ {SamplerKind::BorderBlack},
    /* 264 delta1b  */ {SamplerKind::BorderBlack},
    /* 265 delta1c  */ {SamplerKind::BorderBlack},
    /* 266 deltaT   */ {SamplerKind::BorderBlack, SamplerKind::EdgeCompare},
    /* 267 alpha0a  */ {SamplerKind::BorderBlack},
    /* 268 alpha0b  */ {SamplerKind::BorderBlack},
    /* 269 alpha0c  */ {SamplerKind::BorderBlack},
    /* 270 alpha1   */ {SamplerKind::BorderBlack},
    /* 271 deltaM1  */ {SamplerKind::BorderBlack},
    /* 272 deltaM2  */ {SamplerKind::BorderBlack},
    /* 273 deltaM3  */ {SamplerKind::BorderBlack},
    /* 274 deltaM4  */ {SamplerKind::BorderBlack, SamplerKind::EdgeCompare},
    /* 275 beta0    */ {SamplerKind::BorderWhite},
    /* 276 beta1a   */ {SamplerKind::BorderBlack},
    /* 277 beta1b   */ {SamplerKind::BorderBlack},
    /* 278 beta1c   */ {SamplerKind::BorderBlack},
    /* 279 betaMip  */ {SamplerKind::BorderBlack},
};
} // namespace

const std::vector<SamplerKind>& ShaderBank::SamplersFor(uint32_t id)
{
    if (id >= 255 && id <= 279)
        return kSamplers[id - 255];
    static const std::vector<SamplerKind> empty;
    return empty;
}

const ShaderMeta* ShaderBank::Get(uint32_t id) const
{
    const auto it = m_shaders.find(id);
    return it == m_shaders.end() ? nullptr : &it->second;
}

bool ShaderBank::Load(ID3D12Device* device,
        const std::map<uint32_t, std::vector<uint8_t>>& resources)
{
    m_shaders.clear();
    m_error.clear();
    if (!LoadCompiler())
    {
        m_error = "d3dcompiler_47.dll not available";
        return false;
    }

    for (uint32_t id = 255; id <= 279; ++id)
    {
        const auto resIt = resources.find(id);
        if (resIt == resources.end())
        {
            m_error = "shader id " + std::to_string(id) + " missing from Lossless.dll";
            return false;
        }
        const std::vector<uint8_t>& blob = resIt->second;

        // Reflect bindings + thread group size.
        ID3D12ShaderReflection* refl = nullptr;
        if (FAILED(g_fnReflect(blob.data(), blob.size(), __uuidof(ID3D12ShaderReflection),
                               reinterpret_cast<void**>(&refl))))
        {
            m_error = "D3DReflect failed for shader id " + std::to_string(id);
            return false;
        }
        D3D12_SHADER_DESC desc{};
        refl->GetDesc(&desc);

        ShaderMeta meta{};
        for (UINT i = 0; i < desc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bind{};
            if (FAILED(refl->GetResourceBindingDesc(i, &bind)))
                continue;
            switch (bind.Type)
            {
            case D3D_SIT_TEXTURE: ++meta.srvCount; break;
            case D3D_SIT_UAV_RWTYPED: ++meta.uavCount; break;
            case D3D_SIT_SAMPLER: ++meta.samplerCount; break;
            case D3D_SIT_CBUFFER:
                meta.hasCb = true;
                meta.cbSize = bind.NumSamples; // misuse-proof: see below
                break;
            default: break;
            }
        }
        if (meta.hasCb)
        {
            ID3D12ShaderReflectionConstantBuffer* cb =
                refl->GetConstantBufferByIndex(0);
            D3D12_SHADER_BUFFER_DESC cbDesc{};
            if (cb != nullptr && SUCCEEDED(cb->GetDesc(&cbDesc)))
                meta.cbSize = cbDesc.Size;
        }
        UINT tx = 0, ty = 0, tz = 0;
        refl->GetThreadGroupSize(&tx, &ty, &tz);
        meta.threadsX = tx;
        meta.threadsY = ty;
        refl->Release();

        // The static sampler table must match what the shader binds,
        // otherwise the root signature would be wrong.
        const std::vector<SamplerKind>& kinds = SamplersFor(id);
        if (kinds.size() != meta.samplerCount)
        {
            m_error = "shader id " + std::to_string(id) +
                      ": expected " + std::to_string(kinds.size()) +
                      " samplers, reflected " + std::to_string(meta.samplerCount);
            return false;
        }

        // Root signature: [CBV b0?] [SRV table t0..n] [UAV table u0..m]
        // + static samplers (space 0, SM5 everywhere).
        std::vector<D3D12_ROOT_PARAMETER> params;
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
        ranges.reserve(2);
        if (meta.hasCb)
        {
            D3D12_ROOT_PARAMETER p{};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            meta.cbSlot = static_cast<int>(params.size());
            params.push_back(p);
        }
        if (meta.srvCount > 0)
        {
            D3D12_DESCRIPTOR_RANGE r{};
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r.NumDescriptors = meta.srvCount;
            r.BaseShaderRegister = 0;
            r.RegisterSpace = 0;
            r.OffsetInDescriptorsFromTableStart = 0;
            ranges.push_back(r);
            D3D12_ROOT_PARAMETER p{};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges = &ranges.back();
            meta.srvSlot = static_cast<int>(params.size());
            params.push_back(p);
        }
        if (meta.uavCount > 0)
        {
            D3D12_DESCRIPTOR_RANGE r{};
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            r.NumDescriptors = meta.uavCount;
            r.BaseShaderRegister = 0;
            r.RegisterSpace = 0;
            r.OffsetInDescriptorsFromTableStart = 0;
            ranges.push_back(r);
            D3D12_ROOT_PARAMETER p{};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges = &ranges.back();
            meta.uavSlot = static_cast<int>(params.size());
            params.push_back(p);
        }

        const std::vector<SamplerKind>& samplerKinds = SamplersFor(id);
        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
        staticSamplers.reserve(samplerKinds.size());
        for (uint32_t s = 0; s < samplerKinds.size(); ++s)
        {
            D3D12_STATIC_SAMPLER_DESC ss{};
            ss.ShaderRegister = s;
            ss.RegisterSpace = 0;
            ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            if (samplerKinds[s] == SamplerKind::EdgeCompare)
            {
                ss.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
                ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            else if (samplerKinds[s] == SamplerKind::BorderWhite)
            {
                ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                ss.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            }
            else
            {
                ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                ss.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            }
            ss.MipLODBias = 0.0f;
            ss.MaxAnisotropy = 0;
            ss.MinLOD = 0.0f;
            ss.MaxLOD = D3D12_FLOAT32_MAX;
            staticSamplers.push_back(ss);
        }

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = static_cast<UINT>(params.size());
        rootDesc.pParameters = params.data();
        rootDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
        rootDesc.pStaticSamplers = staticSamplers.data();
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                &sigBlob, &errBlob)))
        {
            m_error = "root signature serialization failed for id " + std::to_string(id);
            if (errBlob != nullptr)
                m_error += std::string(": ",
                    static_cast<const char*>(errBlob->GetBufferPointer()));
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                sigBlob->GetBufferSize(), IID_PPV_ARGS(&meta.rootSig))))
        {
            m_error = "CreateRootSignature failed for id " + std::to_string(id);
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = meta.rootSig.Get();
        psoDesc.CS.pShaderBytecode = blob.data();
        psoDesc.CS.BytecodeLength = blob.size();
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&meta.pso))))
        {
            m_error = "CreateComputePipelineState failed for id " + std::to_string(id);
            return false;
        }

        MFG_LOG_INFO("lsfg: shader id=%u srv=%u uav=%u samp=%u cb=%u threads=%ux%u",
            id, meta.srvCount, meta.uavCount, meta.samplerCount,
            meta.cbSize, meta.threadsX, meta.threadsY);
        m_shaders.emplace(id, std::move(meta));
    }
    return true;
}
} // namespace mfg::lsfg
