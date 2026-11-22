#pragma once
#include "SessionDegradation.h"
#include "StartupHints.h"

// Plain-language text for each degraded state: what is off, that the game
// keeps running, and what to do. Written for players, not for the log; the
// technical status stays in the entry's detail and in Copy details.
namespace TheosRenderPipeline::Degradation
{
    inline std::string Explain(const Entry& entry, const StartupHints::Facts& facts, StartupHints::Translate translate = nullptr)
    {
        const auto tr = [&](const char* text) { return std::string(translate ? translate(text) : text); };
        const auto contains = [&](std::string_view part) { return entry.detail.find(part) != std::string::npos; };
        std::string text;
        switch (entry.reason) {
        case Reason::FrameGenerationStartup: {
            text = tr("Frame generation could not start, so it is off this session. The game runs normally with upscaling, "
                      "but without frame generation, Reflex and Neural Rendering. Your saved settings are unchanged.");
            // The same advice the startup dialog gives for this failure.
            const auto hint = StartupHints::Hint(StartupHints::Classify(entry.detail), facts, translate);
            text += '\n';
            text += !hint.empty() ? hint : tr("Restart Skyrim to try again. If it keeps happening, press Copy details in the "
                                              "Advanced tab and include the text when you ask for help.") + '\n';
            break;
        }
        case Reason::MultiFrameGeneration:
            text = tr("Multipliers above x2 are off this session; frame generation continues at x2.");
            text += '\n';
            text += contains("NVIDIA app is overriding") ?
                tr("NVIDIA App replaces the frame-generation file for Skyrim. Open NVIDIA App > Graphics > Skyrim Special Edition, "
                   "set the DLSS Frame Generation overrides to \"Use the 3D application setting\", then restart Skyrim.") :
                tr("The NVIDIA frame-generation file is not a version TRP can unlock. Reinstall TRP's own files, remove any "
                   "replacement nvngx_dlssg.dll from other mods, then restart Skyrim.");
            text += '\n';
            break;
        case Reason::GuideRefused:
            text = tr("Frame generation and Neural Rendering are paused: an image they need from the game could not be copied. "
                      "The game keeps running normally without them.");
            text += '\n';
            text += tr("Restart Skyrim to bring them back. If it keeps happening, another graphics mod may be changing the game's "
                       "depth or motion images; press Copy details in the Advanced tab and include the text when you ask for help.");
            text += '\n';
            break;
        case Reason::NeuralRenderingStopped:
            text = tr("Neural Rendering stopped after an error and is off this session. The game keeps running without it, and "
                      "your saved settings are unchanged.");
            text += '\n';
            text += contains("Apply to retry") ?
                tr("Press Apply to try again. If it stops again, pick another runtime or network in this tab, or restart Skyrim.") :
                tr("Restart Skyrim to use Neural Rendering again.");
            text += '\n';
            break;
        case Reason::PackagedSettingsMissing:
            text = tr("TRP's settings file (SKSE/Plugins/TheosRenderPipeline.ini) is missing or cannot be read, so TRP uses its "
                      "built-in defaults this session. Your own saved settings still apply.");
            text += '\n';
            text += tr("Reinstall Theo's Render Pipeline to put the file back.");
            text += '\n';
            break;
        case Reason::LegacySettingsIgnored:
            text = tr("Some old settings are no longer used and are ignored this session:");
            text += ' '; text += entry.detail; text += ".\n";
            text += tr("Nothing else changed. To clear this message, delete those lines from "
                       "SKSE/Plugins/TheosRenderPipeline.User.ini (in MO2, check Overwrite).");
            text += '\n';
            break;
        case Reason::LoadingArtworkUnavailable:
            text = tr("\"Request loading artwork\" is off this session: TRP could not attach to that part of the game, usually "
                      "because another mod changed it. Everything else works.");
            text += '\n';
            break;
        }
        while (!text.empty() && text.back() == '\n') { text.pop_back(); }
        return text;
    }

    // A short label for a pipeline box whose feature is off for this reason.
    inline const char* BoxLabel(Reason reason)
    {
        switch (reason) {
        case Reason::MultiFrameGeneration: return "x2 only this session";
        case Reason::GuideRefused: return "Paused";
        default: return "Off this session";
        }
    }
}
