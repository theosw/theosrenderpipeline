#include "FrameGen/BottledLensPatch.h"
#include "FrameGen/BottledLensTarget.h"
#include "FrameGen/CommunityShaderFrame.h"
#include "FrameGen/CommunityShaderLens.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { if (FAILED(hr)) { std::fprintf(stderr, "%08lx: ", hr); } Require(SUCCEEDED(hr), why); }

// CameraFX::GetLensTarget as built in Bottled Shaders V2 2.0.1 (CommunityShaders.dll RVA 0xAF62C0).
static constexpr std::uint8_t kBottledV201[64]{
    0x41, 0xC6, 0x00, 0x00, 0x80, 0x3D, 0x4D, 0x0C, 0xE9, 0x01, 0x00, 0x74, 0x2E, 0x80, 0x3D, 0xEF,
    0x0C, 0xE9, 0x01, 0x00, 0x74, 0x25, 0x80, 0x3D, 0xE7, 0x0C, 0xE9, 0x01, 0x00, 0x74, 0x1C, 0x48,
    0x8B, 0x05, 0xEA, 0xA5, 0xF1, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x10, 0x48, 0x83, 0x78, 0x18, 0x00,
    0x74, 0x09, 0x41, 0xC6, 0x00, 0x01, 0x48, 0x8B, 0x40, 0x18, 0xC3, 0x48, 0x8B, 0x42, 0x08, 0xC3};

static void Signature()
{
    Require(BottledLens::Matches(kBottledV201), "the Bottled Shaders V2 2.0.1 function matches");
    for (std::size_t i = 0; i < std::size(kBottledV201); ++i) {
        std::uint8_t changed[64]; std::memcpy(changed, kBottledV201, sizeof(changed));
        changed[i] ^= 0x40;
        Require(BottledLens::Matches(changed) == (BottledLens::kLensTarget[i] < 0),
            "only RIP displacements may differ; every other byte is part of the contract");
    }
    Require(!BottledLens::Matches(std::span(kBottledV201, 63)), "a truncated function never matches");
    std::vector<std::uint8_t> code(4096, 0xCC);
    Require(BottledLens::Find(code).matches == 0 && BottledLens::Find(std::span(code.data(), 10)).matches == 0, "nothing found in padding");
    std::memcpy(code.data() + 777, kBottledV201, 64);
    auto found = BottledLens::Find(code);
    Require(found.matches == 1 && found.offset == 777, "one function located");
    std::memcpy(code.data() + 4096 - 64, kBottledV201, 64);
    found = BottledLens::Find(code);
    Require(found.matches == 2 && found.offset == 777, "a second copy is counted, up to the last byte");

    // Direct references to an offset, with the range starting at 0x1000.
    std::vector<std::uint8_t> calls(256, 0x90);
    const auto aim = [&](std::size_t at, std::uint8_t opcode, std::int64_t target) {
        const auto relative = static_cast<std::int32_t>(target - static_cast<std::int64_t>(0x1000 + at + 5));
        calls[at] = opcode; std::memcpy(calls.data() + at + 1, &relative, 4);
    };
    auto references = BottledLens::FindReferences(calls, 0x1000, 0x5000);
    Require(references.calls == 0 && references.jumps == 0, "no reference in padding");
    aim(10, 0xE8, 0x5000); aim(40, 0xE8, 0x5004); aim(70, 0xE8, 0x40);
    references = BottledLens::FindReferences(calls, 0x1000, 0x5000);
    Require(references.calls == 1 && references.jumps == 0, "one direct call, forwards; other targets and backward calls ignored");
    Require(BottledLens::FindReferences(calls, 0x1000, 0x40).calls == 1, "a backward call is found by its own target");
    aim(100, 0xE9, 0x5000); aim(251, 0xE8, 0x5000);
    references = BottledLens::FindReferences(calls, 0x1000, 0x5000);
    Require(references.calls == 2 && references.jumps == 1, "a second call, up to the last byte, and a tail jump are counted");
    Require(BottledLens::FindReferences(std::span(calls.data(), 4), 0x1000, 0x5000).calls == 0, "a range shorter than one call holds none");
}

