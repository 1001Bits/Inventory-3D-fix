#include "PCH.h"

#include "ConfigBase.h"
#include "ModBase.h"
#include "Inventory3DFix.h"

using namespace f4cf;

namespace
{
    class Inv3DConfig : public ConfigBase
    {
    public:
        Inv3DConfig() : ConfigBase("Inventory3DFix", "Data/F4SE/Plugins/Inventory3DFix.ini", 0) {}

        void load() override
        {
            logger::info("[Inv3D] Config: no settings, fix always active");
        }

    protected:
        void loadIniConfigInternal(const CSimpleIniA&) override {}
    };

    static Inv3DConfig g_config;

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
            inv3d::Inventory3DFix::GetSingleton().Install();
        }

        void onGameLoaded() override {}
        void onGameSessionLoaded() override {}
        void onFrameUpdate() override {}
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
