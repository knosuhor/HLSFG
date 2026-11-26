// OptiScaler plugin API integration: settings tab under PLUGINS in the
// OptiScaler menu, persisted in the [Plugin.<folder name>] section of
// OptiScaler.ini. OptiScaler calls OptiPluginInit right after loading our
// dll from custom/<name>/dlssg, before it queries PopulateParameters, so
// values saved here are already visible to the NGX surface.
#include "../include/optiscaler_plugin.h"

#include "log.h"
#include "mfg_state.h"

namespace
{
const OptiPluginApi* g_api = nullptr;
OptiPlugin* g_plugin = nullptr;

void Draw(void* /*userData*/)
{
    const OptiPluginApi* api = g_api;
    OptiPlugin* plugin = g_plugin;

    api->Section("Options");
    api->TextDisabled("The FG multiplier lives in the FrameGen tab; with a replacement "
                      "active OptiScaler overrides whatever a provider reports.");

    int flow = mfg::g_settings.lsFlow.load();
    if (api->SliderInt("Flow scale##mfg", &flow, 1, 4, "1/%d"))
        mfg::g_settings.lsFlow.store(flow);
    api->ItemTooltip("LSFG optical flow resolution divisor (lower = higher quality, "
                     "higher = faster). OptiScaler has no counterpart for this option; "
                     "it is specific to the LSFG backend. When never changed here it "
                     "tracks the FrameGen multiplier (2x = 1/2, 4x = 1/4). "
                     "Applies after a game restart.");

    bool uiComposite = mfg::g_settings.uiComposite.load();
    if (api->Checkbox("UI recomposition (hudless)##mfg", &uiComposite))
        mfg::g_settings.uiComposite.store(uiComposite);
    api->ItemTooltip("With a HUD-less capture available (Advanced -> disable hudless off), "
                     "UI is detected from (backbuffer - hudless) of the current frame and "
                     "layered onto the generated frames, so HUD and menus stay sharp. "
                     "Without a HUDLess resource the pass stays inactive.");

    bool temporalUi = mfg::g_settings.temporalUi.load();
    if (api->Checkbox("Temporal UI mask (experimental)##mfg", &temporalUi))
        mfg::g_settings.temporalUi.store(temporalUi);
    api->ItemTooltip("Applies only when no HUDLess capture is tagged: pixels that did "
                     "not change between real frames are taken sharp from the current "
                     "frame instead of being interpolated. The per-pixel threshold has "
                     "no temporal state, so it can introduce judder in low-contrast "
                     "motion; uncheck for pure interpolation (HUD slightly warped "
                     "during camera motion).");

    api->Section("Hudless input");
    const char* invModes[] = { "Auto-detect", "Inverted (255 - x)", "Direct" };
    bool invDisabled[] = { false, false, false };
    int hudlessInvert = mfg::g_settings.hudlessInvert.load();
    if (api->Combo("HUDLess storage##mfg", &hudlessInvert, invModes, 3, invDisabled))
        mfg::g_settings.hudlessInvert.store(hudlessInvert);
    api->ItemTooltip("Auto-detect probes the first frames and negates such input before frame "
                     "generation, so the interpolator sees true colors. Applies after a game restart.");

    const char* previewModes[] = { "Off", "HUDLess", "UI Alpha", "UI Color and Alpha" };
    bool previewDisabled[] = { false, false, false, false };
    int previewSel = mfg::g_settings.inputPreview.load();
    if (previewSel < 0 || previewSel > 3)
        previewSel = 0;
    if (api->Combo("Input preview##mfg", &previewSel, previewModes, 4, previewDisabled))
        mfg::g_settings.inputPreview.store(previewSel);
    api->ItemTooltip("Writes the selected raw input to both frame generation outputs, "
                     "bypassing LSFG entirely: the on-screen picture becomes the input "
                     "exactly as the host delivers it, every frame. HUDLess shows the "
                     "tagged HUDLessColor; UI Alpha and UI Color and Alpha show the "
                     "DLSSG.UIAlpha / DLSSG.UI tags when the host provides them "
                     "(OptiScaler currently forwards these only on its Vulkan path; "
                     "on D3D12 they are usually absent and the normal pipeline runs). "
                     "Applies instantly.");

    bool fileLog = mfg::g_settings.fileLog.load();
    if (api->Checkbox("Write hlsfg.log##mfg", &fileLog))
        mfg::g_settings.fileLog.store(fileLog);

    if (api->ItemDeactivatedAfterEdit())
    {
        api->SetInt(plugin, "Multiplier", mfg::g_settings.multiplier.load());
        api->SetInt(plugin, "Flow", mfg::g_settings.lsFlow.load());
        api->SetBool(plugin, "FileLog", mfg::g_settings.fileLog.load());
        api->SetBool(plugin, "UiComposite", mfg::g_settings.uiComposite.load());
        api->SetBool(plugin, "TemporalUi", mfg::g_settings.temporalUi.load());
        api->SetInt(plugin, "InputPreview", mfg::g_settings.inputPreview.load());
        api->SetInt(plugin, "HudlessInvert", mfg::g_settings.hudlessInvert.load());
        // UiCutoff stays internal (tunable via the ini key only, default 2).
        api->SaveSettings(plugin);
        if (OPTI_API_HAS(api, Log))
            api->Log(plugin, OptiLog_Info, "HLSFG provider settings saved");
    }
}
} // namespace

extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin)
{
    if (api == nullptr || plugin == nullptr)
        return false;

    // OptiPluginInit runs before the NGX surface; initialise logging here so
    // the earliest lines are not dropped.
    mfg::log::InitFromAddress(reinterpret_cast<const void*>(&OptiPluginInit));

    g_api = api;
    g_plugin = plugin;

    // The multiplier is only reported as DLSSG.MultiFrameCountMax; with an
    // nvngx replacement active OptiScaler overrides it with the FrameGen
    // tab value, so it is not duplicated in this tab. The LS flow scale
    // is provider-specific: pinned when the tab slider saved a value,
    // else it tracks the multiplier (2x = 1/2, 4x = 1/4), which measured
    // best in sessions.
    const int multiplier = api->GetInt(plugin, "Multiplier", 2);
    mfg::g_settings.multiplier.store(multiplier);
    const int flow = api->GetInt(plugin, "Flow", 0);
    mfg::g_settings.lsFlow.store(flow > 0 ? flow : multiplier);
    mfg::g_settings.fileLog.store(api->GetBool(plugin, "FileLog", true));
    mfg::g_settings.uiComposite.store(api->GetBool(plugin, "UiComposite", true));
    mfg::g_settings.temporalUi.store(api->GetBool(plugin, "TemporalUi", false));
    mfg::g_settings.inputPreview.store(api->GetInt(plugin, "InputPreview", 0));
    mfg::g_settings.uiCutoff.store(api->GetInt(plugin, "UiCutoff", 2));
    mfg::g_settings.hudlessInvert.store(api->GetInt(plugin, "HudlessInvert", 0));

    api->RegisterTab(plugin, "HLSFG", "HUD-less FG: LSFG shaders below the GUI, UI recomposed from the hudless delta", Draw,
                     nullptr);

    if (OPTI_API_HAS(api, Log))
        api->Log(plugin, OptiLog_Info, "HLSFG provider: ready");

    MFG_LOG_INFO("OptiPluginInit (plugin api v%u), multiplier=%d", api->version,
                 mfg::g_settings.multiplier.load());
    return true;
}
