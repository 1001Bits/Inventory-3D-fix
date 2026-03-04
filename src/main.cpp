#include "PCH.h"

#include "ConfigBase.h"
#include "ModBase.h"
#include "Inventory3DFix.h"

using namespace f4cf;

namespace
{
    // ── Simple config: one setting (iInventory3DFixMode) ──
    class Inv3DConfig : public ConfigBase
    {
    public:
        int fixMode = 16;  // Default: Forward render injection + alpha fixup

        Inv3DConfig() : ConfigBase("Inventory3DFix", "Data/F4SE/Plugins/Inventory3DFix.ini", 0) {}

        void load() override
        {
            // Bypass ConfigBase::load() which requires embedded Win32 resources.
            // Just try to read the INI directly; if it doesn't exist, use defaults.
            logger::info("[Inv3D] Loading config...");
            CSimpleIniA ini;
            SI_Error rc = ini.LoadFile(_iniFilePath.c_str());
            if (rc < 0) {
                logger::info("[Inv3D] No INI file found at '{}', using defaults (fixMode={})", _iniFilePath, fixMode);
                return;
            }
            loadIniConfigInternal(ini);
        }

    protected:
        void loadIniConfigInternal(const CSimpleIniA& ini) override
        {
            fixMode = static_cast<int>(ini.GetLongValue("General", "iInventory3DFixMode", fixMode));
            logger::info("[Inv3D] Config: iInventory3DFixMode = {}", fixMode);
        }
    };

    static Inv3DConfig g_config;

    // ── Mod class ──
    class Inventory3DFixMod : public ModBase
    {
    public:
        Inventory3DFixMod() :
            ModBase(Settings("Inventory3DFix", "1.0.0", &g_config, 128, true))
        {
        }

    protected:
        void onModLoaded(const F4SE::LoadInterface*) override
        {
            logger::info("Inventory3DFix loaded — installing hooks");
            inv3d::Inventory3DFix::GetSingleton().Install(g_config.fixMode);
        }

        void onGameLoaded() override
        {
            inv3d::Inventory3DFix::GetSingleton().LogRendererState();
        }

        void onGameSessionLoaded() override {}
        void onFrameUpdate() override
        {
            inv3d::Inventory3DFix::GetSingleton().OnFrameUpdate();
        }
    };
}

static Inventory3DFixMod g_inv3dMod;

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_skse, F4SE::PluginInfo* a_info)
{
    g_mod = &g_inv3dMod;
    return g_mod->onF4SEPluginQuery(a_skse, a_info);
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    return g_mod->onF4SEPluginLoad(a_f4se);
}
