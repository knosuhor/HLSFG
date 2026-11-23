// SPDX-License-Identifier: GPL-3.0-or-later
//
// MFG Provider is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE for details.
//
// This graph is a native D3D12 port of the lsfg-vk v2.0.0-dev execution
// graph (https://github.com/PancakeTAS/lsfg-vk, tag v2.0.0-dev,
// GPL-3.0-or-later, (c) PancakeTAS et al.) with attribution, driven by the
// DXBC shaders shipped inside Lossless Scaling 3.2.1's Lossless.dll
// (RCDATA ids 255-279). Quality pipeline only (m = 2, fp32 intermediates).
#pragma once

#include "shader_bank.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mfg::lsfg
{
namespace ml = Microsoft::WRL;

struct Extent
{
    uint32_t w = 0;
    uint32_t h = 0;
};

inline Extent ShiftExtent(Extent e, uint32_t i)
{
    return {e.w >> i, e.h >> i};
}
inline Extent AddShiftExtent(Extent e, uint32_t a, uint32_t i)
{
    return {(e.w + a) >> i, (e.h + a) >> i};
}
inline Extent DivideExtent(Extent e, uint32_t d)
{
    return {e.w / d, e.h / d};
}

// Constant buffer carried by every cb-bearing shader (lsfg-vk layout,
// confirmed against the LS 3.2.1 probe report: 48-byte cbs, only
// resolutionInvScale / timestamp / uiThreshold ever set).
struct ConstantBuffer
{
    uint32_t inputOffset[2];   // 0
    uint32_t firstIter;         // 8
    uint32_t firstIterS;        // 12
    uint32_t advancedColorKind; // 16
    uint32_t hdrSupport;        // 20
    float resolutionInvScale;   // 24
    float timestamp;            // 28
    float uiThreshold;          // 32
    uint32_t pad[3];            // 36
};
static_assert(sizeof(ConstantBuffer) == 48);

class Pipeline
{
  public:
    Pipeline() = default;
    ~Pipeline();

    // Builds the whole graph for one output group: textures, descriptors,
    // baked sets, constant slots. flowScale is the LS "flow scale"
    // divisor (2 = quarter-res optical flow). genCount must match the
    // slice count of the external OutputInterpolated resource.
    bool Init(ID3D12Device* device, ShaderBank& bank, Extent sourceExtent,
              uint32_t flowScale, uint32_t genCount, DXGI_FORMAT sourceFormat);

    // Records UAV clears for every transient image. Call once, on the
    // first burst evaluate, before RecordGraph().
    void RecordClears(ID3D12GraphicsCommandList* cmd);
    // tools-only: re-record just the prepass mip dispatch (fidx parity).
    void RecordPrepassOnly(ID3D12GraphicsCommandList* cmd, uint64_t fidx,
        bool dispatch = true);

    // Burst evaluation for real-frame index fidx (0-based, one per group).
    // The pair being interpolated is (F_{fidx-1}, F_fidx): the backend
    // must copy the incoming HUDLess frame into Source(fidx & 1) (and
    // leave both source slots in UAV state) before calling this. interp
    // is the external OutputInterpolated (UAV state), one slice per
    // generated frame.
    void RecordGraph(ID3D12GraphicsCommandList* cmd, uint64_t fidx,
                      ID3D12Resource* interp, uint32_t interpSlices);

    // ring slot holding the real frame of the given parity (UAV state);
    // the backend copies each HUDLess frame into Source(fidx & 1).
    ID3D12Resource* Source(uint32_t parity) const
    {
        return m_source[parity & 1] != nullptr ? m_source[parity & 1]->res.Get() : nullptr;
    }

    uint32_t GenCount() const { return m_genCount; }
    Extent SourceExtent() const { return m_sourceExtent; }

    // GPU address of the constant slot (pass i, or prepass = genCount).
    D3D12_GPU_VIRTUAL_ADDRESS CbGpu(uint32_t slot) const;

    // tools-only: transient texture access for offline inspection
    // (which: 0..6 = flow mips, 10 = gamma1[6], 11 = delta1[2].image0,
    // 12 = delta1[2].image1, 13 = beta0.outs[0])
    ID3D12Resource* DebugResource(uint32_t which, Extent* ext,
                                  DXGI_FORMAT* fmt) const;


  private:
    struct Tex
    {
        ml::ComPtr<ID3D12Resource> res;
        Extent ext{};
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    };

    // One baked descriptor set: shader id + heap slots + dispatch extent.
    // SRV range starts at heapStart; UAV range follows SRVs.
    struct Set
    {
        uint32_t id = 0;
        uint32_t heapStart = 0;
        uint32_t srvCount = 0;
        int cbSlot = -1;      // -1: shader takes no cbuffer
        uint32_t grpW = 0;
        uint32_t grpH = 0;
    };

    // alpha combine at one mip level
    struct AlphaCh
    {
        Tex* temp0[2] = {};   // RGBA8, half flow extent
        Tex* temp1[2] = {};
        Tex* outs[4] = {};    // RGBA8, quarter flow extent
        Set set0;            // 267
        Set set1;            // 268
        Set set2;            // 269
    };
    // temporal alpha combine at one mip level
    struct Alpha1Ch
    {
        uint32_t temporal = 2;         // 3 at level 0, else 2
        std::vector<std::array<Tex*, 4>> outs; // temporal x 4 images
        std::vector<Set> sets;         // one 270 set per temporal phase
    };
    struct Beta0Ch
    {
        Tex* outs[2] = {};
        std::array<Set, 3> sets;       // one 275 set per rotation
    };
    struct Beta1Ch
    {
        Tex* temp0[2] = {};
        Tex* temp1[2] = {};
        Tex* outs[6] = {};    // R8_UNORM shifted mips
        std::array<Set, 4> sets;       // 276, 277, 278, 279
    };
    struct Gamma0Ch
    {
        Tex* outs[3] = {};
        std::vector<Set> sets;         // one 257 set per temporal phase
    };
    struct Gamma1Ch
    {
        Tex* temp0[4] = {};
        Tex* temp1[4] = {};
        Tex* image = nullptr;           // RGBA16F refined half image
        std::array<Set, 4> sets;       // 259, 260, 261, 262
    };
    struct Delta0Ch
    {
        Tex* outs0[3] = {};
        Tex* outs1[2] = {};
        std::vector<Set> sets0;        // one 257 set per temporal phase
        std::vector<Set> sets1;        // one 258 set per temporal phase
    };
    struct Delta1Ch
    {
        Tex* temp0[4] = {};
        Tex* temp1[4] = {};
        Tex* image0 = nullptr;          // RGBA16F
        Tex* image1 = nullptr;          // RGBA16F
        std::array<Set, 8> sets;       // 263,264,265,266,271,272,273,274
    };
    struct PassCh
    {
        std::array<Gamma0Ch, 7> gamma0;
        std::array<Gamma1Ch, 7> gamma1;
        std::array<Delta0Ch, 3> delta0;   // levels 4..6 (j-4)
        std::array<Delta1Ch, 3> delta1;
        // generate: UAV slot rewritten per evaluate (OutputInterpolated)
        struct GenSet { Set set; uint32_t uavSlot = 0; };
        std::array<GenSet, 2> gen;     // fidx parity (identical bindings)
    };

    // construction helpers
    Tex* CreateTex(DXGI_FORMAT fmt, Extent ext, const char* what);
    uint32_t AllocSlots(uint32_t count, const char* what);
    void WriteSrv(uint32_t slot, const Tex& tex);
    void WriteUav(uint32_t slot, const Tex& tex);
    void WriteExternalUav(uint32_t slot, ID3D12Resource* res,
                          DXGI_FORMAT fmt, uint32_t arraySlice);
    Set BuildSet(uint32_t id, const std::vector<Tex*>& srvs,
                 const std::vector<Tex*>& uavs, int cbSlot, Extent grp);
    void CommitHeap();                // create heap + flush pending writes

    // records one compute dispatch with proper UAV barrier and root setup
    void DispatchSet(ID3D12GraphicsCommandList* cmd, const Set& s);

    // deferred descriptor writes collected while BuildSet() runs (the
    // heap does not exist yet); flushed by CommitHeap().
    struct PendingWrite { uint32_t slot = 0; Tex* tex = nullptr; bool uav = false; };

    ID3D12Device* m_device = nullptr;
    ShaderBank* m_bank = nullptr;
    Extent m_sourceExtent{};
    Extent m_flowExtent{};
    uint32_t m_flowScale = 2;
    uint32_t m_genCount = 1;

    ml::ComPtr<ID3D12DescriptorHeap> m_heap;    // SRV + UAV (shader-visible)
    ml::ComPtr<ID3D12DescriptorHeap> m_cpuHeap;  // non-shader-visible mirror (clears)
    ml::ComPtr<ID3D12Resource> m_cbUpload;       // (gen+1) x 256 B
    ConstantBuffer* m_cbMapped = nullptr;
    uint32_t m_heapIncrement = 0;
    uint32_t m_heapCursor = 0;      // next free slot

    Tex* m_black = nullptr;                // 4x4 neutral image
    std::array<Tex*, 7> m_mips{};           // R8_UNORM flow pyramid
    std::array<Tex*, 2> m_source{};        // HD ring pair (RGBA8, full extent)
    Set m_mipEven;                  // id 255 sampling source slot 0
    Set m_mipOdd;                   // id 255 sampling source slot 1
    std::array<AlphaCh, 7> m_alpha;
    std::array<Alpha1Ch, 7> m_alpha1;
    Beta0Ch m_beta0;
    Beta1Ch m_beta1;
    std::vector<PassCh> m_passes;

    std::vector<std::unique_ptr<Tex>> m_pool; // every transient image
    std::vector<PendingWrite> m_pending;
    std::vector<uint32_t> m_clearSlots;       // UAV slot per pool texture

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};
} // namespace mfg::lsfg
