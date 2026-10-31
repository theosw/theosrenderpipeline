#include "GPU.h"
#include "FeatureDouble.h"
#include "FrameGen/NeuralRecovery.h"
#include "FrameGen/NeuralFaultInjection.h"

int main(int argc, char**)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    GPU gpu(argc > 1);
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPNRRecovery";
    Require(RegisterClassW(&wc)!=0,"window class");
    auto window=CreateWindowW(wc.lpszClassName,L"NR recovery fixture",WS_OVERLAPPEDWINDOW,0,0,96,96,nullptr,nullptr,wc.hInstance,nullptr);
    Require(window!=nullptr,"fixture window");
    ComPtr<IDXGIFactory2> factory; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"present factory");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=64; desc.Height=64; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; Check(factory->CreateSwapChainForHwnd(gpu.queue.Get(),window,&desc,nullptr,nullptr,&swap),"native presenter");
    ComPtr<IDXGISwapChain3> chain; Check(swap.As(&chain),"rotating buffers");
    unsigned presents=0, refusals=0, retries=0;
    for (int route : {0,1,2}) for (int passes : {1,2,5}) for (unsigned failAt : {1u,3u,7u}) {
        Fixture::peripheral=false; Fixture::worldOnly=route!=2; Fixture::transform=false;
        Fixture::sourceWidth=Fixture::workWidth=Fixture::guideWidth=64;
        Fixture::sourceHeight=Fixture::workHeight=Fixture::guideHeight=64;
        Fixture::secondWidth=Fixture::secondHeight=64; Fixture::passWidth={}; Fixture::passHeight={};
        Fixture::creations=Fixture::evaluations=Fixture::resets=0; Fixture::lateCreationFrame=~0u;
        Fixture::refuseIndex=~0u; Fixture::objectControls=false;
        NeuralOptions options; options.enabled=true; options.runtimePath="scripted.dll"; options.passes=passes;
        options.beforeUpscaling=route==0; options.worldOnly=route!=2; options.tuning.uiCorrection=route==2;
        auto motion=gpu.Texture(64,64,std::vector<Pixel>(4096,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(64,64,std::vector<Pixel>(4096,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(64,64,std::vector<Pixel>(4096,{.1f,.05f,0,.25f}),desc.Format);
        Recovery recovery; auto pass=std::make_unique<NeuralPass>();
        FaultInjection::Arm(FaultInjection::Point::Evaluate,failAt);
        bool failed=false, retried=false, retryReset=false; unsigned creationsAtRefusal=0;
        for (unsigned frame=0;frame<18;++frame) {
            Fixture::frame=frame;
            // Each producer frame is intact before opening the NR list, matching
            // the D3D11 producer boundary. Colours change while NR is disabled.
            const Pixel colour{float(frame+1)/32,.25f,.5f,1};
            auto scene=gpu.Texture(64,64,std::vector<Pixel>(4096,colour),desc.Format);
            Pixel finalColour=colour;
            if(route==2) {finalColour[0]=colour[0]*.75f+.1f;finalColour[1]=colour[1]*.75f+.05f;finalColour[2]=colour[2]*.75f;}
            auto source=gpu.Texture(64,64,std::vector<Pixel>(4096,finalColour),desc.Format);
            std::printf("CASE route=%d passes=%d fail=%u frame=%u\n",route,passes,failAt,frame); std::fflush(stdout);
            MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            gpu.Begin();
            bool recorded=false;
            if (!recovery.Disabled()) {
                recorded=pass->Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0 || std::exchange(retryReset,false),true,64,64,
                    motion.Get(),depth.Get(),route==2?ui.Get():nullptr,scene.Get(),route==2?source.Get():nullptr);
                if (!recorded) {
                    const auto serial=gpu.serial;
                    const auto reason=pass->Status();
                    Require(RejectedNeuralLists::Get().Contains(gpu.list.Get()),"injected recording rejected");
                    Require(recovery.Refuse(reason,[&]{gpu.Discard();return true;},[&]{return gpu.fence->GetCompletedValue()==gpu.serial;},
                        [&]{return pass->RecoverySafe();},[&]{Require(pass->ReleaseAfterRetirement(),"safe release");pass.reset();return true;}),"recover refusal");
                    Require(gpu.serial==serial && recovery.Retryable(),"no rejected submit; explicit retry only");
                    ++refusals; failed=true; creationsAtRefusal=Fixture::creations;
                    gpu.Begin();
                } else { recovery.Enabled(); }
            }
            ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"backbuffer");
            Check(Interop::RecordCopy(gpu.list.Get(),recorded ? (route==2?pass->Composed():pass->Corrected()) : source.Get(),back.Get()),"fresh ordinary output list");
            gpu.End();
            if (recovery.Disabled()) {
                const auto result=gpu.Read(back.Get());
                for (const auto& pixel:result) for (unsigned c=0;c<4;++c) Near(pixel[c],finalColour[c],1.0/255,"advancing original output after NR refusal");
                Require(Fixture::creations==creationsAtRefusal,"no per-frame recreate");
            }
            Check(chain->Present(0,0),"actual native Present after NR failure"); ++presents;
            if (failed && !retried && frame>=10) {
                Require(recovery.Retry() && options.enabled,"Apply retries without rewriting requested preference");
                pass=std::make_unique<NeuralPass>(); Fixture::creations=0; Fixture::lateCreationFrame=~0u;
                retried=retryReset=true; ++retries;
            }
        }
        Require(failed && retried && !recovery.Disabled() && pass->ReadyPasses()==passes,"transient failure re-enables full chain");
        Require(Fixture::resets<=unsigned(2*passes),"history reset bounded to new owners");
        Require(pass->ReleaseAfterRetirement(),"final owner release");
    }
    // Unsafe ownership and retirement never release or retry. A failed teardown
    // permits independent output, retaining the whole owner until process exit.
    for (int failure=0;failure<4;++failure) {
        Recovery recovery; unsigned released=0;
        const bool independent=recovery.Refuse("original failure",[]{return true;},[&]{return failure!=0;},[&]{return failure!=1;},[&]{++released;return failure!=2;});
        Require(independent==(failure>=2),"separate terminal ownership boundary");
        Require(released==unsigned(failure>=2),"no release before retirement/ownership proof");
        Require(recovery.Retry()==(failure==3),"quarantine cannot create another owner");
    }
    // Present may enqueue work after the frame-copy fence. Retire that tail,
    // then any work DXGI queues while releasing its native buffers.
    gpu.Begin(); gpu.End();
    chain.Reset(); swap.Reset();
    gpu.Begin(); gpu.End();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::printf("PASS: NR recovery routes=3 passes=1/2/5 first/later Evaluate; presents=%u refusals=%u Apply retries=%u; pixel advancement and terminal retention\n",presents,refusals,retries);
}
