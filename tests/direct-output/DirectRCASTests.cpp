#include "FrameGen/GameFacingTargets.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>
using Microsoft::WRL::ComPtr;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Check(HRESULT hr) { Require(SUCCEEDED(hr), "D3D call"); }
int wmain(int argc, wchar_t** argv) {
    Require(argc == 2, "shader path");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<ID3D11InfoQueue> info; Check(device.As(&info));
    unsigned cases = 0;
    for (const char* sharpness : {"0.100", "0.670", "1.000"}) {
        D3D_SHADER_MACRO macros[]{{"SHARPNESS", sharpness}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        auto hr = D3DCompileFromFile(argv[1], macros, nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
        if (FAILED(hr) && errors) { std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr); }
        Check(hr);
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        for (auto size : {std::array<UINT,2>{37,17}, {64,32}, {19,41}}) {
            const auto width = size[0], height = size[1];
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            std::vector<std::array<float,4>> pattern(width*height);
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
                pattern[y*width+x] = {float(x%11)/10, float(y%7)/6, float((x+y)%5)/4, 1.0f};
            }
            D3D11_SUBRESOURCE_DATA initial{pattern.data(), width*16, 0};
            ComPtr<ID3D11Texture2D> input; ComPtr<ID3D11ShaderResourceView> srv;
            Check(device->CreateTexture2D(&desc, &initial, &input));
            Check(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
            for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
                desc.Format=format;
                desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
                ComPtr<ID3D11Texture2D> scratch, copied;
                Check(device->CreateTexture2D(&desc, nullptr, &scratch));
                auto plain=desc; plain.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
                Check(device->CreateTexture2D(&plain, nullptr, &copied));
                TheosRenderPipeline::GameFacingTargets targets;
                Check(targets.CreateUpscaleOutputAfterRetirement(device.Get(), plain));
                auto dispatch = [&](ID3D11Texture2D* output) {
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    Check(device->CreateUnorderedAccessView(output, nullptr, &uav));
                    ID3D11ShaderResourceView* srvs[]{srv.Get()};
                    ID3D11UnorderedAccessView* uavs[]{uav.Get()};
                    context->CSSetShader(shader.Get(), nullptr, 0);
                    context->CSSetShaderResources(0, 1, srvs);
                    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
                    context->Dispatch((width+7)/8, (height+7)/8, 1);
                    srvs[0]=nullptr; uavs[0]=nullptr;
                    context->CSSetShaderResources(0, 1, srvs);
                    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
                    context->CSSetShader(nullptr, nullptr, 0);
                };
                dispatch(scratch.Get()); context->CopyResource(copied.Get(), scratch.Get());
                dispatch(targets.UpscaleOutput());
                auto read = [&](ID3D11Texture2D* texture) {
                    auto stagingDesc=plain;
                    stagingDesc.BindFlags=0; stagingDesc.Usage=D3D11_USAGE_STAGING;
                    stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> staging;
                    Check(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
                    context->CopyResource(staging.Get(), texture);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                    const auto rowBytes=width*(format==DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8);
                    std::vector<unsigned char> bytes(rowBytes*height);
                    for (UINT y=0;y<height;++y) std::memcpy(bytes.data()+y*rowBytes,
                        static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,rowBytes);
                    context->Unmap(staging.Get(),0); return bytes;
                };
                Require(read(copied.Get())==read(targets.UpscaleOutput()), "RCAS direct/copy pixels differ");
                targets.ResetUpscaleOutputAfterRetirement(); ++cases;
            }
        }
    }
    for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size{}; info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data()); info->GetMessage(i,message,&size);
        Require(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,message->pDescription);
    }
    std::printf("PASS: %u RCAS direct/copy comparisons, exact full-image pixels, clean D3D11 debug layer\n",cases);
}