// A mapped-image stand-in: headers, one executable section holding real copies
// of the function with displacements aimed at this image's own data page.
struct Image
{
    static constexpr std::size_t kSize = 0x3000, kCode = 0x1000, kData = 0x2000;
    std::uint8_t* base{};
    Image()
    {
        base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        Require(base != nullptr, "image memory");
        std::memset(base + kCode, 0xCC, kData - kCode);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->FileHeader.TimeDateStamp = 0x12345678;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt->OptionalHeader.SizeOfImage = kSize;
        auto* section = IMAGE_FIRST_SECTION(nt);
        std::memcpy(section[0].Name, ".text", 5); section[0].VirtualAddress = kCode; section[0].Misc.VirtualSize = kData - kCode;
        section[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        // A data section holding the same bytes must never be searched.
        std::memcpy(section[1].Name, ".data", 5); section[1].VirtualAddress = kData; section[1].Misc.VirtualSize = kSize - kData;
        section[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
        std::memcpy(base + kData + 0x800, kBottledV201, 64);
    }
    ~Image() { VirtualFree(base, 0, MEM_RELEASE); }
    // The three flags and the UI buffer pointer the function reads.
    std::uint8_t& Flag(unsigned index) { return base[kData + index]; }
    void*& UIBuffer() { return *reinterpret_cast<void**>(base + kData + 0x10); }
    using Function = ID3D11RenderTargetView* (*)(void*, void*, bool*);
    Function Place(std::size_t offset)
    {
        auto* code = base + kCode + offset;
        std::memcpy(code, kBottledV201, 64);
        const auto aim = [&](std::size_t displacement, std::size_t next, std::size_t target) {
            const auto value = static_cast<std::int32_t>((base + target) - (code + next));
            std::memcpy(code + displacement, &value, sizeof(value));
        };
        aim(6, 11, kData + 0); aim(15, 20, kData + 1); aim(24, 29, kData + 2); aim(34, 38, kData + 0x10);
        FlushInstructionCache(GetCurrentProcess(), code, 64);
        return reinterpret_cast<Function>(code);
    }
    // The caller's direct call (or a tail jump) to a placed function. Never executed here.
    void Refer(std::size_t at, std::size_t function, std::uint8_t opcode = 0xE8)
    {
        auto* code = base + kCode + at;
        const auto relative = static_cast<std::int32_t>(static_cast<std::int64_t>(function) - static_cast<std::int64_t>(at + 5));
        code[0] = opcode; std::memcpy(code + 1, &relative, 4);
    }
};

static ID3D11RenderTargetView* offered{};
static ID3D11RenderTargetView* asked{};
static unsigned asks{};
static ID3D11RenderTargetView* Provider(ID3D11RenderTargetView* scene) { ++asks; asked = scene; return offered; }
static ID3D11RenderTargetView* Other(ID3D11RenderTargetView*) { ++asks; return nullptr; }

static void Redirect()
{
    using BottledLens::State;
    auto* const sceneView = reinterpret_cast<ID3D11RenderTargetView*>(std::uintptr_t{0x1111});
    auto* const ownUI = reinterpret_cast<ID3D11RenderTargetView*>(std::uintptr_t{0x2222});
    auto* const layer = reinterpret_cast<ID3D11RenderTargetView*>(std::uintptr_t{0x3333});
    struct { void* texture; ID3D11RenderTargetView* view; } scene{nullptr, sceneView};
    struct { void* pad[3]; ID3D11RenderTargetView* view; } buffer{{}, ownUI};
    const auto module = [](Image& image) { return reinterpret_cast<HMODULE>(image.base); };
    const auto untouched = [&](Image::Function function) {
        bool ui = true;
        return function(nullptr, &scene, &ui) == sceneView && !ui && asks == 0;
    };

    Require(BottledLens::Install(nullptr, Provider) == State::NoModule, "no module, nothing installed");
    Require(!BottledLens::InstalledBuild().timestamp, "no build is reported before a redirect is in place");
    {
        Image empty;
        Require(BottledLens::Install(module(empty), Provider) == State::NotFound,
            "a build without the function is left alone, data sections are not searched");
        Image twice;
        const auto first = twice.Place(0x100), second = twice.Place(0x400);
        twice.Refer(0x800, 0x100);
        Require(BottledLens::Install(module(twice), Provider) == State::Ambiguous, "two matches are refused");
        Require(untouched(first) && untouched(second), "a refused build runs its own code");
        Require(BottledLens::Installed() == State::Ambiguous && std::strlen(BottledLens::Describe(State::Ambiguous)) > 0, "the reason is kept for the menu");
        // The signature pins the function, not its caller: the call structure must match too.
        Image uncalled;
        const auto orphan = uncalled.Place(0x100);
        Require(BottledLens::Install(module(uncalled), Provider) == State::Uncalled && untouched(orphan), "a function nothing calls is not the checked build's");
        Image shared;
        const auto reused = shared.Place(0x100);
        shared.Refer(0x800, 0x100); shared.Refer(0x900, 0x100);
        Require(BottledLens::Install(module(shared), Provider) == State::Uncalled && untouched(reused), "a second caller may use the answer differently");
        Image tailed;
        const auto jumped = tailed.Place(0x100);
        tailed.Refer(0x800, 0x100); tailed.Refer(0x900, 0x100, 0xE9);
        Require(BottledLens::Install(module(tailed), Provider) == State::Uncalled && untouched(jumped), "a tail jump is a second user");
    }
    static Image image; // The redirect stays for the process lifetime.
    const auto function = image.Place(0x234);
    image.Refer(0x900, 0x234);
    bool ui = true;
    Require(function(nullptr, &scene, &ui) == sceneView && !ui, "the placed function runs: scene target, no UI layer");
    Require(BottledLens::Install(module(image), Provider) == State::Installed &&
        BottledLens::Installed() == State::Installed, "redirect installed");
    const auto build = BottledLens::InstalledBuild();
    Require(build.timestamp == 0x12345678 && build.imageSize == Image::kSize && !build.checked, "the patched build is identified, and not mistaken for the checked one");

    offered = layer; ui = false;
    Require(function(nullptr, &scene, &ui) == layer && ui && asks == 1 && asked == sceneView,
        "the producer is handed the layer and told it is a UI layer");
    offered = nullptr; ui = true;
    Require(function(nullptr, &scene, &ui) == sceneView && !ui && asks == 2, "declined: the lens stays on the scene");
    // The producer's own frame generation: all three flags and its UI buffer.
    image.Flag(0) = image.Flag(1) = image.Flag(2) = 1; image.UIBuffer() = &buffer;
    offered = layer; ui = false;
    Require(function(nullptr, &scene, &ui) == ownUI && ui && asks == 2, "the producer's own UI buffer is never replaced or even asked about");
    image.Flag(1) = 0; ui = true;
    Require(function(nullptr, &scene, &ui) == layer && ui && asks == 3, "relocated flag tests still read the producer's state");
    Require(BottledLens::Install(module(image), Other) == State::Installed, "installing again only replaces the provider");
    ui = true;
    Require(function(nullptr, &scene, &ui) == sceneView && !ui && asks == 4, "the new provider answers");
}

struct Pixel { float r, g, b, a; };
static Pixel Read(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT x = 0, UINT y = 0)
{
    Require(texture != nullptr, "readback texture");
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
    Pixel pixel{};
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        const auto* p = row + x * 4;
        pixel = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    } else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t v; std::memcpy(&v, row + x * 4, 4);
        pixel = {(v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f, (v >> 30) / 3.0f};
    } else if (desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        std::uint16_t v[4]; std::memcpy(v, row + x * 8, 8);
        pixel = {v[0] / 65535.0f, v[1] / 65535.0f, v[2] / 65535.0f, v[3] / 65535.0f};
    } else { Require(false, "readback format"); }
    context->Unmap(staging.Get(), 0);
    return pixel;
}
static bool Near(Pixel p, float r, float g, float b, float a, float tolerance)
{
    return std::fabs(p.r - r) <= tolerance && std::fabs(p.g - g) <= tolerance && std::fabs(p.b - b) <= tolerance && std::fabs(p.a - a) <= tolerance;
}

struct Scene
{
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> view; D3D11_TEXTURE2D_DESC desc{};
    Scene(ID3D11Device* device, DXGI_FORMAT format, UINT width, UINT height, UINT mips = 1)
    {
        desc.Width = width; desc.Height = height; desc.MipLevels = mips; desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc, nullptr, &texture), "scene texture");
        Check(device->CreateRenderTargetView(texture.Get(), nullptr, &view), "scene view");
    }
    void Fill(ID3D11DeviceContext* context, float r, float g, float b, float a) { const float c[]{r, g, b, a}; context->ClearRenderTargetView(view.Get(), c); }
};

