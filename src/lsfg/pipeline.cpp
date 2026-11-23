// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native D3D12 port of the lsfg-vk v2.0.0-dev execution graph
// (https://github.com/PancakeTAS/lsfg-vk, tag v2.0.0-dev,
// GPL-3.0-or-later, (c) PancakeTAS et al.), driving the DXBC shaders
// packed in Lossless Scaling 3.2.1's Lossless.dll (RCDATA 255-279).
#include "pipeline.h"

#include "../log.h"

#include <algorithm>
#include <cstring>

namespace mfg::lsfg
{
namespace
{
constexpr uint32_t kMipCount = 7;    // flow pyramid depth
constexpr uint32_t kCbAlign = 256;   // constant slot alignment

constexpr DXGI_FORMAT kFmtRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kFmtR8 = DXGI_FORMAT_R8_UNORM;
constexpr DXGI_FORMAT kFmtRgba16F = DXGI_FORMAT_R16G16B16A16_FLOAT;

// temporal depth of the alpha combine per mip level (lsfg-vk)
uint32_t TemporalFor(uint32_t level)
{
    return level == 0 ? 3 : 2;
}
} // namespace

Pipeline::~Pipeline()
{
    if (m_cbMapped != nullptr && m_cbUpload != nullptr)
        m_cbUpload->Unmap(0, nullptr);
    m_cbMapped = nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS Pipeline::CbGpu(uint32_t slot) const
{
    return m_cbUpload->GetGPUVirtualAddress() + kCbAlign * slot;
}

Pipeline::Tex* Pipeline::CreateTex(DXGI_FORMAT fmt, Extent ext, const char*)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = ext.w;
    desc.Height = ext.h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto tex = std::make_unique<Tex>();
    tex->fmt = fmt;
    tex->ext = ext;
    if (FAILED(m_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&tex->res))))
        return nullptr;

    // one UAV slot per texture for the one-shot startup clears
    m_clearSlots.push_back(AllocSlots(1, "clear"));
    Tex* out = tex.get();
    m_pool.push_back(std::move(tex));
    return out;
}

uint32_t Pipeline::AllocSlots(uint32_t count, const char*)
{
    const uint32_t start = m_heapCursor;
    m_heapCursor += count;
    return start;
}

void Pipeline::WriteSrv(uint32_t slot, const Tex& tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = tex.fmt;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 0xFFFFFFFF; // -1 = all mips (0 is invalid)
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    m_device->CreateShaderResourceView(tex.res.Get(), &desc,
        {m_heap->GetCPUDescriptorHandleForHeapStart().ptr + m_heapIncrement * slot});
}

void Pipeline::WriteUav(uint32_t slot, const Tex& tex)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = tex.fmt;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(tex.res.Get(), nullptr, &desc,
        {m_heap->GetCPUDescriptorHandleForHeapStart().ptr + m_heapIncrement * slot});
}

void Pipeline::WriteExternalUav(uint32_t slot, ID3D12Resource* res,
        DXGI_FORMAT fmt, uint32_t arraySlice)
{
    if (res == nullptr)
        return;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        rd.DepthOrArraySize <= arraySlice)
        return;

    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = fmt;
    if (rd.DepthOrArraySize > 1)
    {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = arraySlice;
        desc.Texture2DArray.ArraySize = 1;
        desc.Texture2DArray.MipSlice = 0;
    }
    else
    {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    m_device->CreateUnorderedAccessView(res, nullptr, &desc,
        {m_heap->GetCPUDescriptorHandleForHeapStart().ptr + m_heapIncrement * slot});
}

Pipeline::Set Pipeline::BuildSet(uint32_t id,
        const std::vector<Tex*>& srvs, const std::vector<Tex*>& uavs,
        int cbSlot, Extent grp)
{
    Set s{};
    s.id = id;
    s.srvCount = static_cast<uint32_t>(srvs.size());
    s.heapStart = AllocSlots(
        static_cast<uint32_t>(srvs.size() + uavs.size()), "set");
    s.cbSlot = cbSlot;
    s.grpW = grp.w;
    s.grpH = grp.h;

    // the descriptor heap does not exist yet: defer the writes
    for (size_t i = 0; i < srvs.size(); ++i)
        m_pending.push_back({s.heapStart + static_cast<uint32_t>(i), srvs[i], false});
    for (size_t i = 0; i < uavs.size(); ++i)
        m_pending.push_back({s.heapStart + s.srvCount + static_cast<uint32_t>(i),
            uavs[i], true});
    return s;
}

