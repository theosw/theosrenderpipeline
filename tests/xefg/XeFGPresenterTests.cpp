#include "XeFGTestSupport.h"
#include "FrameGen/XeFGPresenter.h"
#include "NvidiaPresenterFixture.h"
#include <dbghelp.h>
#include <chrono>
#include <algorithm>

static void PrintTiming(const char* label,std::vector<double> values) {
    Require(!values.empty(),"XeFG timing samples");std::sort(values.begin(),values.end());
    std::printf("XEFG_TIMING %s samples=%zu p50=%.6f p95=%.6f ms (fixture CPU boundary, not GPU model time)\n",
        label,values.size(),values[(values.size()-1)/2],values[std::size_t((values.size()-1)*.95)]);
}

// Production owner on hardware. Plain DXGI presenters check HWND retirement
// before/after Intel; actual Streamline replacement remains a separate check.
int wmain(int argc, wchar_t** argv) {
    // Keep an unhandled-exception dump from non-debugger runs: debugger timing
    // can hide SDK lifecycle faults. This fixture is run from a private output directory.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        auto file = CreateFileW(L"xefg-presenter-crash.dmp", GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                MiniDumpNormal, &info, nullptr, nullptr);
            CloseHandle(file);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    std::setvbuf(stdout,nullptr,_IONBF,0); Require(argc==2||argc==3,"Intel runtime directory, optional NVIDIA runtime directory");
    const bool profile=GetEnvironmentVariableW(L"TRP_XEFG_PROFILE",nullptr,0)!=0;
    const unsigned width=profile?5120:640,height=profile?1440:360;
    std::vector<double> presentTimes,retirementTimes,sleepTimes;
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory"); Check(factory->EnumAdapters1(0,&adapter),"adapter");
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11; ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)),"D3D12");
    ComPtr<ID3D12InfoQueue> messages; Check(d12.As(&messages),"debug messages");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue;
    Check(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    Interop interop; Check(interop.Initialize(d11.Get(),d12.Get(),queue.Get()),"shared owner");
    std::unique_ptr<NvidiaPresenterFixture> nvidia;
    if(argc==3) nvidia=std::make_unique<NvidiaPresenterFixture>(std::filesystem::absolute(argv[2]),d12.Get(),factory.Get(),interop);
    auto* presentingFactory=nvidia?nvidia->proxyFactory.Get():static_cast<IDXGIFactory*>(factory.Get());
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPXeFGPresenterTests";
    Require(RegisterClassW(&wc)!=0,"register window");
    HWND window=CreateWindowW(wc.lpszClassName,L"TRP production XeFG presenter test",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,700,440,nullptr,nullptr,wc.hInstance,nullptr);
    Require(window!=nullptr,"window"); if(!profile) {
        ShowWindow(window,SW_SHOW);
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferDesc.Width=width; desc.BufferDesc.Height=height;
    // HDR10 layers: AUTO must also cover 2-bit-alpha UI and frames without a UI layer.
    const bool hdr10=GetEnvironmentVariableW(L"TRP_XEFG_HDR10",nullptr,0)!=0;
    const DXGI_FORMAT colorFormat=hdr10?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.Format=colorFormat; desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=window; desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    TheosRenderPipeline::XeFGPresenter presenter;
    presenter.SetLogger([](const char* text) { std::printf("[Owner] %s\n",text); });
    Check(presenter.Probe(d12.Get(),std::filesystem::absolute(argv[1])),"production admission");
    Inputs input; input.Create(interop,d11.Get(),width,height,colorFormat); auto* stable=input.color.texture11.Get(); unsigned generated=0, uiTagged=0, generatedWithUI=0, generatedWithoutUI=0, frameTimeSent=0, frameTimeSkipped=0;
    const bool experimental=GetEnvironmentVariableW(L"TRP_XEFG_MFG_TEST",nullptr,0)!=0;
    const bool expectRefusal=GetEnvironmentVariableW(L"TRP_XEFG_EXPECT_REFUSAL",nullptr,0)!=0;
    Require(!expectRefusal||experimental,"refusal mode requires experimental request");
    std::array<unsigned,4> generatedCounts{};
    const bool nativeOnly=GetEnvironmentVariableW(L"TRP_XEFG_NATIVE_ONLY",nullptr,0)!=0;
    const bool releaseNvidia=GetEnvironmentVariableW(L"TRP_XEFG_RELEASE_NV",nullptr,0)!=0;
    wchar_t cyclesText[8]{};
    unsigned cycles=experimental?3u:2u;
    if(GetEnvironmentVariableW(L"TRP_XEFG_CYCLES",cyclesText,8)) {
        cycles=static_cast<unsigned>(std::wcstoul(cyclesText,nullptr,10));
        Require(cycles>=2 && cycles<=8,"bounded lifecycle cycle count");
    }
    // Production Lab A/B: queue a D3D11 GPU wait for the input copies instead of CPU drains.
    const bool gpuInputWait=GetEnvironmentVariableW(L"TRP_XEFG_GPU_INPUT_WAIT",nullptr,0)!=0;
    std::printf("owner cycles=%u releaseRetiredNvidia=%u experimental=%u inputReuse=%s\n",cycles,releaseNvidia,experimental,gpuInputWait?"gpu-fence":"cpu-drain");
    for(unsigned round=0;round<cycles;++round) {
        ComPtr<IDXGISwapChain> native; Check(presentingFactory->CreateSwapChain(queue.Get(),&desc,&native),"native before Intel");
        if(nvidia) { nvidia->Begin(presenter.Snapshot().frameId); nvidia->BeforePresent(); }
        Check(native->Present(0,0),"native passthrough");
        if(nvidia) nvidia->AfterPresent(S_OK);
        Check(interop.WaitForInputReaders(nullptr,0),"native retirement bridge"); Check(interop.Drain(),"native queue retirement"); native.Reset();
        if(nvidia && releaseNvidia) { nvidia->ReleaseRetiredResources(); }
        if(!nativeOnly) {
        ComPtr<IDXGISwapChain> proxy;
        Check(presenter.Create(d12.Get(),queue.Get(),factory.Get(),desc,round==1,&proxy,
            nvidia?nvidia->session.Snapshot().frameIndex:0,{experimental && round<2,3}),"production Intel presenter");
        ComPtr<IDXGISwapChain3> proxy3; Check(proxy.As(&proxy3),"proxy3");
        for(unsigned frame=0;frame<90;++frame) {
            MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if(!gpuInputWait) Check(interop.Drain(),"producer reuse");
            input.Paint(c11.Get(),frame);
            std::vector<uint16_t> motions(width*height*2,0);
            for(size_t i=0;i<motions.size();i+=2) motions[i]=DirectX::PackedVector::XMConvertFloatToHalf(-2.0f/width);
            c11->UpdateSubresource(input.producerMotion.Get(),0,nullptr,motions.data(),width*4,0);
            Check(interop.CopyInput(input.producerColor.Get(),input.color),"color");
            Check(interop.CopyInput(input.producerHudless.Get(),input.hudless),"hudless");
            Check(interop.CopyInput(input.producerMotion.Get(),input.motion),"motion");
            Check(interop.CopyInput(input.producerDepth.Get(),input.depth),"depth");
            Check(interop.CopyInput(input.producerUI.Get(),input.ui),"UI layer");
            sl::Constants camera{}; camera.cameraRight={1,0,0}; camera.cameraUp={0,1,0}; camera.cameraFwd={0,0,-1};
            camera.cameraPos={-4.0f*frame/width,0,0};
            camera.cameraViewToClip.row[0]={1,0,0,0}; camera.cameraViewToClip.row[1]={0,1,0,0};
            camera.cameraViewToClip.row[2]={0,0,1,0}; camera.cameraViewToClip.row[3]={0,0,0,1};
            camera.jitterOffset={0,0}; camera.depthInverted=round==1?sl::eTrue:sl::eFalse;
            camera.reset=frame==8||frame==45?sl::eTrue:sl::eFalse;
            // Round 1 sends the measured frame time; round 0 leaves Intel's estimate.
            Check(presenter.Prepare(camera,round==1),"production camera"); Require(presenter.Snapshot().prepared,"depth convention");
            // Intervals over 250 ms (a slow profile frame, or a multiplier change that
            // rebuilds Intel's shaders) correctly send 0, so require most frames, not all.
            const float sentFrameTime=presenter.Snapshot().frameTimeMs;
            Require(round==1&&frame>=1 ? sentFrameTime>=0 && sentFrameTime<=250 : sentFrameTime==0,"frame time sent only when requested and measured");
            if(round==1&&frame>=1) ++(sentFrameTime>0?frameTimeSent:frameTimeSkipped);
            // Round 0 exercises both Intel debug views live; recreation starts with them off.
            const bool onlyGenerated=round==0&&frame>=70&&frame<76, tagGenerated=round==0&&frame>=60&&frame<76;
            if(round==1&&frame==0) Require(!presenter.Snapshot().onlyGenerated&&!presenter.Snapshot().tagGenerated,"debug views reset with the context");
            Check(presenter.SetDebugView(onlyGenerated,tagGenerated),"debug view");
            Require(presenter.Snapshot().onlyGenerated==onlyGenerated&&presenter.Snapshot().tagGenerated==tagGenerated,"debug view state");
            ID3D12GraphicsCommandList* list{}; Check(interop.SignalD3D11(Work::SwapChain),"signal source");
            Check(interop.Begin(Work::SwapChain,&list),"begin output");
            ComPtr<ID3D12Resource> back; Check(proxy->GetBuffer(proxy3->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"backbuffer");
            Check(Interop::RecordCopy(list,input.color.texture12.Get(),back.Get()),"source copy");
            const bool enable=frame>=8&&frame<82;
            // Round 0 tags the UI layer, then offers a mismatched layer that must be
            // rejected; later rounds provide none, leaving AUTO to extract from the frame.
            auto* ui=round==0?(frame<45?input.ui.texture12.Get():input.depth.texture12.Get()):nullptr;
            const bool uiExpected=enable&&round==0&&frame<45;
            const unsigned count=experimental?(frame<30?1u:frame<55?2u:3u):1u;
            Check(presenter.BeforePresent(list,input.motion.texture12.Get(),input.depth.texture12.Get(),input.hudless.texture12.Get(),ui,enable,true,profile?0:60,count),"production preparation");
            Require(presenter.Snapshot().uiTexture==uiExpected,"UI layer tagged only when it matches the HUD-less layer");
            if(uiExpected) ++uiTagged;
            Check(interop.Submit(Work::SwapChain),"submit SDK copies"); back.Reset();
            Check(presenter.FinalizePresent(),"markers after queue submission");
            const auto presentStart=std::chrono::steady_clock::now();
            const auto result=proxy->Present(0,0);const auto presentEnd=std::chrono::steady_clock::now();
            Check(result,"Intel present"); Require(result==S_OK,"visible present");
            Check(presenter.AfterPresent(result),"production completion");
            const auto retirementStart=std::chrono::steady_clock::now();
            Check(gpuInputWait?interop.WaitD3D11(Work::SwapChain):interop.Drain(),gpuInputWait?"ONLY_NOW input reuse GPU wait":"retire ONLY_NOW copies");
            const auto retirementEnd=std::chrono::steady_clock::now();
            if(profile&&enable&&frame>=24) {
                presentTimes.push_back(std::chrono::duration<double,std::milli>(presentEnd-presentStart).count());
                retirementTimes.push_back(std::chrono::duration<double,std::milli>(retirementEnd-retirementStart).count());
            }
            const auto& state=presenter.Snapshot(); Require(state.framesPresented>=1&&state.framesPresented<=state.generatedFrames+1,"selected count bounds");
            if(!enable) Require(state.framesPresented==1,"off passthrough");
            Require(state.frameLimitUs==(profile?0:16667),"output cap remains independent of ratio");
            Require(state.maxGeneratedFrames==(experimental && !expectRefusal && round<2?3u:1u),"opt-in capacity, including disable after resident unlock");
            if(expectRefusal) Require(!state.unlockReady && state.generatedFrames==1,"refused unlock retains official x2");
            if(enable && state.framesPresented==state.generatedFrames+1) {++generated;++generatedCounts[state.generatedFrames];++(state.uiTexture?generatedWithUI:generatedWithoutUI);}
            if(frame%30==0) std::printf("production round=%u requested=%u actual=%u outputs=%u capacity=%u optIn=%u\n",round,count+1,state.generatedFrames+1,state.framesPresented,state.maxGeneratedFrames+1,state.experimentalMFG);
            if(frame!=89) {
                const auto start=std::chrono::steady_clock::now();Check(presenter.BeginFrame(),"next frame");
                if(profile&&enable&&frame>=24) sleepTimes.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            }
        }
        Check(presenter.Disable(),"disable"); Check(interop.Drain(),"retire shared resources");
        proxy3.Reset(); proxy.Reset(); Check(presenter.Destroy(),"SDK private retirement");
        }
        Require(input.color.texture11.Get()==stable,"stable host resource");
        Check(presentingFactory->CreateSwapChain(queue.Get(),&desc,&native),"native restored on HWND");
        Check(native->Present(0,0),"establish restored native presentation before latency markers");
        if(nvidia) { nvidia->Begin(presenter.Snapshot().frameId); nvidia->BeforePresent(); }
        Check(native->Present(0,0),"restored present");
        if(nvidia) nvidia->AfterPresent(S_OK);
        Check(interop.WaitForInputReaders(nullptr,0),"restored native retirement bridge");
        Check(interop.Drain(),"restored native queue retirement"); native.Reset();
    }
    Require(nativeOnly||generated>100,"sustained generation both depth conventions");
    Require(presenter.Snapshot().uiTexturePresents==uiTagged && (nativeOnly||uiTagged>0),"UI layer tag count");
    std::printf("frame time sent=%u skipped=%u\n",frameTimeSent,frameTimeSkipped);
    Require(nativeOnly||frameTimeSent>frameTimeSkipped*4,"measured frame time sent on most frames");
    // AUTO must keep generating on frames without a UI layer, in SDR and HDR10.
    std::printf("generated withUI=%u withoutUI=%u hdr10=%u\n",generatedWithUI,generatedWithoutUI,hdr10);
    Require(nativeOnly||(generatedWithUI>20&&generatedWithoutUI>20),"generation with and without a UI layer");
    if(profile) {
        PrintTiming("outer Present",presentTimes);PrintTiming("post-Present reuse wait",retirementTimes);PrintTiming("BeginFrame / XeLL sleep",sleepTimes);
    }
    if(experimental && !nativeOnly && !expectRefusal) Require(generatedCounts[1]>20 && generatedCounts[2]>20 && generatedCounts[3]>20,"production live x2/x3/x4 and disable-to-x2");
    if(expectRefusal && !nativeOnly) Require(generatedCounts[1]>100 && generatedCounts[2]==0 && generatedCounts[3]==0,"sustained official x2 after unlock refusal");
    for(UINT64 n=0;n<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++n) {
        SIZE_T size{}; messages->GetMessage(n,nullptr,&size); std::vector<unsigned char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(n,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr,"%s\n",message->pDescription); Require(false,"debug errors"); }
    }
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::printf("PASS mode=%s productionOwner generatedPresents=%u uiTexturePresents=%u stableSharedResources=true physicalCadenceVerified=false\n",
        nativeOnly?"NVIDIA-control":nvidia?"NVIDIA-XeFG-NVIDIA":"DXGI-XeFG-DXGI",generated,uiTagged);
    std::printf("multiplier coverage x2=%u x3=%u x4=%u experimental=%u\n",generatedCounts[1],generatedCounts[2],generatedCounts[3],experimental);
}
