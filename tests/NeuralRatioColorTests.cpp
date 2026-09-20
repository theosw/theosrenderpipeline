#include "FrameGen/SourceDLSSGNeuralResolve.h"
#include "FrameGen/SourceDLSSGNeuralResolveShader.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::SourceDLSSG::ResolveConstants;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
using Pixel = std::array<float, 4>;
using HalfPixel = std::array<DirectX::PackedVector::HALF, 4>;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void Check(HRESULT hr, const char* why)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s (0x%08lX)\n", why, static_cast<unsigned long>(hr)); std::exit(1); }
}

struct Surface
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

static Surface Texture(ID3D11Device* device, UINT width, UINT height, const std::vector<Pixel>& pixels)
{
    Require(pixels.size() == size_t(width) * height, "texture input extent");
    std::vector<HalfPixel> packed(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i) {
        for (size_t c = 0; c < 4; ++c) { packed[i][c] = XMConvertFloatToHalf(pixels[i][c]); }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = packed.data(); data.SysMemPitch = width * sizeof(HalfPixel);
    Surface result;
    Check(device->CreateTexture2D(&desc, &data, &result.texture), "FP16 texture");
    Check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv), "FP16 SRV");
    Check(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav), "FP16 UAV");
    return result;
}

static std::vector<Pixel> Readback(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "FP16 readback");
    std::vector<Pixel> result(size_t(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const HalfPixel*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
            for (size_t c = 0; c < 4; ++c) { result[size_t(y) * desc.Width + x][c] = XMConvertHalfToFloat(row[x][c]); }
        }
    }
    context->Unmap(staging.Get(), 0);
    return result;
}

static void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, ID3D11Buffer* constants,
    const ResolveConstants& params, const std::array<ID3D11ShaderResourceView*, 3>& inputs,
    ID3D11UnorderedAccessView* output, UINT width, UINT height)
{
    // D3D11 constant buffers round the production structure up to 16 bytes.
    alignas(16) std::array<std::byte, (sizeof(ResolveConstants) + 15) & ~size_t(15)> upload{};
    static_assert(sizeof(params) <= sizeof(upload));
    std::memcpy(upload.data(), &params, sizeof(params));
    context->UpdateSubresource(constants, 0, nullptr, upload.data(), 0, 0);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    // Dispatch over the larger allocation so the active-extent guard is exercised.
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* nullInputs[3]{};
    ID3D11UnorderedAccessView* nullOutput{};
    context->CSSetShaderResources(0, 3, nullInputs);
    context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
}

