// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline pixel-accuracy harness for the phase-1 LSFG pipeline.
// Loads the Lossless shader bank, builds the full multi-frame graph on a
// fresh D3D12 device with the debug layer enabled, then runs pixel tests:
//   1. static pair (A,A): output must equal A and must be identical across
//      consecutive evaluations (oscillation probe for the runtime jitter
//      on static elements), reported for the full frame and the center;
//   2. translated pair (A, A+8px): output should be A shifted by 4px,
//      with residual sampling offsets reported on a 5x5 grid;
//   3. static again after motion (temporal recovery);
//   plus BestShift: exact integer displacement search against the B-channel
//   hash noise (mean abs diff collapses to ~0 only at the true shift).
//
// usage: lsfg_pipeline_test <Lossless.dll> [w h flow gen srcFmt]

#include "../src/lsfg/dll_extract.h"
#include "../src/lsfg/pipeline.h"
#include "../src/lsfg/shader_bank.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void DumpMessages(ID3D12InfoQueue* iq, const char* when)
{
    const UINT64 n = iq->GetNumStoredMessages();
    std::printf("--- %s: %llu stored messages ---\n", when, (unsigned long long) n);
    const UINT64 limit = n < 60 ? n : 60;
    for (UINT64 i = 0; i < limit; ++i)
    {
        SIZE_T len = 0;
        if (FAILED(iq->GetMessage(i, nullptr, &len)) || len == 0)
            continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(iq->GetMessage(i, msg, &len)))
            std::printf("[%llu] %.*s\n", (unsigned long long) i,
                (int) msg->DescriptionByteLength, msg->pDescription);
    }
}

D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

// minimal execute-and-wait context; allocations are intentionally leaked
// (short-lived diagnostic tool)
struct Gpu
{
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 val = 0;

    bool Exec(ID3D12GraphicsCommandList* list)
    {
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ++val;
        fence->SetEventOnCompletion(val, ev);
        queue->Signal(fence, val);
        return WaitForSingleObject(ev, 30000) == WAIT_OBJECT_0;
    }

    ID3D12GraphicsCommandList* NewList()
    {
        static ID3D12CommandAllocator* allocs[1024];
        static int n = 0;
        if (n >= 1024)
            return nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&alloc))))
            return nullptr;
        allocs[n++] = alloc;
        ID3D12GraphicsCommandList* list = nullptr;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                alloc, nullptr, IID_PPV_ARGS(&list))))
            return nullptr;
        return list;
    }
};

UINT RowPitch(uint32_t w)
{
    return (UINT) (((size_t) w * 4 + 255) & ~(size_t) 255);
}

// A(x,y): R = x ramp, G = y ramp (position-carrying channels used by
// SampleOffsets to reconstruct residual sampling displacement), B = hash
// noise (breaks the aperture ambiguity for the flow estimator).
std::vector<uint8_t> MakeRampPattern(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> p((size_t) w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* px = &p[((size_t) y * w + x) * 4];
            px[0] = (uint8_t) (x * 255u / (w > 1 ? w - 1 : 1));
            px[1] = (uint8_t) (y * 255u / (h > 1 ? h - 1 : 1));
            px[2] = (uint8_t) (((x * 73856093u) ^ (y * 19349663u)
                ^ (x * y * 83492791u)) >> 24);
            px[3] = 255;
        }
    return p;
}

// B(x,y) = A(x - dx, y): content shifted right by dx pixels.
std::vector<uint8_t> ShiftPatternX(const std::vector<uint8_t>& in,
    uint32_t w, uint32_t h, uint32_t dx)
{
    std::vector<uint8_t> q(in.size());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            std::memcpy(&q[((size_t) y * w + x) * 4],
                &in[((size_t) y * w + (x + w - (dx % w)) % w) * 4], 4);
    return q;
}

// Upload buffers must outlive command-list execution.
ID3D12Resource* g_keep[32];
int g_keepN = 0;

// Creates a DEFAULT-heap texture from CPU pixels; records the upload onto
// the given list, leaves the texture in COPY_SOURCE state.
ID3D12Resource* CreateTexFromPixels(ID3D12Device* dev,
    ID3D12GraphicsCommandList* list, const std::vector<uint8_t>& px,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    const UINT pitch = RowPitch(w);
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64) pitch * h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upBuf = nullptr;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf))))
        return nullptr;
    void* mapped = nullptr;
    if (FAILED(upBuf->Map(0, nullptr, &mapped)))
        return nullptr;
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy((uint8_t*) mapped + (size_t) y * pitch,
            &px[(size_t) y * w * 4], (size_t) w * 4);
    upBuf->Unmap(0, nullptr);
    if (g_keepN < 32)
        g_keep[g_keepN++] = upBuf;

    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    ID3D12Resource* tex = nullptr;
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
        return nullptr;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upBuf;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = fmt;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = Trans(tex,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    return tex;
}

