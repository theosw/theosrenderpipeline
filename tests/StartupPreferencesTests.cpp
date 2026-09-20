#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/SourceDLSSGMFG.h"
#include <SimpleIni.h>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
void CheckRoute(bool requested, midpoint_fix::AdapterKind adapter,
    TheosRenderPipeline::SourceDLSSG::MFGRoute expected)
{
    TheosRenderPipeline::SourceDLSSG::MFGSnapshot route;
    route.requested = requested;
    Require(route.SelectRoute(adapter) && route.route == expected, "wrong physical-adapter route");
    Require(route.adapter == adapter, "diagnostics lost the observed adapter");
    Require(!route.Ready(), "route selection alone must not establish patch readiness");
}
}

int main(int argc, char** argv)
{
    try {
        Require(argc == 2, "expected packaged INI path");
        auto& owner = *SourceFrameGeneration::GetSingleton();
        CSimpleIniA packaged;
        Require(packaged.LoadFile(argv[1]) >= 0, "packaged INI must load");
        owner.LoadStartupPreferences(packaged);
        Require(owner.settings.sourceDLSSGMFGUnlock && owner.settings.sourceDLSSGMFGUnlockPresent,
            "package must explicitly enable compatibility");

        CSimpleIniA older;
        older.SetBoolValue("FrameGeneration", "Enabled", false);
        older.SetLongValue("SourceDLSSG", "GeneratedFrames", 3);
        older.SetBoolValue("SourceDLSSG", "NeuralRenderingEnabled", true);
        owner.LoadStartupPreferences(older);
        Require(owner.settings.sourceDLSSGMFGUnlock && !owner.settings.sourceDLSSGMFGUnlockPresent,
            "missing key must use packaged true default");
        Require(!owner.RuntimeInterpolationRequested(), "compatibility must not enable interpolation");
        Require(owner.settings.sourceDLSSG.generation.generatedFrames == 3 && owner.settings.sourceDLSSG.neuralEnabled,
            "compatibility migration must retain MFG and NR preferences");
        owner.StoreCompatibilityPreference(older);
        std::string saved;
        Require(older.Save(saved) >= 0, "serialize migrated INI");
        CSimpleIniA reloaded;
        Require(reloaded.LoadData(saved.c_str()) >= 0, "reload migrated INI");
        owner.LoadStartupPreferences(reloaded);
        Require(owner.settings.sourceDLSSGMFGUnlock && owner.settings.sourceDLSSGMFGUnlockPresent,
            "saving must persist the missing startup preference");

        for (const char* value : {"false", "0", "off", "true", "1", "on"}) {
            CSimpleIniA explicitIni;
            explicitIni.SetValue("Experimental", "SourceDLSSGMFGUnlock", value);
            const bool expected = explicitIni.GetBoolValue("Experimental", "SourceDLSSGMFGUnlock", true);
            owner.LoadStartupPreferences(explicitIni);
            Require(owner.settings.sourceDLSSGMFGUnlock == expected && owner.settings.sourceDLSSGMFGUnlockPresent,
                "explicit startup preference must be respected");
            owner.StoreCompatibilityPreference(explicitIni);
            Require(std::string(explicitIni.GetValue("Experimental", "SourceDLSSGMFGUnlock")) == value,
                "save must retain explicit preference spelling/value");
        }
        reloaded.SetBoolValue("Experimental", "SourceDLSSGMFGUnlock", true);
        owner.LoadStartupPreferences(reloaded);
        reloaded.SetBoolValue("Experimental", "SourceDLSSGMFGUnlock", false);
        owner.StoreCompatibilityPreference(reloaded);
        Require(!reloaded.GetBoolValue("Experimental", "SourceDLSSGMFGUnlock", true),
            "save must not replace an on-disk opt-out edited during this session");

        using enum midpoint_fix::AdapterKind;
        using enum TheosRenderPipeline::SourceDLSSG::MFGRoute;
        CheckRoute(true, Ampere, AmpereUnlock);
        CheckRoute(true, Ada, AdaUnlock);
        CheckRoute(true, Other, Native);
        for (const auto adapter : {Ampere, Ada, Other, Unavailable}) { CheckRoute(false, adapter, Native); }
        TheosRenderPipeline::SourceDLSSG::MFGSnapshot unresolved;
        unresolved.requested = true;
        Require(!unresolved.SelectRoute(Unavailable), "unidentified GPU cannot receive compatibility");
        std::cout << "PASS: startup defaults, persistence, explicit opt-out and adapter routing\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