static void ModelIdentity(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader, ID3D11Buffer* constants, bool passthrough, float whitePoint, bool bgra,
    bool producer = false, ID3D11ComputeShader* downsample = nullptr)
{
    constexpr UINT width = 21, height = 17, activeWidth = 13, activeHeight = 9;
    constexpr Pixel sentinel{-0.5f, -0.25f, -0.125f, -2.0f};
    constexpr std::array<Pixel, 15> hdr{{
        {64, 48, 16, -0.5f}, {6.4f, 32, 64, 0.25f}, {64, 16, 48, 1},
        {64, 64, 64, 2}, {32, 32.03125f, 32, -1},
        {64, 0, 0, 0}, {0, 64, 0, 0.5f}, {0, 0, 64, 0.75f},
        {0.18f, 0.18f, 0.18f, 1}, {0.75f, 0.5f, 0.25f, -0.25f},
        {0, 0, 0, -2}, {0.00001f, 0.00002f, 0.00004f, 0},
        {0.002f, 0.001f, 0.0005f, 0.5f}, {2, 0.25f, 1, 1}, {1, 4, 0.5f, 0.125f}
    }};
    constexpr std::array<Pixel, 9> sdr{{
        {0.8f, 0.6f, 0.2f, -0.5f}, {0.1f, 0.5f, 1, 1},
        {0.18f, 0.18f, 0.18f, 2}, {1, 0, 0, 0}, {0, 1, 0, -1},
        {0, 0, 1, 0.75f}, {0, 0, 0, -2},
        {0.00001f, 0.00002f, 0.00004f, 0.5f}, {0.5f, 0.50048828125f, 0.5f, 1}
    }};
    constexpr std::array<Pixel, 6> signedHDR{{
        {65504, 32000, 16, -0.5f}, {-0.05f, 64, 32, 2},
        {-2, -0.5f, -1, -2}, {0, 0, 0, 0.25f},
        {0.0000001f, 0, -0.002f, 1}, {64, -32, 0.5f, 0.5f}
    }};
    std::vector<Pixel> pixels(size_t(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t i = size_t(y) * width + x;
            pixels[i] = passthrough ? sdr[i % sdr.size()] : hdr[i % hdr.size()];
            if (producer && i % 3 == 0) { pixels[i] = signedHDR[(i / 3) % signedHDR.size()]; }
            if (bgra) { std::swap(pixels[i][0], pixels[i][2]); }
        }
    }
    auto original = Texture(device, width, height, pixels);
    auto encoded = Texture(device, width, height, std::vector<Pixel>(pixels.size(), sentinel));
    auto resolved = Texture(device, width, height, std::vector<Pixel>(pixels.size(), sentinel));
    const auto expected = Readback(context, original.texture.Get());
    ResolveConstants params{};
    params.sourceWidth = width; params.sourceHeight = height;
    params.targetWidth = params.workWidth = activeWidth;
    params.targetHeight = params.workHeight = activeHeight;
    params.sourceIsBGRA = bgra; params.passthrough = passthrough; params.whitePoint = whitePoint;
    params.producerColor = producer;
    Dispatch(context, shader, constants, params, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, height);
    const auto proxy = Readback(context, encoded.texture.Get());
    Surface reduced;
    auto* identityInput = encoded.srv.Get();
    if (downsample) {
        Require(producer, "reduced identity tests the producer contract");
        reduced = Texture(device, 11, 8, std::vector<Pixel>(88, sentinel));
        auto reducedParams = params;
        reducedParams.sourceWidth = activeWidth; reducedParams.sourceHeight = activeHeight;
        reducedParams.targetWidth = params.workWidth = 7;
        reducedParams.targetHeight = params.workHeight = 5;
        Dispatch(context, downsample, constants, reducedParams, {encoded.srv.Get(), nullptr, nullptr}, reduced.uav.Get(), 11, 8);
        const auto low = Readback(context, reduced.texture.Get());
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 11; ++x) {
            if (x >= 7 || y >= 5) { Require(low[y * 11 + x] == sentinel, "downsample preserves inactive allocation"); }
        }
        identityInput = reduced.srv.Get();
    }
    params.mode = 1;
    // The identity model returns exactly the encoded proxy. No NVIDIA runtime or
    // second implementation of the encoder supplies this behavioral oracle.
    Dispatch(context, shader, constants, params,
        {identityInput, identityInput, original.srv.Get()}, resolved.uav.Get(), width, height);
    const auto actual = Readback(context, resolved.texture.Get());
    float maxRelativeError = 0;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t i = size_t(y) * width + x;
            if (x >= activeWidth || y >= activeHeight) {
                Require(proxy[i] == sentinel && actual[i] == sentinel, "dispatch outside active extent preserves sentinels");
                continue;
            }
            const float peak = std::max({0.0f, expected[i][0], expected[i][1], expected[i][2]});
            // FP16 storage, sRGB conversion and OkLab/gamut matrix rounding are
            // not lossless. Allow 0.3% of this pixel's peak plus a near-black floor.
            const float tolerance = producer ? 0 : 0.003f * peak + 0.00015f;
            for (size_t c = 0; c < 3; ++c) {
                Require(std::isfinite(proxy[i][c]) && proxy[i][c] >= 0 && proxy[i][c] <= 1,
                    "encoded RGB is finite and bounded");
                const float error = std::abs(actual[i][c] - expected[i][c]);
                maxRelativeError = std::max(maxRelativeError, error / std::max(peak, 0.00015f));
                if (!std::isfinite(actual[i][c]) || error > tolerance) {
                    std::fprintf(stderr, "Identity mismatch: passthrough=%d white=%g bgra=%d xy=%u,%u channel=%zu original=%g returned=%g tolerance=%g\n",
                        passthrough, whitePoint, bgra, x, y, c, expected[i][c], actual[i][c], tolerance);
                    Require(false, "identity model retains the original scene RGB");
                }
                if (passthrough && !producer) { Require(proxy[i][c] == expected[i][c], "SDR passthrough encode is unchanged"); }
            }
            Require(proxy[i][3] == (producer ? 1 : expected[i][3]) && actual[i][3] == expected[i][3], "proxy alpha contract and original output alpha");
        }
    }
    std::printf("Ratio identity: producer=%d reduced=%d passthrough=%d white=%g bgra=%d max peak-relative RGB error=%.6f\n",
        producer, downsample != nullptr, passthrough, whitePoint, bgra, maxRelativeError);
}

