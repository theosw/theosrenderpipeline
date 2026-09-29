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
    NeuralRendering::SecondPassSettings second{};
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

inline const auto& Fields()
{
    // Keys are INI names; never rename them without migrating saved presets.
    static const std::array fields{
        TRP_APPEARANCE_FIELD("NeuralRendering", false, 0, 1, true, neural),
        TRP_APPEARANCE_FIELD("BeforeUpscaling", false, 0, 1, true, beforeUpscaling),
        TRP_APPEARANCE_FIELD("Passes", false, 1, 2, true, passes),
        TRP_APPEARANCE_FIELD("OnePassInCombat", false, 0, 1, true, combat.inCombat),
        TRP_APPEARANCE_FIELD("OnePassWeaponsDrawn", false, 0, 1, true, combat.weaponsDrawn),
        TRP_APPEARANCE_FIELD("ReturnDelay", false, 0, 30, false, combat.recoverySeconds),
        TRP_APPEARANCE_FIELD("PeripheralCompression", false, 0, 1, true, reconstruction.peripheralCompression),
        TRP_APPEARANCE_FIELD("CombinedPreparation", false, 0, 1, true, reconstruction.fusedPreparation),
        Field{"ReconstructionMethod", false, 0, 2, true,
            [](const Setup& s) { return static_cast<float>(static_cast<int>(s.reconstruction.method)); },
            [](Setup& s, float v) { s.reconstruction.method = static_cast<NeuralRendering::ResolveMethod>(static_cast<int>(std::round(v))); }},
        TRP_APPEARANCE_FIELD("EffectStrength", false, 0, 2, false, reconstruction.transferStrength),
        TRP_APPEARANCE_FIELD("ColourStrength", false, 0, 2, false, reconstruction.colourStrength),
        TRP_APPEARANCE_FIELD("MaximumRatio", false, 0.01f, 16, false, reconstruction.maxRatio),
        TRP_APPEARANCE_FIELD("WhitePoint", false, 0.0001f, 10000, false, reconstruction.whitePoint),
        TRP_APPEARANCE_FIELD("InputHDR", false, 0, 1, true, reconstruction.colorIsHDR),
        TRP_APPEARANCE_FIELD("Pass1InputScale", false, 0.25f, 1, false, reconstruction.inputScale),
        TRP_APPEARANCE_FIELD("Pass1Network", false, 0, 1, true, reconstruction.preset),
        TRP_APPEARANCE_FIELD("Pass1Intensity", true, 0, 2, false, tuning.intensity),
        TRP_APPEARANCE_FIELD("Pass1LocalTone", true, 0, 2, false, tuning.localToneStrength),
        TRP_APPEARANCE_FIELD("Pass1LocalStructure", true, 0, 2, false, tuning.localStructureStrength),
        TRP_APPEARANCE_FIELD("Pass1Style", false, 0, 7, true, tuning.style),
        TRP_APPEARANCE_FIELD("Pass1SkinStructure", false, -1, 2, false, tuning.skinStructureStrength),
        TRP_APPEARANCE_FIELD("Pass1AutoSkinMask", false, 0, 1, true, tuning.useAutoSkinMask),
        TRP_APPEARANCE_FIELD("Pass1UICorrection", false, 0, 1, true, tuning.uiCorrection),
        TRP_APPEARANCE_FIELD("Pass2SameAsPass1", false, 0, 1, true, second.linked),
        TRP_APPEARANCE_FIELD("Pass2InputScale", false, 0.25f, 1, false, second.inputScale),
        TRP_APPEARANCE_FIELD("Pass2Network", false, 0, 1, true, second.preset),
        TRP_APPEARANCE_FIELD("Pass2Intensity", true, 0, 2, false, second.tuning.intensity),
        TRP_APPEARANCE_FIELD("Pass2LocalTone", true, 0, 2, false, second.tuning.localToneStrength),
        TRP_APPEARANCE_FIELD("Pass2LocalStructure", true, 0, 2, false, second.tuning.localStructureStrength),
        TRP_APPEARANCE_FIELD("Pass2Style", false, 0, 7, true, second.tuning.style),
        TRP_APPEARANCE_FIELD("Pass2SkinStructure", false, -1, 2, false, second.tuning.skinStructureStrength),
        TRP_APPEARANCE_FIELD("Pass2AutoSkinMask", false, 0, 1, true, second.tuning.useAutoSkinMask),
        TRP_APPEARANCE_FIELD("Pass2UICorrection", false, 0, 1, true, second.tuning.uiCorrection),
        TRP_APPEARANCE_FIELD("SharpeningEnabled", false, 0, 1, true, sharpening),
        TRP_APPEARANCE_FIELD("Sharpness", true, 0, 1, false, sharpness),
    };
    return fields;
}
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
    return key == "BeforeUpscaling" || key == "Pass1InputScale" || key == "Pass1Network" || key == "Pass2InputScale" ||
        key == "Pass2Network" || key == "PeripheralCompression" || key == "CombinedPreparation" ||
        key == "ReconstructionMethod" || key == "InputHDR" || key == "WhitePoint";
}
inline float ClampField(const Field& field, float value)
{
    value = std::clamp(value, field.low, field.high);
    return field.integral ? std::round(value) : value;
}
inline Setup SanitizeSetup(Setup value)
{
    value.passes = std::clamp(value.passes, 1, 2);
    value.combat = NeuralRendering::SanitizeCombatSettings(value.combat);
    value.reconstruction = NeuralRendering::SanitizeReconstruction(value.reconstruction);
    value.tuning = NeuralRendering::SanitizeBuild14Tuning(value.tuning);
    value.second = NeuralRendering::SanitizeSecondPass(value.second);
    value.second.tuning = NeuralRendering::SanitizeBuild14Tuning(value.second.tuning);
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
