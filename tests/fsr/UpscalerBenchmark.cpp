// Explicit real-SDK execution. Synthetic pixels are not Skyrim acceptance.
#include "FrameGen/FSRUpscaler.h"
#include "FrameGen/XeSSUpscaler.h"
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
static void Require(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::fflush(nullptr); ExitProcess(1); }
}
static void Check(HRESULT hr, const char* message)
{
    if (FAILED(hr)) { std::fprintf(stderr, "HRESULT 0x%08X ", static_cast<unsigned>(hr)); Require(false, message); }
}
static float Halton(unsigned i, unsigned base)
{
    float result=0, fraction=1;
    for (; i; i/=base) { fraction/=base; result+=fraction*(i%base); }
    return result-0.5f;
}
static double Quantile(std::vector<double> values, double p)
{
    Require(!values.empty(), "timing samples exist");
    std::sort(values.begin(), values.end()); return values[static_cast<std::size_t>((values.size()-1)*p)];
}
static void PNG(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
    const std::filesystem::path& path)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags=0; desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging),"capture staging");
    context->CopyResource(staging.Get(),texture); D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"capture pixels");
    ComPtr<IWICImagingFactory> factory; Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC");
    ComPtr<IWICStream> stream; Check(factory->CreateStream(&stream),"PNG stream");
    Check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG filename");
    ComPtr<IWICBitmapEncoder> encoder; Check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");
    Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialize");
    ComPtr<IWICBitmapFrameEncode> frame; Check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame");
    Check(frame->Initialize(nullptr),"PNG frame init"); Check(frame->SetSize(desc.Width,desc.Height),"PNG dimensions");
    auto format=GUID_WICPixelFormat24bppBGR; Check(frame->SetPixelFormat(&format),"PNG format");
    Require(IsEqualGUID(format,GUID_WICPixelFormat24bppBGR),"BGR capture");
    std::vector<BYTE> pixels(desc.Width*desc.Height*3);
    for (UINT y=0;y<desc.Height;++y) for (UINT x=0;x<desc.Width;++x) {
        const auto* rgba=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch+x*4;
        auto* bgr=pixels.data()+(y*desc.Width+x)*3;
        bgr[0]=rgba[2];bgr[1]=rgba[1];bgr[2]=rgba[0];
    }
    Check(frame->WritePixels(desc.Height,desc.Width*3,static_cast<UINT>(pixels.size()),pixels.data()),"PNG pixels");
    Check(frame->Commit(),"PNG frame commit"); Check(encoder->Commit(),"PNG commit"); context->Unmap(staging.Get(),0);
}

