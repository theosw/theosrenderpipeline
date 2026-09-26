#include "OverlayLayout.h"
#include <SimpleIni.h>
#include <imgui.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace TheosRenderPipeline::Overlay;
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
bool Near(float a, float b)
{
    return std::abs(a - b) < 0.001f;
}

void Settings()
{
    CSimpleIniA ini;
    Require(ini.LoadData("[Settings]\nQualityLevel=4\n[SourceDLSSG]\nNRPasses=2\n[Unrecognized]\nKeep=hello\n") >= 0,
            "legacy settings load");
    const auto defaults = LoadLayout(ini);
    Require(defaults.width == 1100 && defaults.height == 720 && defaults.leftFraction == 0.5f,
            "missing layout keys keep the ordinary menu defaults");
    const Layout edited{152, 86, 1450, 920, 0.62f};
    StoreLayout(ini, edited);
    std::string serialized;
    Require(ini.Save(serialized) >= 0, "serialize INI with layout");
    CSimpleIniA restart;
    Require(restart.LoadData(serialized.c_str(), serialized.size()) >= 0, "reload saved INI");
    const auto loaded = LoadLayout(restart);
    Require(loaded.x == edited.x && loaded.y == edited.y && loaded.width == edited.width &&
                loaded.height == edited.height && Near(loaded.leftFraction, edited.leftFraction),
            "geometry and shared divider survive serialization");
    Require(restart.GetLongValue("Settings", "QualityLevel") == 4 &&
                restart.GetLongValue("SourceDLSSG", "NRPasses") == 2 &&
                std::string(restart.GetValue("Unrecognized", "Keep", "")) == "hello",
            "layout save preserves rendering and unknown keys");

    restart.SetValue("Overlay", "WindowWidth", "nan");
    restart.SetValue("Overlay", "WindowHeight", "-700");
    restart.SetValue("Overlay", "LeftColumnFraction", "garbage");
    const auto corrupt = LoadLayout(restart);
    Require(corrupt.width == 1100 && corrupt.height == 720 && corrupt.leftFraction == 0.5f,
            "malformed geometry and divider use valid defaults");
    const auto fit = FitLayout({4000, 1000, 4000, 1400, 0.65f}, 1280, 720);
    Require(fit.x == 0 && fit.y == 0 && fit.width == 1280 && fit.height == 720,
            "large ultrawide layout fits a smaller output");
    const auto small = FitLayout({-500, -300, 10, 20, 0.5f}, 640, 480);
    Require(small.x == 0 && small.y == 0 && small.width == 640 && small.height == 480,
            "small output wins over normal minimum size");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = FitLayout({nan, nan, nan, nan, nan}, 1920, 1080);
    Require(invalid.x == 40 && invalid.y == 40 && invalid.width == 1100 && invalid.leftFraction == 0.5f,
            "nonfinite inputs cannot make menu unreachable");
    const auto left = FitColumns(740, 0.01f);
    const auto right = FitColumns(740, 0.99f);
    Require(left.left >= 260 && right.right >= 350 && Near(left.left + left.right + ColumnGap, 740),
            "extreme divider positions leave usable controls without overflow");
    Require(GraphHeight(900) > GraphHeight(500) && GraphHeight(5000) == 240,
            "graph grows with window but leaves room for measurements");
}

void Scale()
{
    CSimpleIniA ini;
    Require(LoadLayout(ini).uiScale == 0, "missing menu size selects automatic scaling");
    Layout edited;
    edited.uiScale = 1.5f;
    StoreLayout(ini, edited);
    Require(Near(LoadLayout(ini).uiScale, 1.5f), "manual menu size survives serialization");
    for (const char* value : {"garbage", "-1", "nan", "0"})
    {
        ini.SetValue("Overlay", "UIScale", value);
        Require(LoadLayout(ini).uiScale == 0, "malformed or zero menu size selects automatic scaling");
    }
    ini.SetValue("Overlay", "UIScale", "9");
    Require(LoadLayout(ini).uiScale == MaxUIScale, "oversized menu size is limited");
    ini.SetValue("Overlay", "UIScale", "0.1");
    Require(LoadLayout(ini).uiScale == MinUIScale, "tiny menu size is limited");

    Require(ResolveUIScale(0, 1920, 1080) == 1 && ResolveUIScale(0, 3840, 2160) == 2 &&
                Near(ResolveUIScale(0, 2560, 1440), 1440.0f / 1080.0f) && ResolveUIScale(0, 5120, 2160) == 2,
            "automatic scale follows output height");
    Require(ResolveUIScale(0, 1280, 720) == 1 && ResolveUIScale(0.75f, 3840, 2160) == 0.75f,
            "automatic scale never shrinks; a manual choice may");
    Require(ResolveUIScale(3, 3840, 2160) == 3 && Near(ResolveUIScale(2, 1920, 1080), 1080.0f / MinWindowHeight),
            "manual scale is honoured until the minimum window would leave the output");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Require(ResolveUIScale(0, nan, 0) == 1 && ResolveUIScale(2, -1, 1080) == 2,
            "unknown output keeps the requested or 1x scale");
    constexpr std::pair<float, float> outputs[]{{1280.0f, 720.0f},  {1920.0f, 1080.0f}, {2560.0f, 1080.0f},
                                                {2560.0f, 1440.0f}, {3440.0f, 1440.0f}, {3840.0f, 2160.0f},
                                                {1024.0f, 768.0f}};
    for (const auto [width, height] : outputs)
    {
        for (const float requested : {0.0f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f})
        {
            const float scale = ResolveUIScale(requested, width, height);
            const auto fitted = FitLayout({}, width, height, scale);
            Require(MinWindowWidth * scale <= width + 0.01f && MinWindowHeight * scale <= height + 0.01f &&
                        fitted.x + fitted.width <= width && fitted.y + fitted.height <= height,
                    "resolved scale keeps the minimum and default windows on the output");
        }
    }

    const auto doubled = FitLayout({}, 3840, 2160, 2);
    Require(doubled.x == 80 && doubled.y == 80 && doubled.width == 2200 && doubled.height == 1440,
            "1x geometry is multiplied by the menu size");
    const auto minimum = FitLayout({0, 0, 10, 20, 0.5f}, 3840, 2160, 2);
    Require(minimum.width == MinWindowWidth * 2 && minimum.height == MinWindowHeight * 2,
            "minimum window grows with the menu size");
    const auto columns = FitColumns(1600, 0.01f, 2);
    Require(columns.left >= 520 && Near(columns.left + columns.right + ColumnGap * 2, 1600),
            "column minimums and gap follow the menu size");
    Require(GraphHeight(5000, 2) == 480 && GraphHeight(100, 2) == 180, "graph limits follow the menu size");
}

