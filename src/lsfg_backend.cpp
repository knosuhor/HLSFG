#include "lsfg_backend.h"

#include "log.h"
#include "mfg_state.h"

#include "lsfg/dll_extract.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace mfg
{
namespace
{
// Lossless.dll lookup order: explicit override first, then the two
// Steam install roots (measured default install locations).
const char* const kDllCandidates[] = {
    "C:/Program Files (x86)/Steam/steamapps/common/Lossless Scaling/Lossless.dll",
    "C:/Program Files/Steam/steamapps/common/Lossless Scaling/Lossless.dll",
};

bool IsSupportedFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* key)
{
    void* resource = nullptr;
    if (params->Get(key, &resource) == NVSDK_NGX_Result_Success)
        return static_cast<ID3D12Resource*>(resource);
    return nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

void CopyFrame(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dst,
        ID3D12Resource* src, D3D12_RESOURCE_STATES srcState)
{
    const D3D12_RESOURCE_BARRIER pre[] = {
        Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList->ResourceBarrier(2, pre);
    cmdList->CopyResource(dst, src);
    const D3D12_RESOURCE_BARRIER post[] = {
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState),
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(2, post);
}
// --- debug frame dump (MFG_DUMP) -------------------------------------
// Optional runtime diagnosis: every burst evaluate copies a small ROI of
// OutputReal and OutputInterpolated into readback buffers; the bytes land
// on disk a few evals later, once the copy has certainly completed. The
// files are packed rows of the source format (4 Bpp) sized per meta.txt,
// analyzed offline by tools/dump_analyze. Environment variables:
//   MFG_DUMP=<count>      dump this many burst evaluates (unset = off)
//   MFG_DUMP_DIR=<path>   output directory (default %TEMP%\mfg-framedumps)
//   MFG_DUMP_ROI=w,h,x,y  crop rectangle (default 640x360 centered)
struct DumpSlot
{
    Microsoft::WRL::ComPtr<ID3D12Resource> rb[4];
    uint64_t fidx = 0;
    uint64_t stamp = 0;
};

struct DumpImpl
{
    bool enabled = false;
    uint64_t remaining = 0;
    uint64_t stamp = 0;
    uint32_t roiW = 0, roiH = 0, roiX = 0, roiY = 0;
    size_t pitch = 0, size = 0;
    std::wstring dir;
    DumpSlot pool[12];
    size_t cursor = 0;
} g_dump;

void DumpSetup(ID3D12Device* device, uint32_t imgW, uint32_t imgH)
{
    (void) device;
    char buf[64] = {};
    if (GetEnvironmentVariableA("MFG_DUMP", buf, sizeof(buf)) == 0)
        return;
    g_dump.enabled = true;
    g_dump.remaining = (uint64_t) _strtoui64(buf, nullptr, 10);
    g_dump.roiW = imgW < 640 ? imgW : 640;
    g_dump.roiH = imgH < 360 ? imgH : 360;
    g_dump.roiX = (imgW - g_dump.roiW) / 2;
    g_dump.roiY = (imgH - g_dump.roiH) / 2;    char roi[96] = {};
    if (GetEnvironmentVariableA("MFG_DUMP_ROI", roi, sizeof(roi)) > 0)
    {
        unsigned w = 0, h = 0, x = 0, y = 0;
        if (sscanf_s(roi, "%u,%u,%u,%u", &w, &h, &x, &y) == 4)
        {
            if (w > imgW) w = imgW;
            if (h > imgH) h = imgH;
            if (x > imgW - w) x = imgW - w;
            if (y > imgH - h) y = imgH - h;
            g_dump.roiW = w;
            g_dump.roiH = h;
            g_dump.roiX = x;
            g_dump.roiY = y;
        }
    }
    g_dump.pitch = ((size_t) g_dump.roiW * 4 + 255) & ~(size_t) 255;
    g_dump.size = g_dump.pitch * g_dump.roiH + 512;
    char d[MAX_PATH] = {};
    std::filesystem::path outDir;
    if (GetEnvironmentVariableA("MFG_DUMP_DIR", d, sizeof(d)) > 0)
        outDir = std::filesystem::path(d);
    else
        outDir = std::filesystem::temp_directory_path() / "mfg-framedumps";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    FILE* mf = _wfopen((outDir.wstring() + L"\\meta.txt").c_str(), L"w");
    if (mf != nullptr)
    {
        std::fprintf(mf, "%u %u %u\n", g_dump.roiW, g_dump.roiH, 4);
        std::fclose(mf);
    }
    g_dump.dir = outDir.wstring();
    MFG_LOG_INFO("lsfg: dump armed roi=%ux%u at +%u+%u, %llu frames",
        g_dump.roiW, g_dump.roiH, g_dump.roiX, g_dump.roiY,
        (unsigned long long) g_dump.remaining);
}

bool DumpWrite(const std::wstring& path, ID3D12Resource* res)
{
    if (res == nullptr)
        return false;
    void* m = nullptr;
    const D3D12_RANGE all{0, g_dump.size};
    if (FAILED(res->Map(0, &all, &m)))        return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f != nullptr)
    {
        const uint8_t* src = static_cast<const uint8_t*>(m);
        for (uint32_t y = 0; y < g_dump.roiH; ++y)
            std::fwrite(src + (size_t) y * g_dump.pitch,
                (size_t) g_dump.roiW * 4, 1, f);
        std::fclose(f);
    }
    res->Unmap(0, nullptr);
    return f != nullptr;
}

void DumpDrain(bool flush)
{
    if (!g_dump.enabled)
        return;
    for (DumpSlot& s : g_dump.pool)
    {
        if (s.stamp == 0)
            continue;
        if (!flush && g_dump.stamp - s.stamp < 5)
            continue;
        std::wstring name = g_dump.dir + L"\\real_" +
            std::to_wstring(s.fidx) + L".raw";
        DumpWrite(name, s.rb[0].Get());
        name = g_dump.dir + L"\\interp_" +
            std::to_wstring(s.fidx) + L".raw";
        DumpWrite(name, s.rb[1].Get());
        name = g_dump.dir + L"\\srcold_" +
            std::to_wstring(s.fidx) + L".raw";
        DumpWrite(name, s.rb[2].Get());
        name = g_dump.dir + L"\\srcnew_" +
            std::to_wstring(s.fidx) + L".raw";
        DumpWrite(name, s.rb[3].Get());
        s.stamp = 0;
    }
}
void DumpCapture(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    uint64_t fidx, ID3D12Resource* real, ID3D12Resource* interp,
    ID3D12Resource* srcOld, ID3D12Resource* srcNew)
{
    if (real == nullptr || interp == nullptr || srcOld == nullptr ||
        srcNew == nullptr || cmdList == nullptr || device == nullptr)
        return;
    const D3D12_RESOURCE_DESC rd = real->GetDesc();
    if (g_dump.pitch == 0)
        DumpSetup(device, (uint32_t) rd.Width, (uint32_t) rd.Height);
    if (!g_dump.enabled || g_dump.remaining == 0)
        return;
    if (rd.Width < g_dump.roiX + g_dump.roiW ||
        rd.Height < g_dump.roiY + g_dump.roiH)
        return;

    DumpSlot& s = g_dump.pool[g_dump.cursor];
    g_dump.cursor = (g_dump.cursor + 1) % 12;
    if (!s.rb[0])
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_dump.size;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int j = 0; j < 4; ++j)
            if (FAILED(device->CreateCommittedResource(&hp,
                    D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&s.rb[j]))))
            {
                g_dump.enabled = false;
                MFG_LOG_ERROR("lsfg: dump readback alloc failed");
                return;
            }
    }    ID3D12Resource* out[4] = {real, interp, srcOld, srcNew};
    D3D12_RESOURCE_BARRIER pre[4] = {};
    D3D12_RESOURCE_BARRIER post[4] = {};
    for (int j = 0; j < 4; ++j)
    {
        pre[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[j].Transition.pResource = out[j];
        pre[j].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        pre[j].Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        pre[j].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        post[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[j].Transition.pResource = out[j];
        post[j].Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        post[j].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        post[j].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(4, pre);
    for (int j = 0; j < 4; ++j)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = out[j];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = s.rb[j].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = rd.Format;
        dst.PlacedFootprint.Footprint.Width = g_dump.roiW;
        dst.PlacedFootprint.Footprint.Height = g_dump.roiH;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = (UINT) g_dump.pitch;
        D3D12_BOX box{g_dump.roiX, g_dump.roiY, 0,
            g_dump.roiX + g_dump.roiW, g_dump.roiY + g_dump.roiH, 1};
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    cmdList->ResourceBarrier(4, post);
    s.fidx = fidx;
    s.stamp = ++g_dump.stamp;
    --g_dump.remaining;
}

} // namespace

NVSDK_NGX_Result LsfgBackend::InitD3D12(ID3D12Device* device)
{
    if (m_bankReady)
        return NVSDK_NGX_Result_Success;

    MFG_LOG_INFO("LsfgBackend init (phase 1), device=%p",
        static_cast<void*>(device));
    m_device = device;

    // locate Lossless.dll
    std::filesystem::path dll;
    char overridePath[MAX_PATH] = {};
    if (GetEnvironmentVariableA("MFG_LS_DLL", overridePath, MAX_PATH) > 0)
        dll = std::filesystem::path(overridePath);
    else
    {
        for (const char* candidate : kDllCandidates)
        {
            if (std::filesystem::exists(candidate))
            {
                dll = std::filesystem::path(candidate);
                break;
            }
        }
    }
    if (dll.empty())
    {
        MFG_LOG_ERROR("lsfg: Lossless.dll not found (set MFG_LS_DLL or "
            "install Lossless Scaling via Steam)");
        m_pipelineFailed = true;
        return NVSDK_NGX_Result_Success; // degrade to fallback copies
    }
    MFG_LOG_INFO("lsfg: using %s", dll.string().c_str());

    try
    {
        const auto resources = lsfg::ExtractRcData(dll);
        MFG_LOG_INFO("lsfg: %zu RT_RCDATA resources", resources.size());
        if (!m_bank.Load(device, resources))
        {
            MFG_LOG_ERROR("lsfg: shader bank failed: %s", m_bank.m_error.c_str());
            m_pipelineFailed = true;
            return NVSDK_NGX_Result_Success;
        }
        m_bankReady = true;
    }
    catch (const std::exception& e)
    {
        MFG_LOG_ERROR("lsfg: extraction failed: %s", e.what());
        m_pipelineFailed = true;
        return NVSDK_NGX_Result_Success;
    }

    return NVSDK_NGX_Result_Success;
}

bool LsfgBackend::EnsurePipeline(uint32_t width, uint32_t height,
        uint32_t slices, DXGI_FORMAT sourceFormat)
{
    if (m_pipelineReady || m_pipelineFailed || !m_bankReady)
        return m_pipelineReady;
    if (width == 0 || height == 0 || slices == 0)
        return false;

    const uint32_t flow = static_cast<uint32_t>(g_settings.lsFlow.load());
    if (!m_pipeline.Init(m_device, m_bank, {width, height}, flow, slices,
            sourceFormat))
    {
        m_pipelineFailed = true;
        return false;
    }
    m_pipelineReady = true;
    return true;
}
NVSDK_NGX_Result LsfgBackend::EvaluateD3D12(ID3D12GraphicsCommandList* cmdList,
        NVSDK_NGX_Parameter* params)
{
    ++m_calls;
    DumpDrain(false);

    ID3D12Resource* backbuffer = GetResource(params, "DLSSG.Backbuffer");
    ID3D12Resource* hudless = GetResource(params, "DLSSG.HUDLess");
    ID3D12Resource* outputReal = GetResource(params, "DLSSG.OutputReal");
    ID3D12Resource* outputInterpolated =
        GetResource(params, "DLSSG.OutputInterpolated");

    // slot identity: pointer-bits contract measured against OptiScaler
    void* rawIndex = nullptr;
    const bool hasIndex =
        params->Get("DLSSG.MultiFrameIndex", &rawIndex) ==
            NVSDK_NGX_Result_Success && rawIndex != nullptr;
    const unsigned long long frameIndex =
        hasIndex ? (unsigned long long)(uintptr_t)rawIndex : 1;
    const bool burst = !hasIndex || frameIndex == 0;

    ID3D12Resource* source = hudless != nullptr ? hudless : backbuffer;
    const D3D12_RESOURCE_STATES sourceState = hudless != nullptr
        ? D3D12_RESOURCE_STATE_COPY_DEST
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // TEMP instrumentation: slot identity + resource pointers per call
    void* rawCount = nullptr;
    const bool hasCount =
        params->Get("DLSSG.MultiFrameCount", &rawCount) ==
            NVSDK_NGX_Result_Success && rawCount != nullptr;
    if (m_calls <= 12 || (m_calls % 500) == 0)
        MFG_LOG_INFO("lsfg: call#%llu mfiHas=%d mfi=%llu | mfcHas=%d mfc=%llu | hudless=%p bb=%p real=%p interp=%p",
            m_calls, (int) hasIndex, frameIndex, (int) hasCount,
            hasCount ? (unsigned long long)(uintptr_t) rawCount : 0ULL,
            (void*) hudless, (void*) backbuffer, (void*) outputReal,
            (void*) outputInterpolated);

    // Blend-style fill used whenever the graph cannot run (first frame,
    // unsupported format, one-Evaluate-per-slot mode, init failure).
    auto fallback = [&]()
    {
        ++m_fallbacks;
        if (outputReal != nullptr && backbuffer != nullptr)
            CopyFrame(cmdList, outputReal, backbuffer,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (outputInterpolated != nullptr && source != nullptr)
            CopyFrame(cmdList, outputInterpolated, source, sourceState);
    };

    if (!burst || source == nullptr || cmdList == nullptr ||
        outputInterpolated == nullptr)
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    // resolution + slice contract for the graph
    const D3D12_RESOURCE_DESC srcDesc = source->GetDesc();
    const D3D12_RESOURCE_DESC interpDesc = outputInterpolated->GetDesc();
    if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        interpDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }
    if (!IsSupportedFormat(srcDesc.Format))
    {
        if (!m_formatWarned)
        {
            MFG_LOG_INFO("lsfg: source format %u not supported yet, "
                "staying on blend fallback", srcDesc.Format);
            m_formatWarned = true;
        }
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    uint32_t slices = static_cast<uint32_t>(interpDesc.DepthOrArraySize);
    if (slices == 0)
        slices = 1;

    if (m_calls <= 3)
        MFG_LOG_INFO("lsfg: eval#%llu fidx=%llu src fmt=%u flags=0x%x | interp fmt=%u flags=0x%x depth=%u",
            m_calls, m_fidx, static_cast<unsigned>(srcDesc.Format),
            static_cast<unsigned>(srcDesc.Flags), static_cast<unsigned>(interpDesc.Format),
            static_cast<unsigned>(interpDesc.Flags), slices);

    if (!EnsurePipeline(static_cast<uint32_t>(srcDesc.Width),
            static_cast<uint32_t>(srcDesc.Height), slices, srcDesc.Format))
    {
        fallback();
        return NVSDK_NGX_Result_Success;
    }

    // ring: store the incoming frame into slot (fidx & 1)
    ID3D12Resource* ring = m_pipeline.Source(
        static_cast<uint32_t>(m_fidx & 1));
    {
        const D3D12_RESOURCE_BARRIER pre[] = {
            Transition(ring, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST),
            Transition(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmdList->ResourceBarrier(2, pre);
        cmdList->CopyResource(ring, source);
        const D3D12_RESOURCE_BARRIER post[] = {
            Transition(ring, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState),
        };
        cmdList->ResourceBarrier(2, post);
    }

    if (m_fidx == 0)
    {
        // first real frame: zero the transient images, blend the group
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=clears");
        m_pipeline.RecordClears(cmdList);
        fallback();
    }
    else
    {
        // the pair (F_{fidx-1}, F_fidx) now sits in the ring: generate
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=graph fidx=%llu", m_fidx);
        m_pipeline.RecordGraph(cmdList, m_fidx, outputInterpolated, slices);
        ++m_generations;
        if (m_calls <= 3)
            MFG_LOG_INFO("lsfg: stage=graph-done");
        if (outputReal != nullptr && backbuffer != nullptr)
        if (outputReal != nullptr)
        {
            // LS latency model: the group is presented as
            // [previous real frame, generated midpoint]. Filling
            // OutputReal with the CURRENT backbuffer instead makes the
            // presented timeline jump backwards on every interpolated
            // slot (visible jitter/oscillation of the whole image).
            ID3D12Resource* prev = m_pipeline.Source(
                static_cast<uint32_t>((m_fidx + 1) & 1));
            if (prev != nullptr &&
                prev->GetDesc().Format == outputReal->GetDesc().Format)
            {
                CopyFrame(cmdList, outputReal, prev,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else if (backbuffer != nullptr)
                CopyFrame(cmdList, outputReal, backbuffer,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

    DumpCapture(m_device, cmdList, m_fidx, outputReal, outputInterpolated,
        m_pipeline.Source(static_cast<uint32_t>((m_fidx + 1) & 1)),
        m_pipeline.Source(static_cast<uint32_t>(m_fidx & 1)));
    ++m_fidx;
    return NVSDK_NGX_Result_Success;
}

void LsfgBackend::Shutdown()
{
    MFG_LOG_INFO("LsfgBackend shutdown after %llu evaluate calls, "
        "%llu generated groups, %llu blend fallbacks",
        m_calls, m_generations, m_fallbacks);
    DumpDrain(true);
    m_fidx = 0;
    m_pipelineReady = false;
    m_device = nullptr;
}
} // namespace mfg
