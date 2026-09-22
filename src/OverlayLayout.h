#pragma once

#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline::Overlay
{
struct Layout
{
    float x{40}, y{40};
    float width{1100}, height{720};
    float leftFraction{0.5f};
};

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
    return value;
}

// Refit on startup/display changes, not every frame while the user is dragging.
inline Layout FitLayout(Layout value, float displayWidth, float displayHeight)
{
    value = SanitizeLayout(value);
    displayWidth = std::isfinite(displayWidth) && displayWidth > 0 ? displayWidth : 1100.0f;
    displayHeight = std::isfinite(displayHeight) && displayHeight > 0 ? displayHeight : 720.0f;
    value.width = std::clamp(value.width, (std::min)(780.0f, displayWidth), displayWidth);
    value.height = std::clamp(value.height, (std::min)(560.0f, displayHeight), displayHeight);
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
}

inline constexpr float ColumnGap = 24.0f;
struct ColumnSizes
{
    float left, right;
};
inline ColumnSizes FitColumns(float width, float leftFraction)
{
    const float available = (std::max)(1.0f, width - ColumnGap);
    const float minLeft = (std::min)(260.0f, available * 0.4f);
    const float minRight = (std::min)(350.0f, available * 0.5f);
    const float left = std::clamp(available * ClampColumnFraction(leftFraction), minLeft, available - minRight);
    return {left, available - left};
}

inline float GraphHeight(float columnHeight)
{
    return std::clamp(columnHeight * 0.22f, 90.0f, 240.0f);
}

// Draws the divider without advancing the cursor; both children use the returned widths.
ColumnSizes DrawColumnSplitter(float width, float height, float& leftFraction);
} // namespace TheosRenderPipeline::Overlay
