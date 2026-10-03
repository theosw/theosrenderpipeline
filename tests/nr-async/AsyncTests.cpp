#include "AsyncPipeline.h"
#include "GPU.h"
#include <d3dcompiler.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using TRPExperiment::AsyncPipeline;
// A deterministic GPU evaluator with a controllable CPU admission gate. While
// this gate is closed, the production async pipeline must keep rendering and
// overwrite only its pending snapshot, never the snapshot owned by this worker.
struct Script {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::mutex mutex;std::condition_variable wake;
    unsigned permits{},entered{};bool released{};
    explicit Script(ID3D12Device* d):device(d) {
        D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable={2,ranges};D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=1;desc.pParameters=&parameter;
        ComPtr<ID3DBlob> blob,error;Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"script signature");
        Check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"script root");
        const char shader[]=R"(Texture2D<float4> A:register(t0);RWTexture2D<float4> B:register(u0);
        [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h;B.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
        float4 c=A.Load(int3(p.xy,0));B[p.xy]=float4(c.rgb+.2+.002*p.x,c.a);})";
        Check(D3DCompile(shader,sizeof(shader)-1,"script",nullptr,nullptr,"main","cs_5_1",0,0,&blob,&error),"script shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        Check(d->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipeline)),"script pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(d->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)),"script heap");
    }
    ~Script(){Release();}
    void Permit(unsigned count=1){std::lock_guard lock(mutex);permits+=count;wake.notify_all();}
    void Release(){std::lock_guard lock(mutex);released=true;wake.notify_all();}
    unsigned Entered(){std::lock_guard lock(mutex);return entered;}
    bool Evaluate(ID3D12GraphicsCommandList* list,const AsyncPipeline::Capture& capture) {
        {std::unique_lock lock(mutex);++entered;wake.notify_all();wake.wait(lock,[&]{return permits || released;});if(permits)--permits;}
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=capture.color->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(capture.color,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=capture.output->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(capture.output,nullptr,&uav,cpu);
        Barrier(list,capture.color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list,capture.output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* h=heap.Get();list->SetDescriptorHeaps(1,&h);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((unsigned(capture.color->GetDesc().Width)+7)/8,(capture.color->GetDesc().Height+7)/8,1);
        Barrier(list,capture.color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(list,capture.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        return true;
    }
    static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
    }
};
template<class Predicate> void Until(Predicate predicate,const char* why) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!predicate() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(predicate(),why);
}
struct Fixture {
    GPU gpu;unsigned w{33},h{17};
    ComPtr<ID3D12Resource> color,motion,depth;
    Fixture(){Set(.4f,0.f);}
    void Set(float brightness,float mv,std::vector<Pixel> depths={}) {
        color=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{brightness,brightness,brightness,.37f}),DXGI_FORMAT_R32G32B32A32_FLOAT);
        motion=gpu.Texture(w,h,std::vector<Pixel>(size_t(w)*h,{mv,0,0,0}),DXGI_FORMAT_R32G32_FLOAT);
        depth=gpu.Texture(w,h,depths.empty()?std::vector<Pixel>(size_t(w)*h,{.5f,0,0,0}):depths,DXGI_FORMAT_R32_FLOAT);
    }
    AsyncPipeline::Config Config(unsigned age=8) {
        AsyncPipeline::Config c;c.width=w;c.height=h;c.format=DXGI_FORMAT_R32G32B32A32_FLOAT;c.enabled=true;c.maxAge=age;return c;
    }
    std::vector<Pixel> Frame(AsyncPipeline& pipeline,bool reset=false,bool enabled=true) {
        gpu.Begin();Check(pipeline.Record(gpu.list.Get(),color.Get(),motion.Get(),depth.Get(),reset,enabled),"async frame record");
        Check(gpu.list->Close(),"host list close");ID3D12CommandList* lists[]{gpu.list.Get()};gpu.queue->ExecuteCommandLists(1,lists);
        Check(gpu.queue->Signal(gpu.fence.Get(),++gpu.serial),"host signal");
        Check(pipeline.Submitted(gpu.fence.Get(),gpu.serial),"host submission token");
        Check(gpu.fence->SetEventOnCompletion(gpu.serial,gpu.event),"host event");Require(WaitForSingleObject(gpu.event,10000)==WAIT_OBJECT_0,"host complete");
        auto result=gpu.Read(pipeline.Output());
        return result;
    }
};
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    unsigned cases=0;
    {
        // Skyrim motion uses RG16F and can be at a different resolution from
        // scene colour. Test the real delayed residual, not only allocation.
        Fixture f; auto script=std::make_shared<Script>(f.gpu.device.Get()); AsyncPipeline p;
        auto c=f.Config(); c.guideWidth=17; c.guideHeight=9; c.motionFormat=DXGI_FORMAT_R16G16_FLOAT;
        c.motionScaleX=float(c.width)/c.guideWidth;
        f.motion=f.gpu.Texture(17,9,std::vector<Pixel>(153,{0,0,0,0}),DXGI_FORMAT_R16G16_FLOAT);
        f.depth=f.gpu.Texture(17,9,std::vector<Pixel>(153,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        Check(p.Initialize(f.gpu.device.Get(),c,[script](auto* list,const auto& capture) {
            Require(capture.motion->GetDesc().Format==DXGI_FORMAT_R16G16_FLOAT && capture.depth->GetDesc().Width==17,"immutable native guides");
            return script->Evaluate(list,capture);
        }),"reduced guides initialize");
        script->Permit(); f.Frame(p); Until([&]{return p.Status().evaluations==1;},"reduced guides evaluated");
        f.motion=f.gpu.Texture(17,9,std::vector<Pixel>(153,{-1,0,0,0}),DXGI_FORMAT_R16G16_FLOAT);
        auto out=f.Frame(p); Near(out[15][0],.4+.2+.002*(15-float(c.width)/c.guideWidth),2e-6,"native guide motion scales into scene pixels");
        Near(out[15][3],.37,1e-6,"different guide resolution preserves alpha");
        script->Release(); Require(p.Stop(),"reduced guides retired"); ++cases;
    }
    {
        Fixture f;AsyncPipeline p;auto config=f.Config();config.maxAllocationBytes=1;
        Require(p.Initialize(f.gpu.device.Get(),config,[](auto*,const auto&){return true;})==E_OUTOFMEMORY,"history memory policy before allocation");
        config=f.Config();config.motionScaleX=NAN;
        Require(p.Initialize(f.gpu.device.Get(),config,[](auto*,const auto&){return true;})==E_INVALIDARG,"nonfinite guide scale rejected");++cases;
    }
    {
        Fixture f;AsyncPipeline p;auto c=f.Config();c.enabled=false;
        Check(p.Initialize(f.gpu.device.Get(),c,[](auto*,const auto&){Require(false,"disabled never evaluates");return false;}),"disabled initialize");
        auto out=f.Frame(p);Near(out[7][0],.4,1e-6,"default disabled bypass");Require(p.Status().evaluations==0,"no disabled inference");Require(p.Stop(),"disabled retire");++cases;
    }
    {
        Fixture f;auto script=std::make_shared<Script>(f.gpu.device.Get());AsyncPipeline p;auto c=f.Config();
        Check(p.Initialize(f.gpu.device.Get(),c,[script](auto* list,const auto& capture){return script->Evaluate(list,capture);}),"moving initialize");
        f.Frame(p);Until([&]{return script->Entered()==1;},"worker entered capture");
        // Move the frame while evaluation 1 is blocked. This is an actual
        // independent command queue test, not an elapsed-time threshold.
        f.Set(.4f,-1.f);
        for(unsigned i=0;i<3;++i){auto out=f.Frame(p);Near(out[15][0],.4,1e-6,"blocked worker preserves original");}
        Require(p.Status().frame==4 && p.Status().evaluations==0,"frame progress with blocked inference");
        Require(p.Status().dropped>=2,"pending capture latest-wins");
        script->Permit();Until([&]{return p.Status().evaluations>=1;},"evaluation completes");
        auto out=f.Frame(p);auto status=p.Status();
        Require(status.displaying && status.resultFrame==1 && status.resultAge==4,"delayed result frame identity");
        Near(out[15][0],.4+.2+.002*(15-4),2e-6,"motion accumulated throughout worker latency");
        Near(out[0][0],.4,1e-6,"offscreen reprojection falls back");Near(out[15][3],.37,1e-6,"current alpha retained");
        script->Release();Require(p.Stop(),"moving pipeline retirement");++cases;
    }
    {
        Fixture f;auto script=std::make_shared<Script>(f.gpu.device.Get());AsyncPipeline p;
        Check(p.Initialize(f.gpu.device.Get(),f.Config(),[script](auto* list,const auto& capture){return script->Evaluate(list,capture);}),"occlusion initialize");
        script->Permit();f.Frame(p);Until([&]{return p.Status().evaluations==1;},"first answer");
        auto out=f.Frame(p);Near(out[15][0],.63,2e-6,"stationary residual");
        std::vector<Pixel> depths(size_t(f.w)*f.h,{.5f,0,0,0});depths[15]={.9f,0,0,0};f.Set(.4f,0,depths);
        out=f.Frame(p);Near(out[15][0],.4,1e-6,"per-link disocclusion rejection");Near(out[14][0],.628,2e-6,"neighbour remains valid");
        f.Set(.4f,0);out=f.Frame(p);Near(out[15][0],.4,1e-6,"invalidated chain never revives");
        f.Set(.9f,0);out=f.Frame(p);Near(out[14][0],.9,1e-6,"lighting change rejects old colour");
        script->Release();Require(p.Stop(),"occlusion retirement");++cases;
    }
    {
        Fixture f;auto script=std::make_shared<Script>(f.gpu.device.Get());AsyncPipeline p;
        Check(p.Initialize(f.gpu.device.Get(),f.Config(3),[script](auto* list,const auto& capture){return script->Evaluate(list,capture);}),"age initialize");
        script->Permit();f.Frame(p);Until([&]{return p.Status().evaluations==1;},"age answer");
        for(unsigned i=0;i<3;++i)f.Frame(p);
        auto out=f.Frame(p);Near(out[15][0],.4,1e-6,"aged result bypass");Require(!p.Status().displaying,"expired result not displayed");
        Until([&]{return script->Entered()>=2;},"old generation worker blocked");
        f.Set(.7f,0);out=f.Frame(p,true);Near(out[15][0],.7,1e-6,"camera cut immediate bypass");
        script->Permit();Until([&]{return p.Status().evaluations>=2;},"old generation completion");
        out=f.Frame(p);Near(out[15][0],.7,1e-6,"late pre-cut result discarded");
        out=f.Frame(p,false,false);Near(out[15][0],.7,1e-6,"disable clears result");
        script->Release();Require(p.Stop(),"age retirement");++cases;
    }
    {
        Fixture f;auto script=std::make_shared<Script>(f.gpu.device.Get());AsyncPipeline p;auto config=f.Config();config.maxRatio=1.f;config.colorTolerance=2.f;
        Check(p.Initialize(f.gpu.device.Get(),config,[script](auto* list,const auto& capture){return script->Evaluate(list,capture);}),"gain initialize");
        script->Permit();f.Frame(p);Until([&]{return p.Status().evaluations==1;},"gain answer");
        f.Set(.3f,0);auto out=f.Frame(p);Near(out[15][0],.3,2e-6,"gain bound uses current original");
        script->Release();Require(p.Stop(),"gain retirement");++cases;
    }
    {
        Fixture f;AsyncPipeline p;Check(p.Initialize(f.gpu.device.Get(),f.Config(),[](auto*,const auto&){return false;}),"failure initialize");
        f.Frame(p);Until([&]{return p.Status().failed;},"recording failure terminal");
        Require(p.Status().evaluations==0,"failed recording never submitted");Require(p.Stop(),"unsubmitted worker safely retired");++cases;
    }
    {
        Fixture f;auto script=std::make_shared<Script>(f.gpu.device.Get());AsyncPipeline p;
        f.color=f.gpu.Texture(f.w,f.h,std::vector<Pixel>(size_t(f.w)*f.h,{-.1f,.4f,.4f,.37f}),DXGI_FORMAT_R32G32B32A32_FLOAT);
        Check(p.Initialize(f.gpu.device.Get(),f.Config(),[script](auto* list,const auto& capture){return script->Evaluate(list,capture);}),"signed initialize");
        script->Permit();f.Frame(p);Until([&]{return p.Status().evaluations==1;},"signed answer");
        auto out=f.Frame(p);Near(out[15][0],-.1,1e-6,"signed producer channel retained");Near(out[15][1],.63,2e-6,"other channels enhanced");
        auto vectors=std::vector<Pixel>(size_t(f.w)*f.h,{});vectors[15]={NAN,0,0,0};
        f.motion=f.gpu.Texture(f.w,f.h,vectors,DXGI_FORMAT_R32G32_FLOAT);out=f.Frame(p);
        Near(out[15][0],-.1,1e-6,"nonfinite motion preserves signed original");Near(out[15][1],.4,1e-6,"nonfinite motion invalidates result");
        script->Release();Require(p.Stop(),"signed retirement");++cases;
    }
    {
        Fixture f;AsyncPipeline p;auto c=f.Config();c.enabled=false;
        Check(p.Initialize(f.gpu.device.Get(),c,[](auto*,const auto&){return true;}),"fence initialize");
        f.gpu.Begin();Check(p.Record(f.gpu.list.Get(),f.color.Get(),f.motion.Get(),f.depth.Get()),"fence record");
        Check(f.gpu.list->Close(),"fence close");ID3D12CommandList* lists[]{f.gpu.list.Get()};f.gpu.queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> gate;Check(f.gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"uncompleted gate");
        Require(p.Submitted(gate.Get(),1)==S_OK,"future retirement token");
        // Retire the actual test allocator while leaving the token supplied to
        // the async owner incomplete, as a failed signalling path would do.
        Check(f.gpu.queue->Signal(f.gpu.fence.Get(),++f.gpu.serial),"test allocator signal");
        Check(f.gpu.fence->SetEventOnCompletion(f.gpu.serial,f.gpu.event),"test allocator event");
        Require(WaitForSingleObject(f.gpu.event,10000)==WAIT_OBJECT_0,"test allocator retired");
        f.gpu.Begin();Require(p.Record(f.gpu.list.Get(),f.color.Get(),f.motion.Get(),f.depth.Get())==S_FALSE,"unfinished host polls admission");
        // Close the unused list; its allocator wasn't the async owner's.
        Check(f.gpu.list->Close(),"empty list close");
        Require(!p.Stop(),"unconfirmed host refuses release");
        Check(f.gpu.queue->Signal(gate.Get(),1),"real completion token");Check(gate->SetEventOnCompletion(1,f.gpu.event),"gate event");
        Require(WaitForSingleObject(f.gpu.event,10000)==WAIT_OBJECT_0 && p.Stop(),"fence confirmed release");++cases;
    }
    std::printf("PASS %u async GPU scenarios: blocked inference, motion, per-link occlusion, age/cut/disable, gain, failures and retirement\n",cases);
}