// One compute dispatch. The UAV barrier between dispatches mirrors the
// per-image GENERAL->GENERAL barriers around every lsfg-vk dispatch.
void Pipeline::DispatchSet(ID3D12GraphicsCommandList* cmd, const Set& s)
{
    const ShaderMeta* m = m_bank->Get(s.id);
    if (m == nullptr)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &barrier);

    cmd->SetPipelineState(m->pso.Get());
    cmd->SetComputeRootSignature(m->rootSig.Get());
    if (m->cbSlot >= 0 && s.cbSlot >= 0)
        cmd->SetComputeRootConstantBufferView(static_cast<UINT>(m->cbSlot),
            CbGpu(static_cast<uint32_t>(s.cbSlot)));
    if (m->srvSlot >= 0)
        cmd->SetComputeRootDescriptorTable(static_cast<UINT>(m->srvSlot),
            {m_heap->GetGPUDescriptorHandleForHeapStart().ptr +
             m_heapIncrement * s.heapStart});
    if (m->uavSlot >= 0)
        cmd->SetComputeRootDescriptorTable(static_cast<UINT>(m->uavSlot),
            {m_heap->GetGPUDescriptorHandleForHeapStart().ptr +
             m_heapIncrement * (s.heapStart + s.srvCount)});
    cmd->Dispatch(s.grpW, s.grpH, 1);
}

void Pipeline::RecordClears(ID3D12GraphicsCommandList* cmd)
{
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    static const UINT zeros[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < m_pool.size(); ++i)
    {
        const Tex& tex = *m_pool[i];
        WriteUav(m_clearSlots[i], tex);
        if (m_cpuHeap)
        {
            // ClearUnorderedAccessView* demands a CPU handle that lives
            // in a non-shader-visible heap; mirror the UAV descriptor there.
            D3D12_UNORDERED_ACCESS_VIEW_DESC cdesc{};
            cdesc.Format = tex.fmt;
            cdesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            m_device->CreateUnorderedAccessView(tex.res.Get(), nullptr, &cdesc,
                {m_cpuHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                 m_heapIncrement * i});
            cmd->ClearUnorderedAccessViewUint(
                {m_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                 m_heapIncrement * m_clearSlots[i]},
                {m_cpuHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                 m_heapIncrement * i},
                tex.res.Get(), zeros, 0, nullptr);
        }
        else
        {
            cmd->ClearUnorderedAccessViewUint(
                {m_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                 m_heapIncrement * m_clearSlots[i]},
                {m_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                 m_heapIncrement * m_clearSlots[i]},
                tex.res.Get(), zeros, 0, nullptr);
        }
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &barrier);
}
void Pipeline::CommitHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = m_heapCursor;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap))))
    {
        MFG_LOG_ERROR("lsfg: failed to create the descriptor heap");
        return;
    }
    m_heapIncrement =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // non-shader-visible mirror heap: ClearUnorderedAccessView*
    // CPU handles must come from a heap the CPU can read.
    D3D12_DESCRIPTOR_HEAP_DESC chd{};
    chd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    chd.NumDescriptors = m_pool.empty() ? 1u : static_cast<UINT>(m_pool.size());
    if (FAILED(m_device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&m_cpuHeap))))
        MFG_LOG_ERROR("lsfg: failed to create the clear descriptor heap");

    // flush the deferred set descriptors
    for (const PendingWrite& w : m_pending)
    {
        if (w.uav)
            WriteUav(w.slot, *w.tex);
        else
            WriteSrv(w.slot, *w.tex);
    }
    m_pending.clear();
    m_pending.shrink_to_fit();
}