struct FrameResult
{
    ColumnSizes columns;
    ImVec2 divider;
    float width;
};
FrameResult Frame(float& fraction, ImVec2 mouse, bool down, const char* tab = "Image", float width = 1200,
                  float scale = 1)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 700), ImGuiCond_Always);
    ImGui::Begin("Layout fixture", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::PushID(tab);
    const auto origin = ImGui::GetCursorScreenPos();
    const float available = ImGui::GetContentRegionAvail().x;
    const auto columns = DrawColumnSplitter(available, 400, fraction, scale);
    FrameResult result{columns, ImVec2(origin.x + columns.left + ColumnGap * scale * 0.5f, origin.y + 100),
                       available};
    ImGui::BeginChild("left", ImVec2(columns.left, 400));
    ImGui::TextUnformatted("measurements");
    ImGui::EndChild();
    ImGui::SameLine(0, ColumnGap * scale);
    ImGui::BeginChild("right", ImVec2(0, 400));
    ImGui::TextUnformatted("controls");
    Require(ImGui::GetWindowSize().x <= columns.right + 1, "right child fits remaining width");
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::End();
    ImGui::Render();
    return result;
}

void Dragging()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    float fraction = 0.5f;
    auto first = Frame(fraction, ImVec2(-100, -100), false);
    Frame(fraction, first.divider, false);
    Frame(fraction, first.divider, true);
    auto moved = Frame(fraction, ImVec2(first.divider.x + 100, first.divider.y), true);
    Frame(fraction, ImVec2(first.divider.x + 100, first.divider.y), false);
    Require(fraction > 0.55f && moved.columns.left > first.columns.left + 90,
            "actual ImGui mouse drag moves the divider");
    const float selected = fraction;
    const auto switched = Frame(fraction, ImVec2(-100, -100), false, "NR");
    Require(fraction == selected && Near(switched.columns.left, moved.columns.left),
            "shared fraction carries into a different tab");
    const auto grown = Frame(fraction, ImVec2(-100, -100), false, "NR", 1600);
    Require(fraction == selected && grown.columns.left > moved.columns.left,
            "window growth preserves divider proportion");
    // Two clicks on an off-centre divider must not implement an implicit reset.
    Frame(fraction, grown.divider, true, "NR", 1600);
    Frame(fraction, grown.divider, false, "NR", 1600);
    Frame(fraction, grown.divider, true, "NR", 1600);
    Frame(fraction, grown.divider, false, "NR", 1600);
    Require(Near(fraction, selected), "double click does not reset the divider");
    ImGui::DestroyContext();
}

void ScaledDragging(const char* fontPath)
{
    // Mirror the overlay's 2x rebuild: the embedded font rasterized from
    // non-owned memory, plus scaled style sizes.
    std::ifstream file(fontPath, std::ios::binary);
    const std::vector<char> font((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Require(!font.empty(), "overlay font is readable");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(3840, 2160);
    io.DeltaTime = 1.0f / 60;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    Require(io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(font.data()), static_cast<int>(font.size()), 28, &config),
            "overlay font loads at 2x");
    ImGui::GetStyle().ScaleAllSizes(2);
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Require(io.Fonts->IsBuilt() && io.Fonts->Fonts[0]->FontSize == 28 && io.Fonts->Fonts[0]->FindGlyphNoFallback('%'),
            "overlay font atlas builds with the menu glyphs");
    float fraction = 0.5f;
    auto first = Frame(fraction, ImVec2(-100, -100), false, "Image", 2400, 2);
    Require(first.columns.left >= 520 && first.columns.right >= 700, "2x columns keep scaled minimum widths");
    Frame(fraction, first.divider, false, "Image", 2400, 2);
    Frame(fraction, first.divider, true, "Image", 2400, 2);
    auto moved = Frame(fraction, ImVec2(first.divider.x + 200, first.divider.y), true, "Image", 2400, 2);
    Frame(fraction, ImVec2(first.divider.x + 200, first.divider.y), false, "Image", 2400, 2);
    Require(fraction > 0.55f && moved.columns.left > first.columns.left + 180,
            "2x divider remains grabbable and draggable");
    ImGui::DestroyContext();
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Settings();
        Scale();
        Dragging();
        Require(argc > 1, "usage: TRPOverlayLayoutTests <overlay font>");
        ScaledDragging(argv[1]);
        std::cout << "Layout persistence, menu scaling, display fitting and ImGui divider interaction passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
