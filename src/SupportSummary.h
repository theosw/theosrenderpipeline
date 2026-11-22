#pragma once
#include "SessionDegradation.h"
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// The text "Copy details" puts on the clipboard: what a bug report needs, in
// plain English so the developer can read any player's paste. It repeats only
// what TheosRenderPipeline.log already records, with the Windows user folder
// shortened to %USERPROFILE%.
namespace TheosRenderPipeline::Support
{
    struct Module
    {
        std::string name, version, path; // version empty when the file has none
    };

    struct Summary
    {
        std::string release, edition, source; // TRP
        std::string skyrim;                   // game runtime version
        std::string gpu, driver;              // rendering adapter
        // What is in effect now, one line each.
        std::string upscaling, frameGeneration, neuralRendering;
        std::vector<std::string> nvidiaApp;   // NVIDIA App and driver override state
        std::vector<Degradation::Entry> degraded;
        bool steppedAside{};                  // TRP declined presentation (NVIDIA Smooth Motion)
        std::vector<Module> modules;          // runtime files loaded in the game
        std::string userProfile;              // e.g. C:\Users\Name; replaced in the text
    };

    // Replaces the user's profile folder, in either slash style and any case.
    inline std::string RedactUserFolder(std::string text, std::string_view profile)
    {
        if (profile.size() < 4) { return text; }
        const auto same = [](char a, char b) {
            const auto fold = [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            return fold(a) == fold(b);
        };
        constexpr std::string_view replacement = "%USERPROFILE%";
        for (std::size_t at = 0; at + profile.size() <= text.size();) {
            bool match = true;
            for (std::size_t i = 0; match && i < profile.size(); ++i) { match = same(text[at + i], profile[i]); }
            // Whole folder name only: C:\Users\Al must not match C:\Users\Alice.
            const auto end = at + profile.size();
            if (match && (end == text.size() || text[end] == '\\' || text[end] == '/')) {
                text.replace(at, profile.size(), replacement);
                at += replacement.size();
            } else { ++at; }
        }
        return text;
    }

    inline std::string Format(const Summary& summary)
    {
        const auto known = [](const std::string& value) { return value.empty() ? std::string("unknown") : value; };
        std::string text = "Theo's Render Pipeline - support details\n";
        text += "TRP: " + known(summary.release) + " | " + known(summary.edition) + " | source " + known(summary.source) + '\n';
        text += "Skyrim: " + known(summary.skyrim) + '\n';
        text += "GPU: " + known(summary.gpu) + " | driver " + known(summary.driver) + '\n';
        text += "Upscaling: " + known(summary.upscaling) + '\n';
        text += "Frame generation: " + known(summary.frameGeneration) + '\n';
        text += "Neural Rendering: " + known(summary.neuralRendering) + '\n';
        text += "NVIDIA App / driver:";
        if (summary.nvidiaApp.empty()) { text += " not read\n"; }
        else if (summary.nvidiaApp.size() == 1) { text += ' ' + summary.nvidiaApp.front() + '\n'; }
        else { text += '\n'; for (const auto& line : summary.nvidiaApp) { text += "- " + line + '\n'; } }

        bool active = summary.steppedAside, resolved = false;
        for (const auto& entry : summary.degraded) { (entry.active ? active : resolved) = true; }
        const auto list = [&](bool wanted) {
            for (const auto& entry : summary.degraded) {
                if (entry.active != wanted) { continue; }
                text += "- "; text += Degradation::Name(Degradation::FeatureOf(entry.reason));
                text += ": "; text += Degradation::Name(entry.reason);
                if (!entry.detail.empty()) { text += " | " + entry.detail; }
                text += '\n';
            }
        };
        if (active) {
            text += "Off or reduced this session:\n";
            if (summary.steppedAside) { text += "- everything: TRP stepped aside for NVIDIA Smooth Motion; the game presents itself\n"; }
            list(true);
        }
        else { text += "Off or reduced this session: nothing\n"; }
        if (resolved) { text += "Earlier this session, since restored:\n"; list(false); }

        text += "Loaded runtime files:";
        if (summary.modules.empty()) { text += " none\n"; }
        else {
            text += '\n';
            for (const auto& module : summary.modules) {
                text += "- " + module.name + (module.version.empty() ? std::string() : " " + module.version) + " | " + module.path + '\n';
            }
        }
        return RedactUserFolder(std::move(text), summary.userProfile);
    }
}
