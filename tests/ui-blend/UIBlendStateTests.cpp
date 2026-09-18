#include "FrameGen/NativeUIBlend.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Require(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static void Check(HRESULT hr,const char* why){Require(SUCCEEDED(hr),why);}
int main(){
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c),"device");
    ComPtr<ID3D11InfoQueue> info;Check(d.As(&info),"info");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=19;desc.Height=13;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    auto texture=[&]{ComPtr<ID3D11Texture2D> t;Check(d->CreateTexture2D(&desc,nullptr,&t),"texture");return t;};
    auto output=texture(),layer=texture(),sentinel=texture(),omStorage=texture(),csStorage=texture();
    ComPtr<ID3D11ShaderResourceView> outputSRV,layerSRV;ComPtr<ID3D11RenderTargetView> outputRTV,layerRTV,sentinelRTV;
    Check(d->CreateShaderResourceView(output.Get(),nullptr,&outputSRV),"output SRV");Check(d->CreateShaderResourceView(layer.Get(),nullptr,&layerSRV),"layer SRV");
    Check(d->CreateRenderTargetView(output.Get(),nullptr,&outputRTV),"output RTV");Check(d->CreateRenderTargetView(layer.Get(),nullptr,&layerRTV),"layer RTV");
    Check(d->CreateRenderTargetView(sentinel.Get(),nullptr,&sentinelRTV),"sentinel RTV");
    ComPtr<ID3D11UnorderedAccessView> omUAV,csUAV;Check(d->CreateUnorderedAccessView(omStorage.Get(),nullptr,&omUAV),"OM UAV");Check(d->CreateUnorderedAccessView(csStorage.Get(),nullptr,&csUAV),"CS UAV");
    const float base[]{0.25f,0.5f,0.75f,0.5f},front[]{0.1f,0.2f,0.3f,0.25f};c->ClearRenderTargetView(outputRTV.Get(),base);c->ClearRenderTargetView(layerRTV.Get(),front);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;ComPtr<ID3D11Buffer> stream;
    Check(d->CreateBuffer(&bd,nullptr,&stream),"stream output");auto* streamRaw=stream.Get();UINT offset=16;c->SOSetTargets(1,&streamRaw,&offset);
    auto* sr=outputSRV.Get();c->PSSetShaderResources(0,1,&sr);c->VSSetShaderResources(5,1,&sr);c->CSSetShaderResources(7,1,&sr);
    auto* rt=sentinelRTV.Get();auto* ou=omUAV.Get();auto* cu=csUAV.Get();c->CSSetUnorderedAccessViews(2,1,&cu,nullptr);
    c->OMSetRenderTargetsAndUnorderedAccessViews(1,&rt,nullptr,3,1,&ou,nullptr);
    const D3D11_VIEWPORT vp{2,3,7,5,0.2f,0.8f};c->RSSetViewports(1,&vp);const D3D11_RECT scissor{1,2,4,6};c->RSSetScissorRects(1,&scissor);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    TheosRenderPipeline::NativeUIBlend blend;Require(blend.Initialize(d.Get(),c.Get(),desc.Format),"direct path available");
    Require(blend.Compose(c.Get(),output.Get(),layerSRV.Get(),19,13),"direct draw");
    ComPtr<ID3D11ShaderResourceView> restored; c->PSGetShaderResources(0,1,&restored);Require(restored.Get()==outputSRV.Get(),"aliased PS SRV restored");
    restored.Reset();c->VSGetShaderResources(5,1,&restored);Require(restored.Get()==outputSRV.Get(),"aliased VS SRV restored");
    restored.Reset();c->CSGetShaderResources(7,1,&restored);Require(restored.Get()==outputSRV.Get(),"aliased CS SRV restored");
    ComPtr<ID3D11RenderTargetView> savedRT;ComPtr<ID3D11UnorderedAccessView> savedOM,savedCS;
    c->OMGetRenderTargetsAndUnorderedAccessViews(1,&savedRT,nullptr,3,1,&savedOM);c->CSGetUnorderedAccessViews(2,1,&savedCS);
    Require(savedRT.Get()==rt&&savedOM.Get()==ou&&savedCS.Get()==cu,"RTV and both UAV binding sets restored");
    ComPtr<ID3D11Buffer> savedStream;c->SOGetTargets(1,&savedStream);Require(savedStream.Get()==stream.Get(),"stream target restored");
    D3D11_VIEWPORT savedVP{};UINT count=1;c->RSGetViewports(&count,&savedVP);Require(count==1&&savedVP.TopLeftX==2&&savedVP.MinDepth==0.2f,"viewport restored");
    D3D11_RECT savedScissor{};count=1;c->RSGetScissorRects(&count,&savedScissor);Require(savedScissor.left==1&&savedScissor.bottom==6,"scissor restored");
    D3D11_PRIMITIVE_TOPOLOGY topology{};c->IAGetPrimitiveTopology(&topology);Require(topology==D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,"topology restored");
    auto read=desc;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
    Check(d->CreateTexture2D(&read,nullptr,&staging),"staging");c->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"readback");
    for(UINT y=0;y<13;++y)for(UINT x=0;x<19;++x){const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)+x*4;
        for(unsigned k=0;k<3;++k)Require(std::abs(p[k]-(front[k]+base[k]*0.75f))<0.00001f,"direct RGB");Require(p[3]==0.5f,"maximum alpha");}
    c->Unmap(staging.Get(),0);
    ComPtr<ID3D11Predicate> predicate;D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE,0};Check(d->CreatePredicate(&query,&predicate),"predicate");
    c->Begin(predicate.Get());c->End(predicate.Get());c->SetPredication(predicate.Get(),TRUE);
    Require(!blend.Compose(c.Get(),output.Get(),layerSRV.Get(),19,13),"predicate takes unchanged fallback");
    ComPtr<ID3D11Predicate> savedPredicate;BOOL value{};c->GetPredication(&savedPredicate,&value);Require(savedPredicate==predicate&&value==TRUE,"predicate untouched");
    c->SetPredication(nullptr,FALSE);c->ClearState();c->Flush();
    // Readback retires the last draw before owner reset.
    c->CopyResource(staging.Get(),output.Get());Check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"retire");c->Unmap(staging.Get(),0);blend.ResetAfterRetirement();
    Require(!blend.Initialize(d.Get(),c.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),"sRGB fallback");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());info->GetMessage(i,message,&size);Require(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,message->pDescription);}
    std::puts("UI blend pixel/binding-alias/OM-UAV/CS-UAV/stream-output/viewport/scissor/predicate/fallback PASS");
}
