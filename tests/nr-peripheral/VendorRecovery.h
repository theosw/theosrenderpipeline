#pragma once
#include "FrameGen/NeuralRecovery.h"
#include "FrameGen/NeuralFaultInjection.h"
#include "FrameGen/NeuralLaunchHook.h"
#include "FrameGen/NeuralAdmission.h"
#include "FrameGen/NeuralRuntimeKernels.h"

// Manual GPU fixture, compiled only in the benchmark. Uses production pass,
// feature session, NVAPI rejection and release. No modified renderer is loaded.
static void VendorRecovery(GPU& gpu, NeuralOptions options, unsigned point, unsigned occurrence, unsigned route, unsigned passes, unsigned injectFrame, bool productionAdmission)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    Require(point>=1 && point<=12 && route<=2 && passes>=1 && passes<=5,"recovery arguments");
    options.beforeUpscaling=route==0; options.worldOnly=route!=2; options.passes=passes;
    options.tuning.uiCorrection=route==2;
    constexpr unsigned w=768,h=432;
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPVendorNRRecovery";
    Require(RegisterClassW(&wc)!=0,"vendor window class");
    auto window=CreateWindowW(wc.lpszClassName,L"TRP NR recovery",WS_POPUP,0,0,w,h,nullptr,nullptr,wc.hInstance,nullptr);
    Require(window!=nullptr,"vendor window");
    ComPtr<IDXGIFactory2> factory; Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"vendor present factory");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=w; desc.Height=h; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; Check(factory->CreateSwapChainForHwnd(gpu.queue.Get(),window,&desc,nullptr,nullptr,&swap),"vendor native presenter");
    ComPtr<IDXGISwapChain3> chain; Check(swap.As(&chain),"vendor rotating buffers");
    auto motion=gpu.Texture(w,h,std::vector<Pixel>(w*h,{}),DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.1f,.05f,0,.25f}),desc.Format);
    auto pass=std::make_unique<NeuralPass>(); Recovery recovery;
    unsigned disabledPresents=0, successfulAfterRetry=0; bool refused=false, retried=false, admissionRefused=false;
    for (unsigned frame=0;frame<injectFrame+18;++frame) {
        const Pixel colour{.1f+float(frame%8)/16,.3f,.5f,1};
        auto scene=gpu.Texture(w,h,std::vector<Pixel>(w*h,colour),desc.Format);
        Pixel finalColour=colour;
        if(route==2) {finalColour[0]=colour[0]*.75f+.1f;finalColour[1]=colour[1]*.75f+.05f;finalColour[2]=colour[2]*.75f;}
        auto source=gpu.Texture(w,h,std::vector<Pixel>(w*h,finalColour),desc.Format);
        if(frame==injectFrame) FaultInjection::Arm(static_cast<FaultInjection::Point>(point),occurrence);
        std::printf("FRAME %u\n",frame); std::fflush(stdout);
        MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        gpu.Begin(); bool recorded=false;
        if(!recovery.Disabled()) {
            recorded=pass->Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0 || retried,true,float(w),float(h),
                motion.Get(),depth.Get(),route==2?ui.Get():nullptr,scene.Get(),route==2?source.Get():nullptr);
            // Release/shutdown/retirement probes first trigger an ordinary NR
            // refusal. Teardown itself is then injected at the vendor boundary.
            if(frame==injectFrame && point>=8 && point<=10) recorded=false;
            if(!recorded) {
                const auto reason=pass->Status();
                const auto serial=gpu.serial;
                const bool independent=recovery.Refuse(reason,[&]{gpu.Discard();return true;},
                    [&]{return !FaultInjection::Hit(FaultInjection::Point::Retirement) && gpu.fence->GetCompletedValue()==gpu.serial;},
                    [&]{return pass->RecoverySafe() && SUCCEEDED(gpu.device->GetDeviceRemovedReason());},
                    [&]{if(!pass->ReleaseAfterRetirement()){(void)pass.release();return false;}pass.reset();return true;});
                Require(gpu.serial==serial,"vendor rejected list never submitted");
                refused=true;
                if(!independent) {
                    Require(!recovery.Retryable(),"uncertain retirement cannot retry");
                    (void)pass.release();
                    std::printf("PASS RECOVERY terminal point=%u original=%s retained=1 submitted=none restart=1\n",point,reason.c_str());
                    gpu.Begin(); gpu.End(); chain.Reset(); swap.Reset(); gpu.Begin(); gpu.End(); DestroyWindow(window); return;
                }
                std::printf("RECOVERY refused point=%u occurrence=%u frame=%u retryable=%d reason=%s\n",point,occurrence,frame,recovery.Retryable(),reason.c_str());
                gpu.Begin();
            } else if(retried) {++successfulAfterRetry;}
        }
        ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"vendor backbuffer");
        Check(Interop::RecordCopy(gpu.list.Get(),recorded?(route==2?pass->Composed():pass->Corrected()):source.Get(),back.Get()),"vendor output copy");
        gpu.End();
        if(recovery.Disabled()) {
            const auto image=gpu.Read(back.Get());
            for(const auto& pixel:image) for(unsigned c=0;c<4;++c) Near(pixel[c],finalColour[c],1.1/255,"vendor NR-off source advances");
            ++disabledPresents;
        }
        Check(chain->Present(0,0),"vendor actual Present");
        if(disabledPresents>=3 && recovery.Retryable() && !retried) {
            if (productionAdmission) {
                auto admission = QueryNeuralAdmission(gpu.device->GetAdapterLuid(), options.runtimePath,
                    w, h, options.passes, RuntimeKernelsSupported(), true);
                if (!admissionRefused && admission.Pascal()) {
                    // Policy fixture, not physical VRAM exhaustion. The actual
                    // runtime failure stays owned while this Apply is refused.
                    const auto reason = recovery.reason;
                    admission.budgetBytes = admission.usageBytes + 1;
                    Require(admission.Refusal() == kPascalBudgetReason && !recovery.Retry(!admission.Refusal()),
                        "budget-refused Apply must preserve retry latch");
                    Require(recovery.Retryable() && recovery.reason == reason,
                        "unchanged Apply retains original runtime refusal");
                    admissionRefused = true;
                    std::printf("RECOVERY APPLY_REFUSED retryable=1 original_preserved=1 reason=%s\n", admission.Refusal());
                    continue;
                }
                Require(!admission.Refusal(), "Apply repeats full production admission before recreation");
                std::printf("RECOVERY APPLY_ADMITTED passes=%u estimate=%llu\n", passes,
                    static_cast<unsigned long long>(admission.estimateBytes));
            }
            Require(ResetLaunchHookAfterRetirement() && recovery.Retry(),"admitted Apply resets failure latch after retirement");
            pass=std::make_unique<NeuralPass>(); retried=true;
        }
    }
    Require(refused && FaultInjection::hits==1,"requested vendor injection occurred exactly once");
    Require(disabledPresents>=3,"multiple advancing native presents with NR disabled");
    if(point<8 || point==11 || point==12) Require(retried && successfulAfterRetry>=5,"Apply re-admits and runs after transient refusal");
    else Require(!retried,"failed teardown forbids another owner");
    if(pass) Require(pass->ReleaseAfterRetirement(),"vendor final release");
    std::printf("PASS RECOVERY point=%u occurrence=%u route=%u passes=%u disabled_presents=%u retry_evaluations=%u injected=1\n",point,occurrence,route,passes,disabledPresents,successfulAfterRetry);
    gpu.Begin(); gpu.End(); chain.Reset(); swap.Reset(); gpu.Begin(); gpu.End(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
}
