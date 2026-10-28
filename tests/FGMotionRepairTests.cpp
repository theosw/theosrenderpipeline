#include "FrameGen/FGMotionRepair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::FGMotionRepair;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

constexpr UINT kWidth = 256, kHeight = 144;  // 0.05 of the width is 12.8 px, past the 4 px threshold

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, DXGI_FORMAT format, UINT texel, const void* data)
{
    D3D11_TEXTURE2D_DESC desc{ kWidth, kHeight, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA initial{ data, kWidth * texel, 0 };
    ComPtr<ID3D11Texture2D> texture; Check(device->CreateTexture2D(&desc, &initial, &texture), "create texture");
    return texture;
}

static std::vector<float> Read(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    std::vector<float> values(kWidth * kHeight * 2);
    for (UINT y = 0; y < kHeight; ++y) {
        std::memcpy(values.data() + y * kWidth * 2, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, kWidth * 8);
    }
    context->Unmap(staging.Get(), 0);
    return values;
}

int main()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context),
        "WARP device");
    const std::vector<float> depthData(kWidth * kHeight, .5f);
    auto depth = Texture(device.Get(), DXGI_FORMAT_R32_FLOAT, 4, depthData.data());
    auto& repair = FGMotionRepair::Get();
    const float identity[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // Row vectors: the previous clip position is this one shifted right by 0.1 clip
    // units, so the camera alone implies previous - current = +0.05 in screen UV x.
    const float panned[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .1f, 0, 0, 1 };
    auto run = [&](const std::vector<float>& motion, const float (&camera)[16]) {
        auto source = Texture(device.Get(), DXGI_FORMAT_R32G32_FLOAT, 8, motion.data());
        auto* result = repair.Apply(context.Get(), source.Get(), depth.Get(), camera, false, kWidth, kHeight, true);
        Require(result && result != source.Get(), "repair produces its own copy");
        return Read(context.Get(), result);
    };
    auto expect = [](const std::vector<float>& values, float x, float y, const char* why) {
        for (std::size_t i = 0; i < values.size(); i += 2) {
            Require(std::abs(values[i] - x) < 1e-4f && std::abs(values[i + 1] - y) < 1e-4f, why);
        }
    };
    unsigned cases = 0;
    // Underwater: one value over the whole frame while the camera is still.
    std::vector<float> stuck(kWidth * kHeight * 2);
    for (std::size_t i = 0; i < stuck.size(); i += 2) { stuck[i] = .05f; }
    expect(run(stuck, identity), 0, 0, "a frame of stuck motion becomes the still camera's zero motion"); ++cases;
    // The same stuck value while panning: replaced by the camera's motion.
    std::vector<float> wrong(kWidth * kHeight * 2);
    for (std::size_t i = 1; i < wrong.size(); i += 2) { wrong[i] = .2f; }
    expect(run(wrong, panned), .05f, 0, "stuck motion while panning becomes camera motion"); ++cases;
    // Correct motion while panning is kept.
    std::vector<float> correct(kWidth * kHeight * 2);
    for (std::size_t i = 0; i < correct.size(); i += 2) { correct[i] = .05f; }
    expect(run(correct, panned), .05f, 0, "motion that agrees with the camera is kept"); ++cases;
    // A still camera with one moving object: real motion, copied unchanged.
    std::vector<float> object(kWidth * kHeight * 2);
    for (UINT y = 40; y < 80; ++y) for (UINT x = 80; x < 160; ++x) { object[(y * kWidth + x) * 2] = .02f; object[(y * kWidth + x) * 2 + 1] = .01f; }
    const auto kept = run(object, identity);
    for (std::size_t i = 0; i < kept.size(); ++i) { Require(kept[i] == object[i], "a moving object keeps its own motion"); }
    ++cases;
    // The game's compute bindings survive the repair, and the context's dispatch
    // table is left alone: a state swap rewrites it and drops the UI routing hooks.
    {
        ComPtr<ID3D11ShaderResourceView> gameView;
        Check(device->CreateShaderResourceView(depth.Get(), nullptr, &gameView), "game view");
        D3D11_BUFFER_DESC bufferDesc{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        ComPtr<ID3D11Buffer> gameBuffer; Check(device->CreateBuffer(&bufferDesc, nullptr, &gameBuffer), "game buffer");
        auto* view = gameView.Get(); auto* buffer = gameBuffer.Get();
        context->CSSetShaderResources(0, 1, &view);
        context->CSSetConstantBuffers(0, 1, &buffer);
        const auto* table = *reinterpret_cast<void* const* const*>(context.Get());
        std::vector<void*> before(table, table + 115);
        run(stuck, identity);
        const auto* after = *reinterpret_cast<void* const* const*>(context.Get());
        Require(after == table && std::equal(before.begin(), before.end(), after), "the context's dispatch table is unchanged");
        ComPtr<ID3D11ShaderResourceView> gotView; ComPtr<ID3D11Buffer> gotBuffer;
        context->CSGetShaderResources(0, 1, &gotView); context->CSGetConstantBuffers(0, 1, &gotBuffer);
        Require(gotView.Get() == view && gotBuffer.Get() == buffer, "the game's compute bindings are restored");
        ID3D11ShaderResourceView* noView{}; ID3D11Buffer* noBuffer{};
        context->CSSetShaderResources(0, 1, &noView); context->CSSetConstantBuffers(0, 1, &noBuffer);
    }
    ++cases;
    // Above water even stuck-looking motion passes through: the repair is armed
    // only while the game reports the player underwater.
    auto source = Texture(device.Get(), DXGI_FORMAT_R32G32_FLOAT, 8, stuck.data());
    Require(repair.Apply(context.Get(), source.Get(), depth.Get(), identity, false, kWidth, kHeight, false) == source.Get(),
        "above water the engine's motion is handed through untouched");
    ++cases;
    // Disabled: the engine's texture is handed through untouched.
    repair.Enable(false);
    Require(repair.Apply(context.Get(), source.Get(), depth.Get(), identity, false, kWidth, kHeight, true) == source.Get(),
        "disabled repair returns the engine's motion");
    ++cases;
    std::printf("PASS: %u motion repair cases: stuck frames, camera motion, agreeing motion, moving objects, above water, disabled\n", cases);
}
