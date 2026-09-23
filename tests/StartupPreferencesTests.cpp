#include "FrameGen/SourceFrameGeneration.h"
#include "FrameGen/SourceDLSSGMFG.h"
#include <SimpleIni.h>
#include "OverlayHotkeys.h"
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
        using TheosRenderPipeline::Overlay::LoadNRHotkeysEnabled;
        Require(!LoadNRHotkeysEnabled(packaged), "packaged NR shortcuts default off");
        CSimpleIniA hotkeys;
        Require(!LoadNRHotkeysEnabled(hotkeys), "old INIs without the key disable NR shortcuts");
        hotkeys.SetBoolValue("Hotkeys", "EnableNRHotkeys", true);
        Require(LoadNRHotkeysEnabled(hotkeys), "explicit NR shortcut opt-in respected");
        hotkeys.SetBoolValue("Hotkeys", "EnableNRHotkeys", false);
        Require(!LoadNRHotkeysEnabled(hotkeys), "explicit NR shortcut opt-out respected");
        using namespace TheosRenderPipeline::NeuralRendering;
        Require(!LoadReconstruction(packaged,"SourceDLSSG").peripheralCompression,
            "peripheral experiment must remain off in packages");
        CSimpleIniA spatial;
        Require(!LoadReconstruction(spatial,"SourceDLSSG").peripheralCompression,"old INIs must retain uniform NR");
        auto reconstruction=LoadReconstruction(spatial,"SourceDLSSG");
        Require(!reconstruction.fusedPreparation,"missing preparation key defaults off");
        Require(!LoadReconstruction(packaged,"SourceDLSSG").fusedPreparation,"combined preparation remains off in packages");
        reconstruction.fusedPreparation=true;
        reconstruction.peripheralCompression=true; reconstruction.inputScale=.5f;
        StoreReconstruction(spatial,"SourceDLSSG",reconstruction);
        Require(LoadReconstruction(spatial,"SourceDLSSG")==reconstruction,"peripheral layout survives save/load");
        auto separate=reconstruction; separate.fusedPreparation=false;
        Require(!SameReconstructionResources(reconstruction,separate),"preparation toggle requires retired recreation");
        spatial.SetBoolValue("SourceDLSSG","NRFusedPreparation",false);
        Require(!LoadReconstruction(spatial,"SourceDLSSG").fusedPreparation,"explicit preparation opt-out wins");
        spatial.SetBoolValue("SourceDLSSG","NRPeripheralCompression",false);
        Require(!LoadReconstruction(spatial,"SourceDLSSG").peripheralCompression,"explicit spatial opt-out wins");
        owner.LoadStartupPreferences(packaged);
        const std::string configuredStreamline = packaged.GetValue("Experimental", "SourceDLSSGStreamlineDirectory", "");
        const std::string configuredNeural = packaged.GetValue("Experimental", "NeuralRenderingRuntimePath", "");
        const std::filesystem::path firstRoot = "C:/First Game/Data/SKSE/Plugins";
        const std::filesystem::path movedRoot = "D:/Moved Game/Data/SKSE/Plugins";
        Require(!configuredStreamline.empty() && !configuredNeural.empty(), "packaged runtime paths present");
        owner.ResolveRuntimePaths(firstRoot);
        Require(std::filesystem::path(owner.settings.sourceDLSSGStreamlineDirectory).is_absolute() &&
            std::filesystem::path(owner.settings.neuralRenderingRuntimePath).is_absolute(), "runtime uses resolved paths");
        // Exercise the same load -> resolve -> save -> reload sequence as startup
        // followed by Save as default, including a new settings file.
        for (const bool seed : {false, true}) {
            CSimpleIniA savedPaths;
            if (!seed) { Require(savedPaths.LoadFile(argv[1]) >= 0, "reload packaged settings"); }
            owner.StoreRuntimePaths(savedPaths);
            std::string serialized;
            Require(savedPaths.Save(serialized) >= 0, "save runtime paths");
            CSimpleIniA pathsReloaded;
            Require(pathsReloaded.LoadData(serialized.c_str()) >= 0, "reload saved paths");
            Require(std::string(pathsReloaded.GetValue("Experimental", "SourceDLSSGStreamlineDirectory")) == configuredStreamline &&
                std::string(pathsReloaded.GetValue("Experimental", "NeuralRenderingRuntimePath")) == configuredNeural,
                "saving must preserve packaged relative spellings");
            owner.LoadStartupPreferences(pathsReloaded);
            owner.ResolveRuntimePaths(movedRoot);
            Require(owner.settings.sourceDLSSGStreamlineDirectory == (movedRoot / configuredStreamline).lexically_normal().string() &&
                owner.settings.neuralRenderingRuntimePath == (movedRoot / configuredNeural).lexically_normal().string(),
                "saved relative paths follow a moved installation");
        }
        CSimpleIniA absolutePaths;
        absolutePaths.SetValue("Experimental", "SourceDLSSGStreamlineDirectory", "E:/Custom/Streamline");
        absolutePaths.SetValue("Experimental", "NeuralRenderingRuntimePath", "E:/Custom/nvngx_dlssnr.dll");
        owner.LoadStartupPreferences(absolutePaths);
        owner.ResolveRuntimePaths(firstRoot);
        CSimpleIniA absoluteSaved;
        owner.StoreRuntimePaths(absoluteSaved);
        Require(owner.settings.sourceDLSSGStreamlineDirectory == "E:/Custom/Streamline" &&
            std::string(absoluteSaved.GetValue("Experimental", "NeuralRenderingRuntimePath")) == "E:/Custom/nvngx_dlssnr.dll",
            "intentional absolute runtime paths preserved");
        absoluteSaved.SetValue("Experimental", "NeuralRenderingRuntimePath", "F:/Edited/nvngx_dlssnr.dll");
        owner.StoreRuntimePaths(absoluteSaved);
        Require(std::string(absoluteSaved.GetValue("Experimental", "NeuralRenderingRuntimePath")) == "F:/Edited/nvngx_dlssnr.dll",
            "menu save must not undo a startup path edited on disk");
        owner.LoadStartupPreferences(packaged);
        Require(owner.settings.sourceDLSSGMFGUnlock && owner.settings.sourceDLSSGMFGUnlockPresent,
            "package must explicitly enable compatibility");

        CSimpleIniA composition;
        composition.SetLongValue("Experimental", "NativeUICompositionMode", 1);
        owner.StoreUIComposition(composition);
        Require(composition.GetLongValue("Experimental", "NativeUICompositionMode", -1) == 1,
            "menu save preserves explicit composition edit made after startup");
        composition.Delete("Experimental", "NativeUICompositionMode");
        composition.SetLongValue("Experimental", "PureDarkHUDFixMethod", 1);
        owner.StoreUIComposition(composition);
        Require(composition.GetLongValue("Experimental", "NativeUICompositionMode", -1) == 1 &&
            !composition.GetValue("Experimental", "PureDarkHUDFixMethod", nullptr),
            "migrate legacy on-disk composition choice before removing old key");
        composition.Delete("Experimental", "NativeUICompositionMode");
        owner.StoreUIComposition(composition);
        Require(composition.GetLongValue("Experimental", "NativeUICompositionMode", -1) == owner.settings.nativeUICompositionMode,
            "seed missing composition choice from startup snapshot");

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
        CheckRoute(true, Turing, TuringUnlock);
        CheckRoute(false, Turing, Native);
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
