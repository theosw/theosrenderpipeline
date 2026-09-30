#include "FrameGen/LoadingFadeIn.h"
#include "FrameGen/PresentationFade.h"
#include <d3d11.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::LoadingFadeIn;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

static void Policy()
{
    LoadingFadeIn fade;
    Require(fade.Update(true, 0.0) == 1.0f && !fade.Active(), "ordinary loading screens are not faded");
    fade.RequestFade(10.0);
    Require(fade.Update(false, 10.1) == 1.0f && !fade.Active(), "pending until the loading menu opens");
    Require(fade.Update(true, 10.2) == 0.0f && fade.Active(), "first loading frame is black");
    float previous = 0.0f;
    for (int i = 1; i < 50; ++i) {
        const float factor = fade.Update(true, 10.2 + i * 0.01);
        Require(factor >= previous && factor < 1.0f, "fade rises monotonically");
        previous = factor;
    }
    Require(std::fabs(fade.Update(true, 10.2 + 0.25) - 0.25f) < 1e-4f, "ease-in: half time is a quarter brightness");
    Require(fade.Update(true, 10.2 + LoadingFadeIn::kDuration) == 1.0f && !fade.Active(), "fade completes");
    Require(fade.Update(true, 11.0) == 1.0f, "one fade per forced transition");

    fade.RequestFade(20.0);
    Require(fade.Update(false, 20.0 + LoadingFadeIn::kPendingLimit + 0.1) == 1.0f, "unused request expires");
    Require(fade.Update(true, 26.0) == 1.0f && !fade.Active(), "expired request does not fade a later loading screen");
    fade.RequestFade(30.0);
    fade.Reset();
    Require(fade.Update(true, 30.1) == 1.0f, "reset clears a pending request");
}

static void Pass()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "WARP device");
    constexpr UINT W = 16, H = 8;
    std::vector<std::uint32_t> pixels(W * H);
    for (UINT i = 0; i < W * H; ++i) {
        const std::uint32_t v = (i * 37) & 0xFF;
        pixels[i] = v | ((255 - v) << 8) | (((v * 3) & 0xFF) << 16) | ((i & 0xFF) << 24);
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = W; desc.Height = H; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{ pixels.data(), W * 4, 0 };
    ComPtr<ID3D11Texture2D> frame, other;
    Check(device->CreateTexture2D(&desc, &initial, &frame), "frame");
    Check(device->CreateTexture2D(&desc, nullptr, &other), "producer target");

    // Producer state that must survive the fade.
    ComPtr<ID3D11RenderTargetView> producerView;
    Check(device->CreateRenderTargetView(other.Get(), nullptr, &producerView), "producer view");
    auto* bound = producerView.Get();
    context->OMSetRenderTargets(1, &bound, nullptr);
    const D3D11_VIEWPORT producerViewport{ 1, 2, 5, 3, 0, 1 };
    context->RSSetViewports(1, &producerViewport);

    TheosRenderPipeline::PresentationFade fade;
    Require(!fade.Apply(context.Get(), frame.Get(), 1.0f), "factor 1 is a no-op");
    Require(!fade.Apply(context.Get(), frame.Get(), std::numeric_limits<float>::quiet_NaN()), "non-finite factor rejected");
    Require(fade.Apply(context.Get(), frame.Get(), 0.25f), "apply fade");

    ComPtr<ID3D11RenderTargetView> after; context->OMGetRenderTargets(1, &after, nullptr);
    D3D11_VIEWPORT viewport{}; UINT count = 1; context->RSGetViewports(&count, &viewport);
    Require(after.Get() == producerView.Get(), "producer render target restored");
    Require(count == 1 && viewport.TopLeftX == 1 && viewport.Width == 5 && viewport.Height == 3, "producer viewport restored");

    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), frame.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
    int worst = 0;
    for (UINT y = 0; y < H; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < W; ++x) {
            const auto in = pixels[y * W + x], out = row[x];
            for (int c = 0; c < 3; ++c) {
                const int expected = static_cast<int>(std::lround(((in >> (c * 8)) & 0xFF) * 0.25));
                worst = (std::max)(worst, std::abs(static_cast<int>((out >> (c * 8)) & 0xFF) - expected));
            }
            Require((out >> 24) == (in >> 24), "alpha unchanged");
        }
    }
    context->Unmap(staging.Get(), 0);
    Require(worst <= 1, "colour scaled by the fade factor");
}

int main()
{
    Policy();
    Pass();
    std::printf("loading fade checks passed\n");
    return 0;
}
