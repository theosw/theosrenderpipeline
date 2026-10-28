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
#include <memory>
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

// Bound at t3 by every dispatch: the measured white point, or null.
static ID3D11ShaderResourceView* g_exposure{};
static ID3D11ShaderResourceView* g_tone{};

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
    context->CSSetShaderResources(3, 1, &g_exposure);
    context->CSSetShaderResources(4, 1, &g_tone);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    // Dispatch over the larger allocation so the active-extent guard is exercised.
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* nullInputs[5]{};
    ID3D11UnorderedAccessView* nullOutput{};
    context->CSSetShaderResources(0, 5, nullInputs);
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

// Compare: columns left of SplitX are the original, columns from it NR's
// result, except a two-pixel divider that alternates black and white every six
// rows and keeps the result's alpha. Pixels outside the target are untouched.
static void SplitCompare(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* shader, ID3D11Buffer* constants)
{
    constexpr UINT width = 23, height = 19, activeWidth = 17, activeHeight = 14;
    constexpr Pixel sentinel{-0.5f, -0.25f, -0.125f, -2.0f};
    std::vector<Pixel> before(size_t(width) * height), after(before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        const float v = float(i % 97);
        before[i] = {v, v * 0.5f + 1, 3, 0.25f};
        after[i] = {64 - v, 2, v * 0.25f + 5, 0.75f};
    }
    auto original = Texture(device, width, height, before);
    auto result = Texture(device, width, height, after);
    for (const UINT splitX : {0u, 1u, 8u, activeWidth - 1, activeWidth}) {
        auto output = Texture(device, width, height, std::vector<Pixel>(before.size(), sentinel));
        ResolveConstants params{};
        params.sourceWidth = params.targetWidth = activeWidth;
        params.sourceHeight = params.targetHeight = activeHeight;
        params.splitX = splitX;
        Dispatch(context, shader, constants, params, {original.srv.Get(), result.srv.Get(), nullptr},
            output.uav.Get(), width, height);
        const auto read = Readback(context, output.texture.Get());
        const auto exact = [](const Pixel& a, const Pixel& b) {
            return std::memcmp(a.data(), b.data(), sizeof(Pixel)) == 0;
        };
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const size_t i = size_t(y) * width + x;
                if (x >= activeWidth || y >= activeHeight) {
                    Require(exact(read[i], sentinel), "split leaves pixels outside the target untouched");
                } else if (x == splitX || x + 1 == splitX) {
                    const float v = (y / 6) & 1 ? 1.0f : 0.0f;
                    Require(read[i][0] == v && read[i][1] == v && read[i][2] == v, "split divider alternates every six rows");
                    Require(read[i][3] == after[i][3], "split divider keeps NR's alpha");
                } else {
                    Require(exact(read[i], x < splitX ? before[i] : after[i]), "split shows the original left of the line");
                }
            }
        }
    }
    std::puts("Split compare: original left, NR right, divider, alpha and active-region guard passed.");
}

struct Exposure
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
static Exposure ExposureTexture(ID3D11Device* device, float value)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data{&value, sizeof(float), 0};
    Exposure result;
    Check(device->CreateTexture2D(&desc, &data, &result.texture), "exposure texture");
    Check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv), "exposure SRV");
    Check(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav), "exposure UAV");
    return result;
}
static float ReadExposure(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "exposure readback texture");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "exposure readback");
    const float value = *static_cast<const float*>(mapped.pData);
    context->Unmap(staging.Get(), 0);
    return value;
}

