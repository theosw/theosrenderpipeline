#include <PCH.h>
#include "SourceDLSSGNeuralExecution.h"
#include <dxgi1_6.h>

namespace TheosRenderPipeline::SourceDLSSG
{
namespace
{
static_assert(kCommandSlots <= TRPExperiment::AsyncPipeline::kHostFrames);
struct FrameOptions { NeuralOptions options; bool depthInverted; float scaleX, scaleY; };
// Reserve room for model allocation and other rendering activity. This checks
// live process-local budget on the SAME physical adapter, before histories exist.
std::uint64_t HistoryBudget(ID3D12Device* device, int passes)
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))) return 0;
    const auto reserve=(512ull+512ull*passes)*1024*1024;
    if (memory.CurrentUsage>=memory.Budget || memory.Budget-memory.CurrentUsage<=reserve) return 0;
    return std::min<std::uint64_t>(2ull*1024*1024*1024,memory.Budget-memory.CurrentUsage-reserve);
}
}
bool NeuralExecution::NeedsRecreation(const NeuralOptions& options, UINT width, UINT height, ID3D12Resource* scene, DXGI_FORMAT motionFormat) const
{
    if (!initialized_) return false;
    if (allocated_.async!=options.async) return true;
    if (scene) {
        const auto desc=scene->GetDesc();
        if (desc.Width!=sceneDesc_.Width || desc.Height!=sceneDesc_.Height || desc.Format!=sceneDesc_.Format) return true;
    }
    if (motionFormat!=DXGI_FORMAT_UNKNOWN && motionFormat_!=motionFormat) return true;
    if (regular_) return regular_->NeedsRecreation(options,width,height);
    return allocated_.runtimePath!=options.runtimePath || allocated_.WorldOnly()!=options.WorldOnly() ||
        allocated_.beforeUpscaling!=options.beforeUpscaling || allocated_.passes!=options.passes ||
        !NeuralRendering::SameReconstructionResources(allocated_.reconstruction,options.reconstruction) ||
        allocated_.reconstruction.maxRatio!=options.reconstruction.maxRatio ||
        (options.passes==2 && (allocated_.EffectiveSecond().preset!=options.EffectiveSecond().preset ||
                             allocated_.EffectiveSecond().inputScale!=options.EffectiveSecond().inputScale)) ||
        ((width||height) && (guideWidth_!=width || guideHeight_!=height));
}
bool NeuralExecution::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
    const NeuralOptions& options, bool reset, bool depthInverted, float scaleX, float scaleY,
    ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* ui,
    ID3D12Resource* color, ID3D12Resource* composed, std::uint64_t frequency)
{
    if (!device || !list || !motion || !depth || !color) { status_="NR input unavailable"; return false; }
    if (!initialized_) {
        sceneDesc_=color->GetDesc(); motionFormat_=motion->GetDesc().Format;
        allocated_=options; guideWidth_=static_cast<UINT>(motion->GetDesc().Width); guideHeight_=motion->GetDesc().Height;
        if (options.async && options.WorldOnly()) {
            TRPExperiment::AsyncPipeline::Config config;
            const auto desc=color->GetDesc();
            config.width=static_cast<UINT>(desc.Width); config.height=desc.Height; config.format=desc.Format;
            config.guideWidth=guideWidth_; config.guideHeight=guideHeight_; config.motionFormat=motion->GetDesc().Format;
            config.motionScaleX=scaleX*config.width/guideWidth_; config.motionScaleY=scaleY*config.height/guideHeight_;
            config.maxRatio=std::clamp(options.reconstruction.maxRatio,1.f,16.f);
            config.enabled=true; config.maxAge=4;
            config.maxAllocationBytes=HistoryBudget(device,options.passes);
            if (config.maxAllocationBytes) {
                auto candidate=std::make_unique<TRPExperiment::AsyncPipeline>();
                auto pass=std::make_shared<NeuralPass>();
                Microsoft::WRL::ComPtr<ID3D12Device> owner=device;
                const auto result=candidate->Initialize(device,config,[pass,owner](auto* workerList,const auto& capture) {
                    const auto frame=std::static_pointer_cast<const FrameOptions>(capture.metadata);
                    // Captures may skip arbitrary source frames: model history is
                    // reset per evaluation. Reprojection owns delayed history.
                    if (!frame) return false;
                    if (!pass->Record(owner.Get(),workerList,0,frame->options,true,frame->depthInverted,
                        frame->scaleX,frame->scaleY,capture.motion,capture.depth,nullptr,capture.color,nullptr))
                        throw std::runtime_error(pass->Status());
                    return SUCCEEDED(Interop::RecordCopy(workerList,pass->Corrected(),capture.output));
                });
                if (result==S_OK) async_=std::move(candidate);
                else if (result==E_OUTOFMEMORY) fallback_="Regular: insufficient VRAM for async history";
                else if (result==E_INVALIDARG) fallback_="Regular: async does not support this scene format";
                else { status_="Async initialization failed; NR stopped"; return false; }
            } else fallback_="Regular: insufficient or unavailable VRAM budget";
        } else if (options.async) fallback_="Regular: async requires a world scene before UI composition";
        if (!async_) regular_=std::make_unique<NeuralPass>();
        initialized_=true;
        logger::info("[SourceDLSSG NR] execution requested={} active={} {}",options.async?"async":"regular",async_?"async":"regular",fallback_);
    }
    if (regular_) {
        const bool ok=regular_->Record(device,list,slot,options,reset,depthInverted,scaleX,scaleY,motion,depth,ui,color,composed,frequency);
        corrected_=regular_->Corrected(); status_=fallback_.empty()?regular_->Status():fallback_+"; "+regular_->Status(); return ok;
    }
    auto frame=std::make_shared<FrameOptions>(FrameOptions{options,depthInverted,scaleX,scaleY});
    const auto result=async_->Record(list,color,motion,depth,reset,true,std::move(frame));
    if (FAILED(result)) { status_="Async NR failed: "+async_->Status().error; return false; }
    pendingSubmission_=true; corrected_=async_->Output();
    const auto state=async_->Status();
    status_=std::format("Async: {}; result age {} frames; {} evaluations; {:.1f} MiB history; worker {:.2f} ms",
        state.displaying?"delayed correction":"waiting for correction",state.resultAge,state.evaluations,
        state.allocationBytes/(1024.*1024.),state.evaluationMs);
    if(state.frame==1 || reset || state.frame%600==0)
        logger::info("[SourceDLSSG async] sourceFrames={} composedFrames={} captures={} evaluations={} generation={} droppedSnapshots={} hostInFlight={} hostPeak={}",
            state.frame,state.compositions,state.captures,state.evaluations,state.generation,state.dropped,
            state.hostFramesInFlight,state.peakHostFramesInFlight);
    return true;
}
HRESULT NeuralExecution::Submitted(ID3D12Fence* fence,std::uint64_t value)
{
    if (!pendingSubmission_) return S_OK;
    const auto result=async_->Submitted(fence,value);
    if (SUCCEEDED(result)) pendingSubmission_=false;
    return result;
}
}
