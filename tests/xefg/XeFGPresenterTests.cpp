#include "XeFGTestSupport.h"
#include "FrameGen/XeFGPresenter.h"
#include "NvidiaPresenterFixture.h"
#include <dbghelp.h>

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
    Require(window!=nullptr,"window"); ShowWindow(window,SW_SHOW);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferDesc.Width=640; desc.BufferDesc.Height=360;
    // HDR10 layers: AUTO must also cover 2-bit-alpha UI and frames without a UI layer.
    const bool hdr10=GetEnvironmentVariableW(L"TRP_XEFG_HDR10",nullptr,0)!=0;
    const DXGI_FORMAT colorFormat=hdr10?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.Format=colorFormat; desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=window; desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    TheosRenderPipeline::XeFGPresenter presenter;
    presenter.SetLogger([](const char* text) { std::printf("[Owner] %s\n",text); });
    Check(presenter.Probe(d12.Get(),std::filesystem::absolute(argv[1])),"production admission");
    Inputs input; input.Create(interop,d11.Get(),640,360,colorFormat); auto* stable=input.color.texture11.Get(); unsigned generated=0, uiTagged=0, generatedWithUI=0, generatedWithoutUI=0;
    const bool nativeOnly=GetEnvironmentVariableW(L"TRP_XEFG_NATIVE_ONLY",nullptr,0)!=0;
    const bool releaseNvidia=GetEnvironmentVariableW(L"TRP_XEFG_RELEASE_NV",nullptr,0)!=0;
    wchar_t cyclesText[8]{};
    unsigned cycles=2u;
    if(GetEnvironmentVariableW(L"TRP_XEFG_CYCLES",cyclesText,8)) {
        cycles=static_cast<unsigned>(std::wcstoul(cyclesText,nullptr,10));
        Require(cycles>=2 && cycles<=8,"bounded lifecycle cycle count");
    }
    // Production Lab A/B: queue a D3D11 GPU wait for the input copies instead of CPU drains.
    const bool gpuInputWait=GetEnvironmentVariableW(L"TRP_XEFG_GPU_INPUT_WAIT",nullptr,0)!=0;
    std::printf("owner cycles=%u releaseRetiredNvidia=%u inputReuse=%s\n",cycles,releaseNvidia,gpuInputWait?"gpu-fence":"cpu-drain");
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
            nvidia?nvidia->session.Snapshot().frameIndex:0),"production Intel presenter");
        ComPtr<IDXGISwapChain3> proxy3; Check(proxy.As(&proxy3),"proxy3");
        for(unsigned frame=0;frame<90;++frame) {
            MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if(!gpuInputWait) Check(interop.Drain(),"producer reuse");
            input.Paint(c11.Get(),frame);
            std::vector<uint16_t> motions(640*360*2,0);
            for(size_t i=0;i<motions.size();i+=2) motions[i]=DirectX::PackedVector::XMConvertFloatToHalf(-2.0f/640.0f);
            c11->UpdateSubresource(input.producerMotion.Get(),0,nullptr,motions.data(),640*4,0);
            Check(interop.CopyInput(input.producerColor.Get(),input.color),"color");
            Check(interop.CopyInput(input.producerHudless.Get(),input.hudless),"hudless");
            Check(interop.CopyInput(input.producerMotion.Get(),input.motion),"motion");
            Check(interop.CopyInput(input.producerDepth.Get(),input.depth),"depth");
            Check(interop.CopyInput(input.producerUI.Get(),input.ui),"UI layer");
            sl::Constants camera{}; camera.cameraRight={1,0,0}; camera.cameraUp={0,1,0}; camera.cameraFwd={0,0,-1};
            camera.cameraPos={-4.0f*frame/640.0f,0,0};
            camera.cameraViewToClip.row[0]={1,0,0,0}; camera.cameraViewToClip.row[1]={0,1,0,0};
            camera.cameraViewToClip.row[2]={0,0,1,0}; camera.cameraViewToClip.row[3]={0,0,0,1};
            camera.jitterOffset={0,0}; camera.depthInverted=round==1?sl::eTrue:sl::eFalse;
            camera.reset=frame==8||frame==45?sl::eTrue:sl::eFalse;
            // Round 1 sends the measured frame time; round 0 leaves Intel's estimate.
            Check(presenter.Prepare(camera,round==1),"production camera"); Require(presenter.Snapshot().prepared,"depth convention");
            Require(round==1&&frame>=1 ? presenter.Snapshot().frameTimeMs>0 && presenter.Snapshot().frameTimeMs<=250 :
                presenter.Snapshot().frameTimeMs==0,"frame time sent only when requested and measured");
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
            // rejected; round 1 provides none, leaving AUTO to extract from the frame.
            auto* ui=round==0?(frame<45?input.ui.texture12.Get():input.depth.texture12.Get()):nullptr;
            const bool uiExpected=enable&&round==0&&frame<45;
            Check(presenter.BeforePresent(list,input.motion.texture12.Get(),input.depth.texture12.Get(),input.hudless.texture12.Get(),ui,enable,true,60),"production preparation");
            Require(presenter.Snapshot().uiTexture==uiExpected,"UI layer tagged only when it matches the HUD-less layer");
            if(uiExpected) ++uiTagged;
            Check(interop.Submit(Work::SwapChain),"submit SDK copies"); back.Reset();
            Check(presenter.FinalizePresent(),"markers after queue submission");
            const auto result=proxy->Present(0,0); Check(result,"Intel present"); Require(result==S_OK,"visible present");
            Check(presenter.AfterPresent(result),"production completion");
            Check(gpuInputWait?interop.WaitD3D11(Work::SwapChain):interop.Drain(),gpuInputWait?"ONLY_NOW input reuse GPU wait":"retire ONLY_NOW copies");
            const auto& state=presenter.Snapshot(); Require(state.framesPresented>=1&&state.framesPresented<=2,"x2 bounds");
            if(!enable) Require(state.framesPresented==1,"off passthrough");
            if(enable&&state.framesPresented==2) { ++generated; ++(state.uiTexture?generatedWithUI:generatedWithoutUI); }
            if(frame!=89) Check(presenter.BeginFrame(),"next frame");
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
    Require(nativeOnly||generated>100,"sustained production x2 both depth conventions");
    Require(presenter.Snapshot().uiTexturePresents==uiTagged && (nativeOnly||uiTagged>0),"UI layer tag count");
    // AUTO must keep generating on frames without a UI layer, in SDR and HDR10.
    std::printf("generated withUI=%u withoutUI=%u hdr10=%u\n",generatedWithUI,generatedWithoutUI,hdr10);
    Require(nativeOnly||(generatedWithUI>20&&generatedWithoutUI>20),"generation with and without a UI layer");
    for(UINT64 n=0;n<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++n) {
        SIZE_T size{}; messages->GetMessage(n,nullptr,&size); std::vector<unsigned char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(n,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr,"%s\n",message->pDescription); Require(false,"debug errors"); }
    }
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::printf("PASS mode=%s productionOwner generatedPresents=%u uiTexturePresents=%u stableSharedResources=true physicalCadenceVerified=false\n",
        nativeOnly?"NVIDIA-control":nvidia?"NVIDIA-XeFG-NVIDIA":"DXGI-XeFG-DXGI",generated,uiTagged);
}