// Adaptive exposure: the geometric mean luminance maps to mid-grey (0.18),
// shifted by the bias; a reset snaps, otherwise it moves in log space at the
// given rate. Encoding and Ratio with that measured white point match the same
// fixed white point exactly.
static void AdaptiveExposure(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* measure, ID3D11ComputeShader* ratio, ID3D11Buffer* constants)
{
    constexpr UINT width = 37, height = 23;
    const auto close = [](float a, double b) { return std::abs(a - b) <= 1e-4 * std::abs(b); };
    const auto uniform = [&](float luma) {
        return Texture(device, width, height, std::vector<Pixel>(size_t(width) * height, Pixel{luma, luma, luma, 1}));
    };
    auto exposure = ExposureTexture(device, 0);
    ResolveConstants params{};
    params.sourceWidth = width; params.sourceHeight = height;
    params.exposureReset = 1; params.exposureRate = 0.5f;
    // FP16-exact luminances: 3/32 and 29/4.
    auto dim = uniform(0.09375f);
    g_exposure = nullptr;
    Dispatch(context, measure, constants, params, {dim.srv.Get(), nullptr, nullptr}, exposure.uav.Get(), 1, 1);
    Require(close(ReadExposure(context, exposure.texture.Get()), 0.09375 / 0.18), "reset snaps to the mean over mid-grey");
    auto bright = uniform(7.25f);
    params.exposureReset = 0;
    Dispatch(context, measure, constants, params, {bright.srv.Get(), nullptr, nullptr}, exposure.uav.Get(), 1, 1);
    // Halfway in log space between the two targets.
    Require(close(ReadExposure(context, exposure.texture.Get()), std::sqrt(0.09375 / 0.18 * (7.25 / 0.18))), "adaptation moves in log space");
    params.exposureReset = 1; params.exposureBias = 1;
    Dispatch(context, measure, constants, params, {bright.srv.Get(), nullptr, nullptr}, exposure.uav.Get(), 1, 1);
    Require(close(ReadExposure(context, exposure.texture.Get()), 7.25 / 0.18 / 2), "a positive bias brightens the model view");
    // A non-finite pixel counts as black instead of poisoning the mean.
    std::vector<Pixel> spotted(size_t(width) * height, Pixel{0.09375f, 0.09375f, 0.09375f, 1});
    spotted[0] = {std::numeric_limits<float>::infinity(), 0, 0, 1};
    auto withInf = Texture(device, width, height, spotted);
    params.exposureBias = 0;
    Dispatch(context, measure, constants, params, {withInf.srv.Get(), nullptr, nullptr}, exposure.uav.Get(), 1, 1);
    const float measured = ReadExposure(context, exposure.texture.Get());
    Require(std::isfinite(measured) && measured > 0 && measured <= 0.09375f / 0.18f, "a non-finite pixel is ignored");

    // The encode and the resolve read the measured value exactly as a fixed one.
    std::vector<Pixel> scene(size_t(width) * height);
    for (size_t i = 0; i < scene.size(); ++i) { scene[i] = {float(i % 13) * 0.75f, 1.5f, float(i % 5) * 4, 0.5f}; }
    auto original = Texture(device, width, height, scene);
    const auto run = [&](bool adaptive, float white) {
        auto encoded = Texture(device, width, height, std::vector<Pixel>(scene.size()));
        auto resolved = Texture(device, width, height, std::vector<Pixel>(scene.size()));
        auto measuredWhite = ExposureTexture(device, white);
        g_exposure = adaptive ? measuredWhite.srv.Get() : nullptr;
        ResolveConstants p{};
        p.sourceWidth = p.targetWidth = p.workWidth = width;
        p.sourceHeight = p.targetHeight = p.workHeight = height;
        p.passthrough = 0; p.whitePoint = adaptive ? 1234.0f : white; p.adaptiveExposure = adaptive;
        Dispatch(context, ratio, constants, p, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, height);
        // A model that brightens by a quarter, so the resolve has work to do.
        auto pixels = Readback(context, encoded.texture.Get());
        for (auto& pixel : pixels) { for (int c = 0; c < 3; ++c) { pixel[c] = (std::min)(pixel[c] * 1.25f, 1.0f); } }
        auto model = Texture(device, width, height, pixels);
        p.mode = 1;
        Dispatch(context, ratio, constants, p, {encoded.srv.Get(), model.srv.Get(), original.srv.Get()},
            resolved.uav.Get(), width, height);
        g_exposure = nullptr;
        return std::pair{Readback(context, encoded.texture.Get()), Readback(context, resolved.texture.Get())};
    };
    for (const float white : {0.5f, 3.0f, 40.0f}) {
        const auto fixed = run(false, white), adaptive = run(true, white);
        Require(std::memcmp(fixed.first.data(), adaptive.first.data(), fixed.first.size() * sizeof(Pixel)) == 0,
            "the encode reads the measured white point like a fixed one");
        Require(std::memcmp(fixed.second.data(), adaptive.second.data(), fixed.second.size() * sizeof(Pixel)) == 0,
            "the resolve reads the measured white point like a fixed one");
    }
    std::puts("Adaptive exposure: mid-grey mapping, bias, log-space adaptation, non-finite input and exact encode/resolve parity passed.");
}

