// MFG Provider, Phase 0 skeleton
// Global runtime state shared between the plugin API (settings UI) and the NGX surface.
#pragma once

#include <atomic>

namespace mfg
{
// Backend selection. Phase 0 ships Passthrough only; Blend exists as a documented stub
// so that the UI and dispatch table are already in their final shape.
enum class BackendKind
{
    Passthrough = 0,
    Blend = 1,
    Lsfg = 2,       // phase 1: LS 3.2.1 native blob + lsfg-vk graph
};

struct Settings
{
    std::atomic<int> backend{ static_cast<int>(BackendKind::Passthrough) };
    std::atomic<int> multiplier{ 2 };     // reported to OptiScaler as DLSSG.MultiFrameCountMax (2..4)
    std::atomic<int> lsFlow{ 2 };         // LSFG flow scale divisor (1..4)
    std::atomic<bool> fileLog{ true };   // write mfg-provider.log next to the dll
};

extern Settings g_settings;
} // namespace mfg
