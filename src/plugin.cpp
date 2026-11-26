// OptiScaler plugin API integration: settings tab under PLUGINS in the
// OptiScaler menu, persisted in the [Plugin.<folder name>] section of
// OptiScaler.ini. OptiScaler calls OptiPluginInit right after loading our
// dll from custom/<name>/dlssg, before it queries PopulateParameters, so
// values saved here are already visible to the NGX surface.
#include "../include/optiscaler_plugin.h"

#include "backends.h"
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

    api->Section("Backend");
    const char* backends[] = { "Passthrough (phase 0)", "Blend (phase 0.5)", "LSFG (phase 1)" };
    const bool disabled[] = { false, false, false };
    int backend = mfg::g_settings.backend.load();
    if (api->Combo("Backend##mfg", &backend, backends, 3, disabled))
        mfg::g_settings.backend.store(backend);
    api->ItemTooltip("Passthrough observes the frame generation inputs and produces nothing "
                     "(FG slots stay black with FGOutput=dlssg). Blend copies the frame through "
                     "the FG output chain so the image stays visible while backend work "
                     "continues. Switching backends requires a game restart.");
    if (backend == 2)
        api->TextDisabled("LSFG runs Lossless Scaling's frame generation shaders (from your own "
                          "Steam installation) on the frame pair. Requires Lossless Scaling "
                          "installed; falls back to blend copies otherwise.");
    else if (backend == 1)
        api->TextDisabled("Blend repeats the source frame in every FG slot; motion is not "
                          "interpolated. Boot-to-exit stability is the phase 0.5 target.");
    else
        api->TextDisabled("Real frames only. The passthrough backend validates the contract "
                          "and writes hlsfg.log next to the dll.");

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
    api->ItemTooltip("Applies only when no usable HUDLess capture exists (broken or "
                     "not tagged): pixels that did not change between real frames are "
                     "taken sharp from the current frame instead of being interpolated. "
                     "The per-pixel threshold has no temporal state, so it can introduce "
                     "judder in low-contrast motion; uncheck for pure interpolation "
                     "(HUD slightly warped during camera motion).");

    api->Section("Hudless input");
    const char* invModes[] = { "Auto-detect", "Inverted (255 - x)", "Direct" };
    bool invDisabled[] = { false, false, false };
    int hudlessInvert = mfg::g_settings.hudlessInvert.load();
    if (api->Combo("HUDLess storage##mfg", &hudlessInvert, invModes, 3, invDisabled))
        mfg::g_settings.hudlessInvert.store(hudlessInvert);
    api->ItemTooltip("Auto-detect probes the first frames and negates such input before frame "
                     "generation, so the interpolator sees true colors. Applies after a game restart.");

    const int hudHealth = mfg::g_hudlessHealth.load();
    api->TextDisabled(hudHealth == 2
        ? "HUDLess capture broken (static/garbage): interpolating the backbuffer this session."
        : hudHealth == 1
            ? "HUDLess capture healthy this session."
            : "HUDLess capture not measured yet.");
    api->ItemTooltip("The provider probes the tagged HUDLessColor against the backbuffer "
                     "(content, structure and frame-to-frame tracking). An input that does "
                     "not track the scene is dropped for the rest of the session and the "
                     "frames are generated straight from the backbuffer.");

    const char* previewModes[] = { "Off", "Raw HUDLess on outputs" };
    bool previewDisabled[] = { false, false };
    int previewSel = mfg::g_settings.hudlessPreview.load() ? 1 : 0;
    if (api->Combo("HUDLess preview##mfg", &previewSel, previewModes, 2, previewDisabled))
        mfg::g_settings.hudlessPreview.store(previewSel == 1);
    api->ItemTooltip("Writes the raw tagged HUDLessColor texture to both frame generation "
                     "outputs, bypassing LSFG entirely: the on-screen picture becomes the "
                     "hudless input exactly as the game delivers it, every frame. Frames "
                     "without a tagged hudless fall back to the normal pipeline. "
                     "Applies instantly.");

    bool fileLog = mfg::g_settings.fileLog.load();
    if (api->Checkbox("Write hlsfg.log##mfg", &fileLog))
        mfg::g_settings.fileLog.store(fileLog);

    if (api->ItemDeactivatedAfterEdit())
    {
        api->SetInt(plugin, "Backend", mfg::g_settings.backend.load());
        api->SetInt(plugin, "Multiplier", mfg::g_settings.multiplier.load());
        api->SetInt(plugin, "Flow", mfg::g_settings.lsFlow.load());
        api->SetBool(plugin, "FileLog", mfg::g_settings.fileLog.load());
        api->SetBool(plugin, "UiComposite", mfg::g_settings.uiComposite.load());
        api->SetBool(plugin, "TemporalUi", mfg::g_settings.temporalUi.load());
        api->SetBool(plugin, "HudlessPreview", mfg::g_settings.hudlessPreview.load());
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

    mfg::g_settings.backend.store(api->GetInt(plugin, "Backend", static_cast<int>(mfg::BackendKind::Passthrough)));
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
    mfg::g_settings.hudlessPreview.store(api->GetBool(plugin, "HudlessPreview", false));
    mfg::g_settings.uiCutoff.store(api->GetInt(plugin, "UiCutoff", 2));
    mfg::g_settings.hudlessInvert.store(api->GetInt(plugin, "HudlessInvert", 0));

    api->RegisterTab(plugin, "HLSFG", "HUD-less FG: LSFG shaders below the GUI, UI recomposed from the hudless delta", Draw,
                     nullptr);

    if (OPTI_API_HAS(api, Log))
        api->Log(plugin, OptiLog_Info, "HLSFG provider: backend ready");

    MFG_LOG_INFO("OptiPluginInit (plugin api v%u), backend=%s multiplier=%d", api->version,
                 mfg::BackendName(mfg::g_settings.backend.load()), mfg::g_settings.multiplier.load());
    return true;
}
