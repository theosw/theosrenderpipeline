// Draws the in-game menu offline and writes PNGs, so menu changes can be
// reviewed without launching Skyrim. See README.md.

#include "Localization.h"
#include "OverlayPreview.h"
#include "OverlayUIStyle.h"
#include "ScreenshotFile.h"

#include <imgui_impl_dx11.h>
#include <imgui_internal.h>
#include <SimpleIni.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <ranges>
#include <set>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::Overlay;

namespace
{
struct Size
{
    UINT width{}, height{};
};

struct Tab
{
    const char* name;
    SettingsPage page;
};

constexpr Tab kTabs[]{
    {"image", SettingsPage::Image},
#if !defined(TRP_NO_NEURAL_RENDERING)
    {"neural", SettingsPage::NeuralRendering},
#endif
    {"generation", SettingsPage::FrameGeneration},
    {"advanced", SettingsPage::Advanced},
};

struct Options
{
    std::filesystem::path out{"out/overlay-preview"};
    std::set<std::string> scenarios, tabs;
    std::vector<Size> sizes;
    std::optional<float> zoom; // Percent; empty selects automatic zoom.
    std::optional<Size> menu;  // Menu window in 100% units.
    std::filesystem::path ini;
    std::string language{"auto"}; // As [Overlay] Language: auto, en or a dictionary code.
    // The shipped language files in this checkout, read in place: an edit shows on
    // the next language pick, without a rebuild.
    std::filesystem::path languages{TRP_PREVIEW_LANG_DIR};
    bool full{}, warp{}, list{}, live{}, support{};
    std::vector<LivePreviewOptions::Step> script;
    std::filesystem::path save;
};

// Enough frames for the window, tab selection and table widths to settle.
constexpr int kFrames = 4;

std::set<std::string> SplitList(const std::string& text)
{
    std::set<std::string> values;
    for (const auto part : std::views::split(text, ','))
    {
        if (std::string value(part.begin(), part.end()); !value.empty())
            values.insert(value);
    }
    return values;
}

std::optional<Size> ParseSize(const std::string& text)
{
    Size size;
    char separator{};
    if (std::sscanf(text.c_str(), "%u%c%u", &size.width, &separator, &size.height) != 3 ||
        (separator != 'x' && separator != 'X') || size.width < 320 || size.height < 240 || size.width > 16384 ||
        size.height > 16384)
        return std::nullopt;
    return size;
}

void PrintUsage()
{
    std::puts(
        "TRPOverlayPreview [options]\n"
        "  --out DIR           Output folder (default out/overlay-preview)\n"
        "  --scenario A,B      Scenarios to draw (default all; --list shows them)\n"
        "  --tab A,B           image, neural, generation, advanced (default all)\n"
        "  --size WxH,WxH      Game output sizes (default 1920x1080)\n"
        "  --zoom PERCENT      Menu zoom; default is automatic, as in game\n"
        "  --menu WxH          Menu window size at 100% zoom (default from the layout)\n"
        "  --ini PATH          Read the [Overlay] layout from a TheosRenderPipeline.ini\n"
        "  --language CODE     Overlay language: auto (default, follows Windows), en or a\n"
        "                      dictionary code from TheosRenderPipeline\\Lang\n"
        "  --lang-dir DIR      Read language files from DIR (default: this checkout's\n"
        "                      package/SKSE/Plugins/TheosRenderPipeline/Lang)\n"
        "  --full              Keep the whole output instead of cropping to the menu\n"
        "  --warp              Draw with the WARP software rasterizer\n"
        "  --list              List scenarios and exit\n"
        "  --live              Open an interactive window instead of writing PNGs. It starts\n"
        "                      with the first --scenario and --size given\n"
        "  --click X,Y         With --live: click at X,Y from the menu window's top-left\n"
        "                      (repeatable, in order)\n"
        "  --hover X,Y         With --live: then rest the mouse at X,Y, for tooltips\n"
        "  --type TEXT         With --live: type TEXT into the focused field ({enter} presses Enter)\n"
        "  --save PATH         With --live: after the clicks and hover, save the menu and exit");
}

std::optional<Options> ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc)
                return std::nullopt;
            return std::string(argv[++i]);
        };
        if (arg == "--full")
            options.full = true;
        else if (arg == "--support")
            options.support = true;
        else if (arg == "--warp")
            options.warp = true;
        else if (arg == "--list")
            options.list = true;
        else if (arg == "--live")
            options.live = true;
        else if (arg == "--help" || arg == "-h")
            return std::nullopt;
        else if (const auto text = value(); !text)
            return std::nullopt;
        else if (arg == "--out")
            options.out = *text;
        else if (arg == "--save")
            options.save = *text;
        else if (arg == "--type")
        {
            LivePreviewOptions::Step step;
            step.text = *text;
            for (std::size_t at; (at = step.text.find("{enter}")) != std::string::npos;)
                step.text.replace(at, 7, "\n");
            if (!options.script.empty())
                step.x = options.script.back().x, step.y = options.script.back().y;
            options.script.push_back(step);
        }
        else if (arg == "--click" || arg == "--hover")
        {
            LivePreviewOptions::Step step;
            if (std::sscanf(text->c_str(), "%f,%f", &step.x, &step.y) != 2)
                return std::nullopt;
            step.click = arg == "--click";
            options.script.push_back(step);
        }
        else if (arg == "--scenario")
            options.scenarios = SplitList(*text);
        else if (arg == "--tab")
            options.tabs = SplitList(*text);
        else if (arg == "--ini")
            options.ini = *text;
        else if (arg == "--language")
            options.language = *text;
        else if (arg == "--lang-dir")
            options.languages = *text;
        else if (arg == "--zoom")
        {
            float percent{};
            if (std::sscanf(text->c_str(), "%f", &percent) != 1 || percent < MinUIScale * 100.0f ||
                percent > MaxUIScale * 100.0f)
                return std::nullopt;
            options.zoom = percent;
        }
        else if (arg == "--menu")
        {
            if (!(options.menu = ParseSize(*text)))
                return std::nullopt;
        }
        else if (arg == "--size")
        {
            for (const auto& item : SplitList(*text))
            {
                const auto size = ParseSize(item);
                if (!size)
                    return std::nullopt;
                options.sizes.push_back(*size);
            }
        }
        else
            return std::nullopt;
    }
    if (options.sizes.empty())
        options.sizes.push_back({1920, 1080});
    return options;
}