// Bottled's lens passes as its CameraFX.cpp sets them up: a full-screen
// triangle whose pixel shader returns premultiplied colour and coverage,
// blended ONE / INV_SRC_ALPHA. On the scene only RGB is written; on a UI layer
// coverage accumulates in alpha the same way.
struct LensPass
{
    ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel; ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> sceneBlend, layerBlend; ComPtr<ID3D11RasterizerState> rasterizer;
    explicit LensPass(ID3D11Device* device)
    {
        constexpr char vs[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * 2 - 1, 0, 1); }";
        constexpr char ps[] = "cbuffer C : register(b0) { float4 color; }; float4 main() : SV_Target { return color; }";
        ComPtr<ID3DBlob> code;
        Check(D3DCompile(vs, sizeof(vs) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr), "lens VS code");
        Check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex), "lens VS");
        Check(D3DCompile(ps, sizeof(ps) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, code.ReleaseAndGetAddressOf(), nullptr), "lens PS code");
        Check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pixel), "lens PS");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 16; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&buffer, nullptr, &constants), "lens constants");
        D3D11_BLEND_DESC blend{};
        auto& target = blend.RenderTarget[0];
        target.BlendEnable = TRUE; target.SrcBlend = D3D11_BLEND_ONE; target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO; target.DestBlendAlpha = D3D11_BLEND_ONE; target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        Check(device->CreateBlendState(&blend, &sceneBlend), "scene blend");
        target.SrcBlendAlpha = D3D11_BLEND_ONE; target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&blend, &layerBlend), "layer blend");
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&raster, &rasterizer), "lens rasterizer");
    }
    void Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, bool layer, UINT width, UINT height, float r, float g, float b, float a)
    {
        const float color[]{r, g, b, a};
        context->UpdateSubresource(constants.Get(), 0, nullptr, color, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport); context->RSSetState(rasterizer.Get());
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(layer ? layerBlend.Get() : sceneBlend.Get(), nullptr, 0xFFFFFFFF);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0); context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
};

