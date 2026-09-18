#pragma once

#include <imgui.h>
#include <array>

namespace TheosRenderPipeline::Overlay
{
enum class SettingsPage
{
    None,
    Image,
    NeuralRendering,
    FrameGeneration
};

struct PipelineStage
{
    const char* title{};
    const char* detail{};
    const char* tooltip{};
    SettingsPage page{SettingsPage::None};
    bool muted{};
};

struct PipelineDiagram
{
#if defined(TRP_NO_NEURAL_RENDERING)
    static constexpr std::size_t StageCount = 4;
#else
    static constexpr std::size_t StageCount = 5;
#endif
    static constexpr std::size_t GenerationStage = StageCount - 2;
    std::array<PipelineStage, StageCount> stages;
    const char* nativeDetail{};
    bool nativeUI{};
    const char* status{};
    ImVec4 statusColor{};
};

// Draws the applied rendering path. A click requests navigation, never a setting change.
SettingsPage DrawPipelineDiagram(const PipelineDiagram& diagram);
} // namespace TheosRenderPipeline::Overlay
