// RT_RCDATA extraction from a PE file on disk.
//
// Phase 1 (LSFG): Lossless Scaling packs its frame generation comput
// shaders as RT_RCDATA resources inside Lossless.dll. The provider reads
// the dll from the users own installation at runtime and never ships the
// resources (see docs/PHASE1.md, licensing notes).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace mfg::lsfg
{
// Returns a map of resource id -> raw resource bytes for every RT_RCDATA
// entry in the given PE file. Throws std::runtime_error on malformed input.
std::map<uint32_t, std::vector<uint8_t>> ExtractRcData(const std::filesystem::path& peFile);
}