static void Routes(ID3D11Device* device)
{
    using enum LensRoute;
    using Mode = CommunityLensMode;
    Scene scene(device, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4), ui(device, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4), other(device, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4);
    auto* s = scene.texture.Get(); auto* u = ui.texture.Get(); auto* o = other.texture.Get();
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, s, s, s, nullptr) == Layer, "a direct framebuffer gets TRP's layer");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, s, s, nullptr, nullptr) == Layer, "no interface target reported: still the scene itself");
    Require(ChooseLensRoute(Mode::Scene, true, false, true, s, s, s, nullptr) == SceneChosen, "the user's choice comes first");
    Require(ChooseLensRoute(Mode::WithHUD, false, false, true, s, s, s, nullptr) == SceneNotGenerating, "nothing changes while frame generation is off");
    Require(ChooseLensRoute(Mode::WithHUD, true, true, true, s, s, s, nullptr) == SceneSuspended, "a lens once laid over the HUD turns the layer off");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, false, s, s, s, nullptr) == SceneOutsideBoundary, "outside the interface boundary nothing can go under the HUD");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, nullptr, s, s, nullptr) == SceneNoWorld, "no completed scene this frame");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, s, o, s, nullptr) == SceneForeignTarget, "the producer draws somewhere TRP did not capture");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, s, s, u, nullptr) == SceneUnknownSurface &&
        ChooseLensRoute(Mode::WithHUD, true, false, true, s, s, u, o) == SceneUnknownSurface, "an interface surface not seen at a boundary is not trusted");
    Require(ChooseLensRoute(Mode::WithHUD, true, false, true, s, s, u, u) == ProducerSurface, "CS HDR Display: the lens joins the HUD on the known surface");
    for (auto route : {SceneChosen, SceneNotGenerating, SceneSuspended, SceneOutsideBoundary, SceneNoWorld, SceneForeignTarget, SceneUnknownSurface, ProducerSurface, Layer}) {
        Require(std::strlen(Describe(route)) > 10, "every route has a line for the log and the menu");
    }
}