HRESULT CreateDevice(bool warp, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context)
{
    HRESULT result = E_FAIL;
    if (!warp)
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
            nullptr, &context);
    return result;
}

} // namespace

PreviewScenario AtOutputSize(PreviewScenario scenario, UINT width, UINT height)
{
    const Size size{width, height};
    auto& view = scenario.view;
    const auto scale = [](auto value, auto from, UINT to) {
        return from > 0 ? static_cast<decltype(value)>(std::lround(static_cast<double>(value) * to / from)) : value;
    };
    view.renderWidth = scale(view.renderWidth, view.outputWidth, size.width);
    view.renderHeight = scale(view.renderHeight, view.outputHeight, size.height);
    view.outputWidth = view.outputWidth ? size.width : 0;
    view.outputHeight = view.outputHeight ? size.height : 0;
    view.renderSizeX = scale(view.renderSizeX, view.displaySizeX, size.width);
    view.renderSizeY = scale(view.renderSizeY, view.displaySizeY, size.height);
    view.displaySizeX = static_cast<int>(size.width);
    view.displaySizeY = static_cast<int>(size.height);
    return scenario;
}

std::optional<PreviewImage> ReadPreviewTexture(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture, const ImRect& rect)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return std::nullopt;
    context->CopyResource(staging.Get(), texture);

    UINT left = 0, top = 0, right = desc.Width, bottom = desc.Height;
    if (rect.GetWidth() > 0 && rect.GetHeight() > 0)
    {
        const auto width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
        left = static_cast<UINT>(std::clamp(rect.Min.x, 0.0f, width));
        top = static_cast<UINT>(std::clamp(rect.Min.y, 0.0f, height));
        right = static_cast<UINT>(std::clamp(std::ceil(rect.Max.x), static_cast<float>(left + 1), width));
        bottom = static_cast<UINT>(std::clamp(std::ceil(rect.Max.y), static_cast<float>(top + 1), height));
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return std::nullopt;
    PreviewImage image{right - left, bottom - top};
    image.bgr.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    for (UINT y = 0; y < image.height; ++y)
    {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + (top + y) * mapped.RowPitch + left * 4;
        auto* destination = image.bgr.data() + static_cast<std::size_t>(y) * image.width * 3;
        for (UINT x = 0; x < image.width; ++x, source += 4, destination += 3)
        {
            destination[0] = source[2];
            destination[1] = source[1];
            destination[2] = source[0];
        }
    }
    context->Unmap(staging.Get(), 0);
    return image;
}

namespace
{
// Draws kFrames frames of the menu at the given output size and returns the last.
std::optional<PreviewImage> DrawMenu(ID3D11Device* device, ID3D11DeviceContext* context, PreviewScenario& scenario,
    const Layout& layout, SettingsPage page, Size size, bool crop, std::string_view language, std::string* support = nullptr)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width;
    desc.Height = size.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &view)))
        return std::nullopt;

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(size.width), static_cast<float>(size.height));
    io.DeltaTime = 1.0f / 60.0f;
    // In game the language is chosen before the atlas is built, so the preview
    // takes the same route and merges the dictionary's glyphs into the font.
    TheosRenderPipeline::Localization::Activate(language);
    // As UpdateUIScale does in game: the style and font follow the zoom.
    const float scale = ResolveUIScale(layout.uiScale, io.DisplaySize.x, io.DisplaySize.y);
    ApplyRendererStyle(scale);
    if (!BuildRendererFont(scale))
        std::fputs("warning: embedded menu font unavailable; using ImGui's default font\n", stderr);
    ImGui_ImplDX11_Init(device, context);
    ImRect window;
    {
        OverlayPreview menu(scenario, layout, page);
        for (int frame = 0; frame < kFrames; ++frame)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            menu.BuildFrame();
            if (const auto* drawn = ImGui::FindWindowByName(Plugin::DISPLAY_NAME.data()))
                window = drawn->Rect();
            ImGui::Render();
        }
        if (support)
            *support = menu.SupportText();
    }
    // A neutral backdrop in place of the game frame.
    const float backdrop[4]{0.16f, 0.17f, 0.18f, 1.0f};
    context->ClearRenderTargetView(view.Get(), backdrop);
    context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    return ReadPreviewTexture(device, context, target.Get(), crop ? window : ImRect());
}
} // namespace

