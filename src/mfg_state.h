// HLSFG Provider
// Global runtime state shared between the plugin API (settings UI) and the NGX surface.
#pragma once

#include <atomic>

namespace mfg
{
struct Settings
{
    std::atomic<int> multiplier{ 2 };     // reported to OptiScaler as DLSSG.MultiFrameCountMax (2..4)
    std::atomic<int> lsFlow{ 2 };         // LSFG flow scale divisor (1..4)
    std::atomic<bool> fileLog{ true };   // write hlsfg.log next to the dll
    std::atomic<bool> uiComposite{ true }; // hudless UI recomposition
    // Experimental temporal UI mask for sessions without a tagged
    // HUDLessColor: per-pixel threshold without temporal state, can
    // judder in low-contrast motion, hence opt-in (ini TemporalUi /
    // tab checkbox).
    std::atomic<bool> temporalUi{ false };
    // Input preview (tab dropdown): 0 = off, 1 = raw tagged
    // HUDLessColor, 2 = DLSSG.UIAlpha, 3 = DLSSG.UI (UI Color and
    // Alpha). Writes the selected input straight to both FG outputs,
    // bypassing the graph (on-screen inspection of what the host
    // actually delivers; live, ini InputPreview).
    std::atomic<int> inputPreview{ 0 };
    std::atomic<int> uiCutoff{ 2 };         // UI delta threshold, percent (0..10)
};

extern Settings g_settings;
}