static void ProducerEdits(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader, ID3D11Buffer* constants)
{
    // Controlled model output checks the resolve, independently of a learned
    // model's preferred appearance. Identity alone would also pass a no-op.
    const std::vector<Pixel> pixels{{64, 32, 16, -0.25f}, {64, -2, 16, 0.5f},
        {0, 0, 0, -1}, {-2, -1, -0.5f, 2}, {65504, 32000, 16000, 1}, {4, 2, 1, 0.25f}};
    const UINT width = static_cast<UINT>(pixels.size());
    auto original = Texture(device, width, 1, pixels);
    auto encoded = Texture(device, width, 1, std::vector<Pixel>(pixels.size()));
    auto resolved = Texture(device, width, 1, std::vector<Pixel>(pixels.size()));
    const auto expected = Readback(context, original.texture.Get());
    ResolveConstants params;
    params.sourceWidth = params.targetWidth = params.workWidth = width;
    params.sourceHeight = params.targetHeight = params.workHeight = 1;
    params.producerColor = 1;
    Dispatch(context, shader, constants, params, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, 1);
    const auto proxy = Readback(context, encoded.texture.Get());
    for (const float edit : {-0.125f, 0.125f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto changed = proxy;
        for (auto& p : changed) { for (size_t c = 0; c < 3; ++c) { p[c] = std::isfinite(edit) ? std::clamp(p[c] + edit, 0.0f, 1.0f) : edit; } }
        auto model = Texture(device, width, 1, changed);
        params.mode = 1;
        for (const float strength : {0.0f, 1.0f, 2.0f}) for (const float colour : {0.0f, 1.0f, 2.0f}) {
            params.transferStrength = strength; params.colourStrength = colour;
            Dispatch(context, shader, constants, params,
                {encoded.srv.Get(), model.srv.Get(), original.srv.Get()}, resolved.uav.Get(), width, 1);
            const auto actual = Readback(context, resolved.texture.Get());
            for (size_t i = 0; i < pixels.size(); ++i) {
                const float peak = std::max({0.0f, expected[i][0], expected[i][1], expected[i][2]});
                Require(actual[i][3] == expected[i][3], "model edits cannot change original alpha");
                for (size_t c = 0; c < 3; ++c) {
                    Require(std::isfinite(actual[i][c]), "model edits return finite scene RGB");
                    if (!strength || !std::isfinite(edit) || expected[i][c] < 0 || peak == 0) {
                        Require(actual[i][c] == expected[i][c], "zero strength, invalid model, negative channels and black retain original");
                    } else {
                        Require(actual[i][c] >= 0 && actual[i][c] <= std::min(65504.0f, peak * params.maxRatio), "model edits obey gain and FP16 bounds");
                    }
                }
            }
            if (std::isfinite(edit) && strength == 1 && colour == 1) {
                Require(edit < 0 ? actual[0][1] < expected[0][1] : actual[0][1] > expected[0][1], "finite model changes reach scene with correct direction");
                Require(actual[0][0] > 32, "bright source does not collapse into bounded model range");
            }
        }
    }
    std::puts("Producer edits: real changes, zero strength, invalid output, black, signed RGB, alpha and FP16 bounds passed.");
}

static void RatioGain(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader, ID3D11Buffer* constants)
{
    // The limit must constrain final luminance, including colour extrapolation,
    // rather than only a branch that full colour strength can discard.
    const auto luma = [](const Pixel& p) { return p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f; };
    for (const bool hdr : {false, true}) for (const bool bgra : {false, true}) {
        const float white = hdr ? 64.0f : 1.0f;
        std::vector<Pixel> pixels{{0.01f, 0.01f, 0.01f, -0.5f}, {0.003f, 0.015f, 0.001f, 2},
            {0, 0, 0, 0.5f}, {0.00001f, 0.00002f, 0.00004f, 0.25f}, {0.2f, 0.1f, 0.4f, 1}};
        for (auto& p : pixels) { for (size_t c = 0; c < 3; ++c) { p[c] *= white; } }
        if (bgra) { for (auto& p : pixels) { std::swap(p[0], p[2]); } }
        const UINT width = static_cast<UINT>(pixels.size());
        auto original = Texture(device, width, 1, pixels);
        auto encoded = Texture(device, width, 1, std::vector<Pixel>(pixels.size()));
        auto model = Texture(device, width, 1, std::vector<Pixel>(pixels.size(), Pixel{1, 1, 1, 1}));
        auto resolved = Texture(device, width, 1, std::vector<Pixel>(pixels.size()));
        auto expected = Readback(context, original.texture.Get());
        ResolveConstants params;
        params.sourceWidth = params.targetWidth = params.workWidth = width;
        params.sourceHeight = params.targetHeight = params.workHeight = 1;
        params.passthrough = !hdr; params.whitePoint = white; params.sourceIsBGRA = bgra;
        Dispatch(context, shader, constants, params, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, 1);
        params.mode = 1;
        for (float limit : {0.5f, 1.0f, 2.0f, 4.0f}) for (float strength : {0.0f, 0.5f, 1.0f, 2.0f})
            for (float colour : {0.0f, 1.0f, 2.0f}) {
                params.maxRatio = limit; params.transferStrength = strength; params.colourStrength = colour;
                Dispatch(context, shader, constants, params, {encoded.srv.Get(), model.srv.Get(), original.srv.Get()},
                    resolved.uav.Get(), width, 1);
                auto actual = Readback(context, resolved.texture.Get());
                for (size_t i = 0; i < actual.size(); ++i) {
                    Require(actual[i][3] == expected[i][3], "Ratio gain cannot change source alpha");
                    if (!strength) { Require(actual[i] == expected[i], "zero Ratio effect strength is exact identity even with a sub-unity cap"); continue; }
                    auto a = actual[i], e = expected[i];
                    if (bgra) { std::swap(a[0], a[2]); std::swap(e[0], e[2]); }
                    for (size_t c = 0; c < 3; ++c) { Require(std::isfinite(a[c]) && a[c] >= 0, "Ratio gain returns finite nonnegative RGB"); }
                    const float bound = luma(e) * limit;
                    if (luma(a) > bound * 1.002f + 0.0000001f) {
                        std::fprintf(stderr, "Gain mismatch: HDR=%d BGRA=%d strength=%g colour=%g max=%g pixel=%zu originalY=%g actualY=%g\n",
                            hdr, bgra, strength, colour, limit, i, luma(e), luma(a));
                        Require(false, "maximum Ratio luminance holds after colour mixing");
                    }
                    if (i == 0 && limit == 2 && strength == 1 && colour == 1) {
                        Require(luma(a) > luma(e) * 1.9f, "Ratio still transfers brightening up to the requested cap");
                    }
                }
            }
    }
    std::puts("Ratio gain: final luminance bound, zero effect, black, HDR/SDR, BGRA, alpha and colour extrapolation passed.");
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    ComPtr<ID3DBlob> code, errors;
    const auto& source = TheosRenderPipeline::SourceDLSSG::kNeuralResolveShader;
    const auto hr = D3DCompile(source, sizeof(source) - 1, "TRP-Ratio-color-test", nullptr, nullptr,
        "Ratio", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
    }
    Check(hr, "compile production Ratio shader");
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "Ratio compute shader");
    ComPtr<ID3D11ComputeShader> downsample;
    code.Reset(); errors.Reset();
    Check(D3DCompile(source, sizeof(source) - 1, "TRP-downsample-color-test", nullptr, nullptr,
        "Downsample", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "compile production downsample shader");
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &downsample), "downsample compute shader");
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = (sizeof(ResolveConstants) + 15) & ~UINT(15); desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    Check(device->CreateBuffer(&desc, nullptr, &constants), "Ratio constants");
    for (const bool bgra : {false, true}) {
        for (const float white : {1.0f, 16.0f, 64.0f}) {
            ModelIdentity(device.Get(), context.Get(), shader.Get(), constants.Get(), false, white, bgra);
            ModelIdentity(device.Get(), context.Get(), shader.Get(), constants.Get(), true, white, bgra, true);
            ModelIdentity(device.Get(), context.Get(), shader.Get(), constants.Get(), true, white, bgra, true, downsample.Get());
        }
        ModelIdentity(device.Get(), context.Get(), shader.Get(), constants.Get(), true, 1, bgra);
    }
    ProducerEdits(device.Get(), context.Get(), shader.Get(), constants.Get());
    RatioGain(device.Get(), context.Get(), shader.Get(), constants.Get());
    context->ClearState();
    std::puts("Ratio color: FP16 HDR/SDR identity, bounded input, channel order, alpha and active-region guards passed.");
}
