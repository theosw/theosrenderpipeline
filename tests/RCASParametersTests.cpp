#include "RCASParameters.h"
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }
constexpr UINT Size = 16;
using Pixel = std::array<float, 4>;
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT bind, const std::vector<Pixel>* values = nullptr)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = Size; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data{values ? values->data() : nullptr, Size * sizeof(Pixel), 0};
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, values ? &data : nullptr, &texture), "texture");
    return texture;
}
static ComPtr<ID3D11ComputeShader> Shader(ID3D11Device* device, const wchar_t* path, const char* constant = nullptr)
{
    const D3D_SHADER_MACRO macros[]{{"SHARPNESS", constant}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompileFromFile(path, constant ? macros : nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
    if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer())); }
    Check(hr, "compile actual packaged RCAS shader");
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "shader");
    return shader;
}
static std::vector<Pixel> Run(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
    ID3D11ShaderResourceView* input, ID3D11Texture2D* output, ID3D11UnorderedAccessView* uav)
{
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &input); context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch(Size / 8, Size / 8, 1);
    ID3D11ShaderResourceView* noInput{}; ID3D11UnorderedAccessView* noOutput{};
    context->CSSetShaderResources(0, 1, &noInput); context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback; Check(device->CreateTexture2D(&desc, nullptr, &readback), "readback texture");
    context->CopyResource(readback.Get(), output);
    D3D11_MAPPED_SUBRESOURCE data{};
    Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &data), "readback map");
    std::vector<Pixel> values(Size * Size);
    for (UINT y = 0; y < Size; ++y) {
        std::memcpy(values.data() + y * Size, static_cast<const char*>(data.pData) + y * data.RowPitch, Size * sizeof(Pixel));
    }
    context->Unmap(readback.Get(), 0);
    return values;
}
int main(int argc, char** argv)
{
    Require(argc == 2, "shader path argument");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "WARP device");
    std::vector<Pixel> original(Size * Size);
    for (UINT y = 0; y < Size; ++y) { for (UINT x = 0; x < Size; ++x) {
        const auto f = .15f + .6f * static_cast<float>((x * 13 + y * 7) % 29) / 28;
        original[y * Size + x] = {f, .2f + f * .6f, .8f - f * .4f, 1};
    } }
    auto input = Texture(device.Get(), D3D11_BIND_SHADER_RESOURCE, &original);
    auto output = Texture(device.Get(), D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav;
    Check(device->CreateShaderResourceView(input.Get(), nullptr, &srv), "SRV");
    Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "UAV");
    const auto path = std::filesystem::path(argv[1]).wstring();
    const auto dynamic = Shader(device.Get(), path.c_str());
    const D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ComPtr<ID3D11Buffer> originalBinding;
    Check(device->CreateBuffer(&desc, nullptr, &originalBinding), "original constant buffer");
    auto* before = originalBinding.Get(); context->CSSetConstantBuffers(0, 1, &before);
    TheosRenderPipeline::RCASParameters parameters;
    ID3D11Buffer* firstBuffer{};
    float difference{};
    for (float strength : {0.0f, .125f, .3f, .672f, 1.0f}) {
        Check(parameters.Update(device.Get(), context.Get(), strength), "production runtime parameter upload");
        if (!firstBuffer) { firstBuffer = parameters.Buffer(); }
        Require(parameters.Buffer() == firstBuffer, "strength changes reuse constant buffer");
        std::vector<Pixel> actual;
        {
            auto binding = parameters.Bind(context.Get());
            actual = Run(device.Get(), context.Get(), dynamic.Get(), srv.Get(), output.Get(), uav.Get());
        }
        ComPtr<ID3D11Buffer> restored; context->CSGetConstantBuffers(0, 1, &restored);
        Require(restored.Get() == before, "caller constant binding restored after dispatch");
        const auto literal = std::to_string(strength);
        const auto legacy = Shader(device.Get(), path.c_str(), literal.c_str());
        const auto expected = Run(device.Get(), context.Get(), legacy.Get(), srv.Get(), output.Get(), uav.Get());
        for (std::size_t i = 0; i < actual.size(); ++i) { for (std::size_t c = 0; c < 3; ++c) {
            Require(std::isfinite(actual[i][c]) && std::abs(actual[i][c] - expected[i][c]) < 0.000002f,
                "runtime strength matches original compile-time RCAS pixels");
            if (strength == 1) { difference = (std::max)(difference, std::abs(actual[i][c] - original[i][c])); }
        } }
    }
    Require(difference > .01f, "fixture observes a real sharpening change");
    for (int i = 0; i <= 100; ++i) {
        Check(parameters.Update(device.Get(), context.Get(), i / 100.0f), "continuous ramp uploads");
        Require(parameters.Buffer() == firstBuffer, "ramp does not recreate resources");
    }
    std::puts("PASS: runtime RCAS strength matches constant shader pixels and preserves caller bindings");
}