int main(int argc, char** argv)
{
    const auto options = ParseOptions(argc, argv);
    if (!options)
    {
        PrintUsage();
        return 2;
    }
    TheosRenderPipeline::Localization::SetDirectory(options->languages);
    auto scenarios = PreviewScenarios();
    if (options->list)
    {
        for (const auto& scenario : scenarios)
            std::printf("%-20s %s\n", scenario.name.c_str(), scenario.summary.c_str());
        return 0;
    }
    for (const auto& name : options->scenarios)
    {
        if (std::ranges::none_of(scenarios, [&](const auto& scenario) { return scenario.name == name; }))
        {
            std::fprintf(stderr, "Unknown scenario '%s'; --list shows the scenarios.\n", name.c_str());
            return 2;
        }
    }
    for (const auto& name : options->tabs)
    {
        if (std::ranges::none_of(kTabs, [&](const Tab& tab) { return name == tab.name; }))
        {
            std::fprintf(stderr, "Unknown tab '%s'.\n", name.c_str());
            return 2;
        }
    }

    Layout layout;
    if (!options->ini.empty())
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(options->ini.c_str()) < 0)
        {
            std::fprintf(stderr, "Could not read %s.\n", options->ini.string().c_str());
            return 1;
        }
        layout = LoadLayout(ini);
    }
    if (options->menu)
    {
        layout.width = static_cast<float>(options->menu->width);
        layout.height = static_cast<float>(options->menu->height);
    }
    if (options->zoom)
        layout.uiScale = SanitizeUIScale(*options->zoom / 100.0f);

    if (options->live)
    {
        LivePreviewOptions live;
        live.scenario = options->scenarios.empty() ? scenarios.front().name : *options->scenarios.begin();
        live.width = options->sizes.front().width;
        live.height = options->sizes.front().height;
        if (!options->ini.empty() || options->menu || options->zoom)
            live.layout = layout;
        live.out = options->out;
        live.warp = options->warp;
        live.script = options->script;
        live.language = options->language;
        live.save = options->save;
        live.full = options->full;
        if (options->tabs.size() > 1)
        {
            std::fputs("--live opens one tab; give a single --tab.\n", stderr);
            return 2;
        }
        for (const auto& tab : kTabs)
            if (options->tabs.contains(tab.name))
                live.page = tab.page;
        return RunLivePreview(live);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(CreateDevice(options->warp, device, context)))
    {
        std::fputs("Could not create a D3D11 device.\n", stderr);
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(options->out, error);

    int written = 0, failed = 0;
    for (const auto& scenario : scenarios)
    {
        if (!options->scenarios.empty() && !options->scenarios.contains(scenario.name))
            continue;
        for (const auto& tab : kTabs)
        {
            if (!options->tabs.empty() && !options->tabs.contains(tab.name))
                continue;
            for (const auto size : options->sizes)
            {
                auto name = std::format("{}-{}-{}x{}", scenario.name, tab.name, size.width, size.height);
                if (options->zoom)
                    name += std::format("-zoom{:.0f}", *options->zoom);
                const auto path = std::filesystem::absolute(options->out / (name + ".png"));
                auto sized = AtOutputSize(scenario, size.width, size.height);
                std::string support;
                const auto image = DrawMenu(device.Get(), context.Get(), sized, layout, tab.page, size, !options->full,
                    options->language, options->support ? &support : nullptr);
                // What Copy details would put on the clipboard for this scenario.
                if (options->support)
                {
                    const auto text = std::filesystem::absolute(options->out / (scenario.name + "-support.txt"));
                    if (FILE* file = nullptr; _wfopen_s(&file, text.c_str(), L"wb") == 0 && file)
                    {
                        std::fwrite(support.data(), 1, support.size(), file);
                        std::fclose(file);
                    }
                }
                const auto result = image ? TheosRenderPipeline::ScreenshotFile::Replace(path, image->width,
                                                image->height, image->bgr, 0)
                                          : E_FAIL;
                if (FAILED(result))
                {
                    std::fprintf(stderr, "Failed %s (0x%08X)\n", name.c_str(), static_cast<unsigned>(result));
                    ++failed;
                    continue;
                }
                std::printf("%s\n", path.string().c_str());
                ++written;
            }
        }
    }
    if (!written && !failed)
        std::fputs("Nothing selected.\n", stderr);
    return failed || !written ? 1 : 0;
}
