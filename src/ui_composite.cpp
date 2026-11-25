// UI recomposition (hudless path), see ui_composite.h for the contract.
#include "ui_composite.h"

#include "log.h"

#include <cstring>

namespace mfg
{
namespace
{
const char* const kShader = R"(Texture2D<float4> Hudless : register(t0);
Texture2D<float4> Present : register(t1);
Texture2D<float4> Interp : register(t2);
RWTexture2D<float4> Dst : register(u0);
cbuffer Params : register(b0) { uint W; uint H; float Cutoff; uint Mode; };
[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    float3 hcol = Hudless.Load(int3(id.xy, 0)).rgb;
    float3 pcol = Present.Load(int3(id.xy, 0)).rgb;
    float3 d = abs(hcol - pcol);
    float delta = max(d.r, max(d.g, d.b));
    float t = max(Cutoff, 0.0001);
    float mask;
    if (Mode == 0)
    {
        // hudless delta: present - hudless is exactly the UI layer
        mask = smoothstep(t, t * 2.0, delta);
        float4 it = Interp.Load(int3(id.xy, 0));
        Dst[id.xy] = float4(it.rgb + (pcol - hcol) * mask, it.a);
    }
    else
    {
        // temporal: reference is the previous backbuffer; pixels that
        // did not change between real frames are UI or static content.
        // The threshold is RELATIVE to the pixel's own luminance
        // (motion perception is contrast-scaled, not absolute: an
        // absolute floor over-masks dark scenes and visually disables
        // frame gen, a bright-only gate is single-game tuning). A
        // pixel counts as unchanged when its delta stays below
        // max(0.5*Cutoff, 25% of its own luma), so flat low-contrast
        // areas mask (indistinguishable from their interpolation)
        // while real detail motion stays interpolated.
        float lp = max(pcol.r, max(pcol.g, pcol.b));
        float lr = max(hcol.r, max(hcol.g, hcol.b));
        float l = max(lp, lr);
        float thr = max(t * 0.5, 0.25 * l);        mask = 1.0 - smoothstep(thr, thr * 2.0, delta);
        float4 it = Interp.Load(int3(id.xy, 0));
        Dst[id.xy] = float4(it.rgb + (pcol - it.rgb) * mask, it.a);
    }
})";
DXGI_FORMAT Concrete(DXGI_FORMAT f)
{
    // Typeless resources need a concrete view format.
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
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

bool UiComposite::Init(ID3D12Device* device)
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
        MFG_LOG_ERROR("ui: d3dcompiler_47.dll not found");
        return false;
    }
    using PFN_D3DCOMPILE = HRESULT(WINAPI*)(LPCVOID, UINT, LPCSTR,
        const void*, void*, LPCSTR, LPCSTR, UINT,
        UINT, ID3DBlob**, ID3DBlob**);
    const auto compile = reinterpret_cast<PFN_D3DCOMPILE>(
        GetProcAddress(lib, "D3DCompile"));
    if (compile == nullptr)
    {
        MFG_LOG_ERROR("ui: D3DCompile not available");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errs;
    const HRESULT hr = compile(kShader, (UINT) std::strlen(kShader),
        nullptr, nullptr, nullptr, "CSMain", "cs_5_1", 0, 0,
        &code, &errs);
    if (FAILED(hr))
    {
        MFG_LOG_ERROR("ui: shader compile failed hr=0x%lx %s",
            (unsigned long) hr,
            errs != nullptr ? (const char*) errs->GetBufferPointer()
                           : "");
        return false;
    }

    // root signature: constants(w, h, cutoff, mode) | SRV table(3) | UAV(1)
    D3D12_DESCRIPTOR_RANGE srv{}, uav{};
    srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv.NumDescriptors = 3;
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
    rp[0].Constants.Num32BitValues = 4;
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
        MFG_LOG_ERROR("ui: root signature serialize failed");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0,
            rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS(&m_root))))
    {
        MFG_LOG_ERROR("ui: root signature create failed");        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = m_root.Get();
    ps.CS.pShaderBytecode = code->GetBufferPointer();
    ps.CS.BytecodeLength = code->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&m_pso))))
    {
        MFG_LOG_ERROR("ui: pipeline state create failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4; // 3 SRV + 1 UAV
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap))))
    {
        MFG_LOG_ERROR("ui: descriptor heap create failed");
        return false;
    }

    m_ready = true;
    MFG_LOG_INFO("ui: composite pass ready");
    return true;
}
bool UiComposite::EnsureScratch(const D3D12_RESOURCE_DESC& interpDesc)
{
    if (m_scratch != nullptr && m_scratchW == interpDesc.Width &&
        m_scratchH == interpDesc.Height &&
        m_scratchFmt == interpDesc.Format)
        return true;
    m_scratch.Reset();
    m_scratchW = interpDesc.Width;
    m_scratchH = interpDesc.Height;
    m_scratchFmt = interpDesc.Format;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = interpDesc;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateCommittedResource(&hp,
            D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_scratch))))
    {
        m_scratchW = 0;
        MFG_LOG_ERROR("ui: scratch alloc failed (%ux%u fmt %u)",
            (unsigned) interpDesc.Width,
            (unsigned) interpDesc.Height,
            (unsigned) interpDesc.Format);
        return false;
    }
    return true;
}
void UiComposite::Dispatch(ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* hudless, ID3D12Resource* present,
    ID3D12Resource* interp, uint32_t width, uint32_t height, float cutoff,
    uint32_t mode)
{
    if (!m_ready || cmd == nullptr || hudless == nullptr ||
        present == nullptr || interp == nullptr || width == 0 ||
        height == 0)
        return;
    const D3D12_RESOURCE_DESC interpDesc = interp->GetDesc();
    if (interpDesc.DepthOrArraySize != 1)
    {
        if (!m_warned)
        {
            m_warned = true;
            MFG_LOG_INFO("ui: interp array size %u, composite skipped",
                (unsigned) interpDesc.DepthOrArraySize);
        }
        return;
    }
    if (!EnsureScratch(interpDesc))
        return;

    // heap: [0] hudless [1] present [2] interp [3] scratch UAV
    const auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    ID3D12Resource* sr[3] = {hudless, present, interp};
    for (UINT i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = Concrete(sr[i]->GetDesc().Format);
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(sr[i], &sv,
            {cpu.ptr + m_heapInc * i});
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = Concrete(interpDesc.Format);
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_scratch.Get(), nullptr, &uv,
        {cpu.ptr + m_heapInc * 3});

    const D3D12_RESOURCE_STATES npsr =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_BARRIER pre[] = {
        Trans(interp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, npsr),
        Trans(hudless, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, npsr),
    };
    cmd->ResourceBarrier(2, pre);

    struct Cb { uint32_t w, h; float cutoff; uint32_t mode; };
    const Cb cb{width, height, cutoff, mode};
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root.Get());
    cmd->SetComputeRoot32BitConstants(0, 4, &cb, 0);
    cmd->SetComputeRootDescriptorTable(1, {gpu.ptr});
    cmd->SetComputeRootDescriptorTable(2,
        {gpu.ptr + (uint64_t) m_heapInc * 3});
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    const D3D12_RESOURCE_BARRIER mid[] = {
        Trans(interp, npsr, D3D12_RESOURCE_STATE_COPY_DEST),
        Trans(m_scratch.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cmd->ResourceBarrier(2, mid);
    cmd->CopyResource(interp, m_scratch.Get());
    const D3D12_RESOURCE_BARRIER post[] = {
        Trans(interp, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Trans(m_scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Trans(hudless, npsr,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmd->ResourceBarrier(3, post);
}

void UiComposite::Release()
{
    m_scratch.Reset();
    m_heap.Reset();
    m_pso.Reset();
    m_root.Reset();
    m_device.Reset();
    m_ready = false;
    m_warned = false;
    m_scratchW = 0;
    m_scratchH = 0;
    m_scratchFmt = DXGI_FORMAT_UNKNOWN;
}
} // namespace mfg
