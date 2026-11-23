// SPDX-License-Identifier: GPL-3.0-or-later
// DXBC disassembler for the LSFG shader bank: dumps the RT_RCDATA
// compute shaders through D3DDisassemble so the sampling math can be
// audited offline (usage: dump_shader <Lossless.dll> <outdir> <first> <last>).
#include "../src/lsfg/dll_extract.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: dump_shader <dll> <outdir> [first] [last]\n");
        return 1;
    }
    const fs::path dll = argv[1];
    const fs::path outDir = argv[2];
    const uint32_t first = argc > 3 ? (uint32_t) std::atoi(argv[3]) : 255u;
    const uint32_t last = argc > 4 ? (uint32_t) std::atoi(argv[4]) : 279u;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    try
    {
        const auto res = mfg::lsfg::ExtractRcData(dll);
        for (uint32_t id = first; id <= last; ++id)
        {
            const auto it = res.find(id);
            if (it == res.end())
                continue;
            ComPtr<ID3DBlob> dis;
            if (FAILED(D3DDisassemble(it->second.data(), it->second.size(),
                    0, "", &dis)))
            {
                std::fprintf(stderr, "id %u disassemble failed\n", id);
                continue;
            }
            const fs::path out = outDir / (std::to_string(id) + ".disasm");
            FILE* f = nullptr;
            if (fopen_s(&f, out.string().c_str(), "wb") != 0 || f == nullptr)
                continue;
            fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize(), f);
            fclose(f);
            std::printf("id=%u -> %s (%zu bytes)\n", id, out.string().c_str(),
                dis->GetBufferSize());
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}