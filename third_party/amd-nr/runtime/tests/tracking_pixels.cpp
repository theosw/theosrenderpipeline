// Exercises the shipped production passes on WARP; no model, CUDA or HIP.
// Windows WARP 10.0.26100.9711 misexecutes the optimized compose shader's
// nested halo accumulation (even with fully initialized float textures).
// The identical executable/DXBC passes with Microsoft.Direct3D.WARP 1.0.21
// and RTX hardware. Select a tested WARP DLL with TRP_AMD_TEST_WARP_DLL;
// this is a test-process override, never a shipped runtime dependency.
// Keep every pixel assertion active, including on the affected system WARP.
#include "../core/passes.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <filesystem>
#include <string>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
using namespace dlss5nr;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) {
    if (FAILED(hr)) {
        fprintf(stderr,"D3D12 operation failed: 0x%08lx\n",static_cast<unsigned long>(hr));
        throw std::runtime_error("D3D12 operation failed");
    }
}
static void SelectTestWarp() {
    const DWORD size=GetEnvironmentVariableW(L"TRP_AMD_TEST_WARP_DLL",nullptr,0);
    if (!size) return;
    std::wstring path(size,L'\0');
    const DWORD copied=GetEnvironmentVariableW(L"TRP_AMD_TEST_WARP_DLL",path.data(),size);
    Check(copied && copied<size,"TRP_AMD_TEST_WARP_DLL could not be read");path.resize(copied);
    Check(std::filesystem::path(path).is_absolute(),"TRP_AMD_TEST_WARP_DLL must be an absolute path");
    Check(LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)!=nullptr,
        "could not load the explicitly selected test WARP DLL");
    // Retain the module for this test process, including D3D12 teardown.
}
static void ReportWarp() {
    wchar_t path[32768]{};const HMODULE module=GetModuleHandleW(L"d3d10warp.dll");
    Check(module && GetModuleFileNameW(module,path,32768),"loaded WARP identity unavailable");
    DWORD unused{};const DWORD size=GetFileVersionInfoSizeW(path,&unused);
    Check(size!=0,"WARP version unavailable");std::vector<char> data(size);
    Check(GetFileVersionInfoW(path,0,size,data.data())!=FALSE,"WARP version read failed");
    VS_FIXEDFILEINFO* version{};UINT bytes{};
    Check(VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&version),&bytes)!=FALSE &&
        bytes>=sizeof(*version),"WARP version record unavailable");
    printf("WARP=%ls API-reported version=%u.%u.%u.%u\n",path,HIWORD(version->dwFileVersionMS),
        LOWORD(version->dwFileVersionMS),HIWORD(version->dwFileVersionLS),LOWORD(version->dwFileVersionLS));
    // Windows can report 6.2 for an executable without a supportedOS manifest;
    // the diagnosed DLL is 10.0.26100.9711 on disk in both cases.
    if((version->dwFileVersionMS==MAKELONG(0,10) || version->dwFileVersionMS==MAKELONG(2,6)) &&
        version->dwFileVersionLS==MAKELONG(9711,26100))
        fprintf(stderr,"Known WARP 10.0.26100.9711 halo code-generation defect; assertions remain enabled. "
            "Repeat with TRP_AMD_TEST_WARP_DLL pointing to Microsoft.Direct3D.WARP 1.0.21, or --hardware.\n");
}
struct Texture { ComPtr<ID3D12Resource> resource; D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON; };
struct Fixture {
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence; ComPtr<ID3D12InfoQueue> messages;
    Passes passes; HANDLE event{}; UINT64 tick{}; UINT width=32,height=16; DWORD waitMs=15000;
    Fixture(bool hardware=false,bool gpuValidation=false,UINT adapterIndex=0) {
        if(gpuValidation) waitMs=120000;
        if(!hardware) SelectTestWarp();
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();puts("D3D12 debug layer enabled");
            if(gpuValidation) { ComPtr<ID3D12Debug1> validation;HR(debug.As(&validation));
                validation->SetEnableGPUBasedValidation(TRUE);puts("D3D12 GPU-based validation enabled"); }
        } else Check(!gpuValidation,"GPU-based validation requested but debug layer unavailable");
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp;
        HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        HR(hardware?factory->EnumAdapters(adapterIndex,&warp):factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        DXGI_ADAPTER_DESC identity{};HR(warp->GetDesc(&identity));
        printf("adapter=%ls mode=%s\n",identity.Description,hardware?"hardware":"WARP");fflush(stdout);
        printf("vendor=0x%04x device=0x%04x LUID=%08lx:%08lx adapterIndex=%u\n",identity.VendorId,
            identity.DeviceId,static_cast<unsigned long>(identity.AdapterLuid.HighPart),identity.AdapterLuid.LowPart,adapterIndex);
        HR(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        if(!hardware) ReportWarp();
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{}; HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        HR(list->Close()); HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(event!=nullptr,"event");
        std::string error; Check(passes.init(device.Get(),error),error.c_str());
    }
    ~Fixture() {
        if(device && FAILED(device->GetDeviceRemovedReason())) {
            fprintf(stderr,"Device removed reason: 0x%08lx\n",static_cast<unsigned long>(device->GetDeviceRemovedReason()));
            try { DebugClean(); } catch(const std::exception& e) { fprintf(stderr,"%s\n",e.what()); }
        }
        passes.shutdown(); if(event) CloseHandle(event);
    }
    void Begin() { HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr)); }
    void Finish() {
        HR(list->Close()); ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);
        HR(queue->Signal(fence.Get(),++tick));HR(fence->SetEventOnCompletion(tick,event));
        Check(WaitForSingleObject(event,waitMs)==WAIT_OBJECT_0,"GPU completion timeout");
    }
    void State(Texture& t,D3D12_RESOURCE_STATES after) {
        if(t.state==after)return;
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={t.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,t.state,after};
        list->ResourceBarrier(1,&b);t.state=after;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES h{};h.Type=type;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;
        d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;HR(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&r)));return r;
    }
    Texture Make(const std::vector<float>& values) {
        Check(values.size()==width*height*4,"texture size");Texture t;
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HR(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,t.state,nullptr,IID_PPV_ARGS(&t.resource)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
        device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
        void* data{};HR(upload->Map(0,nullptr,&data));
        for(UINT y=0;y<height;++y)memcpy(static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch,values.data()+y*width*4,width*16);
        upload->Unmap(0,nullptr);Begin();State(t,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=t.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);State(t,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Finish();return t;
    }
    std::vector<float> Read(Texture& t) {
        auto d=t.resource->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
        device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);auto buffer=Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
        Begin();State(t,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=t.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);State(t,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Finish();
        void* data{};HR(buffer->Map(0,nullptr,&data));std::vector<float> result(width*height*4);
        for(UINT y=0;y<height;++y)memcpy(result.data()+y*width*4,static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch,width*16);
        buffer->Unmap(0,nullptr);return result;
    }
    std::vector<float> Solid(float r,float g=0,float b=0,float a=0) {
        std::vector<float> v(width*height*4);for(size_t i=0;i<v.size();i+=4){v[i]=r;v[i+1]=g;v[i+2]=b;v[i+3]=a;}return v;
    }
    Constants K(UINT flags=0) {
        Constants k{};k.size[0]=width;k.size[1]=height;k.inv_size[0]=k.uv1_scale[0]=1.f/width;
        k.inv_size[1]=k.uv1_scale[1]=1.f/height;k.flags=flags;k.strength=k.fade=k.exposure_mul=k.pre_exposure=1;
        k.max_ratio=8;k.blend=1;k.encoding=2;k.exposure_mode=2;return k;
    }
    void Run(Pass pass,Constants k,Bindings bindings,Texture& out) {
        Begin();State(out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bindings.uav[0]=out.resource.Get();
        passes.dispatch(list.Get(),pass,k,bindings,width,height);
        State(out,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Finish();
    }
    void Camera(float before,float now,bool inverted,bool visible) {
        auto prev=Make(Solid(before)),depth=Make(Solid(now)),flow=Make(Solid(0)),motion=Make(Solid(0)),out=Make(Solid(0));
        Bindings b;b.srv[0]=flow.resource.Get();b.srv[1]=depth.resource.Get();b.srv[2]=motion.resource.Get();b.srv[3]=prev.resource.Get();
        Run(Pass::Track,K(FLAG_IDENTITY|FLAG_DEPTH_CHECK|(inverted?FLAG_DEPTH_INVERTED:0)),b,out);
        const auto pixels=Read(out);for(size_t i=0;i<pixels.size();i+=4)
            Check(visible?std::abs(pixels[i])<1e-5:pixels[i]>=1e8,"camera retention / true occlusion");
    }
    void BlendAndHalo() {
        auto scene=Make(Solid(.5f,.5f,.5f,.7f)),now=Make(Solid(.12f,.12f,.12f)),old=Make(Solid(.04f,.04f,.04f)),flow=Make(Solid(0)),out=Make(Solid(0));
        Bindings b;b.srv[0]=scene.resource.Get();b.srv[3]=now.resource.Get();b.srv[2]=old.resource.Get();b.srv[4]=b.srv[6]=flow.resource.Get();
        float samples[3]{};for(int i=0;i<3;++i){auto k=K(FLAG_PREVIOUS);k.blend=.5f*i;Run(Pass::Compose,k,b,out);auto p=Read(out);samples[i]=p[0];Check(std::abs(p[3]-.7f)<1e-6,"scene alpha preserved");}
        Check(samples[0]<samples[1] && samples[1]<samples[2],"blending moves continuously between distinct corrections");
        auto residual=Solid(0,0,0,std::log2(.1f)),edges=Solid(10, .73535698f);size_t at=(8*width+16)*4;
        residual[at]=residual[at+1]=residual[at+2]=.15f;edges[at]=0;
        auto r=Make(residual),edge=Make(edges),depth=Make(Solid(.1f));
        printf("halo fixture alpha %.6f edge %.6f nearbyEdge %.6f\n",Read(r)[at+3],Read(edge)[at],Read(edge)[at+16]);
        b={};b.srv[0]=scene.resource.Get();b.srv[3]=r.resource.Get();b.srv[4]=flow.resource.Get();b.srv[5]=depth.resource.Get();b.srv[8]=edge.resource.Get();
        Run(Pass::Compose,K(FLAG_DEPTH_INVERTED),b,out);const float halo=Read(out)[at];
        Run(Pass::Compose,K(FLAG_DEPTH_INVERTED|FLAG_EDGES),b,out);const float corrected=Read(out)[at];
        printf("blend endpoints %.6f %.6f %.6f; halo %.6f -> %.6f\n",samples[0],samples[1],samples[2],halo,corrected);
        fflush(stdout);
        DebugClean();
        Check(halo>.52f && std::abs(corrected-.5f)<.002f,"stale edge halo removed using same-surface residual");
        // No displaced edge: both capture/current distances are EDGE_REACH,
        // so the existing correction must remain untouched.
        auto untouched=Make(Solid(10, .73535698f));b.srv[8]=untouched.resource.Get();
        Run(Pass::Compose,K(FLAG_DEPTH_INVERTED|FLAG_EDGES),b,out);
        const float retained=Read(out)[at];printf("unchanged edge distance %.6f\n",retained);
        Check(std::abs(retained-halo)<.002f,"unchanged edge distance retains correction");
    }
    void AppearanceValidity() {
        auto scene=Make(Solid(.5f,.5f,.5f,.7f)), residual=Make(Solid(.12f,.12f,.12f,std::log2(.1f))),
            depth=Make(Solid(.1f)), flow=Make(Solid(0)), neutralLight=Make(Solid(1,1,1)),
            matched=Make(Solid(10,.73535698f,0,0)), wrongLight=Make(Solid(10,.2f,0,0)),
            wrongColour=Make(Solid(10,.73535698f,.5f,0)), out=Make(Solid(0));
        Bindings b; b.srv[0]=scene.resource.Get();b.srv[1]=neutralLight.resource.Get();
        b.srv[3]=residual.resource.Get();b.srv[4]=flow.resource.Get();b.srv[5]=depth.resource.Get();
        const auto k=K(FLAG_DEPTH_INVERTED|FLAG_EDGES);
        b.srv[8]=matched.resource.Get();Run(Pass::Compose,k,b,out);const auto accepted=Read(out);
        b.srv[8]=wrongLight.resource.Get();Run(Pass::Compose,k,b,out);const auto rejectedLight=Read(out);
        b.srv[8]=wrongColour.resource.Get();Run(Pass::Compose,k,b,out);const auto rejectedColour=Read(out);
        Check(accepted[0]>.52f,"matching captured appearance keeps NR detail");
        Check(std::abs(rejectedLight[0]-.5f)<.002f,"changed luminance rejects stale detail");
        Check(std::abs(rejectedColour[0]-.5f)<.002f,"changed chroma rejects stale detail at equal luminance");
        Check(std::abs(accepted[3]-.7f)<1e-6 && std::abs(rejectedLight[3]-.7f)<1e-6 &&
            std::abs(rejectedColour[3]-.7f)<1e-6,"validity fallback preserves alpha");
        printf("appearance matching %.6f, wrong-light %.6f, wrong-colour %.6f\n",accepted[0],rejectedLight[0],rejectedColour[0]);
    }
    void DebugClean() {
        if(!messages)return;UINT errors=0,warnings=0;for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T size{};messages->GetMessage(i,nullptr,&size);
            std::vector<char> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());HR(messages->GetMessage(i,m,&size));
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)fprintf(stderr,"D3D12 severity=%d id=%d: %s\n",int(m->Severity),int(m->ID),m->pDescription);
            errors+=m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR;warnings+=m->Severity==D3D12_MESSAGE_SEVERITY_WARNING;}
        printf("D3D12 debug messages: %u errors, %u warnings\n",errors,warnings);
        messages->ClearStoredMessages();Check(errors==0,"D3D12 debug layer error");
    }
};
int main(int argc,char** argv){setvbuf(stdout,nullptr,_IONBF,0);try{
    bool hardware=false,gpuValidation=false;UINT adapterIndex=0;
    for(int i=1;i<argc;++i) { if(std::strcmp(argv[i],"--hardware")==0)hardware=true;
        else if(std::strcmp(argv[i],"--gpu-validation")==0)gpuValidation=true;
        else if(std::strcmp(argv[i],"--adapter")==0) {
            Check(++i<argc,"--adapter requires an index");char* end{};
            const auto index=std::strtoul(argv[i],&end,10);
            Check(argv[i][0]>='0' && argv[i][0]<='9' && *end=='\0' && index<128,"invalid adapter index");
            adapterIndex=static_cast<UINT>(index);hardware=true;
        } else throw std::runtime_error("usage: tracking_pixels [--hardware | --adapter INDEX] [--gpu-validation]"); }
    Fixture f(hardware,gpuValidation,adapterIndex);f.Camera(.1f,.105f,true,true);f.Camera(.1f,.05f,true,false);
    f.Camera(.9f,.905f,false,true);f.Camera(.99f,.995f,false,false);f.Camera(.00001f,0,true,false);
    f.BlendAndHalo();f.AppearanceValidity();f.DebugClean();puts("AMD NR 1.2 tracking/blending/halo/appearance pixels passed");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