// Copies (older, newer) into the pipeline source ring for evaluate fidx,
// matching the backend convention: newer frame goes to slot fidx & 1.
void FillRingPair(ID3D12GraphicsCommandList* list, mfg::lsfg::Pipeline* pipe,
    ID3D12Resource* older, ID3D12Resource* newer, uint64_t fidx)
{
    ID3D12Resource* slot[2] = {pipe->Source(0), pipe->Source(1)};
    const uint32_t oldIdx = static_cast<uint32_t>(fidx + 1) & 1u;
    const uint32_t newIdx = static_cast<uint32_t>(fidx) & 1u;
    const D3D12_RESOURCE_BARRIER pre[] = {
        Trans(slot[oldIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST),
        Trans(slot[newIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(2, pre);
    list->CopyResource(slot[oldIdx], older);
    list->CopyResource(slot[newIdx], newer);
    const D3D12_RESOURCE_BARRIER post[] = {
        Trans(slot[oldIdx], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Trans(slot[newIdx], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, post);
}

std::vector<uint8_t> ReadSlice(Gpu& gpu, ID3D12Resource* tex, uint32_t slice,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    std::vector<uint8_t> out;
    const UINT pitch = RowPitch(w);
    const size_t bufSize = (size_t) pitch * h;
    D3D12_HEAP_PROPERTIES rp{};
    rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bufSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(gpu.dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
        return out;
    ID3D12GraphicsCommandList* list = gpu.NewList();
    if (list == nullptr)
    {
        rb->Release();
        return out;
    }
    D3D12_RESOURCE_BARRIER b0 = Trans(tex,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    b0.Transition.Subresource = slice;
    list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = slice;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b1 = Trans(tex,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b1.Transition.Subresource = slice;
    list->ResourceBarrier(1, &b1);
    if (!gpu.Exec(list))
    {
        list->Release();
        rb->Release();
        return out;
    }
    list->Release();
    void* m = nullptr;
    const D3D12_RANGE rng{0, bufSize};
    if (SUCCEEDED(rb->Map(0, &rng, &m)))
    {
        out.resize((size_t) w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&out[(size_t) y * w * 4],
                (uint8_t*) m + (size_t) y * pitch, (size_t) w * 4);
        rb->Unmap(0, nullptr);
    }
    rb->Release();
    return out;
}

std::vector<uint8_t> ReadRaw(Gpu& gpu, ID3D12Resource* tex, uint32_t w,
    uint32_t h, DXGI_FORMAT fmt, uint32_t bpp);

// upload a known R8 pattern and dump the prepass pyramid: decides
// whether typed UAV stores to R8 actually land.
ID3D12Resource* UploadR8(Gpu& gpu, uint32_t w, uint32_t h,
    const std::vector<uint8_t>& pix)
{
    const UINT pitch = (UINT) (((size_t) w + 255) & ~(size_t) 255);
    const size_t bufSize = (size_t) pitch * h;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bufSize; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr;
    if (FAILED(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up))))
        return nullptr;
    void* m = nullptr;
    if (FAILED(up->Map(0, nullptr, &m)))
    {
        up->Release();
        return nullptr;
    }
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy((uint8_t*) m + (size_t) y * pitch,
            &pix[(size_t) y * w], w);
    up->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES dp{};
    dp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Alignment = 0;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* tex = nullptr;
    if (FAILED(gpu.dev->CreateCommittedResource(&dp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
    {
        up->Release();
        return nullptr;
    }
    ID3D12GraphicsCommandList* list = gpu.NewList();
    if (list != nullptr)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        src.PlacedFootprint.Footprint.Width = w;
        src.PlacedFootprint.Footprint.Height = h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER b = Trans(tex,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &b);
        gpu.Exec(list);
        list->Release();
    }
    up->Release();
    return tex;
}

void PrepassAndMipCheck(Gpu& gpu, mfg::lsfg::Pipeline& pipe)
{
    // 1. validate the R8 readback path with a known pattern
    {
        std::vector<uint8_t> pat((size_t) 64 * 64);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                pat[(size_t) y * 64 + x] =
                    (uint8_t) ((x * 3 + y * 5) & 255);
        ID3D12Resource* t = UploadR8(gpu, 64, 64, pat);
        std::vector<uint8_t> rb;
        if (t != nullptr)
        {
            rb = ReadRaw(gpu, t, 64, 64, DXGI_FORMAT_R8_UNORM, 1);
            t->Release();
        }
        uint32_t bad = 0;
        if (rb.size() == pat.size())
            for (size_t i = 0; i < pat.size(); ++i)
                if (rb[i] != pat[i]) ++bad;
        std::printf("r8 readback validation: %s (%u mismatches of %zu)\n",
            (bad == 0 && rb.size() == pat.size()) ? "PASS" : "FAIL",
            bad, pat.size());
    }
    // 2a. sentinel clear without dispatch: does the clear reach the mips?
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list != nullptr)
        {
            pipe.RecordPrepassOnly(list, 2, false);
            gpu.Exec(list);
            list->Release();
        }
        mfg::lsfg::Extent ext{};
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* res = pipe.DebugResource(0, &ext, &fmt);
        if (res != nullptr)
        {
            const std::vector<uint8_t> raw = ReadRaw(gpu, res, ext.w, ext.h,
                fmt, 4u);
            double mean = 0.0;
            uint32_t mx = 0;
            for (uint8_t v : raw) { mean += v; if ((uint32_t) v > mx) mx = v; }
            if (!raw.empty()) mean /= (double) raw.size();
            std::printf("clearonly mip0: %ux%u mean=%.3f max=%u\n",
                ext.w, ext.h, mean, mx);
        }
    }
    // 2. run only the prepass for fidx=2, then dump the mip pyramid
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list != nullptr)
        {
            pipe.RecordPrepassOnly(list, 2);
            gpu.Exec(list);
            list->Release();
        }
        for (uint32_t lvl = 0; lvl < 16; ++lvl)
        {
            mfg::lsfg::Extent ext{};
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            ID3D12Resource* res = pipe.DebugResource(lvl, &ext, &fmt);
            if (res == nullptr)
                continue;
            const uint32_t bp = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? 4u :
                (fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? 4u : 1u);
            const std::vector<uint8_t> raw = ReadRaw(gpu, res, ext.w, ext.h,
                fmt, bp);
            if (raw.empty())
            {
                std::printf("prepass mip%u: empty\n", lvl);
                continue;
            }
            double mean = 0.0;
            uint32_t mn = 255, mx = 0;
            for (uint8_t v : raw)
            {
                mean += v;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            mean /= (double) raw.size();
            std::printf("prepass mip%u: %ux%u fmt=%u mean=%.3f min=%u max=%u\n",
                lvl, ext.w, ext.h, (unsigned) fmt, mean, mn, mx);
        }
    }
}


// tools-only forensics: read back the generate-stage flow textures so
// the vertical latent can be traced to its origin (net bias vs bug).
float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h & 0x8000u) ? 0x80000000u : 0u;
    const uint32_t e = (h >> 10) & 0x1fu;
    const uint32_t m = h & 0x3ffu;
    if (e == 0)
    {
        float f = std::ldexp((float) m, -24);
        return (s != 0) ? -f : f;
    }
    const uint32_t bits = s |
        ((e == 31) ? 0x7f800000u : ((e + 112) << 23)) | (m << 13);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::vector<uint8_t> ReadRaw(Gpu& gpu, ID3D12Resource* tex, uint32_t w,
    uint32_t h, DXGI_FORMAT fmt, uint32_t bpp)
{
    std::vector<uint8_t> out;
    const UINT pitch = (UINT) (((size_t) w * bpp + 255) & ~(size_t) 255);
    const size_t bufSize = (size_t) pitch * h;
    D3D12_HEAP_PROPERTIES rp{};
    rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bufSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(gpu.dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
        return out;
    ID3D12GraphicsCommandList* list = gpu.NewList();
    if (list == nullptr)
    {
        rb->Release();
        return out;
    }
    D3D12_RESOURCE_BARRIER b0 = Trans(tex,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b1 = Trans(tex,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b1);
    if (!gpu.Exec(list))
    {
        list->Release();
        rb->Release();
        return out;
    }
    list->Release();
    void* m = nullptr;
    const D3D12_RANGE rng{0, bufSize};
    if (SUCCEEDED(rb->Map(0, &rng, &m)))
    {
        out.resize((size_t) w * h * bpp);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&out[(size_t) y * w * bpp],
                (uint8_t*) m + (size_t) y * pitch, (size_t) w * bpp);
        rb->Unmap(0, nullptr);
    }
    rb->Release();
    return out;
}

void DumpFlowStats(Gpu& gpu, mfg::lsfg::Pipeline* pipe)
{
    struct Ent { uint32_t which; const char* name; };
    const Ent ents[] = {
        {10, "gamma1[6] t2"}, {11, "delta1[2].im0 t3"},
        {12, "delta1[2].im1 t4"}, {0, "mips[0]"},
        {44, "delta1[2].t0[0]"}, {45, "delta1[2].t1[0]"},
        {40, "delta1[1].im0"}, {41, "delta1[1].im1"},
        {42, "delta1[0].im0"}, {43, "delta1[0].im1"},
        {30, "gamma1[5].image"}, {31, "gamma1[4].image"},
        {32, "gamma1[0].image"}, {33, "gamma1[6].t0[3]"},
        {50, "gamma0[6].o0"}, {51, "gamma0[6].o1"}, {52, "gamma0[6].o2"},
        {53, "gamma0[5].o0"}, {54, "gamma0[0].o0"},
        {60, "delta0[2].s0[0]"}, {61, "delta0[2].s0[1]"},
        {62, "delta0[2].s0[2]"}, {63, "delta0[2].s1[0]"},
        {64, "delta0[2].s1[1]"},
    };
    for (const Ent& en : ents)
    {
        mfg::lsfg::Extent ext{};
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* res = pipe->DebugResource(en.which, &ext, &fmt);
        if (res == nullptr || ext.w == 0)
        {
            std::printf("flowdump %-18s: null\n", en.name);
            continue;
        }
        const bool is16F = (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
        const bool isR8 = (fmt == DXGI_FORMAT_R8_UNORM);
        const uint32_t bpp = is16F ? 8u : (isR8 ? 1u : 4u);
        const std::vector<uint8_t> raw = ReadRaw(gpu, res, ext.w, ext.h, fmt, bpp);
        if (raw.empty())
        {
            std::printf("flowdump %-18s: empty\n", en.name);
            continue;
        }
        const uint32_t ch = isR8 ? 1u : 4u;
        const size_t n = (size_t) ext.w * ext.h;
        double mean[4] = {0, 0, 0, 0};
        double mn[4] = {1e30, 1e30, 1e30, 1e30};
        double mx[4] = {-1e30, -1e30, -1e30, -1e30};
        for (size_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < ch; ++c)
            {
                const double v = is16F
                    ? (double) HalfToFloat(((const uint16_t*) raw.data())[i * 4 + c])
                    : (double) raw[i * bpp + c];
                mean[c] += v;
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        std::printf("flowdump %-18s %3ux%3u fmt=%3u mean=(%+9.4f,%+9.4f,%+9.4f,%+9.4f)",
            en.name, ext.w, ext.h, (unsigned) fmt,
            mean[0] / (double) n, mean[1] / (double) n,
            mean[2] / (double) n, mean[3] / (double) n);
        std::printf(" min=(%+8.3f,%+8.3f,%+8.3f,%+8.3f)",
            mn[0], mn[1], mn[2], mn[3]);
        std::printf(" max=(%+8.3f,%+8.3f,%+8.3f,%+8.3f)\n",
            mx[0], mx[1], mx[2], mx[3]);
        const uint32_t cx = ext.w / 2, cy = ext.h / 2;
        std::printf("  center 3x3 (x,y,z,w):\n");
        for (int dy = -1; dy <= 1; ++dy)
        {
            std::printf("   ");
            for (int dx = -1; dx <= 1; ++dx)
            {
                const uint32_t x = cx + dx, y = cy + dy;
                const size_t i = (size_t) y * ext.w + x;
                if (is16F)
                {
                    const uint16_t* p = ((const uint16_t*) raw.data()) + i * 4;
                    std::printf(" (%+7.3f,%+7.3f,%+7.3f,%+7.3f)",
                        HalfToFloat(p[0]), HalfToFloat(p[1]),
                        HalfToFloat(p[2]), HalfToFloat(p[3]));
                }
                else
                {
                    const uint8_t* p = raw.data() + i * bpp;
                    std::printf(" (%3u,%3u,%3u,%3u)",
                        p[0], ch > 1 ? p[1] : 0u, ch > 2 ? p[2] : 0u,
                        ch > 3 ? p[3] : 0u);
                }
            }
            std::printf("\n");
        }
    }
}


// max per-pixel channel delta over a region
void DiffRgn(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
    uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
    const char* label)
{
    uint64_t sum = 0, cnt = 0;
    uint32_t max = 0, mx = 0, my = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const uint8_t* pa = &a[((size_t) y * w + x) * 4];
            const uint8_t* pb = &b[((size_t) y * w + x) * 4];
            uint32_t m = 0;
            for (int c = 0; c < 3; ++c)
            {
                const uint32_t d = pa[c] > pb[c] ? pa[c] - pb[c] : pb[c] - pa[c];
                if (d > m)
                    m = d;
            }
            sum += m;
            ++cnt;
            if (m > max)
            {
                max = m;
                mx = x;
                my = y;
            }
        }
    std::printf("  %-32s mean=%9.4f max=%3u at (%u,%u)\n", label,
        cnt ? (double) sum / (double) cnt : 0.0, max, mx, my);
}

void Report(const std::vector<uint8_t>& out, const std::vector<uint8_t>& ref,
    uint32_t w, uint32_t h, const char* name, const char* refName)
{
    char label[96];
    std::snprintf(label, sizeof(label), "%s vs %s [full]", name, refName);
    DiffRgn(out, ref, w, 0, 0, w, h, label);
    std::snprintf(label, sizeof(label), "%s vs %s [center]", name, refName);
    DiffRgn(out, ref, w, w * 3 / 8, h * 3 / 8, w - w * 3 / 8, h - h * 3 / 8,
        label);
}

// Exact integer displacement search over [-12,12]^2 using the B-channel hash
// noise: out(x,y) == ref(x-dx, y-dy) collapses meanB to ~0 only at the true
// shift. A non-zero residual at the best shift means the graph resamples or
// blends rather than copying, and quantifies how far from exact it is.
void BestShift(const std::vector<uint8_t>& ref,
    const std::vector<uint8_t>& out, uint32_t w, uint32_t h,
    const char* label)
{
    const int R = 12;
    const uint32_t inset = 16;
    double bestMean = 1e300;
    int bdx = 999, bdy = 999;
    double zeroMean = 0.0;
    for (int dy = -R; dy <= R; ++dy)
    {
        for (int dx = -R; dx <= R; ++dx)
        {
            uint64_t sum = 0;
            uint64_t cnt = 0;
            for (uint32_t y = inset; y < h - inset; ++y)
            {
                const int sy = (int) y - dy;
                const uint8_t* refRow = &ref[(size_t) sy * w * 4];
                const uint8_t* outRow = &out[(size_t) y * w * 4];
                for (uint32_t x = inset; x < w - inset; ++x)
                {
                    const int sx = (int) x - dx;
                    const uint32_t ra = refRow[(size_t) sx * 4 + 2];
                    const uint32_t ob = outRow[(size_t) x * 4 + 2];
                    sum += ra > ob ? ra - ob : ob - ra;
                    ++cnt;
                }
            }
            const double mean = cnt ? (double) sum / (double) cnt : 0.0;
            if (mean < bestMean)
            {
                bestMean = mean;
                bdx = dx;
                bdy = dy;
            }
            if (dx == 0 && dy == 0)
                zeroMean = mean;
        }
    }
    std::printf("  %-32s best shift=(%+d,%+d) meanB=%8.4f (unshifted meanB=%8.4f)\n",
        label, bdx, bdy, bestMean, zeroMean);
}

// The pattern stores x in R and y in G, so each output pixel reveals where
// it sampled the source; residual (xs-x, ys-y) should be ~0 on a static
// scene and ~(-half-shift) on the translation test.

// Mean subpixel displacement field inferred from the R/G ramps: each
// output pixel R,G reveal where the composition sampled the source, so
// the per-region mean of (recovered position - actual position) is the
// displacement field applied by the composition path.
void RampField(const std::vector<uint8_t>& out, uint32_t w, uint32_t h,
    const char* label)
{
    const uint32_t BW = w / 5, BH = h / 5;
    std::printf("  %s\n", label);
    for (int by = 0; by < 5; ++by)
    {
        std::printf("   ");
        for (int bx = 0; bx < 5; ++bx)
        {
            double sxs = 0.0, sys = 0.0;
            uint32_t cnt = 0;
            const uint32_t x0 = (uint32_t) bx * BW, x1 = (bx + 1) == 5 ? w : x0 + BW;
            const uint32_t y0 = (uint32_t) by * BH, y1 = (by + 1) == 5 ? h : y0 + BH;
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = x0; x < x1; ++x)
                {
                    const uint8_t* p = &out[((size_t) y * w + x) * 4];
                    sxs += (double) p[0] / 255.0 * (double) (w - 1) - (double) x;
                    sys += (double) p[1] / 255.0 * (double) (h - 1) - (double) y;
                    ++cnt;
                }
            std::printf(" (%+7.2f,%+7.2f)", sxs / cnt, sys / cnt);
        }
        std::printf("\n");
    }
}
// Mean (recovered source position - actual position) over the central
// region: the ramp R/G channels encode where the frame sampled the
// source, so this is the net applied displacement (compact form of
// RampField for the sustained-motion round).
void CenterShift(const std::vector<uint8_t>& out, uint32_t w, uint32_t h,
    uint32_t k, uint32_t s)
{
    const uint32_t x0 = w / 4, x1 = w - w / 4, y0 = h / 4, y1 = h - h / 4;
    double sx = 0.0, sy = 0.0;
    uint64_t cnt = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const uint8_t* p = &out[((size_t) y * w + x) * 4];
            sx += (double) p[0] / 255.0 * (double) (w - 1) - (double) x;
            sy += (double) p[1] / 255.0 * (double) (h - 1) - (double) y;
            ++cnt;
        }
    std::printf("motion fidx=%u shift=%u  center=(%+8.3f,%+7.3f)  (expect dx near %.1f..%.1f incl. latent)\n",
        k, s, cnt ? sx / (double) cnt : 0.0, cnt ? sy / (double) cnt : 0.0,
        -((double) s - 4.0) - 1.2, -((double) s - 4.0));
}

void SampleOffsets(const std::vector<uint8_t>& out, uint32_t w, uint32_t h)
{
    for (int gy = 0; gy < 5; ++gy)
    {
        std::printf("   ");
        for (int gx = 0; gx < 5; ++gx)
        {
            const uint32_t x = w * (uint32_t) (gx + 1) / 6;
            const uint32_t y = h * (uint32_t) (gy + 1) / 6;
            const uint8_t* p = &out[((size_t) y * w + x) * 4];
            const int xs = (int) ((double) p[0] / 255.0 * (w - 1) + 0.5);
            const int ys = (int) ((double) p[1] / 255.0 * (h - 1) + 0.5);
            std::printf(" (%+4d,%+4d)", xs - (int) x, ys - (int) y);
        }
        std::printf("\n");
    }
}

} // namespace

// --- replay mode: rerun runtime-captured source pairs ------------------
// Loads srcold_k/srcnew_k raw dumps (produced by the backend MFG_DUMP
// instrumentation), feeds them through the graph one fidx at a time and
// checks whether the offline generated frame reproduces the runtime
// interp_k dump. Agreement means the warp is produced by the graph
// itself from those inputs; disagreement points at the environment
// (buffers, states, timing).
std::vector<uint8_t> LoadRawFile(const fs::path& p)
{
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t) len);
    if (len > 0 && std::fread(d.data(), 1, (size_t) len, f) != (size_t) len)
        d.clear();
    std::fclose(f);
    return d;
}

float RLum(const std::vector<uint8_t>& img, uint32_t w, uint32_t x, uint32_t y)
{
    const uint8_t* p = &img[((size_t) y * w + x) * 4];
    return (float) (((int) p[0] + (int) p[1] + (int) p[2]) / (3.0 * 255.0));
}

double RProfileShift(const std::vector<double>& ref,
    const std::vector<double>& cur, int r)
{
    const int n = (int) ref.size();
    if ((int) cur.size() != n || n < 2 * r + 2)
        return 0.0;
    double best = 1e300;
    int bestI = 0;
    for (int d = -r; d <= r; ++d)
    {
        double s = 0.0;
        for (int x = r; x < n - r; ++x)
        {
            const double a = ref[(size_t) x] - cur[(size_t) (x + d)];
            s += a * a;
        }
        if (s < best) { best = s; bestI = d; }
    }
    double e0 = 0.0;
    for (int x = r; x < n - r; ++x)
    {
        const double a = ref[(size_t) x] - cur[(size_t) (x + bestI)];
        e0 += a * a;
    }
    double sub = 0.0;
    if (bestI > -r && bestI < r)
    {
        double em = 0.0, ep = 0.0;
        for (int x = r; x < n - r; ++x)
        {
            const double a = ref[(size_t) x];
            em += (a - cur[(size_t) (x + bestI - 1)]) *
                  (a - cur[(size_t) (x + bestI - 1)]);
            ep += (a - cur[(size_t) (x + bestI + 1)]) *
                  (a - cur[(size_t) (x + bestI + 1)]);
        }
        const double den = em - 2.0 * e0 + ep;
        if (den != 0.0)
            sub = 0.5 * (em - ep) / den;
    }
    return (double) bestI + sub;
}

double RBandDx(const std::vector<uint8_t>& ref,
    const std::vector<uint8_t>& cur, uint32_t w, uint32_t y0, uint32_t y1)
{
    std::vector<double> pr((size_t) w, 0.0);
    std::vector<double> pc((size_t) w, 0.0);
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            pr[(size_t) x] += RLum(ref, w, x, y);
            pc[(size_t) x] += RLum(cur, w, x, y);
        }
    const double norm = 1.0 / (double) (y1 - y0);
    for (uint32_t x = 0; x < w; ++x)
    {
        pr[(size_t) x] *= norm;
        pc[(size_t) x] *= norm;
    }
    return RProfileShift(pr, pc, 12);
}

int ReplayMain(mfg::lsfg::Pipeline& pipe, Gpu& gpu, ID3D12Resource* interp,
    uint32_t W, uint32_t H, DXGI_FORMAT srcFmt, const fs::path& dir,
    uint32_t first, uint32_t last)
{
    std::printf("replay %s k=%u..%u\n", dir.string().c_str(), first, last);
    const uint32_t M = 12;
    const uint32_t bh = (H > 2 * M) ? (H - 2 * M) / 3 : H / 3;
    double sumDiff = 0.0;
    uint64_t nDiff = 0;
    uint32_t done = 0;
    for (uint32_t k = first; k <= last; ++k)
    {
        const auto srcOld = LoadRawFile(dir /
            ("srcold_" + std::to_string(k) + ".raw"));
        const auto srcNew = LoadRawFile(dir /
            ("srcnew_" + std::to_string(k) + ".raw"));
        const auto rtInterp = LoadRawFile(dir /
            ("interp_" + std::to_string(k) + ".raw"));
        const size_t need = (size_t) W * H * 4;
        if (srcOld.size() != need || srcNew.size() != need ||
            rtInterp.size() != need)
            continue;
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        ID3D12Resource* texO = CreateTexFromPixels(gpu.dev, list, srcOld,
            W, H, srcFmt);
        ID3D12Resource* texN = CreateTexFromPixels(gpu.dev, list, srcNew,
            W, H, srcFmt);
        FillRingPair(list, &pipe, texO, texN, k);
        pipe.RecordGraph(list, k, interp, 1);
        if (!gpu.Exec(list))
        {
            list->Release();
            texO->Release();
            texN->Release();
            return 1;
        }
        list->Release();
        texO->Release();
        texN->Release();
        const auto offInterp = ReadSlice(gpu, interp, 0, W, H, srcFmt);
        double bandOff[3] = {0, 0, 0}, bandRt[3] = {0, 0, 0};
        for (int b = 0; b < 3; ++b)
        {
            const uint32_t y0 = M + (uint32_t) b * bh;
            const uint32_t y1 = y0 + bh;
            bandOff[b] = RBandDx(srcOld, offInterp, W, y0, y1);
            bandRt[b] = RBandDx(srcOld, rtInterp, W, y0, y1);
        }
        double dsum = 0.0;
        uint64_t dn = 0;
        for (size_t i = 0; i + 3 < offInterp.size() &&
             i + 3 < rtInterp.size(); i += 4)
        {
            const int dd = (int) offInterp[i] - (int) rtInterp[i];
            dsum += dd < 0 ? -dd : dd;
            ++dn;
        }
        sumDiff += dsum;
        nDiff += dn;
        ++done;
        std::printf("k=%3u o: %7.2f %7.2f %7.2f | r: %7.2f %7.2f %7.2f"
            " | dR %5.2f\n",
            k, bandOff[0], bandOff[1], bandOff[2],
            bandRt[0], bandRt[1], bandRt[2], dn ? dsum / (double) dn : 0.0);
    }
    std::printf("replay done: %u groups, mean abs dR %.3f\n", done,
        nDiff ? sumDiff / (double) nDiff : 0.0);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: lsfg_pipeline_test <Lossless.dll> [w h flow gen srcFmt]\n");
        return 2;
    }
    const fs::path dll = argv[1];
    const uint32_t W = argc > 2 ? (uint32_t) std::atoi(argv[2]) : 512;
    const uint32_t H = argc > 3 ? (uint32_t) std::atoi(argv[3]) : 512;
    const uint32_t flow = argc > 4 ? (uint32_t) std::atoi(argv[4]) : 2;
    const uint32_t gen = argc > 5 ? (uint32_t) std::atoi(argv[5]) : 1;
    const uint32_t fmtArg = argc > 6 ? (uint32_t) std::atoi(argv[6]) : 87;
    const DXGI_FORMAT srcFmt = (DXGI_FORMAT) fmtArg;

    // Enable the debug layer BEFORE creating the device.
    HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
    if (hD3D12 == nullptr)
        hD3D12 = LoadLibraryW(L"d3d12.dll");
    auto fnGetDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
        GetProcAddress(hD3D12, "D3D12GetDebugInterface"));
    if (fnGetDebug != nullptr)
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(fnGetDebug(IID_PPV_ARGS(&dbg))) && dbg)
        {
            dbg->EnableDebugLayer();
            dbg->Release();
            std::printf("debug layer enabled\n");
        }
    }

    // --- device ---
    ID3D12Device* dev = nullptr;
    {
        IDXGIFactory1* fac = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))))
        {
            std::fprintf(stderr, "CreateDXGIFactory1 failed\n");
            return 1;
        }
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0;
             fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND;
             ++i, ad->Release(), ad = nullptr)
        {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            {
                std::printf("adapter: %ls\n", d.Description);
                break;
            }
        }
        if (ad == nullptr)
        {
            std::fprintf(stderr, "no hardware adapter\n");
            return 1;
        }
        if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0,
                IID_PPV_ARGS(&dev))))
        {
            std::fprintf(stderr, "D3D12CreateDevice failed\n");
            return 1;
        }
        ad->Release();
        fac->Release();
    }
    if (dev == nullptr)
        return 1;

    // --- info queue ---
    ID3D12InfoQueue* iq = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&iq));
    if (iq != nullptr)
        iq->SetMessageCountLimit(65536);

    // --- shader bank ---
    std::map<uint32_t, std::vector<uint8_t>> res;
    try
    {
        res = mfg::lsfg::ExtractRcData(dll);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "extract failed: %s\n", e.what());
        return 1;
    }
    std::printf("resources: %zu\n", res.size());
    mfg::lsfg::ShaderBank bank;
    if (!bank.Load(dev, res))
    {
        std::fprintf(stderr, "bank load failed: %s\n", bank.m_error.c_str());
        return 1;
    }
    std::printf("bank ok\n");
    if (iq != nullptr)
        DumpMessages(iq, "after bank load");

    // --- pipeline ---
    mfg::lsfg::Pipeline pipe;
    if (!pipe.Init(dev, bank, {W, H}, flow, gen, srcFmt))
    {
        std::fprintf(stderr, "pipeline init failed\n");
        if (iq != nullptr)
            DumpMessages(iq, "after pipeline init");
        return 1;
    }
    std::printf("pipeline ok, removed=0x%08x\n",
        (unsigned) dev->GetDeviceRemovedReason());
    if (iq != nullptr)
        DumpMessages(iq, "after pipeline init");

    Gpu gpu{dev, nullptr, nullptr, nullptr, 0};
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&gpu.fence))))
        {
            std::fprintf(stderr, "queue/fence failed\n");
            return 1;
        }
        gpu.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    // --- external OutputInterpolated stand-in (texture array, UAV) ---
    ID3D12Resource* interp = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = W;
        d.Height = H;
        d.DepthOrArraySize = (UINT16) gen;
        d.MipLevels = 1;
        d.Format = srcFmt;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&interp))))
        {
            std::fprintf(stderr, "interp alloc failed\n");
            return 1;
        }
    }

    const std::vector<uint8_t> patA = MakeRampPattern(W, H);
    const std::vector<uint8_t> patB = ShiftPatternX(patA, W, H, 8);
    const std::vector<uint8_t> patMid = ShiftPatternX(patA, W, H, 4);
    ID3D12Resource* texA = nullptr;
    ID3D12Resource* texB = nullptr;

    // replay mode: rerun runtime-captured source pairs through the graph
    // in this offline harness and compare against the runtime dump
    if (argc > 9 && std::string(argv[7]) == "replay")
    {
        const uint32_t rFirst = 2;
        const int rc = ReplayMain(pipe, gpu, interp, W, H, srcFmt,
            fs::path(argv[8]), rFirst, (uint32_t) std::atoi(argv[9]));
        if (iq != nullptr)
            DumpMessages(iq, "replay end");
        return rc;
    }

    // --- round 0: clears + uploads + static pair (A,A) for fidx=1 ---
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        pipe.RecordClears(list);
        texA = CreateTexFromPixels(dev, list, patA, W, H, srcFmt);
        texB = CreateTexFromPixels(dev, list, patB, W, H, srcFmt);
        if (texA == nullptr || texB == nullptr)
            return 1;
        FillRingPair(list, &pipe, texA, texA, 1);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
    }

    // --- rounds 1-2: static pair, two consecutive graph evaluations ---
    std::vector<uint8_t> out1, out2, out3, out4;
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        pipe.RecordGraph(list, 1, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        std::printf("graph fidx=1 done, removed=0x%08x\n",
            (unsigned) dev->GetDeviceRemovedReason());
        out1 = ReadSlice(gpu, interp, 0, W, H, srcFmt);
    }
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        pipe.RecordGraph(list, 2, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        out2 = ReadSlice(gpu, interp, 0, W, H, srcFmt);
    }
    if (out1.empty() || out2.empty())
    {
        std::fprintf(stderr, "readback empty\n");
        return 1;
    }

    std::printf("\n=== static pair test (ring = A,A) ===\n");
    Report(out1, patA, W, H, "out1 fidx=1", "A");
    Report(out2, patA, W, H, "out2 fidx=2", "A");
    Report(out2, out1, W, H, "out2 fidx=2", "out1 fidx=1");
    BestShift(patA, out1, W, H, "bestshift out1 vs A");
    BestShift(patA, out2, W, H, "bestshift out2 vs A");
    RampField(out2, W, H, "rampfield out2 vs A");
    std::printf("-- offsets out2 --\n");
    SampleOffsets(out2, W, H);
    DumpFlowStats(gpu, &pipe);
    PrepassAndMipCheck(gpu, pipe);

    // --- round 3: translated pair (A, A+8), expect mid = A shifted by 4 ---
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        FillRingPair(list, &pipe, texA, texB, 3);
        pipe.RecordGraph(list, 3, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        std::printf("graph fidx=3 done, removed=0x%08x\n",
            (unsigned) dev->GetDeviceRemovedReason());
        out3 = ReadSlice(gpu, interp, 0, W, H, srcFmt);
    }
    if (!out3.empty())
    {
        std::printf("\n=== translation test (ring = A, A+8) ===\n");
        Report(out3, patMid, W, H, "out3 fidx=3", "A+4(expect)");
        BestShift(patA, out3, W, H, "bestshift out3 vs A");
    RampField(out3, W, H, "rampfield out3 vs A");
    }
    else
        std::fprintf(stderr, "readback 3 empty\n");

    // --- round 4: translation at even parity (A, A+8), expect A+4 ---
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        FillRingPair(list, &pipe, texA, texB, 4);
        pipe.RecordGraph(list, 4, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        std::printf("graph fidx=4 done, removed=0x%08x\n",
            (unsigned) dev->GetDeviceRemovedReason());
        out4 = ReadSlice(gpu, interp, 0, W, H, srcFmt);
    }
    if (!out4.empty())
    {
        std::printf("\n=== translation at even parity (ring = A, A+8) ===\n");
        Report(out4, patMid, W, H, "out4 fidx=4", "A+4(expect)");
        Report(out4, out3, W, H, "out4 fidx=4", "out3 fidx=3");
        BestShift(patA, out4, W, H, "bestshift out4 vs A");
    RampField(out4, W, H, "rampfield out4 vs A");
    }
    else
        std::fprintf(stderr, "readback 4 empty\n");

    // --- round 5: static pair again after motion (temporal recovery) ---
    std::vector<uint8_t> out5;
    {
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        FillRingPair(list, &pipe, texA, texA, 5);
        pipe.RecordGraph(list, 5, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        std::printf("graph fidx=5 done, removed=0x%08x\n",
            (unsigned) dev->GetDeviceRemovedReason());
        out5 = ReadSlice(gpu, interp, 0, W, H, srcFmt);
    }
    if (!out5.empty())
    {
        std::printf("\n=== static again after motion (ring = A,A) ===\n");
        Report(out5, patA, W, H, "out5 fidx=5", "A");
        Report(out5, out2, W, H, "out5 fidx=5", "out2 fidx=2");
        BestShift(patA, out5, W, H, "bestshift out5 vs A");
        RampField(out5, W, H, "rampfield out5 vs A");
    }
    else
        std::fprintf(stderr, "readback 5 empty\n");

    // --- round 6: sustained linear motion with runtime-like single-slot
    // ring writes: at fidx k, frame A+8*(k-5) goes to slot k&1 only.
    // For a correct sustained interpolation the center offset must step
    // by exactly -8 per frame; alternating signs mean a parity bug.
    std::printf("\n=== sustained motion (single-slot ring writes) ===\n");
    for (uint32_t k = 6; k <= 17; ++k)
    {
        const uint32_t s = 8u * (k - 5u);
        const std::vector<uint8_t> pat = ShiftPatternX(patA, W, H, (int) s);
        ID3D12GraphicsCommandList* list = gpu.NewList();
        if (list == nullptr)
            return 1;
        ID3D12Resource* tex = CreateTexFromPixels(dev, list, pat, W, H, srcFmt);
        if (tex == nullptr)
            return 1;
        ID3D12Resource* slot = pipe.Source(static_cast<uint32_t>(k & 1u));        const D3D12_RESOURCE_BARRIER pre = Trans(slot,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &pre);
        list->CopyResource(slot, tex);
        const D3D12_RESOURCE_BARRIER post = Trans(slot,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &post);
        pipe.RecordGraph(list, k, interp, gen);
        if (!gpu.Exec(list))
            return 1;
        list->Release();
        tex->Release();
        const std::vector<uint8_t> out =
            ReadSlice(gpu, interp, 0, W, H, srcFmt);
        if (out.empty())
        {
            std::fprintf(stderr, "readback %u empty\n", k);
            return 1;
        }
        CenterShift(out, W, H, k, s);
    }
    std::printf("\ndevice removed reason: 0x%08x\n",
        (unsigned) dev->GetDeviceRemovedReason());
    if (iq != nullptr)
    {
        DumpMessages(iq, "after execution");
        iq->Release();
    }
    return dev->GetDeviceRemovedReason() == S_OK ? 0 : 3;
}
