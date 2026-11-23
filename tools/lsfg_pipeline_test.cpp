// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline repro for the phase-1 LSFG pipeline: builds the shader bank
// and the full graph on a fresh D3D12 device WITH the debug layer on,
// records the startup clears + one burst graph evaluation, waits for
// the GPU and prints the device removal reason plus every stored
// debug-layer message. Illegal GPU usage gets named exactly instead
// of crashing the host game.
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
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace
{
void DumpMessages(ID3D12InfoQueue* iq, const char* when)
{
    const UINT64 n = iq->GetNumStoredMessages();
    std::printf("--- %s: %llu stored messages ---\n", when, (unsigned long long) n);
    const UINT64 limit = n < 40 ? n : 40;
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
}} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: lsfg_pipeline_test <Lossless.dll> [w h flow gen srcFmt]\n");
        return 2;
    }
    const fs::path dll = argv[1];
    const uint32_t W = argc > 2 ? (uint32_t) std::atoi(argv[2]) : 1920;
    const uint32_t H = argc > 3 ? (uint32_t) std::atoi(argv[3]) : 1080;
    const uint32_t flow = argc > 4 ? (uint32_t) std::atoi(argv[4]) : 2;
    const uint32_t gen = argc > 5 ? (uint32_t) std::atoi(argv[5]) : 1;
    const uint32_t fmtArg = argc > 6 ? (uint32_t) std::atoi(argv[6]) : 87;
    const DXGI_FORMAT srcFmt = (DXGI_FORMAT) fmtArg;

    // --- debug layer first, so the device is created with it ---
    HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
    if (hD3D12 == nullptr)
        hD3D12 = LoadLibraryW(L"d3d12.dll");
    auto fnGetDebug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(hD3D12, "D3D12GetDebugInterface"));
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
    ID3D12Device* devRaw = nullptr;
    {
        IDXGIFactory1* fac = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))))
        {
            std::fprintf(stderr, "CreateDXGIFactory1 failed\n");
            return 1;
        }
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i, ad->Release(), ad = nullptr)
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
        if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&devRaw))))
        {
            std::fprintf(stderr, "D3D12CreateDevice failed\n");
            return 1;
        }
        ad->Release();
        fac->Release();
    }
    if (devRaw == nullptr)
        return 1;    ID3D12Device* dev = devRaw;

    // --- format support probe (BGRA8 as UAV is the phase-1 suspect) ---
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT f{};
        f.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof(f))))
        {
            std::printf("BGRA8 Support1=0x%08x Support2=0x%08x uav1=0x%08x typedload2=0x%08x\n",
                (unsigned) f.Support1, (unsigned) f.Support2,
                (unsigned) (f.Support1 & 0x40u), (unsigned) (f.Support2 & 0x8000u));
        }
    }

    // --- debug info queue ---
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
    std::printf("resources: %zu\n", res.size());    mfg::lsfg::ShaderBank bank;
    if (!bank.Load(dev, res))
    {
        std::fprintf(stderr, "bank load failed: %s\n", bank.m_error.c_str());
        return 1;
    }
    std::printf("bank ok\n");
    if (iq) DumpMessages(iq, "after bank load");

    // --- pipeline ---
    mfg::lsfg::Pipeline pipe;
    if (!pipe.Init(dev, bank, {W, H}, flow, gen, srcFmt))
    {
        std::fprintf(stderr, "pipeline init failed\n");
        if (iq) DumpMessages(iq, "after pipeline init");
        return 1;
    }
    std::printf("pipeline ok, removed=0x%08x\n", (unsigned) dev->GetDeviceRemovedReason());
    std::fflush(stdout);
    if (iq) DumpMessages(iq, "after pipeline init");

    // --- queue + list ---
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
    {
        std::fprintf(stderr, "CreateCommandQueue failed\n");
        return 1;
    }
    ID3D12CommandAllocator* alloc = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
    {
        std::fprintf(stderr, "CreateCommandAllocator failed\n");
        return 1;
    }
    ID3D12GraphicsCommandList* cmd = nullptr;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))))
    {
        std::fprintf(stderr, "CreateCommandList failed\n");
        return 1;
    }
    // --- dummy source frame (the game's HUDLess placeholder) ---
    ID3D12Resource* dummySource = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = W;
        d.Height = H;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = srcFmt;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&dummySource))))
        {
            std::fprintf(stderr, "dummy source alloc failed\n");
            return 1;
        }
    }

    // --- external OutputInterpolated stand-in (BGRA8, UAV) ---
    ID3D12Resource* interp = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = W;
        d.Height = H;
        d.DepthOrArraySize = (UINT) gen;
        d.MipLevels = 1;
        d.Format = srcFmt;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&interp))))
        {
            std::fprintf(stderr, "interp alloc failed\n");
            return 1;
        }
    }
    // --- record: clears, source copy, graph (burst, fidx = 1) ---
    std::printf("recording clears... removed=0x%08x\\n", (unsigned) dev->GetDeviceRemovedReason());
    pipe.RecordClears(cmd);
    {
        ID3D12Resource* ring0 = pipe.Source(0);
        ID3D12Resource* ring1 = pipe.Source(1);
        const D3D12_RESOURCE_BARRIER toD0[] = {
            Trans(ring0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            Trans(ring1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            Trans(dummySource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmd->ResourceBarrier(3, toD0);
        cmd->CopyResource(ring0, dummySource);
        cmd->CopyResource(ring1, dummySource);
        const D3D12_RESOURCE_BARRIER toU[] = {
            Trans(ring0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Trans(ring1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Trans(dummySource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(3, toU);
    }
    std::printf("recording graph... removed=0x%08x\\n", (unsigned) dev->GetDeviceRemovedReason());
    pipe.RecordGraph(cmd, 1, interp, gen);
    std::printf("recorded, closing, removed=0x%08x\\n", (unsigned) dev->GetDeviceRemovedReason());
    std::fflush(stdout);
    if (FAILED(cmd->Close()))
    {
        std::fprintf(stderr, "Close failed\n");
        return 1;
    }
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    // --- fence wait ---
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    queue->Signal(fence, 1);
    WaitForSingleObject(ev, 30000);
    // --- verdict ---
    const HRESULT rr = dev->GetDeviceRemovedReason();
    std::printf("device removed reason: 0x%08x\n", (unsigned) rr);
    if (iq != nullptr)
    {
        DumpMessages(iq, "after execution");
        iq->Release();
    }
    else
    {
        std::printf("(no info queue: device was created without the debug layer)\n");
    }
    return rr == S_OK ? 0 : 3;
}
