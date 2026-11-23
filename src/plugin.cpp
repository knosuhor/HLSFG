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
    api->Combo("Backend##mfg", &backend, backends, 3, disabled);
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
                          "and writes mfg-provider.log next to the dll.");

    api->Section("Frame generation");
    int multiplier = mfg::g_settings.multiplier.load();
    if (api->SliderInt("Multiplier##mfg", &multiplier, 2, 4, "%dx"))
    {
        mfg::g_settings.multiplier.store(multiplier);
        api->ItemTooltip("Reported to OptiScaler as DLSSG.MultiFrameCountMax. "
                         "Applies after a game restart.");
    }

    int flow = mfg::g_settings.lsFlow.load();
    if (api->SliderInt("Flow scale##mfg", &flow, 1, 4, "1/%d"))
    {
        mfg::g_settings.lsFlow.store(flow);
        api->ItemTooltip("LSFG optical flow resolution divisor (lower = higher quality, "
                         "higher = faster). Matches the Lossless Scaling flow scale. "
                         "Applies after a game restart.");
    }

    bool fileLog = mfg::g_settings.fileLog.load();
    if (api->Checkbox("Write mfg-provider.log##mfg", &fileLog))
        mfg::g_settings.fileLog.store(fileLog);

    if (api->ItemDeactivatedAfterEdit())
    {
        api->SetInt(plugin, "Backend", mfg::g_settings.backend.load());
        api->SetInt(plugin, "Multiplier", mfg::g_settings.multiplier.load());
        api->SetInt(plugin, "Flow", mfg::g_settings.lsFlow.load());
        api->SetBool(plugin, "FileLog", mfg::g_settings.fileLog.load());
        api->SaveSettings(plugin);
        if (OPTI_API_HAS(api, Log))
            api->Log(plugin, OptiLog_Info, "MFG provider settings saved");
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
    mfg::g_settings.multiplier.store(api->GetInt(plugin, "Multiplier", 2));
    mfg::g_settings.lsFlow.store(api->GetInt(plugin, "Flow", 2));
    mfg::g_settings.fileLog.store(api->GetBool(plugin, "FileLog", true));

    api->RegisterTab(plugin, "MFG Provider", "Multi-frame generation provider (phases 0-1)", Draw,
                     nullptr);

    if (OPTI_API_HAS(api, Log))
        api->Log(plugin, OptiLog_Info, "MFG provider: backend ready");

    MFG_LOG_INFO("OptiPluginInit (plugin api v%u), backend=%s multiplier=%d", api->version,
                 mfg::BackendName(mfg::g_settings.backend.load()), mfg::g_settings.multiplier.load());
    return true;
}
