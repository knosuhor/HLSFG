// Invert copy pass implementation, see invert_copy.h for the contract.
#include "invert_copy.h"

#include "log.h"

#include <cstring>

namespace mfg
{
namespace
{
const char* const kShader = R"(Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);
cbuffer Params : register(b0) { uint W; uint H; };
[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    float4 s = Src.Load(int3(id.xy, 0));
    Dst[id.xy] = float4(1.0 - s.rgb, s.a);
})";
DXGI_FORMAT Concrete(DXGI_FORMAT f)
{
    // Typeless resources need a concrete view format.
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return f;
    }
}

D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
} // namespace

bool InvertCopy::Init(ID3D12Device* device)
{
    if (m_ready)
        return true;
    if (device == nullptr)
        return false;
    m_device = device;
    m_heapInc = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    if (lib == nullptr)
    {
        MFG_LOG_ERROR("inv: d3dcompiler_47.dll not found");
        return false;
    }
    using PFN_D3DCOMPILE = HRESULT(WINAPI*)(LPCVOID, UINT, LPCSTR,
        const void*, void*, LPCSTR, LPCSTR, UINT,
        UINT, ID3DBlob**, ID3DBlob**);
    const auto compile = reinterpret_cast<PFN_D3DCOMPILE>(
        GetProcAddress(lib, "D3DCompile"));
    if (compile == nullptr)
    {
        MFG_LOG_ERROR("inv: D3DCompile not available");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errs;
    const HRESULT hr = compile(kShader, (UINT) std::strlen(kShader),
        nullptr, nullptr, nullptr, "CSMain", "cs_5_1", 0, 0,
        &code, &errs);
    if (FAILED(hr))
    {
        MFG_LOG_ERROR("inv: shader compile failed hr=0x%lx %s",
            (unsigned long) hr,
            errs != nullptr ? (const char*) errs->GetBufferPointer()
                           : "");
        return false;
    }

    // root signature: constants(w, h) | SRV table(1) | UAV table(1)
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 1;
    srv.BaseShaderRegister = 0;
    srv.OffsetInDescriptorsFromTableStart = 0;
    uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav.NumDescriptors = 1;
    uav.BaseShaderRegister = 0;
    uav.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.RegisterSpace = 0;
    rp[0].Constants.Num32BitValues = 2;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &srv;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].DescriptorTable.NumDescriptorRanges = 1;
    rp[2].DescriptorTable.pDescriptorRanges = &uav;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = rp;
    Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rs,
            D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
    {
        MFG_LOG_ERROR("inv: root signature serialize failed");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0,
            rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS(&m_root))))
    {
        MFG_LOG_ERROR("inv: root signature create failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = m_root.Get();
    ps.CS.pShaderBytecode = code->GetBufferPointer();
    ps.CS.BytecodeLength = code->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&m_pso))))
    {
        MFG_LOG_ERROR("inv: pipeline state create failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2; // 1 SRV + 1 UAV
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap))))
    {
        MFG_LOG_ERROR("inv: descriptor heap create failed");
        return false;
    }

    m_ready = true;
    MFG_LOG_INFO("inv: invert copy pass ready");
    return true;
}

void InvertCopy::Dispatch(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* src, D3D12_RESOURCE_STATES srcBefore,
    D3D12_RESOURCE_STATES srcAfter, ID3D12Resource* dst)
{
    if (!m_ready || cmd == nullptr || src == nullptr || dst == nullptr)
        return;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    const D3D12_RESOURCE_DESC dd = dst->GetDesc();
    if (sd.Width != dd.Width || sd.Height != dd.Height)
        return; // sizes must match the copy contract

    const auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = Concrete(sd.Format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(src, &sv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = Concrete(dd.Format);
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(dst, nullptr, &uv,
        {cpu.ptr + m_heapInc});

    const D3D12_RESOURCE_STATES npsr =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_BARRIER pre[] = {
        Trans(src, srcBefore, npsr),
    };
    cmd->ResourceBarrier(1, pre);

    struct Cb { uint32_t w, h; };
    const Cb cb{static_cast<uint32_t>(sd.Width),
        static_cast<uint32_t>(sd.Height)};
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root.Get());
    cmd->SetComputeRoot32BitConstants(0, 2, &cb, 0);
    cmd->SetComputeRootDescriptorTable(1, {gpu.ptr});
    cmd->SetComputeRootDescriptorTable(2, {gpu.ptr + (uint64_t) m_heapInc});
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((cb.w + 15) / 16, (cb.h + 15) / 16, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = dst;
    cmd->ResourceBarrier(1, &uavBarrier);
    const D3D12_RESOURCE_BARRIER post[] = {
        Trans(src, npsr, srcAfter),
    };
    cmd->ResourceBarrier(1, post);
}

void InvertCopy::Release()
{
    m_heap.Reset();
    m_pso.Reset();
    m_root.Reset();
    m_device.Reset();
    m_ready = false;
}
} // namespace mfg