bool Pipeline::Init(ID3D12Device* device, ShaderBank& bank,
        Extent sourceExtent, uint32_t flowScale, uint32_t genCount,
        DXGI_FORMAT sourceFormat)
{
    m_device = device;
    m_bank = &bank;
    m_sourceExtent = sourceExtent;
    m_flowScale = flowScale;
    m_genCount = genCount;
    m_flowExtent = DivideExtent(sourceExtent, flowScale);

    if (genCount == 0 || flowScale == 0)
    {
        MFG_LOG_ERROR("lsfg: invalid pipeline configuration");
        return false;
    }

    // --- persistent upload constant slots (one per pass + prepass) ---
    {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kCbAlign * (static_cast<uint64_t>(genCount) + 1);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_cbUpload))))
        {
            MFG_LOG_ERROR("lsfg: failed to create the constant upload buffer");
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(m_cbUpload->Map(0, nullptr, &mapped)))
        {
            MFG_LOG_ERROR("lsfg: failed to map the constant upload buffer");
            return false;
        }
        m_cbMapped = static_cast<ConstantBuffer*>(mapped);
        for (uint32_t i = 0; i <= genCount; ++i)
        {
            ConstantBuffer cb{};
            cb.resolutionInvScale = static_cast<float>(flowScale);
            cb.uiThreshold = 0.5f;
            if (i < genCount)
                cb.timestamp = static_cast<float>(i + 1) /
                    static_cast<float>(genCount + 1);
            else
                cb.timestamp = 0.5f; // prepass slot (index 0 of 1)
            *reinterpret_cast<ConstantBuffer*>(
                reinterpret_cast<uint8_t*>(m_cbMapped) +
                kCbAlign * i) = cb;
        }
    }
    const int prepassCb = static_cast<int>(genCount);

    // --- transient images ---
    if ((m_black = CreateTex(kFmtRgba8, {4, 4}, "black")) == nullptr)
        return false;
    for (uint32_t lvl = 0; lvl < kMipCount; ++lvl)
    {
        // R8 typed UAV stores from the DXBC (float4 dcl_uav_typed)
        // silently no-op on D3D12/NVIDIA: the mip pyramid read back as
        // all-zero and the flow nets ran on a zero pyramid. The stored
        // value is a broadcast scalar, so RGBA8 is semantically identical.
        m_mips[lvl] = CreateTex(kFmtRgba8, ShiftExtent(m_flowExtent, lvl), "flow mip");
        if (m_mips[lvl] == nullptr)
            return false;
    }
    // The ring pair holds byte-exact copies of the source frame: it must be
    // created in the source format family or CopyResource is invalid (BGRA8
    // is the measured swapchain/slot family). UAV clears/casts above rely
    // on the clear+SRV path only where the format allows it.
    m_source[0] = CreateTex(sourceFormat, sourceExtent, "source even");
    m_source[1] = CreateTex(sourceFormat, sourceExtent, "source odd");
    if (m_source[0] == nullptr || m_source[1] == nullptr)
        return false;

    std::vector<Tex*> mipUavs = {m_mips[0], m_mips[1], m_mips[2], m_mips[3],
        m_mips[4], m_mips[5], m_mips[6]};

    // mipmaps: id 255, uav = 7 flow mips, cb = prepass. "first" of the
    // pair for evaluate N is ring slot (N+1)&1, hence two sets.
    {
        const Extent grp = AddShiftExtent(m_flowExtent, 63, 6);
        m_mipEven = BuildSet(255, {m_source[0]}, mipUavs, prepassCb, grp);
        m_mipOdd = BuildSet(255, {m_source[1]}, mipUavs, prepassCb, grp);
    }

    // --- alpha (mip pyramid combine) per level: 267 -> 268 -> 269 ---
    for (uint32_t lvl = 0; lvl < kMipCount; ++lvl)
    {
        AlphaCh& a = m_alpha[lvl];
        const Extent e = m_mips[lvl]->ext;
        const Extent half = AddShiftExtent(e, 1, 1);
        const Extent quarter = AddShiftExtent(half, 1, 1);
        for (uint32_t k = 0; k < 2; ++k)
        {
            a.temp0[k] = CreateTex(kFmtRgba8, half, "alpha temp0");
            a.temp1[k] = CreateTex(kFmtRgba8, half, "alpha temp1");
            if (a.temp0[k] == nullptr || a.temp1[k] == nullptr)
                return false;
        }
        for (uint32_t k = 0; k < 4; ++k)
        {
            a.outs[k] = CreateTex(kFmtRgba8, quarter, "alpha out");
            if (a.outs[k] == nullptr)
                return false;
        }
        a.set0 = BuildSet(267, {m_mips[lvl]},
            {a.temp0[0], a.temp0[1]}, prepassCb, AddShiftExtent(half, 7, 3));
        a.set1 = BuildSet(268, {a.temp0[0], a.temp0[1]},
            {a.temp1[0], a.temp1[1]}, prepassCb, AddShiftExtent(half, 7, 3));
        a.set2 = BuildSet(269, {a.temp1[0], a.temp1[1]},
            {a.outs[0], a.outs[1], a.outs[2], a.outs[3]}, prepassCb,
            AddShiftExtent(quarter, 7, 3));
    }

    // --- alpha1 (temporal combine) per level: 270 xT ---
    for (uint32_t lvl = 0; lvl < kMipCount; ++lvl)
    {
        Alpha1Ch& a = m_alpha1[lvl];
        a.temporal = TemporalFor(lvl);
        const Extent quarter = m_alpha[lvl].outs[0]->ext;
        const std::vector<Tex*> in = {m_alpha[lvl].outs[0], m_alpha[lvl].outs[1],
            m_alpha[lvl].outs[2], m_alpha[lvl].outs[3]};
        a.outs.resize(a.temporal);
        a.sets.resize(a.temporal);
        for (uint32_t t = 0; t < a.temporal; ++t)
        {
            for (uint32_t k = 0; k < 4; ++k)
            {
                a.outs[t][k] = CreateTex(kFmtRgba8, quarter, "alpha1 out");
                if (a.outs[t][k] == nullptr)
                    return false;
            }
            a.sets[t] = BuildSet(270, in,
                {a.outs[t][0], a.outs[t][1], a.outs[t][2], a.outs[t][3]},
                prepassCb, AddShiftExtent(quarter, 7, 3));
        }
    }

    // --- beta0: 275 x 3 rotations, source = alpha1[0] temporal vecs ---
    {
        const Extent quarter = m_alpha[0].outs[0]->ext;
        m_beta0.outs[0] = CreateTex(kFmtRgba8, quarter, "beta0 out");
        m_beta0.outs[1] = CreateTex(kFmtRgba8, quarter, "beta0 out");
        if (m_beta0.outs[0] == nullptr || m_beta0.outs[1] == nullptr)
            return false;
        const Alpha1Ch& a1 = m_alpha1[0];
        (void)a1;
        for (uint32_t t = 0; t < 3; ++t)
        {
            // rotation (i+1)%3, (i+2)%3, i%3 of the temporal vectors
            const std::array<uint32_t, 3> rot = {(t + 1) % 3, (t + 2) % 3, t % 3};
            std::vector<Tex*> srvs;
            srvs.reserve(12);
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t k = 0; k < 4; ++k)
                    srvs.push_back(m_alpha1[0].outs[rot[r]][k]);
            m_beta0.sets[t] = BuildSet(275, srvs,
                {m_beta0.outs[0], m_beta0.outs[1]}, prepassCb,
                AddShiftExtent(quarter, 7, 3));
        }
    }

    // --- beta1: 276 -> 277 -> 278 -> 279 (motion mips) ---
    {
        const Extent quarter = m_beta0.outs[0]->ext;
        for (uint32_t k = 0; k < 2; ++k)
        {
            m_beta1.temp0[k] = CreateTex(kFmtRgba8, quarter, "beta1 temp0");
            m_beta1.temp1[k] = CreateTex(kFmtRgba8, quarter, "beta1 temp1");
            if (m_beta1.temp0[k] == nullptr || m_beta1.temp1[k] == nullptr)
                return false;
        }
        for (uint32_t k = 0; k < 6; ++k)
        {
            // beta1 mips: same D3D12 R8 typed-store issue as the flow mips
        m_beta1.outs[k] = CreateTex(kFmtRgba8, ShiftExtent(quarter, k), "beta1 mip");
            if (m_beta1.outs[k] == nullptr)
                return false;
        }
        const Extent grp = AddShiftExtent(quarter, 7, 3);
        m_beta1.sets[0] = BuildSet(276, {m_beta0.outs[0], m_beta0.outs[1]},
            {m_beta1.temp0[0], m_beta1.temp0[1]}, prepassCb, grp);
        m_beta1.sets[1] = BuildSet(277, {m_beta1.temp0[0], m_beta1.temp0[1]},
            {m_beta1.temp1[0], m_beta1.temp1[1]}, prepassCb, grp);
        m_beta1.sets[2] = BuildSet(278, {m_beta1.temp1[0], m_beta1.temp1[1]},
            {m_beta1.temp0[0], m_beta1.temp0[1]}, prepassCb, grp);
        m_beta1.sets[3] = BuildSet(279, {m_beta1.temp0[0], m_beta1.temp0[1]},
            {m_beta1.outs[0], m_beta1.outs[1], m_beta1.outs[2], m_beta1.outs[3],
             m_beta1.outs[4], m_beta1.outs[5]}, prepassCb,
            AddShiftExtent(quarter, 31, 5));
    }

    // --- per generated frame: gamma x7, delta x3, generate ---
    m_passes.resize(genCount);
    for (uint32_t i = 0; i < genCount; ++i)
    {
        PassCh& pass = m_passes[i];
        const int cb = static_cast<int>(i);
        for (uint32_t j = 0; j < kMipCount; ++j)
        {
            const uint32_t y = 6 - j;         // level being refined
            const uint32_t T = TemporalFor(y);
            const Extent quarter = m_alpha[y].outs[0]->ext;
            const Extent grp = AddShiftExtent(quarter, 7, 3);
            const Tex* add1 = (j == 0) ? m_beta1.outs[5]
                : m_beta1.outs[6 - j];

            // gamma0: 257 per temporal phase (prev + cur + add0)
            {
                Gamma0Ch& g = pass.gamma0[j];
                g.sets.resize(T);
                for (uint32_t k = 0; k < 3; ++k)
                {
                    g.outs[k] = CreateTex(kFmtRgba8, quarter, "gamma0 out");
                    if (g.outs[k] == nullptr)
                        return false;
                }
                const Tex* add0 = (j == 0) ? m_black
                    : pass.gamma1[j - 1].image;
                for (uint32_t t = 0; t < T; ++t)
                {
                    std::vector<Tex*> srvs;
                    srvs.reserve(9);
                    const std::array<Tex*, 4>& prev =
                        m_alpha1[y].outs[(t + T - 1) % T];
                    const std::array<Tex*, 4>& cur = m_alpha1[y].outs[t % T];
                    for (uint32_t k = 0; k < 4; ++k) srvs.push_back(prev[k]);
                    for (uint32_t k = 0; k < 4; ++k) srvs.push_back(cur[k]);
                    srvs.push_back(const_cast<Tex*>(add0));
                    g.sets[t] = BuildSet(257, srvs,
                        {g.outs[0], g.outs[1], g.outs[2]}, cb, grp);
                }
            }
            // gamma1: 259 -> 260 -> 261 -> 262 (refined half image)
            {
                Gamma1Ch& g = pass.gamma1[j];
                const Tex* add0 = (j == 0) ? m_black
                    : pass.gamma1[j - 1].image;
                for (uint32_t k = 0; k < 4; ++k)
                {
                    g.temp0[k] = CreateTex(kFmtRgba8, quarter, "gamma1 temp0");
                    g.temp1[k] = CreateTex(kFmtRgba8, quarter, "gamma1 temp1");
                    if (g.temp0[k] == nullptr || g.temp1[k] == nullptr)
                        return false;
                }
                g.image = CreateTex(kFmtRgba16F, quarter, "gamma1 image");
                if (g.image == nullptr)
                    return false;
                g.sets[0] = BuildSet(259,
                    {pass.gamma0[j].outs[0], pass.gamma0[j].outs[1],
                     pass.gamma0[j].outs[2]},
                    {g.temp0[0], g.temp0[1], g.temp0[2], g.temp0[3]}, cb, grp);
                g.sets[1] = BuildSet(260,
                    {g.temp0[0], g.temp0[1], g.temp0[2], g.temp0[3]},
                    {g.temp1[0], g.temp1[1], g.temp1[2], g.temp1[3]}, cb, grp);
                g.sets[2] = BuildSet(261,
                    {g.temp1[0], g.temp1[1], g.temp1[2], g.temp1[3]},
                    {g.temp0[0], g.temp0[1], g.temp0[2], g.temp0[3]}, cb, grp);
                g.sets[3] = BuildSet(262,
                    {g.temp0[0], g.temp0[1], g.temp0[2], g.temp0[3],
                     const_cast<Tex*>(add0), const_cast<Tex*>(add1)},
                    {g.image}, cb, grp);
            }

            // delta, only for the low levels (j >= 4)
            if (j >= 4)
            {
                const uint32_t k = j - 4;
                const Tex* dAdd0 = (j == 4) ? m_black
                    : pass.delta1[k - 1].image0;
                const Tex* dAdd1 = pass.gamma1[j - 1].image;
                // 266 refine needs the beta1 motion mip at 6 - j, same as
                // gamma1 262 (lsfg-vk v2 wiring), not the refined gamma image
                const Tex* dBeta = m_beta1.outs[6 - j];
                const Tex* dAdd2 = (j == 4) ? m_black
                    : pass.delta1[k - 1].image1;

                // delta0: 257 (prev + cur + add0) and 258 (+add1)
                {
                    Delta0Ch& d = pass.delta0[k];
                    d.sets0.resize(T);
                    d.sets1.resize(T);
                    d.outs0[0] = CreateTex(kFmtRgba8, quarter, "delta0 out0");
                    d.outs0[1] = CreateTex(kFmtRgba8, quarter, "delta0 out0");
                    d.outs0[2] = CreateTex(kFmtRgba8, quarter, "delta0 out0");
                    d.outs1[0] = CreateTex(kFmtRgba8, quarter, "delta0 out1");
                    d.outs1[1] = CreateTex(kFmtRgba8, quarter, "delta0 out1");
                    if (d.outs0[0] == nullptr || d.outs0[1] == nullptr ||
                        d.outs0[2] == nullptr || d.outs1[0] == nullptr ||
                        d.outs1[1] == nullptr)
                        return false;
                    for (uint32_t t = 0; t < T; ++t)
                    {
                        const std::array<Tex*, 4>& prev =
                            m_alpha1[y].outs[(t + T - 1) % T];
                        const std::array<Tex*, 4>& cur = m_alpha1[y].outs[t % T];
                        std::vector<Tex*> srvs0 = {prev[0], prev[1], prev[2],
                            prev[3], cur[0], cur[1], cur[2], cur[3],
                            const_cast<Tex*>(dAdd0)};
                        d.sets0[t] = BuildSet(257, srvs0,
                            {d.outs0[0], d.outs0[1], d.outs0[2]}, cb, grp);
                        std::vector<Tex*> srvs1 = {prev[0], prev[1], prev[2],
                            prev[3], cur[0], cur[1], cur[2], cur[3],
                            const_cast<Tex*>(dAdd1), const_cast<Tex*>(dAdd0)};
                        d.sets1[t] = BuildSet(258, srvs1,
                            {d.outs1[0], d.outs1[1]}, cb, grp);
                    }
                }

                // delta1: 263,264,265,266 + 271,272,273,274
                {
                    Delta1Ch& d = pass.delta1[k];
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        d.temp0[c] = CreateTex(kFmtRgba8, quarter, "delta1 temp0");
                        d.temp1[c] = CreateTex(kFmtRgba8, quarter, "delta1 temp1");
                        if (d.temp0[c] == nullptr || d.temp1[c] == nullptr)
                            return false;
                    }
                    d.image0 = CreateTex(kFmtRgba16F, quarter, "delta1 image0");
                    d.image1 = CreateTex(kFmtRgba16F, quarter, "delta1 image1");
                    if (d.image0 == nullptr || d.image1 == nullptr)
                        return false;
                    const std::vector<Tex*> t0a = {d.temp0[0], d.temp0[1],
                        d.temp0[2], d.temp0[3]};
                    const std::vector<Tex*> t1a = {d.temp1[0], d.temp1[1],
                        d.temp1[2], d.temp1[3]};
                    const std::vector<Tex*> t0s = {d.temp0[0], d.temp0[1],
                        d.temp0[2]};
                    const std::vector<Tex*> t0pair = {d.temp0[0], d.temp0[1]};
                    const std::vector<Tex*> t1pair = {d.temp1[0], d.temp1[1]};
                    d.sets[0] = BuildSet(263,
                        {pass.delta0[k].outs0[0], pass.delta0[k].outs0[1],
                         pass.delta0[k].outs0[2]}, t0a, cb, grp);
                    d.sets[1] = BuildSet(264, t0a, t1a, cb, grp);
                    d.sets[2] = BuildSet(265, t1a, t0a, cb, grp);
                    d.sets[3] = BuildSet(266,
                        {d.temp0[0], d.temp0[1], d.temp0[2], d.temp0[3],
                         const_cast<Tex*>(dAdd0), const_cast<Tex*>(dBeta)},
                        {d.image0}, cb, grp);
                    d.sets[4] = BuildSet(271,
                        {pass.delta0[k].outs1[0], pass.delta0[k].outs1[1]},
                        t0pair, cb, grp);
                    d.sets[5] = BuildSet(272, t0pair, t1pair, cb, grp);
                    d.sets[6] = BuildSet(273, t1pair, t0pair, cb, grp);
                    d.sets[7] = BuildSet(274,
                        {d.temp0[0], d.temp0[1], const_cast<Tex*>(dAdd2)},
                        {d.image1}, cb, grp);
                }
            }
        }

        // generate: 256. The pair order alternates per parity so the
        // shader always sees s0 = older, s1 = newest (lsfg-vk v2);
        // the UAV slot is rewritten to the OutputInterpolated slice
        // on every evaluate.
        {
            const Extent grp = AddShiftExtent(m_sourceExtent, 15, 4);
            // s0 = older (slot 1-p), s1 = newest (slot p) at parity p,
            // matching the alternating generate binding in lsfg-vk v2
            for (uint32_t p = 0; p < 2; ++p)
            {
                const std::vector<Tex*> srvs = {m_source[1 - p],
                    m_source[p], pass.gamma1[6].image,
                    pass.delta1[2].image0, pass.delta1[2].image1};
                pass.gen[p].set = BuildSet(256, srvs, {m_black}, cb, grp);
                pass.gen[p].uavSlot = pass.gen[p].set.heapStart +
                    pass.gen[p].set.srvCount;
            }
        }
    }

    CommitHeap();
    if (m_heap == nullptr)
        return false;
    MFG_LOG_INFO("lsfg: pipeline built (flow 1/%u, %u generated frames)",
        flowScale, genCount);
    return true;
}
// tools-only: re-record just the prepass mip dispatch (fidx parity).
// Debug instrumentation: the mips are first cleared to a sentinel so
// the readback shows whether shader 255 actually writes these slots
// (sentinel gone = the dispatch writes; sentinel kept = it does not).
void Pipeline::RecordPrepassOnly(ID3D12GraphicsCommandList* cmd, uint64_t fidx, bool dispatch)
{
    if (m_heap == nullptr)
        return;
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    const Set& ms = ((fidx & 1u) != 0u) ? m_mipOdd : m_mipEven;
    static ID3D12DescriptorHeap* dbgCpu = nullptr;
    if (dbgCpu == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 8;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&dbgCpu))))
            dbgCpu = nullptr;
    }
    if (dbgCpu != nullptr)
    {
        const UINT sentinel[4] = {200, 200, 200, 200};
        for (int lvl = 0; lvl < 7; ++lvl)
        {
            const Tex& t = *m_mips[static_cast<size_t>(lvl)];
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = t.fmt;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu{
                dbgCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                static_cast<SIZE_T>(m_heapIncrement) * lvl};
            m_device->CreateUnorderedAccessView(t.res.Get(), nullptr, &ud, cpu);
            cmd->ClearUnorderedAccessViewUint(
                D3D12_GPU_DESCRIPTOR_HANDLE{
                    m_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                    static_cast<SIZE_T>(m_heapIncrement) *
                        (ms.heapStart + ms.srvCount +
                         static_cast<uint32_t>(lvl))},
                cpu, t.res.Get(), sentinel, 0, nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &b);
    }
    if (dispatch)
        DispatchSet(cmd, ms);
}
void Pipeline::RecordGraph(ID3D12GraphicsCommandList* cmd, uint64_t fidx,
        ID3D12Resource* interp, uint32_t interpSlices)
{
    if (m_heap == nullptr || interp == nullptr)
        return;

    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    const uint32_t parity = static_cast<uint32_t>(fidx & 1);

    // --- prepass on the flow pyramid ---
    // prepass always mips the NEWEST frame (slot fidx&1), as in lsfg-vk v2
    DispatchSet(cmd, (parity == 0) ? m_mipEven : m_mipOdd);
    for (int lvl = 6; lvl >= 0; --lvl)
    {
        const AlphaCh& a = m_alpha[lvl];
        DispatchSet(cmd, a.set0);
        DispatchSet(cmd, a.set1);
        DispatchSet(cmd, a.set2);
        const Alpha1Ch& a1 = m_alpha1[lvl];
        DispatchSet(cmd, a1.sets[static_cast<uint32_t>(fidx) % a1.temporal]);
    }
    DispatchSet(cmd, m_beta0.sets[static_cast<uint32_t>(fidx) % 3]);
    for (uint32_t k = 0; k < 4; ++k)
        DispatchSet(cmd, m_beta1.sets[k]);

    // --- one command sequence per generated frame ---
    DXGI_FORMAT interpFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t slices = interpSlices ? interpSlices : 1;
    {
        const D3D12_RESOURCE_DESC rd = interp->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            interpFmt = rd.Format;
            if (rd.DepthOrArraySize > 0 &&
                slices > static_cast<uint32_t>(rd.DepthOrArraySize))
                slices = static_cast<uint32_t>(rd.DepthOrArraySize);
        }
    }

    for (uint32_t i = 0; i < m_genCount; ++i)
    {
        const PassCh& pass = m_passes[i];
        for (uint32_t j = 0; j < kMipCount; ++j)
        {
            const uint32_t y = 6 - j;
            const uint32_t T = TemporalFor(y);
            const uint32_t phase = static_cast<uint32_t>(fidx) % T;
            DispatchSet(cmd, pass.gamma0[j].sets[phase]);
            for (uint32_t k = 0; k < 4; ++k)
                DispatchSet(cmd, pass.gamma1[j].sets[k]);
            if (j < 4)
                continue;
            const uint32_t k = j - 4;
            DispatchSet(cmd, pass.delta0[k].sets0[phase]);
            DispatchSet(cmd, pass.delta0[k].sets1[phase]);
            for (uint32_t s = 0; s < 8; ++s)
                DispatchSet(cmd, pass.delta1[k].sets[s]);
        }
        // generate: UAV points at OutputInterpolated slice i
        const PassCh::GenSet& g = pass.gen[parity];
        WriteExternalUav(g.uavSlot, interp, interpFmt, i % slices);
        DispatchSet(cmd, g.set);
    }
}
// tools-only: transient texture access for offline inspection
ID3D12Resource* Pipeline::DebugResource(uint32_t which, Extent* ext,
        DXGI_FORMAT* fmt) const
{
    const Tex* t = nullptr;
    switch (which)
    {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6:
        t = m_mips[which];
        break;
    case 13:
        t = m_beta0.outs[0];
        break;
    case 14:
        t = m_source[0];
        break;
    case 15:
        t = m_source[1];
        break;
    case 10: case 11: case 12: case 30: case 31: case 32: case 33:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 50: case 51: case 52: case 53: case 54:
    case 60: case 61: case 62: case 63: case 64: case 65: case 66:
    {
        if (m_passes.empty())
            return nullptr;
        const PassCh& p = m_passes.back();
        if (which == 10)
            t = p.gamma1[6].image;
        else if (which == 11)
            t = p.delta1[2].image0;
        else if (which == 12)
            t = p.delta1[2].image1;
        else if (which == 30)
            t = p.gamma1[5].image;
        else if (which == 31)
            t = p.gamma1[4].image;
        else if (which == 32)
            t = p.gamma1[0].image;
        else if (which == 33)
            t = p.gamma1[6].temp0[3];
        else if (which == 40)
            t = p.delta1[1].image0;
        else if (which == 41)
            t = p.delta1[1].image1;
        else if (which == 42)
            t = p.delta1[0].image0;
        else if (which == 43)
            t = p.delta1[0].image1;
        else if (which == 40 + 4)
            t = p.delta1[2].temp0[0];
        else if (which == 40 + 5)
            t = p.delta1[2].temp1[0];
        else if (which == 50)
            t = p.gamma0[6].outs[0];
        else if (which == 51)
            t = p.gamma0[6].outs[1];
        else if (which == 52)
            t = p.gamma0[6].outs[2];
        else if (which == 53)
            t = p.gamma0[5].outs[0];
        else if (which == 54)
            t = p.gamma0[0].outs[0];
        else if (which == 60)
            t = p.delta0[2].outs0[0];
        else if (which == 61)
            t = p.delta0[2].outs0[1];
        else if (which == 62)
            t = p.delta0[2].outs0[2];
        else if (which == 63)
            t = p.delta0[2].outs1[0];
        else
            t = p.delta0[2].outs1[1];
        break;
    }
    default:
        return nullptr;
    }
    if (t == nullptr)
        return nullptr;
    if (ext != nullptr)
        *ext = t->ext;
    if (fmt != nullptr)
        *fmt = t->fmt;
    return t->res.Get();
}

} // namespace mfg::lsfg
