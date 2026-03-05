#include "PCH.h"

#include "ConfigBase.h"
#include "ModBase.h"
#include "Inventory3DFix.h"

using namespace f4cf;

namespace
{
    class Inventory3DFixMod : public ModBase
    {
    public:
        Inventory3DFixMod() :
            ModBase(Settings("Inventory3DFix", "1.0.0", nullptr, 128, true))
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
