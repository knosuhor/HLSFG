// lsfg_probe: offline introspection of Lossless Scaling's packed DXBC
// compute shaders. Phase 1 tool: extracts every RT_RCDATA resource from
// Lossless.dll, reflects each shader (bindings, constant layouts) via
// d3dcompiler_47 and disassembles it, writing a report used to port the
// LSFG execution graph onto D3D12 (docs/PHASE1.md).
//
// usage: lsfg_probe <Lossless.dll> [report-dir]

#include "../src/lsfg/dll_extract.h"

#include <windows.h>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using FnReflect = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
using FnDisassemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

std::string BlobToString(ID3DBlob* blob)
{
    if (!blob)
        return {};
    const char* p = static_cast<const char*>(blob->GetBufferPointer());
    return std::string(p, p + blob->GetBufferSize());
}

std::string FindThreadGroup(const std::string& disasm)
{
    const size_t k = disasm.find("dcl_thread_group");
    if (k == std::string::npos)
        return "?";
    const size_t eol = disasm.find('\n', k);
    std::string line = disasm.substr(k, eol == std::string::npos ? std::string::npos : eol - k);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    return line;
}

const char* TypeName(D3D_SHADER_INPUT_TYPE t)
{
    switch (t)
    {
    case D3D_SIT_CBUFFER: return "cbuffer";
    case D3D_SIT_TBUFFER: return "tbuffer";
    case D3D_SIT_TEXTURE: return "srv";
    case D3D_SIT_SAMPLER: return "sampler";
    case D3D_SIT_UAV_RWTYPED: return "uav";
    case D3D_SIT_UAV_RWSTRUCTURED: return "uav-structured";
    case D3D_SIT_STRUCTURED: return "srv-structured";
    case D3D_SIT_BYTEADDRESS: return "srv-ba";
    case D3D_SIT_UAV_RWBYTEADDRESS: return "uav-ba";
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return "uav-structured-cnt";
    default: return "other";
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: lsfg_probe <Lossless.dll> [report-dir]\n");
        return 2;
    }
    const fs::path dll = argv[1];
    const fs::path outDir = argc >= 3 ? fs::path(argv[2]) : fs::path("lsfg-report");
    fs::create_directories(outDir);

    std::map<uint32_t, std::vector<uint8_t>> resources;
    try
    {
        resources = mfg::lsfg::ExtractRcData(dll);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "extract failed: %s\n", e.what());
        return 1;
    }

    const HMODULE hc = LoadLibraryA("d3dcompiler_47.dll");
    if (!hc)
    {
        std::fprintf(stderr, "d3dcompiler_47.dll not available\n");
        return 1;
    }
    const auto fnReflect = reinterpret_cast<FnReflect>(GetProcAddress(hc, "D3DReflect"));
    const auto fnDisassemble = reinterpret_cast<FnDisassemble>(GetProcAddress(hc, "D3DDisassemble"));
    if (!fnReflect || !fnDisassemble)
    {
        std::fprintf(stderr, "D3DReflect/D3DDisassemble not found\n");
        return 1;
    }

    std::ofstream report((outDir / "report.txt").string());
    report << "# LSFG shader probe: " << dll.string() << "\n"
           << "# resources: " << resources.size() << "\n\n";

    for (const auto& [id, blob] : resources)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "id_%03u", id);
        std::ofstream raw((outDir / (std::string(name) + ".cso")).string(), std::ios::binary);
        raw.write(reinterpret_cast<const char*>(blob.data()), blob.size());

        std::string tg = "?";
        std::string bindDesc;
        ID3D11ShaderReflection* refl = nullptr;
        if (SUCCEEDED(fnReflect(blob.data(), blob.size(), __uuidof(ID3D11ShaderReflection),
                               reinterpret_cast<void**>(&refl))))
        {
            D3D11_SHADER_DESC d{};
            refl->GetDesc(&d);
            for (UINT i = 0; i < d.BoundResources; ++i)
            {
                D3D11_SHADER_INPUT_BIND_DESC b{};
                if (FAILED(refl->GetResourceBindingDesc(i, &b)))
                    continue;
                char row[160];
                std::snprintf(row, sizeof(row), "  %s:%s@%u",
                              TypeName(b.Type), b.Name, b.BindPoint);
                bindDesc += row;
            }
            for (UINT i = 0; i < d.ConstantBuffers; ++i)
            {
                ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
                D3D11_SHADER_BUFFER_DESC cbd{};
                if (cb && SUCCEEDED(cb->GetDesc(&cbd)))
                {
                    char row[160];
                    std::snprintf(row, sizeof(row), "  [cb %s size=%u vars=%u]",
                                  cbd.Name, cbd.Size, cbd.Variables);
                    bindDesc += row;
                    for (UINT v = 0; v < cbd.Variables && v < 8; ++v)
                    {
                        D3D11_SHADER_TYPE_DESC td{};
                        ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                        if (var)
                        {
                            D3D11_SHADER_VARIABLE_DESC vd{};
                            if (SUCCEEDED(var->GetDesc(&vd)))
                            {
                                char vr[96];
                                std::snprintf(vr, sizeof(vr), " {var %s off=%u sz=%u}",
                                              vd.Name, vd.StartOffset, vd.Size);
                                bindDesc += vr;
                            }
                        }
                        (void)td;
                    }
                }
            }
            if (refl)
                refl->Release();
        }
        else
        {
            bindDesc = "  (reflection failed)";
        }

        ID3DBlob* dis = nullptr;
        std::string disasm;
        if (SUCCEEDED(fnDisassemble(blob.data(), blob.size(), 0, nullptr, &dis)))
        {
            disasm = BlobToString(dis);
            dis->Release();
            std::ofstream asmFile((outDir / (std::string(name) + ".asm")).string());
            asmFile << disasm;
            tg = FindThreadGroup(disasm);
        }

        char line[256];
        std::snprintf(line, sizeof(line), "id=%u size=%zu numthreads=[%s]", id, blob.size(), tg.c_str());
        report << line << bindDesc << "\n";
        std::cout << line << bindDesc << "\n";
    }

    std::cout << "\nreport: " << (outDir / "report.txt").string() << "\n";
    return 0;
}
