// HLSFG Provider, Phase 0 skeleton
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
    std::atomic<bool> fileLog{ true };   // write hlsfg.log next to the dll
    std::atomic<bool> uiComposite{ true }; // hudless UI recomposition
    // Experimental temporal UI mask for sessions without a usable
    // HUDLess capture (garbage verdict or untagged game): per-pixel
    // threshold without temporal state, can judder in low-contrast
    // motion, hence opt-in (ini TemporalUi / tab checkbox).
    std::atomic<bool> temporalUi{ false };
    // HUDLess preview: write the raw tagged HUDLessColor straight to
    // both FG outputs, bypassing the graph (on-screen inspection of
    // what the game actually delivers; live, ini HudlessPreview).
    std::atomic<bool> hudlessPreview{ false };
    std::atomic<int> uiCutoff{ 2 };         // UI delta threshold, percent (0..10)
    // HUDLessColor byte polarity: 0 = auto-detect (readback probe),
    // 1 = complemented storage (255 - color), 2 = direct storage.
    // Applies after a game restart.
    std::atomic<int> hudlessInvert{ 0 };
};

extern Settings g_settings;

// Live hudless diagnostics for the plugin tab, written by the LSFG
// backend probes: 0 = not measured yet, 1 = hudless healthy,
// 2 = hudless broken (session interpolates the raw backbuffer).
extern std::atomic<int> g_hudlessHealth;
} // namespace mfg
