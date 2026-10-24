#pragma once

#include "FrameGen/NeuralRenderingPassSettings.h"
#include "FrameGen/NeuralRenderingReconstruction.h"
#include "FrameGen/NeuralRenderingTuning.h"
#include "NeuralCombatPolicy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace TheosRenderPipeline::Appearance
{
// Every setting a preset can change: the Neural Rendering tab and sharpening.
struct Setup
{
    bool neural{};
    bool beforeUpscaling{true};
    int passes{1};
    NeuralRendering::CombatSettings combat{};
    NeuralRendering::Reconstruction reconstruction{};
    NeuralRendering::Tuning tuning{};
    // Passes 2 to 5; index 0 is Pass 2.
    NeuralRendering::ExtraPasses extra{};
    bool sharpening{true};
    float sharpness{0.3f};
    bool operator==(const Setup&) const = default;
};

// A preset setting. Look settings blend between weathers and times of day, and
// keep NR history while they change. Other settings switch once per change.
struct Field
{
    const char* key;
    bool look;
    float low, high;
    bool integral;
    float (*get)(const Setup&);
    void (*set)(Setup&, float);
};

#define TRP_APPEARANCE_FIELD(key, look, low, high, integral, member)                                          \
    Field{key, look, low, high, integral, [](const Setup& s) { return static_cast<float>(s.member); },          \
        [](Setup& s, float v) { s.member = static_cast<decltype(s.member)>(integral ? std::round(v) : v); }}

// Pass n's own settings (2 to 5), named as Pass 2's released keys.
#define TRP_APPEARANCE_PASS_FIELDS(n)                                                                          \
    TRP_APPEARANCE_FIELD("Pass" #n "SameAsPass1", false, 0, 1, true, extra[n - 2].linked),                    \
    TRP_APPEARANCE_FIELD("Pass" #n "InputScale", false, 0.25f, 1, false, extra[n - 2].inputScale),            \
    TRP_APPEARANCE_FIELD("Pass" #n "Network", false, 0, 1, true, extra[n - 2].preset),                        \
    TRP_APPEARANCE_FIELD("Pass" #n "Intensity", true, 0, 2, false, extra[n - 2].tuning.intensity),            \
    TRP_APPEARANCE_FIELD("Pass" #n "LocalTone", true, 0, 2, false, extra[n - 2].tuning.localToneStrength),    \
    TRP_APPEARANCE_FIELD("Pass" #n "LocalStructure", true, 0, 2, false, extra[n - 2].tuning.localStructureStrength), \
    TRP_APPEARANCE_FIELD("Pass" #n "Style", false, 0, 255, true, extra[n - 2].tuning.style),                  \
    TRP_APPEARANCE_FIELD("Pass" #n "SkinStructure", false, -1, 2, false, extra[n - 2].tuning.skinStructureStrength), \
    TRP_APPEARANCE_FIELD("Pass" #n "AutoSkinMask", false, 0, 1, true, extra[n - 2].tuning.useAutoSkinMask),   \
    TRP_APPEARANCE_FIELD("Pass" #n "UICorrection", false, 0, 1, true, extra[n - 2].tuning.uiCorrection)

inline const auto& Fields()
{
    // Keys are INI names; never rename them without migrating saved presets.
    static const std::array fields{
        TRP_APPEARANCE_FIELD("NeuralRendering", false, 0, 1, true, neural),
        TRP_APPEARANCE_FIELD("BeforeUpscaling", false, 0, 1, true, beforeUpscaling),
        TRP_APPEARANCE_FIELD("Passes", false, 1, NeuralRendering::kMaxNeuralPasses, true, passes),
        TRP_APPEARANCE_FIELD("OnePassInCombat", false, 0, 1, true, combat.inCombat),
        TRP_APPEARANCE_FIELD("OnePassWeaponsDrawn", false, 0, 1, true, combat.weaponsDrawn),
        TRP_APPEARANCE_FIELD("ReturnDelay", false, 0, 30, false, combat.recoverySeconds),
        TRP_APPEARANCE_FIELD("PeripheralCompression", false, 0, 1, true, reconstruction.peripheralCompression),
        TRP_APPEARANCE_FIELD("CombinedPreparation", false, 0, 1, true, reconstruction.fusedPreparation),
        // 0 every frame, 1 every other frame. Switches live without restarting NR.
        Field{"ModelRate", false, 0, 1, true,
            [](const Setup& s) { return static_cast<float>(static_cast<int>(s.reconstruction.modelRate)); },
            [](Setup& s, float v) { s.reconstruction.modelRate = static_cast<NeuralRendering::ModelRate>(static_cast<int>(std::round(v))); }},
        Field{"ReconstructionMethod", false, 0, 2, true,
            [](const Setup& s) { return static_cast<float>(static_cast<int>(s.reconstruction.method)); },
            [](Setup& s, float v) { s.reconstruction.method = static_cast<NeuralRendering::ResolveMethod>(static_cast<int>(std::round(v))); }},
        TRP_APPEARANCE_FIELD("EffectStrength", false, 0, 2, false, reconstruction.transferStrength),
        TRP_APPEARANCE_FIELD("ColourStrength", false, 0, 2, false, reconstruction.colourStrength),
        TRP_APPEARANCE_FIELD("BroadStrength", true, 0, 2, false, reconstruction.broadStrength),
        TRP_APPEARANCE_FIELD("DetailStrength", true, 0, 2, false, reconstruction.detailStrength),
        TRP_APPEARANCE_FIELD("MaximumRatio", false, 0.01f, 16, false, reconstruction.maxRatio),
        TRP_APPEARANCE_FIELD("WhitePoint", false, 0.0001f, 10000, false, reconstruction.whitePoint),
        TRP_APPEARANCE_FIELD("InputHDR", false, 0, 1, true, reconstruction.colorIsHDR),
        TRP_APPEARANCE_FIELD("AdaptiveExposure", false, 0, 1, true, reconstruction.adaptiveExposure),
        TRP_APPEARANCE_FIELD("ExposureBias", false, -2, 2, false, reconstruction.exposureBias),
        TRP_APPEARANCE_FIELD("Pass1InputScale", false, 0.25f, 1, false, reconstruction.inputScale),
        TRP_APPEARANCE_FIELD("Pass1Network", false, 0, 1, true, reconstruction.preset),
        TRP_APPEARANCE_FIELD("Pass1Intensity", true, 0, 2, false, tuning.intensity),
        TRP_APPEARANCE_FIELD("Pass1LocalTone", true, 0, 2, false, tuning.localToneStrength),
        TRP_APPEARANCE_FIELD("Pass1LocalStructure", true, 0, 2, false, tuning.localStructureStrength),
        TRP_APPEARANCE_FIELD("Pass1Style", false, 0, 255, true, tuning.style),
        TRP_APPEARANCE_FIELD("Pass1SkinStructure", false, -1, 2, false, tuning.skinStructureStrength),
        TRP_APPEARANCE_FIELD("Pass1AutoSkinMask", false, 0, 1, true, tuning.useAutoSkinMask),
        TRP_APPEARANCE_FIELD("Pass1UICorrection", false, 0, 1, true, tuning.uiCorrection),
        TRP_APPEARANCE_PASS_FIELDS(2),
        TRP_APPEARANCE_PASS_FIELDS(3),
        TRP_APPEARANCE_PASS_FIELDS(4),
        TRP_APPEARANCE_PASS_FIELDS(5),
        TRP_APPEARANCE_FIELD("SharpeningEnabled", false, 0, 1, true, sharpening),
        TRP_APPEARANCE_FIELD("Sharpness", true, 0, 1, false, sharpness),
    };
    static_assert(NeuralRendering::kMaxNeuralPasses == 5, "a field set for every extra pass");
    return fields;
}
#undef TRP_APPEARANCE_PASS_FIELDS
#undef TRP_APPEARANCE_FIELD

inline const Field* FindField(std::string_view key)
{
    for (const auto& field : Fields()) { if (key == field.key) { return &field; } }
    return nullptr;
}
// Registry position; changes are kept in this order so equal presets compare equal.
inline std::size_t FieldIndex(std::string_view key)
{
    const auto* field = FindField(key);
    return field ? static_cast<std::size_t>(field - Fields().data()) : Fields().size();
}
// Settings whose change retires and recreates the NR feature (see NeedsRecreation).
inline bool RecreatesNR(std::string_view key)
{
    // Every pass's raster and network, Pass1InputScale to Pass5Network.
    if (key.size() > 5 && key.starts_with("Pass") && key[4] >= '1' && key[4] <= '0' + NeuralRendering::kMaxNeuralPasses &&
        (key.substr(5) == "InputScale" || key.substr(5) == "Network")) { return true; }
    return key == "BeforeUpscaling" || key == "PeripheralCompression" || key == "CombinedPreparation" ||
        key == "ReconstructionMethod" || key == "InputHDR" || key == "WhitePoint";
}
inline float ClampField(const Field& field, float value)
{
    value = std::clamp(value, field.low, field.high);
    return field.integral ? std::round(value) : value;
}
inline Setup SanitizeSetup(Setup value)
{
    value.passes = std::clamp(value.passes, 1, NeuralRendering::kMaxNeuralPasses);
    value.combat = NeuralRendering::SanitizeCombatSettings(value.combat);
    value.reconstruction = NeuralRendering::SanitizeReconstruction(value.reconstruction);
    value.tuning = NeuralRendering::SanitizeBuild14Tuning(value.tuning);
    value.extra = NeuralRendering::SanitizeExtraPasses(value.extra);
    value.sharpness = std::isfinite(value.sharpness) ? std::clamp(value.sharpness, 0.0f, 1.0f) : 0.3f;
    return value;
}
// Look settings interpolate; other settings take the nearer endpoint.
inline Setup Blend(const Setup& a, const Setup& b, float weight)
{
    const float t = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 1.0f;
    Setup result = t < 0.5f ? a : b;
    for (const auto& field : Fields()) {
        if (field.look) { field.set(result, std::lerp(field.get(a), field.get(b), t)); }
    }
    return result;
}
}
