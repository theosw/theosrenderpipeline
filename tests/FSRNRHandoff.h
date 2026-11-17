// Standalone integration check: feed the actual NR result through the production
// FSR D3D11/D3D12 bridge on the same transport, then inspect its output.
#include "FrameGen/FSRUpscaler.h"
struct FSRHandoff {
    TheosRenderPipeline::FSRUpscaler fsr;
    TheosRenderPipeline::SourceDLSSG::SharedTexture color,motion,depth;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
    bool enabled{};
    void Initialize(ID3D11Device* d11,ID3D12Device* d12,TheosRenderPipeline::SourceDLSSG::Interop& interop,
        UINT w,UINT h,UINT gw,UINT gh) {
        wchar_t directory[32768]{};const auto length=GetEnvironmentVariableW(L"TRP_NR_FSR_TEST_ROOT",directory,32768);
        if(!length)return;
        if(length>=32768)throw std::runtime_error("FSR test runtime path too long");
        check(fsr.Open(d12,directory),"FSR open");
        check(fsr.Initialize(interop,d11,{w*2,h*2},1,DXGI_FORMAT_R16G16B16A16_FLOAT,false,{w,h}),"FSR initialize");
        auto shared=[&](UINT x,UINT y,DXGI_FORMAT f,TheosRenderPipeline::SourceDLSSG::SharedTexture& out){
            D3D11_TEXTURE2D_DESC desc{};desc.Width=x;desc.Height=y;desc.MipLevels=desc.ArraySize=1;desc.Format=f;
            desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
            check(interop.CreateSharedTexture(desc,out),"FSR shared input");
        };
        shared(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,color);shared(gw,gh,DXGI_FORMAT_R16G16_FLOAT,motion);shared(gw,gh,DXGI_FORMAT_R32_FLOAT,depth);
        D3D11_TEXTURE2D_DESC desc=color.desc;desc.Width=w*2;desc.Height=h*2;desc.MiscFlags=0;
        check(d11->CreateTexture2D(&desc,nullptr,&output),"FSR output");enabled=true;
    }
    void Evaluate(ID3D11DeviceContext* c11,TheosRenderPipeline::SourceDLSSG::Interop& interop,
        ID3D12Resource* nr,ID3D12Resource* mv,ID3D12Resource* dep,bool reset) {
        if(!enabled)return;
        ID3D12GraphicsCommandList* list{};check(interop.Begin(TheosRenderPipeline::SourceDLSSG::Work::Upscaling,&list),"FSR handoff begin");
        check(TheosRenderPipeline::SourceDLSSG::Interop::RecordCopy(list,nr,color.texture12.Get()),"NR to FSR color");
        check(TheosRenderPipeline::SourceDLSSG::Interop::RecordCopy(list,mv,motion.texture12.Get()),"NR to FSR motion");
        check(TheosRenderPipeline::SourceDLSSG::Interop::RecordCopy(list,dep,depth.texture12.Get()),"NR to FSR depth");
        check(interop.Submit(TheosRenderPipeline::SourceDLSSG::Work::Upscaling),"FSR handoff submit");
        check(interop.WaitD3D11(TheosRenderPipeline::SourceDLSSG::Work::Upscaling),"FSR handoff wait");
        check(fsr.Evaluate(c11,color.texture11.Get(),motion.texture11.Get(),depth.texture11.Get(),output.Get(),0,0,reset,0,{}),"FSR evaluate after NR");
    }
    void Verify(ID3D11Device* d11,ID3D11DeviceContext* c11,TheosRenderPipeline::SourceDLSSG::Interop& interop) {
        if(!enabled)return;
        D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> readback;check(d11->CreateTexture2D(&desc,nullptr,&readback),"FSR readback");
        c11->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};check(c11->Map(readback.Get(),0,D3D11_MAP_READ,0,&map),"FSR map");
        unsigned finite=0,nonblack=0;for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width*4;++x){
            if(x%4==3)continue;const auto v=reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch)[x];
            finite+=(v&0x7c00)!=0x7c00;nonblack+=(v&0x7fff)!=0;
        }
        c11->Unmap(readback.Get(),0);if(finite!=desc.Width*desc.Height*3||!nonblack)throw std::runtime_error("FSR output invalid after NR");
        check(interop.SignalD3D11(TheosRenderPipeline::SourceDLSSG::Work::Upscaling),"FSR final producer fence");check(interop.Drain(),"FSR final drain");
        std::cout<<"PASS FSR after NR: frames="<<fsr.Frames()<<" finite="<<finite<<" nonblack="<<nonblack<<"\n";
        check(fsr.ReleaseAfterRetirement(),"FSR release");
    }
};
