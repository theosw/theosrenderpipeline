// Runs the producer copy route against real WARP copies. The scenario mirrors
// Modex's 3D item preview: save, clear, capture and restore a native-space
// rectangle of the "backbuffer" while the engine holds a render-extent surface.
#include "FrameGen/NativeUICopyRoute.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace Route = TheosRenderPipeline::NativeUICopyRoute;
using Decision = Route::Decision;

static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

// Reduced 3413x960 -> 5120x1440 ratio.
constexpr UINT kSurfaceWidth = 341, kSurfaceHeight = 96;
constexpr UINT kNativeWidth = 512, kNativeHeight = 144;
constexpr UINT kPreviewSize = 64;
constexpr std::uint32_t kWorld = 0xFF203040, kUI = 0x80A0B0C0, kFrame = 0xFF112233, kPreview = 0xFFFFFFFF;

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height, std::uint32_t fill,
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, UINT bindings = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = bindings;
    std::vector<std::uint32_t> data(width * height, fill);
    D3D11_SUBRESOURCE_DATA initial{ data.data(), width * 4, 0 };
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, &initial, &texture), "create texture");
    return texture;
}

static std::vector<std::uint32_t> Pixels(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    std::vector<std::uint32_t> pixels(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(pixels.data() + y * desc.Width, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

static bool RegionIs(const std::vector<std::uint32_t>& pixels, UINT width, D3D11_BOX box, std::uint32_t value)
{
    for (UINT y = box.top; y < box.bottom; ++y) {
        for (UINT x = box.left; x < box.right; ++x) {
            if (pixels[y * width + x] != value) { return false; }
        }
    }
    return true;
}

static bool AllAre(const std::vector<std::uint32_t>& pixels, std::uint32_t value)
{
    for (const auto pixel : pixels) { if (pixel != value) { return false; } }
    return true;
}

// Issues the copy exactly as the hook does after routing.
static Decision Issue(ID3D11DeviceContext* context, Route::Copy copy, bool redirect,
    std::span<ID3D11Resource* const> surfaces, ID3D11Resource* native)
{
    const auto decision = Route::Route(copy, redirect, surfaces, native);
    if (decision != Decision::Dropped) {
        context->CopySubresourceRegion(copy.dst, copy.dstSubresource, copy.dstX, copy.dstY, copy.dstZ,
            copy.src, copy.srcSubresource, copy.box);
    }
    return decision;
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "WARP device");
    ComPtr<ID3D11DeviceContext1> context1;
    Check(context.As(&context1), "ID3D11DeviceContext1");

    auto surface = Texture(device.Get(), kSurfaceWidth, kSurfaceHeight, kWorld);
    auto native = Texture(device.Get(), kNativeWidth, kNativeHeight, kUI);
    auto scratch = Texture(device.Get(), kPreviewSize, kPreviewSize, 0);
    auto capture = Texture(device.Get(), kPreviewSize, kPreviewSize, 0);
    ID3D11Resource* const surfaces[]{ surface.Get(), nullptr };

    // A native-space preview rectangle past the render extent, as on the right
    // side of a 5120-wide screen.
    const D3D11_BOX box{ 400, 40, 0, 400 + kPreviewSize, 40 + kPreviewSize, 1 };

    // Pure region checks.
    const Route::Extent surfaceExtent{ kSurfaceWidth, kSurfaceHeight, DXGI_FORMAT_R8G8B8A8_UNORM };
    const Route::Extent previewExtent{ kPreviewSize, kPreviewSize, DXGI_FORMAT_R8G8B8A8_UNORM };
    Require(!Route::RegionFits(previewExtent, 0, 0, 0, surfaceExtent, &box), "a native-space box exceeds the render-extent source");
    const D3D11_BOX inside{ 10, 10, 0, 74, 74, 1 };
    Require(Route::RegionFits(previewExtent, 0, 0, 0, surfaceExtent, &inside), "an in-range box fits");
    Require(!Route::RegionFits(surfaceExtent, kSurfaceWidth - 10, 0, 0, previewExtent, nullptr), "a destination overflow is rejected");
    const D3D11_BOX empty{ 5, 5, 0, 5, 9, 1 };
    Require(Route::RegionFits(previewExtent, 0, 0, 0, surfaceExtent, &empty), "an empty box is the runtime's no-op");
    const D3D11_BOX deep{ 0, 0, 0, 8, 8, 2 };
    Require(!Route::RegionFits(previewExtent, 0, 0, 0, surfaceExtent, &deep), "2D copies have one slice");

    // Without redirection (guard only): the out-of-range save is dropped and
    // neither resource changes.
    {
        const auto decision = Issue(context.Get(), { scratch.Get(), 0, 0, 0, 0, surface.Get(), 0, &box }, false, surfaces, native.Get());
        Require(decision == Decision::Dropped, "an out-of-range surface copy is dropped");
        Require(AllAre(Pixels(context.Get(), scratch.Get()), 0), "a dropped copy leaves its destination unchanged");
        Require(AllAre(Pixels(context.Get(), surface.Get()), kWorld), "a dropped copy leaves the surface unchanged");
        const auto restore = Issue(context.Get(), { surface.Get(), 0, box.left, box.top, 0, scratch.Get(), 0, nullptr }, false, surfaces, native.Get());
        Require(restore == Decision::Dropped, "an out-of-range restore into the surface is dropped");
        Require(AllAre(Pixels(context.Get(), surface.Get()), kWorld), "a dropped restore leaves the surface unchanged");
    }

    // Producer scope: save, clear, draw, capture and restore all use the native target.
    {
        auto decision = Issue(context.Get(), { scratch.Get(), 0, 0, 0, 0, surface.Get(), 0, &box }, true, surfaces, native.Get());
        Require(decision == Decision::Redirected, "the producer save reads the native target");
        Require(AllAre(Pixels(context.Get(), scratch.Get()), kUI), "the saved region holds the native UI pixels");

        ComPtr<ID3D11RenderTargetView> surfaceRTV, nativeRTV;
        Check(device->CreateRenderTargetView(surface.Get(), nullptr, &surfaceRTV), "surface RTV");
        Check(device->CreateRenderTargetView(native.Get(), nullptr, &nativeRTV), "native RTV");
        auto* view = Route::RouteClearView(surfaceRTV.Get(), true, surfaces, nativeRTV.Get());
        Require(view == nativeRTV.Get(), "a surface ClearView uses the native RTV");
        Require(Route::RouteClearView(surfaceRTV.Get(), false, surfaces, nativeRTV.Get()) == surfaceRTV.Get(),
            "ClearView is unchanged outside the producer scope");
        Require(Route::RouteClearView(nativeRTV.Get(), true, surfaces, nativeRTV.Get()) == nativeRTV.Get(),
            "a non-surface view keeps its target");
        const float frame[4]{ 0x33 / 255.0f, 0x22 / 255.0f, 0x11 / 255.0f, 1.0f };
        const D3D11_RECT rect{ static_cast<LONG>(box.left), static_cast<LONG>(box.top), static_cast<LONG>(box.right), static_cast<LONG>(box.bottom) };
        context1->ClearView(view, frame, &rect, 1);
        // The engine's preview model, drawn through the redirected render target.
        const D3D11_RECT model{ static_cast<LONG>(box.left + 16), static_cast<LONG>(box.top + 16),
            static_cast<LONG>(box.left + 48), static_cast<LONG>(box.top + 48) };
        const float white[4]{ 1, 1, 1, 1 };
        context1->ClearView(nativeRTV.Get(), white, &model, 1);

        decision = Issue(context.Get(), { capture.Get(), 0, 0, 0, 0, surface.Get(), 0, &box }, true, surfaces, native.Get());
        Require(decision == Decision::Redirected, "the producer capture reads the native target");
        const auto captured = Pixels(context.Get(), capture.Get());
        Require(RegionIs(captured, kPreviewSize, { 16, 16, 0, 48, 48, 1 }, kPreview), "the capture contains the preview model");
        Require(RegionIs(captured, kPreviewSize, { 0, 0, 0, kPreviewSize, 16, 1 }, kFrame), "the capture background is the cleared frame colour");

        const D3D11_BOX all{ 0, 0, 0, kPreviewSize, kPreviewSize, 1 };
        decision = Issue(context.Get(), { surface.Get(), 0, box.left, box.top, 0, scratch.Get(), 0, &all }, true, surfaces, native.Get());
        Require(decision == Decision::Redirected, "the producer restore writes the native target");
        Require(AllAre(Pixels(context.Get(), native.Get()), kUI), "the restore returns the native UI pixels");
        Require(AllAre(Pixels(context.Get(), surface.Get()), kWorld), "the render-extent surface is never written");
    }

    // An engine-style full copy of the surface keeps the surface: its native
    // equivalent would not fit the render-extent destination.
    {
        auto copyTarget = Texture(device.Get(), kSurfaceWidth, kSurfaceHeight, 0);
        const auto decision = Issue(context.Get(), { copyTarget.Get(), 0, 0, 0, 0, surface.Get(), 0, nullptr }, true, surfaces, native.Get());
        Require(decision == Decision::Original, "a full render-extent copy keeps its original source");
        Require(AllAre(Pixels(context.Get(), copyTarget.Get()), kWorld), "the original copy delivers the world pixels");
    }

    // Incompatible formats fall back to the original route and its guard.
    {
        auto bgra = Texture(device.Get(), kPreviewSize, kPreviewSize, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        Route::Copy copy{ bgra.Get(), 0, 0, 0, 0, surface.Get(), 0, &box };
        Require(Route::Route(copy, true, surfaces, native.Get()) == Decision::Dropped && copy.src == surface.Get(),
            "a format mismatch does not redirect and its out-of-range original is dropped");
        Route::Copy fitting{ bgra.Get(), 0, 0, 0, 0, surface.Get(), 0, &inside };
        Require(Route::Route(fitting, true, surfaces, native.Get()) == Decision::Original && fitting.src == surface.Get(),
            "a format mismatch keeps an in-range original copy");
    }

    // Copies touching only the native target are guarded too; unrelated copies are untouched.
    {
        Route::Copy overflow{ native.Get(), 0, kNativeWidth - 8, 0, 0, scratch.Get(), 0, nullptr };
        Require(Route::Route(overflow, false, surfaces, native.Get()) == Decision::Dropped, "an overflowing native copy is dropped");
        Route::Copy unrelated{ capture.Get(), 0, 32, 32, 0, scratch.Get(), 0, nullptr };
        Require(Route::Route(unrelated, true, surfaces, native.Get()) == Decision::Original, "unrelated copies are left to the runtime");
        Route::Copy noSurface{ scratch.Get(), 0, 0, 0, 0, surface.Get(), 0, &box };
        ID3D11Resource* const none[]{ nullptr, nullptr };
        Require(Route::Route(noSurface, true, none, nullptr) == Decision::Original, "an unconfigured host changes nothing");
    }

    std::puts("Native UI copy route: guard, producer redirection and engine copies verified");
    return 0;
}