// A continuous scene sampled with the same camera jitter for both methods.
// Fine diagonals, foliage-like detail, bright surfaces and a moving foreground
// rectangle expose spatial/temporal failures without claiming game fidelity.
struct SceneRenderer
{
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> parameters;
    ComPtr<ID3D11UnorderedAccessView> color, motion, depth;
    SceneRenderer(ID3D11Device* device, ID3D11Texture2D* c, ID3D11Texture2D* m, ID3D11Texture2D* d)
    {
        static constexpr char source[]=R"(
cbuffer Parameters : register(b0) { uint2 inputSize; uint2 outputSize; float2 jitter; uint frame; uint inverted; };
RWTexture2D<float4> color : register(u0);
RWTexture2D<float2> motion : register(u1);
RWTexture2D<float> depth : register(u2);
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=inputSize)) return;
    float2 uv=(id.xy+0.5-jitter)/inputSize;
    float u=uv.x,v=uv.y;
    float3 rgb=float3(0.12+0.5*u,0.16+0.35*v,0.18);
    if (u>0.52 && v<0.5) rgb=sin((u+v*0.7)*outputSize.x*1.1)>0?0.92:0.08;
    if (u<0.48 && v<0.45) rgb.g=0.25+0.5*pow(abs(sin(u*310)*sin(v*287)),3);
    if (v>0.7) rgb=0.5+0.4*sin((u*u+v*v)*outputSize.x*0.25);
    float position=0.28+0.08*sin(frame*0.025),previous=0.28+0.08*sin((int(frame)-1)*0.025);
    bool object=u>position && u<position+0.17 && v>0.38 && v<0.66;
    if (object) rgb=float3(0.85,0.16,0.07);
    color[id.xy]=float4(rgb,1);
    motion[id.xy]=float2(object?previous-position:0,0);
    float z=object?0.65:0.98;
    depth[id.xy]=inverted?1-z:z;
})";
        ComPtr<ID3DBlob> code,errors;
        const auto hr=D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if (errors) std::fprintf(stderr,"%s",static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr,"GPU scene shader");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),"GPU scene");
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=32;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&desc,nullptr,&parameters),"scene parameters");
        Check(device->CreateUnorderedAccessView(c,nullptr,&color),"scene color");
        Check(device->CreateUnorderedAccessView(m,nullptr,&motion),"scene motion");
        Check(device->CreateUnorderedAccessView(d,nullptr,&depth),"scene depth");
    }
    void Render(ID3D11DeviceContext* context,FrameExtent input,FrameExtent output,unsigned frame,float jx,float jy,bool inverted)
    {
        struct Constants { UINT inputX,inputY,outputX,outputY;float jitterX,jitterY;UINT frame,inverted; };
        const Constants constants{input.width,input.height,output.width,output.height,jx,jy,frame,inverted?1u:0u};
        context->UpdateSubresource(parameters.Get(),0,nullptr,&constants,0,0);
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,parameters.GetAddressOf());
        ID3D11UnorderedAccessView* views[]{color.Get(),motion.Get(),depth.Get()};
        context->CSSetUnorderedAccessViews(0,3,views,nullptr);context->Dispatch((input.width+7)/8,(input.height+7)/8,1);
        ID3D11UnorderedAccessView* empty[3]{};context->CSSetUnorderedAccessViews(0,3,empty,nullptr);
        context->CSSetShader(nullptr,nullptr,0);
    }
};

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0); Require(argc>=4,"Intel directory, AMD directory, capture directory [--debug]");
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    const bool debug=argc>4;
    if (debug) { ComPtr<ID3D12Debug> d; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&d)),"debug layer installed");d->EnableDebugLayer(); }
    std::filesystem::create_directories(argv[3]);
    ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> candidate;
        Require(SUCCEEDED(factory->EnumAdapters1(i,&candidate)),"NVIDIA hardware adapter required");
        DXGI_ADAPTER_DESC1 desc{}; candidate->GetDesc1(&desc);
        if (desc.VendorId==0x10DE && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter=candidate; std::printf("adapter=%ls vendor=0x%04X device=0x%04X\n",desc.Description,desc.VendorId,desc.DeviceId);break;
        }
    }
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11; ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,debug?D3D11_CREATE_DEVICE_DEBUG:0,
        nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)),"D3D12");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{};
    Check(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    Interop interop; Check(interop.Initialize(d11.Get(),d12.Get(),queue.Get()),"production interop");
    std::vector<double> vendorTimes;
    interop.Timer().SetSink([](void* p,const D3D12StageSample& sample) {
        if (sample.stage==D3D12Stage::kEarlyNeuralList) { static_cast<std::vector<double>*>(p)->push_back(sample.ms); }
    },&vendorTimes); interop.Timer().SetEnabled(true);
    XeSSUpscaler xess; FSRUpscaler fsr;
    Check(xess.Open(d12.Get(),std::filesystem::absolute(argv[1])),xess.Status().c_str());
    Check(fsr.Open(d12.Get(),std::filesystem::absolute(argv[2])),fsr.Status().c_str());
    std::ofstream csv(std::filesystem::path(argv[3])/"timings.csv");
    csv<<"output_width,output_height,input_width,input_height,round,backend,quality,frame,stage_ms,sdk_list_ms\n";
    for (FrameExtent output : {FrameExtent{1280,720},FrameExtent{1920,1080}}) {
        const FrameExtent input=output.width==1280?FrameExtent{753,424}:FrameExtent{1129,635};
        auto make=[&](FrameExtent size,DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=size.width;desc.Height=size.height;desc.MipLevels=desc.ArraySize=1;
            desc.Format=format;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> texture;Check(d11->CreateTexture2D(&desc,nullptr,&texture),"fixture texture");return texture;
        };
        auto color=make(input,DXGI_FORMAT_R8G8B8A8_UNORM), motion=make(input,DXGI_FORMAT_R16G16_FLOAT),
            depth=make(input,DXGI_FORMAT_R32_FLOAT), destination=make(output,DXGI_FORMAT_R8G8B8A8_UNORM);
        SceneRenderer scene(d11.Get(),color.Get(),motion.Get(),depth.Get());
        // Some SDK presets round their minimum up (1130x636 at 1080p).
        // Preserve the requested comparison extent and the SDK's admission rule:
        // use Balanced's supported fixed-input range if Quality excludes it.
        int xessQuality=2;UpscalerInputResolution range;
        Check(xess.QueryInputRange(output.width,output.height,xessQuality,range),"XeSS Quality range");
        if (!range.Accepts(input,output)) { xessQuality=1;Check(xess.QueryInputRange(output.width,output.height,xessQuality,range),"XeSS Balanced range"); }
        Require(range.Accepts(input,output),"requested fixed comparison extent is admitted by the SDK");
        FrameExtent preferred{};Check(xess.QuerySize(output.width,output.height,xessQuality,preferred),"XeSS preferred resolution");
        std::printf("XESS quality=%d preferred=%ux%u minimum=%ux%u matched=%ux%u\n",xessQuality,preferred.width,preferred.height,range.minimum.width,range.minimum.height,input.width,input.height);
        // Alternating order over three rounds avoids systematically warming one method first.
        for (unsigned round=0;round<3;++round) for (unsigned method=0;method<2;++method) {
            const bool useFSR=(method+(round%2))%2; const char* name=useFSR?"FSR":"XeSS";
            auto hr=useFSR?fsr.Initialize(interop,d11.Get(),output,1,DXGI_FORMAT_R8G8B8A8_UNORM,false,input):
                xess.Initialize(interop,d11.Get(),output,xessQuality,DXGI_FORMAT_R8G8B8A8_UNORM,false,nullptr,input);
            if (FAILED(hr)) { std::printf("status=%s\n",useFSR?fsr.Status().c_str():xess.Status().c_str()); } Check(hr,"initialize fixed extent");
            if (useFSR) { std::printf("FSR provider=%s\n",fsr.Version().c_str());Require(fsr.Version()=="3.1.4","pinned official FSR provider"); }
            std::vector<double> full, sdk;
            const unsigned warmup=debug?12:240, measured=debug?16:240;
            for (unsigned frame=0;frame<warmup+measured;++frame) {
                const int phases=static_cast<int>(8.0*output.width*output.height/(static_cast<double>(input.width)*input.height));
                const float jx=Halton(frame%phases+1,2),jy=Halton(frame%phases+1,3);
                scene.Render(c11.Get(),input,output,frame,jx,jy,false);
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0}; ComPtr<ID3D11Query> disjoint,start,end;
                Check(d11->CreateQuery(&desc,&disjoint),"disjoint query");desc.Query=D3D11_QUERY_TIMESTAMP;
                Check(d11->CreateQuery(&desc,&start),"start query");Check(d11->CreateQuery(&desc,&end),"end query");
                vendorTimes.clear();c11->Begin(disjoint.Get());c11->End(start.Get());
                FSRCamera camera;camera.frameTimeMs=16.6667f;
                // A mid-run reset exercises history invalidation in the timed path too.
                const bool reset=frame==0 || frame==warmup+measured/2;
                hr=useFSR?fsr.Evaluate(c11.Get(),color.Get(),motion.Get(),depth.Get(),destination.Get(),jx,jy,reset,0,camera):
                    xess.Evaluate(c11.Get(),color.Get(),motion.Get(),depth.Get(),destination.Get(),jx,jy,reset,false,0);
                if (FAILED(hr)) { std::printf("status=%s\n",useFSR?fsr.Status().c_str():xess.Status().c_str()); } Check(hr,"production upscale");
                c11->End(end.Get());c11->End(disjoint.Get());c11->Flush();
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};UINT64 begin{},finish{};
                while ((hr=c11->GetData(disjoint.Get(),&timing,sizeof(timing),0))==S_FALSE) { Sleep(0); }Check(hr,"GPU timing");
                while ((hr=c11->GetData(start.Get(),&begin,sizeof(begin),0))==S_FALSE) { Sleep(0); }Check(hr,"GPU start");
                while ((hr=c11->GetData(end.Get(),&finish,sizeof(finish),0))==S_FALSE) { Sleep(0); }Check(hr,"GPU end");
                interop.Timer().Harvest();Require(!timing.Disjoint && timing.Frequency && vendorTimes.size()==1,"completed comparable timestamps");
                if (frame>=warmup) {
                    const double ms=static_cast<double>(finish-begin)*1000/timing.Frequency;
                    full.push_back(ms);sdk.push_back(vendorTimes.back());
                    csv<<output.width<<','<<output.height<<','<<input.width<<','<<input.height<<','<<round<<','<<name<<','<<(useFSR?1:xessQuality)<<','<<frame<<','<<ms<<','<<vendorTimes.back()<<'\n';
                }
            }
            Check(interop.SignalD3D11(Work::Upscaling),"retire consumer");Check(interop.Drain(),"retire SDK work");
            PNG(d11.Get(),c11.Get(),destination.Get(),std::filesystem::path(argv[3])/(std::string(name)+"-"+std::to_string(output.width)+"-r"+std::to_string(round)+".png"));
            std::printf("RESULT backend=%s input=%ux%u output=%ux%u round=%u n=%zu stage_p50=%.4f stage_p95=%.4f sdk_p50=%.4f sdk_p95=%.4f ms\n",
                name,input.width,input.height,output.width,output.height,round,full.size(),Quantile(full,.5),Quantile(full,.95),Quantile(sdk,.5),Quantile(sdk,.95));
            Check(useFSR?fsr.ReleaseAfterRetirement():xess.ReleaseAfterRetirement(),"release retired context");
            if (!useFSR) { Check(xess.Open(d12.Get(),std::filesystem::absolute(argv[1])),"reopen XeSS"); }
        }
    }
    // Behavior checks outside the measured population: known constant pixels,
    // depth-convention recreation, reset, integrated sharpening and bad camera rejection.
    {
        auto make=[&](FrameExtent size,DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC d{};d.Width=size.width;d.Height=size.height;d.MipLevels=d.ArraySize=1;
            d.Format=format;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> t;Check(d11->CreateTexture2D(&d,nullptr,&t),"validation texture");return t;
        };
        auto color=make({188,106},DXGI_FORMAT_R8G8B8A8_UNORM),motion=make({188,106},DXGI_FORMAT_R16G16_FLOAT),
            depth=make({188,106},DXGI_FORMAT_R32_FLOAT),output=make({320,180},DXGI_FORMAT_R8G8B8A8_UNORM);
        Check(fsr.Initialize(interop,d11.Get(),{320,180},1,DXGI_FORMAT_R8G8B8A8_UNORM,false,{188,106}),"FSR validation context");
        for (unsigned f=0;f<18;++f) {
            for (auto* t : {color.Get(),motion.Get(),depth.Get()}) {
                ComPtr<ID3D11UnorderedAccessView> view;Check(d11->CreateUnorderedAccessView(t,nullptr,&view),"clear view");
                const float rgb[]{.25f,.5f,.75f,1},zero[]{0,0,0,0},z[]{.5f,.5f,.5f,.5f};
                c11->ClearUnorderedAccessViewFloat(view.Get(),t==color.Get()?rgb:t==motion.Get()?zero:z);
            }
            FSRCamera camera;camera.invertedDepth=f>=6 && f<12;
            Check(fsr.Evaluate(c11.Get(),color.Get(),motion.Get(),depth.Get(),output.Get(),0,0,f%6==0,f>=12?.5f:0,camera),"FSR reset, reverse/forward depth and sharpening");
        }
        FSRCamera invalid;invalid.nearPlane=-1;
        Require(FAILED(fsr.Evaluate(c11.Get(),color.Get(),motion.Get(),depth.Get(),output.Get(),0,0,false,0,invalid)),"bad camera rejected");
        Check(interop.SignalD3D11(Work::Upscaling),"validation consumer retirement");Check(interop.Drain(),"validation drain");
        D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;Check(d11->CreateTexture2D(&desc,nullptr,&read),"constant readback");
        c11->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Check(c11->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"constant pixels");
        const auto* pixel=static_cast<const BYTE*>(mapped.pData)+90*mapped.RowPitch+160*4;
        std::printf("CONSTANT rgba=%u,%u,%u,%u expected=64,128,191,255\n",pixel[0],pixel[1],pixel[2],pixel[3]);
        Require(std::abs(int(pixel[0])-64)<6 && std::abs(int(pixel[1])-128)<6 && std::abs(int(pixel[2])-191)<6,"constant color preserved through FSR and sharpening");
        c11->Unmap(read.Get(),0);Require(fsr.Frames()==18,"failed dispatch never counted");
        Check(fsr.ReleaseAfterRetirement(),"validation context release");
    }
    if (debug) {
        ComPtr<ID3D12InfoQueue> messages; Check(d12.As(&messages),"D3D12 debug messages");
        for (UINT64 n=0;n<messages->GetNumStoredMessages();++n) {
            SIZE_T size{};messages->GetMessage(n,nullptr,&size);std::vector<unsigned char> storage(size);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());messages->GetMessage(n,m,&size);
            if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr,"%s\n",m->pDescription);Require(false,"D3D12 validation"); }
        }
    }
    std::puts("PASS real SDK fixed-extent comparison, reset and recreate; synthetic quality only");
}