static void Layer()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    ComPtr<ID3D11DeviceContext> deferred; Check(device->CreateDeferredContext(0, &deferred), "deferred context");
    Routes(device.Get());
    Require(CommunityShaderLens::Precompile() && CommunityShaderLens::Precompile(), "the composition shader compiles ahead of the first lens");
    // Producer compute state that the composition must put back.
    constexpr char program[] = "RWTexture2D<float4> o:register(u0); Texture2D<float4> i:register(t0); [numthreads(1,1,1)] void main(){ o[uint2(0,0)] = i.Load(int3(0,0,0)); }";
    ComPtr<ID3DBlob> code; Check(D3DCompile(program, sizeof(program) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr), "producer shader code");
    ComPtr<ID3D11ComputeShader> producerShader; Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &producerShader), "producer shader");
    Scene other(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4);
    ComPtr<ID3D11ShaderResourceView> producerSRV; Check(device->CreateShaderResourceView(other.texture.Get(), nullptr, &producerSRV), "producer SRV");
    LensPass pass(device.Get());

    CommunityShaderLens lens;
    Scene scene(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4), direct(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4);
    scene.Fill(context.Get(), 0.5f, 0.25f, 1.0f, 1.0f); direct.Fill(context.Get(), 0.5f, 0.25f, 1.0f, 1.0f);
    Require(!lens.Begin(nullptr, scene.texture.Get()) && !lens.Begin(deferred.Get(), scene.texture.Get()) && !lens.Begin(context.Get(), nullptr),
        "no context, a deferred context or no scene: the lens stays on the scene");
    Require(!lens.Pending() && !lens.Layer() && lens.Compose(context.Get()) == S_FALSE, "nothing to lay over the scene yet");

    auto* target = lens.Begin(context.Get(), scene.texture.Get());
    Require(target && lens.LastRefusal() == CommunityShaderLens::Refusal::None && lens.Pending(), "layer offered for this scene");
    Require(!lens.Begin(context.Get(), scene.texture.Get()) && lens.LastRefusal() == CommunityShaderLens::Refusal::Busy, "one lens layer per frame");
    {
        ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture; Check(resource.As(&texture), "layer texture");
        Require(Near(Read(context.Get(), texture.Get(), 7, 3), 0, 0, 0, 0, 0), "the layer starts transparent");
        Require(!lens.Layer(), "a layer not yet on the scene is not the frame's lens");
    }
    // Two passes, as Bottled draws weather then combat: both ways of drawing them.
    pass.Draw(context.Get(), target, true, 8, 4, 0.2f, 0.1f, 0.0f, 0.4f);
    pass.Draw(context.Get(), target, true, 8, 4, 0.05f, 0.0f, 0.1f, 0.25f);
    pass.Draw(context.Get(), direct.view.Get(), false, 8, 4, 0.2f, 0.1f, 0.0f, 0.4f);
    pass.Draw(context.Get(), direct.view.Get(), false, 8, 4, 0.05f, 0.0f, 0.1f, 0.25f);
    Require(Near(Read(context.Get(), scene.texture.Get()), 0.5f, 0.25f, 1.0f, 1.0f, 0.004f), "the producer drew nothing on the scene");

    // The engine has bound the framebuffer for the menus by the time TRP composes.
    context->ClearState();
    context->OMSetRenderTargets(1, scene.view.GetAddressOf(), nullptr);
    context->CSSetShader(producerShader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, producerSRV.GetAddressOf());
    Check(lens.Compose(context.Get()), "lens laid over the scene");
    Require(lens.Compose(context.Get()) == S_FALSE, "never laid over the scene twice");
    ComPtr<ID3D11RenderTargetView> boundView; ComPtr<ID3D11ComputeShader> boundShader; ComPtr<ID3D11ShaderResourceView> boundSRV;
    ComPtr<ID3D11UnorderedAccessView> boundUAV;
    context->OMGetRenderTargets(1, &boundView, nullptr); context->CSGetShader(&boundShader, nullptr, nullptr);
    context->CSGetShaderResources(0, 1, &boundSRV); context->CSGetUnorderedAccessViews(0, 1, &boundUAV);
    Require(boundView == scene.view && boundShader == producerShader && boundSRV == producerSRV && !boundUAV,
        "render target and compute bindings are as the producer left them");
    // pass2 + (pass1 + scene * (1 - a1)) * (1 - a2), scene alpha kept.
    for (UINT y : {0u, 3u}) { for (UINT x : {0u, 7u}) {
        const auto composed = Read(context.Get(), scene.texture.Get(), x, y), drawn = Read(context.Get(), direct.texture.Get(), x, y);
        Require(Near(composed, 0.425f, 0.1875f, 0.55f, 1.0f, 0.008f), "the two passes reach the scene with their coverage");
        Require(Near(composed, drawn.r, drawn.g, drawn.b, drawn.a, 0.0079f), "within two 8-bit steps of Bottled's direct draws");
    } }
    Require(lens.Layer() && !lens.Pending(), "the composed layer is this frame's lens");
    Require(Near(Read(context.Get(), lens.Layer(), 7, 3), 0.2f, 0.075f, 0.1f, 0.55f, 0.006f), "the layer holds both passes' premultiplied colour and coverage");

    lens.NextFrame();
    Require(!lens.Layer() && !lens.Pending(), "a new frame starts without a lens");
    scene.Fill(context.Get(), 0.5f, 0.25f, 1.0f, 1.0f);
    Require(lens.Begin(context.Get(), scene.texture.Get()) == target, "the layer is reused for the same scene");
    lens.NextFrame();
    Require(lens.Compose(context.Get()) == S_FALSE && Near(Read(context.Get(), scene.texture.Get()), 0.5f, 0.25f, 1.0f, 1.0f, 0.004f),
        "a lens from a finished frame is never laid over a later scene");

    // A ten-bit scene has two bits of alpha: the layer keeps coverage in 16
    // bits, and stays UNORM so each pass clamps as on the scene itself.
    Scene deep(device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, 12, 6);
    deep.Fill(context.Get(), 0.8f, 0.4f, 0.2f, 1.0f);
    auto* deepTarget = lens.Begin(context.Get(), deep.texture.Get());
    Require(deepTarget != nullptr, "a ten-bit scene is layered too");
    pass.Draw(context.Get(), deepTarget, true, 12, 6, 0.1f, 0.3f, -0.5f, 0.25f);
    Check(lens.Compose(context.Get()), "ten-bit composition");
    Require(Near(Read(context.Get(), deep.texture.Get(), 11, 5), 0.7f, 0.6f, 0.15f, 1.0f, 0.003f), "ten-bit scene composed with fractional coverage, negative output clamped");
    D3D11_TEXTURE2D_DESC deepLayer{};
    Require(lens.Layer() != nullptr, "ten-bit layer composed");
    lens.Layer()->GetDesc(&deepLayer);
    Require(deepLayer.Width == 12 && deepLayer.Height == 6 && deepLayer.Format == DXGI_FORMAT_R16G16B16A16_UNORM,
        "resources follow the scene's size, with 16-bit coverage");
    Require(Near(Read(context.Get(), lens.Layer(), 0, 0), 0.1f, 0.3f, 0.0f, 0.25f, 0.0001f), "the layer clamps like a UNORM scene");
    lens.NextFrame();

    Scene bgra(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 8, 4), wide(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 4);
    Require(!lens.Begin(context.Get(), bgra.texture.Get()) && lens.LastRefusal() == CommunityShaderLens::Refusal::Format &&
        !lens.Begin(context.Get(), wide.texture.Get()) && lens.LastRefusal() == CommunityShaderLens::Refusal::Format,
        "an unknown or extended-range scene format keeps the lens on the scene");
    Scene mipped(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4, 2);
    Require(!lens.Begin(context.Get(), mipped.texture.Get()) && lens.LastRefusal() == CommunityShaderLens::Refusal::Scene, "a mipped target is not a framebuffer");
    lens.ResetAfterRetirement();
    Require(!lens.Layer() && !lens.Pending(), "retired resources are no longer exposed");

    // The frame's order: the HUD-less scene is taken before the lens is laid
    // over the same resource, and the interface surface is tracked per boundary.
    auto guide = [&](UINT width, UINT height) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture; Check(device->CreateTexture2D(&desc, nullptr, &texture), "guide"); return texture;
    };
    const auto guides = guide(8, 4);
    context->ClearState();
    CommunityShaderFrame frame;
    Require(!frame.OpenScene() && !frame.UISurface(), "no scene and no UI surface before a world frame");
    scene.Fill(context.Get(), 0.5f, 0.25f, 1.0f, 1.0f);
    Check(frame.CaptureGuides(context.Get(), 1, guides.Get(), guides.Get(), {4, 2}, {8, 4}), "guides");
    Check(frame.CaptureScene(context.Get(), scene.texture.Get()), "scene");
    Require(frame.OpenScene() == scene.texture.Get(), "the captured scene is open until the menus");
    auto* framed = lens.Begin(context.Get(), frame.OpenScene());
    Require(framed != nullptr, "the lens is offered a layer for the open scene");
    pass.Draw(context.Get(), framed, true, 8, 4, 0.2f, 0.1f, 0.0f, 0.4f);
    Require(frame.RefreshScene(context.Get()) == S_OK && !frame.OpenScene(), "the HUD-less scene is taken, and closed, first");
    Check(lens.Compose(context.Get()), "then the lens goes on the framebuffer");
    D3D11_TEXTURE2D_DESC like{}; scene.texture->GetDesc(&like);
    Require(Near(Read(context.Get(), frame.Hudless(like)), 0.5f, 0.25f, 1.0f, 1.0f, 0.004f), "the HUD-less scene does not hold the lens");
    Require(Near(Read(context.Get(), scene.texture.Get()), 0.5f, 0.25f, 0.6f, 1.0f, 0.006f), "the presented frame does, before any menu is drawn on it");
    frame.SetUIBoundary(other.texture.Get());
    Require(frame.UISurface() == other.texture.Get(), "a separate interface surface is remembered");
    frame.Consume(); lens.NextFrame();
    Check(frame.CaptureGuides(context.Get(), 2, guides.Get(), guides.Get(), {4, 2}, {8, 4}), "next guides");
    Check(frame.CaptureScene(context.Get(), scene.texture.Get()), "next scene");
    Require(frame.OpenScene() == scene.texture.Get() && frame.UISurface() == other.texture.Get(),
        "the next frame's interface can be matched to the surface before its own boundary");
    frame.SetUIBoundary(scene.texture.Get());
    Require(!frame.OpenScene() && !frame.UISurface(), "a boundary on the scene itself closes it and drops the old surface");
    frame.SetUIBoundary(other.texture.Get());
    frame.ResetAfterRetirement();
    Require(!frame.UISurface(), "retirement forgets the surface");
}

int main()
{
    Signature();
    Redirect();
    Layer();
    std::puts("CS lens: Bottled V2 2.0.1 signature and single caller, in-memory redirect with fallback and own-UI passthrough, routes, two-pass blended composition against direct draws, HUD-less order and frame state passed.");
}
