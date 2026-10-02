#pragma once

#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline::Overlay
{
// Geometry is stored in 1x units and multiplied by the effective UI scale.
struct Layout
{
    float x{40}, y{40};
    float width{1100}, height{720};
    float leftFraction{0.5f};
    float uiScale{0}; // 0 selects the automatic scale.
};

inline constexpr float MinUIScale = 0.75f;
inline constexpr float MaxUIScale = 3.0f;
inline constexpr float MinWindowWidth = 780.0f;
inline constexpr float MinWindowHeight = 560.0f;

inline float SanitizeUIScale(float value)
{
    if (!std::isfinite(value) || value <= 0)
        return 0;
    return std::clamp(value, MinUIScale, MaxUIScale);
}

// Automatic scaling treats 1080 output lines as 1x. Either choice is limited so
// the minimum window still fits the output.
inline float ResolveUIScale(float requested, float displayWidth, float displayHeight)
{
    requested = SanitizeUIScale(requested);
    if (!std::isfinite(displayWidth) || displayWidth <= 0 || !std::isfinite(displayHeight) || displayHeight <= 0)
        return requested > 0 ? requested : 1.0f;
    const float automatic = std::clamp(displayHeight / 1080.0f, 1.0f, MaxUIScale);
    const float fits = (std::max)(MinUIScale, (std::min)(displayWidth / MinWindowWidth, displayHeight / MinWindowHeight));
    return (std::min)(requested > 0 ? requested : automatic, fits);
}

// The header's size buttons move to the next 25% step from the current size,
// so an automatic 183% becomes 200% or 175%.
inline constexpr float UIScaleStep = 0.25f;
inline float StepUIScale(float current, int direction)
{
    if (!std::isfinite(current) || current <= 0)
        current = 1.0f;
    const float steps = current / UIScaleStep;
    const float next = direction > 0 ? (std::floor(steps + 0.001f) + 1.0f) * UIScaleStep
                                     : (std::ceil(steps - 0.001f) - 1.0f) * UIScaleStep;
    return std::clamp(next, MinUIScale, MaxUIScale);
}

inline float ClampColumnFraction(float value)
{
    if (!std::isfinite(value) || value <= 0 || value >= 1)
        return 0.5f;
    return std::clamp(value, 0.2f, 0.8f);
}

inline Layout SanitizeLayout(Layout value)
{
    const Layout defaults;
    if (!std::isfinite(value.x))
        value.x = defaults.x;
    if (!std::isfinite(value.y))
        value.y = defaults.y;
    if (!std::isfinite(value.width) || value.width <= 0)
        value.width = defaults.width;
    if (!std::isfinite(value.height) || value.height <= 0)
        value.height = defaults.height;
    value.width = std::clamp(value.width, 1.0f, 32768.0f);
    value.height = std::clamp(value.height, 1.0f, 32768.0f);
    value.leftFraction = ClampColumnFraction(value.leftFraction);
    value.uiScale = SanitizeUIScale(value.uiScale);
    return value;
}

// Refit on startup/display/scale changes, not every frame while the user is dragging.
// Takes 1x geometry and returns screen pixels.
inline Layout FitLayout(Layout value, float displayWidth, float displayHeight, float scale = 1.0f)
{
    value = SanitizeLayout(value);
    scale = std::isfinite(scale) && scale > 0 ? scale : 1.0f;
    value.x *= scale;
    value.y *= scale;
    value.width *= scale;
    value.height *= scale;
    displayWidth = std::isfinite(displayWidth) && displayWidth > 0 ? displayWidth : 1100.0f;
    displayHeight = std::isfinite(displayHeight) && displayHeight > 0 ? displayHeight : 720.0f;
    value.width = std::clamp(value.width, (std::min)(MinWindowWidth * scale, displayWidth), displayWidth);
    value.height = std::clamp(value.height, (std::min)(MinWindowHeight * scale, displayHeight), displayHeight);
    value.x = std::clamp(value.x, 0.0f, displayWidth - value.width);
    value.y = std::clamp(value.y, 0.0f, displayHeight - value.height);
    return value;
}

template <class Ini> Layout LoadLayout(const Ini& ini)
{
    Layout value;
    value.x = static_cast<float>(ini.GetDoubleValue("Overlay", "WindowX", value.x));
    value.y = static_cast<float>(ini.GetDoubleValue("Overlay", "WindowY", value.y));
    value.width = static_cast<float>(ini.GetDoubleValue("Overlay", "WindowWidth", value.width));
    value.height = static_cast<float>(ini.GetDoubleValue("Overlay", "WindowHeight", value.height));
    value.leftFraction = static_cast<float>(ini.GetDoubleValue("Overlay", "LeftColumnFraction", value.leftFraction));
    value.uiScale = static_cast<float>(ini.GetDoubleValue("Overlay", "UIScale", value.uiScale));
    return SanitizeLayout(value);
}

template <class Ini> void StoreLayout(Ini& ini, Layout value)
{
    value = SanitizeLayout(value);
    ini.SetDoubleValue("Overlay", "WindowX", value.x);
    ini.SetDoubleValue("Overlay", "WindowY", value.y);
    ini.SetDoubleValue("Overlay", "WindowWidth", value.width);
    ini.SetDoubleValue("Overlay", "WindowHeight", value.height);
    ini.SetDoubleValue("Overlay", "LeftColumnFraction", value.leftFraction);
    ini.SetDoubleValue("Overlay", "UIScale", value.uiScale);
}

inline constexpr float ColumnGap = 24.0f;
struct ColumnSizes
{
    float left, right;
};
inline ColumnSizes FitColumns(float width, float leftFraction, float scale = 1.0f)
{
    const float available = (std::max)(1.0f, width - ColumnGap * scale);
    const float minLeft = (std::min)(260.0f * scale, available * 0.4f);
    const float minRight = (std::min)(350.0f * scale, available * 0.5f);
    const float left = std::clamp(available * ClampColumnFraction(leftFraction), minLeft, available - minRight);
    return {left, available - left};
}

inline float GraphHeight(float columnHeight, float scale = 1.0f)
{
    return std::clamp(columnHeight * 0.22f, 90.0f * scale, 240.0f * scale);
}

// Draws the divider without advancing the cursor; both children use the returned widths.
ColumnSizes DrawColumnSplitter(float width, float height, float& leftFraction, float scale = 1.0f);
} // namespace TheosRenderPipeline::Overlay
