#include "FrameGen/GameFacingTargets.h"
#include <d3d11sdklayers.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Require(bool ok) { if (!ok) { std::fputs("direct output check failed\n",stderr); std::exit(1); } }
static void Check(HRESULT hr) { Require(SUCCEEDED(hr)); }
int main() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info; Check(device.As(&info));
    TheosRenderPipeline::GameFacingTargets targets;
    for (UINT width : {37u,64u,19u}) {
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=width;desc.Height=17;
            desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Check(targets.CreateUpscaleOutputAfterRetirement(device.Get(),desc));
            D3D11_TEXTURE2D_DESC actual{};targets.UpscaleOutput()->GetDesc(&actual);
            Require(actual.Width==width && actual.Height==17 && actual.Format==format && actual.MiscFlags==0);
            const bool fallback=format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            Require(bool(actual.BindFlags&D3D11_BIND_UNORDERED_ACCESS)!=fallback);
            if (!fallback) {
                ComPtr<ID3D11UnorderedAccessView> view;
                Check(device->CreateUnorderedAccessView(targets.UpscaleOutput(),nullptr,&view));
                const float color[]{0.25f,0.5f,0.75f,1};context->ClearUnorderedAccessViewFloat(view.Get(),color);
                auto read=actual;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&read,nullptr,&staging));
                context->CopyResource(staging.Get(),targets.UpscaleOutput());D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                if (format==DXGI_FORMAT_R8G8B8A8_UNORM) {
                    const auto* pixel=static_cast<const unsigned char*>(mapped.pData);
                    Require(pixel[0]==64 && pixel[1]==128 && pixel[2]==191 && pixel[3]==255);
                } else {
                    const auto* pixel=static_cast<const unsigned short*>(mapped.pData);
                    Require(pixel[0]==0x3400 && pixel[1]==0x3800 && pixel[2]==0x3a00 && pixel[3]==0x3c00);
                }
                context->Unmap(staging.Get(),0);
            }
            // Readback has retired all work referring to this output.
            targets.ResetUpscaleOutputAfterRetirement();
        }
    }
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());info->GetMessage(i,message,&size);
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) {std::fputs(message->pDescription,stderr);return 1;}
    }
    std::puts("direct output allocation/UAV writes/fallback/resize PASS");
}
