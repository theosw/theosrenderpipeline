#include "FrameGen/D3D11FrameCopy.h"
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string_view>

using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::FrameExtent;
namespace Copy = TheosRenderPipeline::D3D11FrameCopy;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height,
    DXGI_FORMAT format, UINT bindings, const void* data = nullptr)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = bindings;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = data; initial.SysMemPitch = width * 4;
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture), "create texture");
    return texture;
}

static std::vector<float> Pixels(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc, nullptr, &staging), "staging");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback");
    std::vector<float> pixels(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(pixels.data() + y * desc.Width, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

static void ExpectRegion(const std::vector<float>& pixels, const std::vector<float>& original,
    FrameExtent extent, UINT sourceWidth, float tolerance = 0)
{
    Require(pixels.size() == extent.width * extent.height, "tight output allocation");
    for (UINT y = 0; y < extent.height; ++y) {
        for (UINT x = 0; x < extent.width; ++x) {
            Require(std::abs(pixels[y * extent.width + x] - original[y * sourceWidth + x]) <= tolerance,
                "active pixels preserve coordinates, including the partial dispatch edge");
        }
    }
}

int main(int argc, char** argv)
{
    const bool requireWrapped = argc == 2 && std::string_view(argv[1]) == "--require-wrapped";
    Require(argc == 1 || requireWrapped, "supported arguments");
    // Import DXGI so a locally staged ReShade proxy is loaded in the explicit
    // wrapped run. Normal CTest uses WARP with no external DLL requirement.
    ComPtr<IDXGIFactory> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, requireWrapped ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "copy device");
    constexpr UINT width = 22, height = 17;
    constexpr FrameExtent active{13, 9};
    std::vector<float> scene(width * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) { scene[y * width + x] = float(y * width + x + 1) / 512; }
    }
    auto world = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, scene.data());
    auto captured = Texture(device.Get(), active.width, active.height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    Check(Copy::Color(context.Get(), world.Get(), captured.Get(), active), "crop color/motion input");
    ExpectRegion(Pixels(context.Get(), captured.Get()), scene, active, width);
    const std::vector<float> replacement(active.width * active.height, 0.875f);
    context->UpdateSubresource(captured.Get(), 0, nullptr, replacement.data(), active.width * 4, 0);
    Check(Copy::Color(context.Get(), captured.Get(), world.Get(), active), "return active NR result");
    auto returned = Pixels(context.Get(), world.Get());
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Require(returned[y * width + x] == (x < active.width && y < active.height ? 0.875f : scene[y * width + x]),
                "return copy changes only the active area");
        }
    }
    context->UpdateSubresource(world.Get(), 0, nullptr, scene.data(), width * 4, 0);
    Require(Pixels(context.Get(), captured.Get()) == replacement, "snapshot survives producer overwrite");
    auto full = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, 0);
    Check(Copy::Color(context.Get(), world.Get(), full.Get(), {width, height}), "unchanged full-size copy");
    Require(Pixels(context.Get(), full.Get()) == scene, "native full-size pixels");
    Require(FAILED(Copy::Color(context.Get(), world.Get(), captured.Get(), {0, 9})), "zero region rejected");
    Require(FAILED(Copy::Color(context.Get(), world.Get(), captured.Get(), {14, 9})), "destination overflow rejected");
    Require(FAILED(Copy::Color(context.Get(), captured.Get(), world.Get(), {14, 9})), "source overflow rejected");
    Require(FAILED(Copy::Color(context.Get(), world.Get(), world.Get(), active)), "alias rejected");
    Require(FAILED(Copy::Color(nullptr, world.Get(), captured.Get(), active)), "null context rejected");
    auto incompatible = Texture(device.Get(), active.width, active.height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    Require(FAILED(Copy::Color(context.Get(), world.Get(), incompatible.Get(), active)), "format mismatch rejected");
    Require(Pixels(context.Get(), captured.Get()) == replacement, "invalid copies leave previous pixels untouched");
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, &otherContext), "second WARP device");
    Require(FAILED(Copy::Color(otherContext.Get(), world.Get(), captured.Get(), active)), "foreign device rejected");
    ComPtr<ID3D11DeviceContext> deferred; Check(device->CreateDeferredContext(0, &deferred), "deferred context");
    Require(FAILED(Copy::Color(deferred.Get(), world.Get(), captured.Get(), active)), "deferred ordering rejected");

    // Use unrelated bound resources to verify that the producer's compute state
    // survives capture. These are real views/shaders, not scripted callbacks.
    ComPtr<ID3D11ShaderResourceView> savedSRV; Check(device->CreateShaderResourceView(world.Get(), nullptr, &savedSRV), "saved SRV");
    auto scratch = Texture(device.Get(), 4, 4, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11UnorderedAccessView> savedUAV; Check(device->CreateUnorderedAccessView(scratch.Get(), nullptr, &savedUAV), "saved UAV");
    constexpr char noop[] = "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){}";
    ComPtr<ID3DBlob> code; Check(D3DCompile(noop, sizeof(noop) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr), "sentinel shader code");
    ComPtr<ID3D11ComputeShader> savedShader; Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &savedShader), "sentinel shader");
    ComPtr<ID3D11Device> shaderOwner;
    savedShader->GetDevice(&shaderOwner);
    const bool differentShaderOwner = !Copy::SameObject(device.Get(), shaderOwner.Get());
    std::printf("shaderDeviceDiffersFromCreationDevice=%u\n", differentShaderOwner);
    Require(!requireWrapped || differentShaderOwner, "wrapped mode must reproduce the shader/creation-device identity split");
    context->CSSetShader(savedShader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, savedSRV.GetAddressOf()); context->CSSetShaderResources(3, 1, savedSRV.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, savedUAV.GetAddressOf(), nullptr);

    Copy::Depth depthCopy;
    for (const auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS}) {
        std::vector<UINT> packed(scene.size());
        if (format == DXGI_FORMAT_R24G8_TYPELESS) {
            for (size_t i = 0; i < scene.size(); ++i) { packed[i] = UINT(scene[i] * 16777215.0f) | 0xAB000000; }
        }
        const bool depthStencil = format != DXGI_FORMAT_R32_FLOAT;
        auto depth = Texture(device.Get(), width, height, format,
            D3D11_BIND_SHADER_RESOURCE | (depthStencil ? D3D11_BIND_DEPTH_STENCIL : 0),
            format == DXGI_FORMAT_R24G8_TYPELESS ? static_cast<void*>(packed.data()) : static_cast<void*>(scene.data()));
        for (const auto extent : {active, FrameExtent{7, 5}, FrameExtent{width, height}}) {
            auto target = Texture(device.Get(), extent.width, extent.height, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
            Check(depthCopy.Copy(context.Get(), depth.Get(), target.Get(), extent), "capture typed/typeless depth");
            ExpectRegion(Pixels(context.Get(), target.Get()), scene, extent, width, 0.0000002f);
            ComPtr<ID3D11ComputeShader> shader; ComPtr<ID3D11ShaderResourceView> srv, untouched;
            ComPtr<ID3D11UnorderedAccessView> uav;
            context->CSGetShader(&shader, nullptr, nullptr); context->CSGetShaderResources(0, 1, &srv);
            context->CSGetShaderResources(3, 1, &untouched); context->CSGetUnorderedAccessViews(0, 1, &uav);
            Require(shader == savedShader && srv == savedSRV && untouched == savedSRV && uav == savedUAV, "compute bindings restored");
            Require(FAILED(depthCopy.Copy(context.Get(), depth.Get(), target.Get(), {extent.width + 1, extent.height})), "depth extent mismatch rejected");
            Require(FAILED(depthCopy.Copy(otherContext.Get(), depth.Get(), target.Get(), extent)), "foreign depth context rejected");
            auto foreignSource = Texture(otherDevice.Get(), width, height, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE, scene.data());
            auto foreignTarget = Texture(otherDevice.Get(), extent.width, extent.height, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
            Require(FAILED(depthCopy.Copy(otherContext.Get(), foreignSource.Get(), foreignTarget.Get(), extent)),
                "cached shader cannot be reused on another otherwise valid device");
            Check(depthCopy.Copy(context.Get(), depth.Get(), target.Get(), extent), "legitimate reuse after foreign rejection");
            ExpectRegion(Pixels(context.Get(), target.Get()), scene, extent, width, 0.0000002f);
            if (depthStencil && extent.width == width) {
                ComPtr<ID3D11DepthStencilView> dsv;
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                desc.Format = format == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
                Check(device->CreateDepthStencilView(depth.Get(), &desc, &dsv), "source DSV");
                context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.625f, 0);
                ExpectRegion(Pixels(context.Get(), target.Get()), scene, extent, width, 0.0000002f);
                Check(depthCopy.Copy(context.Get(), depth.Get(), target.Get(), extent), "refresh same cached views");
                const auto refreshed = Pixels(context.Get(), target.Get());
                for (auto pixel : refreshed) { Require(std::abs(pixel - 0.625f) < 0.0000002f, "next frame uses fresh depth"); }
            }
        }
        depthCopy.ResetViews();
    }
    context->ClearState();
    std::puts("Frame copies: cropped/full color, bounded return, source overwrite, three depth formats, resize, state restoration and invalid inputs passed.");
}