// Before upscaling, the frame position u was rendered at u + jitter. The encode
// moves a jittered ramp onto the steady grid exactly (Catmull-Rom reproduces a
// linear ramp), and the resolve samples NR's change back at p - jitter, so an
// unchanged model returns the original jittered frame. Zero jitter keeps the
// direct per-pixel path.
static void SteadyGrid(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* ratio, ID3D11Buffer* constants)
{
    constexpr UINT width = 41, height = 29;
    constexpr float jitterX = 0.37f, jitterY = -0.21f;
    // A smooth scene in 0..1, sampled at each jittered pixel centre.
    const auto scene = [](float x, float y) {
        return Pixel{0.2f + 0.012f * x, 0.6f - 0.009f * y, 0.3f + 0.004f * x + 0.006f * y, 1};
    };
    std::vector<Pixel> jittered(size_t(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) { jittered[size_t(y) * width + x] = scene(x + 0.5f - jitterX, y + 0.5f - jitterY); }
    }
    auto original = Texture(device, width, height, jittered);
    auto encoded = Texture(device, width, height, std::vector<Pixel>(jittered.size()));
    ResolveConstants p{};
    p.sourceWidth = p.targetWidth = p.workWidth = width;
    p.sourceHeight = p.targetHeight = p.workHeight = height;
    p.passthrough = 1; p.jitterX = jitterX; p.jitterY = jitterY;
    g_exposure = nullptr;
    Dispatch(context, ratio, constants, p, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, height);
    const auto steady = Readback(context, encoded.texture.Get());
    float encodeError = 0;
    for (UINT y = 2; y + 2 < height; ++y) {
        for (UINT x = 2; x + 2 < width; ++x) {
            const auto expected = scene(x + 0.5f, y + 0.5f);
            for (int c = 0; c < 3; ++c) {
                encodeError = (std::max)(encodeError, std::abs(steady[size_t(y) * width + x][c] - expected[c]));
            }
        }
    }
    Require(encodeError < 0.002f, "the encode moves the jittered frame onto the steady grid");

    // An unchanged model: the resolve must return the jittered original.
    auto resolved = Texture(device, width, height, std::vector<Pixel>(jittered.size()));
    p.mode = 1;
    Dispatch(context, ratio, constants, p, {encoded.srv.Get(), encoded.srv.Get(), original.srv.Get()},
        resolved.uav.Get(), width, height);
    const auto result = Readback(context, resolved.texture.Get());
    float resolveError = 0;
    for (UINT y = 3; y + 3 < height; ++y) {
        for (UINT x = 3; x + 3 < width; ++x) {
            const size_t i = size_t(y) * width + x;
            for (int c = 0; c < 3; ++c) { resolveError = (std::max)(resolveError, std::abs(result[i][c] - jittered[i][c])); }
            Require(result[i][3] == jittered[i][3], "the resolve keeps the original alpha");
        }
    }
    Require(resolveError < 0.004f, "an unchanged model returns the jittered original");

    // NR brightening by a quarter shows up at the matching jittered pixels.
    std::vector<Pixel> brighter = steady;
    for (auto& pixel : brighter) { for (int c = 0; c < 3; ++c) { pixel[c] *= 1.25f; } }
    auto model = Texture(device, width, height, brighter);
    Dispatch(context, ratio, constants, p, {encoded.srv.Get(), model.srv.Get(), original.srv.Get()},
        resolved.uav.Get(), width, height);
    const auto brightened = Readback(context, resolved.texture.Get());
    const auto luma = [](const Pixel& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    for (UINT y = 3; y + 3 < height; ++y) {
        for (UINT x = 3; x + 3 < width; ++x) {
            const size_t i = size_t(y) * width + x;
            Require(std::abs(luma(brightened[i]) / luma(jittered[i]) - 1.25f) < 0.01f, "NR's change lands on the jittered pixel");
        }
    }
    std::printf("Steady grid: encode error %.5f, unchanged-model error %.5f, brightening and alpha passed.\n",
        encodeError, resolveError);
}

// The hue guard: a grey original stays grey when NR answers with a green cast,
// while a clearly coloured original still takes NR's colour change. And black
// is a legitimate answer: NR painting a shadow black over a lit surface darkens
// it, while a model output with nothing anywhere keeps the original.
static void GuardAndShadows(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ComputeShader* ratio, ID3D11Buffer* constants)
{
    constexpr UINT width = 16, height = 16;
    const auto luma = [](const Pixel& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    const auto run = [&](bool producer, const std::vector<Pixel>& scene, const std::vector<Pixel>& answer) {
        auto original = Texture(device, width, height, scene);
        auto encoded = Texture(device, width, height, std::vector<Pixel>(scene.size()));
        auto resolved = Texture(device, width, height, std::vector<Pixel>(scene.size()));
        ResolveConstants p{};
        p.sourceWidth = p.targetWidth = p.workWidth = width;
        p.sourceHeight = p.targetHeight = p.workHeight = height;
        p.passthrough = 0; p.producerColor = producer; p.maxRatio = 4;
        g_exposure = nullptr;
        Dispatch(context, ratio, constants, p, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, height);
        auto proxy = Readback(context, encoded.texture.Get());
        std::vector<Pixel> model(proxy.size());
        for (size_t i = 0; i < proxy.size(); ++i) {
            for (int c = 0; c < 3; ++c) { model[i][c] = answer[i][c] < 0 ? proxy[i][c] * -answer[i][c] : answer[i][c]; }
            model[i][3] = proxy[i][3];
        }
        auto modelTexture = Texture(device, width, height, model);
        p.mode = 1;
        Dispatch(context, ratio, constants, p, {encoded.srv.Get(), modelTexture.srv.Get(), original.srv.Get()},
            resolved.uav.Get(), width, height);
        return Readback(context, resolved.texture.Get());
    };
    // Negative answer entries scale the proxy channel; others are absolute.
    const Pixel greenCast{-0.97f, -1.04f, -0.97f, 1}, unchanged{-1, -1, -1, 1};
    for (const bool producer : {false, true}) {
        std::vector<Pixel> scene(size_t(width) * height, Pixel{0.4f, 0.4f, 0.4f, 1});
        std::vector<Pixel> answer(scene.size(), greenCast);
        for (size_t i = 0; i < scene.size(); i += 2) { scene[i] = {0.6f, 0.15f, 0.1f, 1}; }
        const auto out = run(producer, scene, answer);
        for (size_t i = 1; i < scene.size(); i += 2) {
            const float spread = (std::max)({out[i][0], out[i][1], out[i][2]}) - (std::min)({out[i][0], out[i][1], out[i][2]});
            Require(spread < 0.002f, "a grey original stays grey under a green cast");
        }
        for (size_t i = 0; i < scene.size(); i += 2) {
            Require(out[i][1] > scene[i][1] * 1.01f, "a coloured original still takes NR's colour change");
        }
    }
    // Black shadow over a lit surface: one pixel answers black, the rest unchanged.
    std::vector<Pixel> lit(size_t(width) * height, Pixel{0.8f, 0.7f, 0.6f, 1});
    std::vector<Pixel> shadow(lit.size(), unchanged);
    shadow[5 * width + 5] = {0, 0, 0, 1};
    auto out = run(false, lit, shadow);
    Require(luma(out[5 * width + 5]) < 0.01f * luma(lit[5 * width + 5]), "NR painting black darkens a lit pixel");
    Require(std::abs(luma(out[0]) - luma(lit[0])) < 0.01f, "unchanged pixels keep their brightness");
    // An empty model output (all black) is no answer: the original is kept, on
    // producer colour too.
    for (const bool producer : {false, true}) {
        out = run(producer, lit, std::vector<Pixel>(lit.size(), Pixel{0, 0, 0, 1}));
        for (size_t i = 0; i < lit.size(); ++i) {
            Require(std::abs(luma(out[i]) - luma(lit[i])) < 0.01f, "an empty model output keeps the original");
        }
    }
    std::puts("Hue guard and shadows: grey stays grey, colour still moves, black answers darken, empty output kept.");
}

// Broad and Detail. NR's change in stops is split into its edge-aware blur
// (broad) and the rest (detail). A uniform brightening is all broad; a fine
// checkerboard is all detail. At 1 and 1 the result is bit-identical to off.
struct Tone { ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav; };
static Tone ToneTexture(ID3D11Device* device, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Tone result;
    Check(device->CreateTexture2D(&desc, nullptr, &result.texture), "tone texture");
    Check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv), "tone SRV");
    Check(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav), "tone UAV");
    return result;
}
static void BroadDetail(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ComputeShader* ratio,
    ID3D11ComputeShader* prepare, ID3D11ComputeShader* filter, ID3D11Buffer* constants)
{
    constexpr UINT width = 64, height = 48;
    const auto luma = [](const Pixel& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    std::vector<Pixel> scene(size_t(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) { scene[size_t(y) * width + x] = {0.3f + 0.004f * x, 0.35f, 0.3f + 0.003f * y, 1}; }
    }
    auto original = Texture(device, width, height, scene);
    auto encoded = Texture(device, width, height, std::vector<Pixel>(scene.size()));
    ResolveConstants p{};
    p.sourceWidth = p.targetWidth = p.workWidth = width;
    p.sourceHeight = p.targetHeight = p.workHeight = height;
    p.passthrough = 0; p.maxRatio = 4;
    g_exposure = g_tone = nullptr;
    Dispatch(context, ratio, constants, p, {original.srv.Get(), nullptr, nullptr}, encoded.uav.Get(), width, height);
    const auto proxy = Readback(context, encoded.texture.Get());
    const auto resolve = [&](const std::vector<Pixel>& answer, bool bands, float broad, float detail) {
        auto model = Texture(device, width, height, answer);
        auto tone = ToneTexture(device, width, height), scratch = ToneTexture(device, width, height);
        auto resolved = Texture(device, width, height, std::vector<Pixel>(scene.size()));
        ResolveConstants q = p;
        q.broad = broad; q.detail = detail; q.toneBands = bands;
        if (bands) {
            Dispatch(context, prepare, constants, q, {encoded.srv.Get(), model.srv.Get(), nullptr}, tone.uav.Get(), width, height);
            q.mode = 0;
            Dispatch(context, filter, constants, q, {tone.srv.Get(), nullptr, nullptr}, scratch.uav.Get(), width, height);
            q.mode = 1;
            Dispatch(context, filter, constants, q, {scratch.srv.Get(), nullptr, nullptr}, tone.uav.Get(), width, height);
            g_tone = tone.srv.Get();
        }
        q.mode = 1;
        Dispatch(context, ratio, constants, q, {encoded.srv.Get(), model.srv.Get(), original.srv.Get()}, resolved.uav.Get(), width, height);
        g_tone = nullptr;
        return Readback(context, resolved.texture.Get());
    };
    // NR's answers in the proxy's display encoding: uniform +25% light, or a
    // one-pixel checkerboard of +/-20% light.
    const auto encode = [](float linear) { return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f; };
    const auto decode = [](float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); };
    std::vector<Pixel> uniform(proxy), checker(proxy);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t i = size_t(y) * width + x;
            const float flip = ((x + y) & 1) ? 1.2f : 0.8f;
            for (int c = 0; c < 3; ++c) {
                uniform[i][c] = encode((std::min)(decode(proxy[i][c]) * 1.25f, 1.0f));
                checker[i][c] = encode((std::min)(decode(proxy[i][c]) * flip, 1.0f));
            }
        }
    }
    const auto off = resolve(uniform, false, 1, 1), neutral = resolve(uniform, true, 1, 1);
    Require(std::memcmp(off.data(), neutral.data(), off.size() * sizeof(Pixel)) == 0, "Broad and Detail at 1 match off exactly");
    // An empty answer is no edit, whatever Broad and Detail are set to.
    for (const float broad : {0.0f, 2.0f}) {
        const auto empty = resolve(std::vector<Pixel>(proxy.size(), Pixel{0, 0, 0, 1}), true, broad, 1);
        for (size_t i = 0; i < scene.size(); ++i) {
            for (int c = 0; c < 3; ++c) { Require(std::abs(empty[i][c] - scene[i][c]) < 0.002f, "an empty answer leaves the tone bands inert"); }
        }
    }
    // Interior pixels, away from the blur's clamped border.
    const auto average = [&](const std::vector<Pixel>& image, int parity) {
        double sum = 0; int count = 0;
        for (UINT y = 12; y < height - 12; ++y) {
            for (UINT x = 12; x < width - 12; ++x) {
                if (parity >= 0 && int((x + y) & 1) != parity) { continue; }
                const size_t i = size_t(y) * width + x;
                sum += luma(image[i]) / luma(scene[i]); ++count;
            }
        }
        return sum / count;
    };
    Require(std::abs(average(off, -1) - 1.25) < 0.02, "the uniform edit brightens by a quarter");
    Require(std::abs(average(resolve(uniform, true, 0, 1), -1) - 1.0) < 0.02, "Broad 0 removes a uniform relighting");
    Require(std::abs(average(resolve(uniform, true, 1, 0), -1) - 1.25) < 0.02, "Detail 0 keeps a uniform relighting");
    const auto keptDetail = resolve(checker, true, 0, 1), noDetail = resolve(checker, true, 1, 0);
    Require(average(keptDetail, 1) > 1.12 && average(keptDetail, 0) < 0.88, "Broad 0 keeps fine detail");
    Require(std::abs(average(noDetail, 1) - average(noDetail, 0)) < 0.06, "Detail 0 removes fine detail");
    std::puts("Broad and Detail: neutral is exact, uniform relighting is broad, a checkerboard is detail.");
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
    ComPtr<ID3D11ComputeShader> split;
    code.Reset(); errors.Reset();
    Check(D3DCompile(source, sizeof(source) - 1, "TRP-split-compare-test", nullptr, nullptr,
        "Split", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "compile production split shader");
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &split), "split compute shader");
    SplitCompare(device.Get(), context.Get(), split.Get(), constants.Get());
    ComPtr<ID3D11ComputeShader> measure;
    code.Reset(); errors.Reset();
    Check(D3DCompile(source, sizeof(source) - 1, "TRP-exposure-test", nullptr, nullptr,
        "MeasureExposure", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "compile production exposure shader");
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &measure), "exposure compute shader");
    AdaptiveExposure(device.Get(), context.Get(), measure.Get(), shader.Get(), constants.Get());
    SteadyGrid(device.Get(), context.Get(), shader.Get(), constants.Get());
    GuardAndShadows(device.Get(), context.Get(), shader.Get(), constants.Get());
    ComPtr<ID3D11ComputeShader> prepareTone, filterTone;
    for (auto [entry, target] : {std::pair{"PrepareTone", std::addressof(prepareTone)}, std::pair{"FilterTone", std::addressof(filterTone)}}) {
        code.Reset(); errors.Reset();
        Check(D3DCompile(source, sizeof(source) - 1, entry, nullptr, nullptr, entry, "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "compile production tone shader");
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, target->GetAddressOf()), "tone compute shader");
    }
    BroadDetail(device.Get(), context.Get(), shader.Get(), prepareTone.Get(), filterTone.Get(), constants.Get());
    context->ClearState();
    std::puts("Ratio color: FP16 HDR/SDR identity, bounded input, channel order, alpha and active-region guards passed.");
}
