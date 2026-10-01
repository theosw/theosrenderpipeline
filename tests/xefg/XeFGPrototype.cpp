#include "XeFGTestSupport.h"
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::filesystem::path runtime;
    unsigned frames=120, adapterIndex=0;
    bool probeOnly=false, visible=false, debug=false;
    for(int n=1;n<argc;++n) {
        const std::wstring_view arg=argv[n];
        if(arg==L"--runtime-dir" && n+1<argc) runtime=std::filesystem::absolute(argv[++n]);
        else if(arg==L"--frames" && n+1<argc) frames=static_cast<unsigned>(std::wcstoul(argv[++n],nullptr,10));
        else if(arg==L"--adapter" && n+1<argc) adapterIndex=static_cast<unsigned>(std::wcstoul(argv[++n],nullptr,10));
        else if(arg==L"--probe-only") probeOnly=true;
        else if(arg==L"--visible") visible=true;
        else if(arg==L"--debug-layer") debug=true;
        else { std::fprintf(stderr,"Unknown/incomplete argument\n"); return 2; }
    }
    Require(!runtime.empty() && frames>=48 && frames<=3600,"runtime directory and frames 48..3600");
    std::printf("TRP XeFG public-API prototype; no NVIDIA unlock; no Skyrim hooks\n");
    Api api(runtime);
    xefg_swapchain_version_t fv{}; xell_version_t lv{};
    FG(api.xefgSwapChainGetVersion(&fv),"FG version"); LL(api.xellGetVersion(&lv),"LL version");
    std::printf("API XeFG=%u.%u.%u XeLL=%u.%u.%u\n",fv.major,fv.minor,fv.patch,lv.major,lv.minor,lv.patch);
    Require(fv.major==1 && fv.minor==3 && lv.major==1 && lv.minor>=3,"supported component API");
    if(debug) {
        ComPtr<ID3D12Debug> layer;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)),"D3D12 debug layer");
        layer->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    Check(factory->EnumAdapters1(adapterIndex,&adapter),"requested adapter");
    DXGI_ADAPTER_DESC1 ad{}; Check(adapter->GetDesc1(&ad),"adapter identity");
    std::printf("adapter vendor=0x%04X device=0x%04X LUID=%08lX:%08lX name=%ls\n",
        ad.VendorId,ad.DeviceId,static_cast<unsigned long>(ad.AdapterLuid.HighPart),ad.AdapterLuid.LowPart,ad.Description);
    Require(!(ad.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"hardware adapter required");
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11; ComPtr<ID3D12Device> d12;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,debug?D3D11_CREATE_DEVICE_DEBUG:0,
        nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11 device");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)),"matching D3D12 device");
    ComPtr<ID3D12InfoQueue> debugMessages;
    if(debug) Check(d12.As(&debugMessages),"D3D12 diagnostic queue");
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_4};
    Check(d12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"SM6.4");
    Require(sm.HighestShaderModel>=D3D_SHADER_MODEL_6_4,"non-Intel SM6.4 prerequisite");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"presenting queue");
    Interop interop; Check(interop.Initialize(d11.Get(),d12.Get(),queue.Get()),"production interop");
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPXeFGPrototype";
    Require(RegisterClassW(&wc)!=0,"register test window");
    HWND window=CreateWindowW(wc.lpszClassName,L"TRP XeFG prototype: official x2",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,840,520,nullptr,nullptr,wc.hInstance,nullptr);
    Require(window!=nullptr,"create test window");
    // An occluded/minimized swapchain cannot establish interpolation acceptance.
    ShowWindow(window,visible?SW_SHOW:SW_SHOWNOACTIVATE);
    unsigned totalGenerated=0; uint32_t id=0;
    for(unsigned round=0;round<(probeOnly?1u:2u);++round) {
        xell_context_handle_t ll{}; xefg_swapchain_handle_t fg{};
        LL(api.xellD3D12CreateContext(d12.Get(),&ll),"create XeLL");
        xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=1; sleep.minimumIntervalUs=16667;
        LL(api.xellSetSleepMode(ll,&sleep),"enable XeLL before XeFG");
        FG(api.xefgSwapChainD3D12CreateContext(d12.Get(),&fg),"create XeFG");
        FG(api.xefgSwapChainSetLoggingCallback(fg,XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,Log,nullptr),"logging callback");
        FG(api.xefgSwapChainSetLatencyReduction(fg,ll),"connect XeLL");
        xefg_swapchain_properties_t props{};
        FG(api.xefgSwapChainGetProperties(fg,&props),"capability properties");
        std::printf("round=%u maxSupportedInterpolations=%u requested=1\n",round,props.maxSupportedInterpolations);
        Require(props.maxSupportedInterpolations>=1,"official x2 admission");
        if(probeOnly) {
            FG(api.xefgSwapChainDestroy(fg),"destroy probe XeFG");
            LL(api.xellDestroyContext(ll),"destroy probe XeLL"); break;
        }
        const UINT width=round?800:640, height=round?448:360;
        xefg_swapchain_d3d12_init_params_t init{};
        init.maxInterpolatedFrames=1; init.uiMode=XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
        FG(api.xefgSwapChainD3D12GetProperties(fg,&init,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,&props),"allocation properties");
        std::printf("heapBytes buffers=%llu textures=%llu descriptors=%u\n",
            props.tempBufferHeapSize,props.tempTextureHeapSize,props.requiredDescriptorCount);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width=width; desc.Height=height; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        FG(api.xefgSwapChainD3D12InitFromSwapChainDesc(fg,window,&desc,nullptr,queue.Get(),factory.Get(),&init),"init proxy");
        ComPtr<IDXGISwapChain4> proxy;
        FG(api.xefgSwapChainD3D12GetSwapChainPtr(fg,IID_PPV_ARGS(&proxy)),"get exclusive XeFG proxy");
        Inputs input; input.Create(interop,d11.Get(),width,height);
        FG(api.xefgSwapChainSetNumInterpolatedFrames(fg,1),"select official x2 once");
        FG(api.xefgSwapChainSetUiCompositionState(fg,XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED),"enable HUD extraction composition");
        FG(api.xefgSwapChainEnableDebugFeature(fg,XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,1,nullptr),"debug generated-frame markers");
        unsigned generated=0, off=0, cuts=0;
        bool enabled=false;
        FG(api.xefgSwapChainSetEnabled(fg,0),"initial passthrough");
        for(unsigned frame=0;frame<frames;++frame) {
            MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            const bool desired=frame>=8 && frame<frames-8;
            if(desired!=enabled) {
                FG(api.xefgSwapChainSetEnabled(fg,desired),"frame-boundary enable change"); enabled=desired;
                std::printf("phase round=%u frame=%u enabled=%u\n",round,frame,enabled);
            }
            ++id; LL(api.xellSleep(ll,id),"sleep before first frame marker");
            LL(api.xellAddMarkerData(ll,id,XELL_SIMULATION_START),"simulation start");
            LL(api.xellAddMarkerData(ll,id,XELL_SIMULATION_END),"simulation end");
            LL(api.xellAddMarkerData(ll,id,XELL_RENDERSUBMIT_START),"render submit start");
            // ONLY_NOW tags record SDK copies on this list. Retire those copies
            // before D3D11 reuses inputs. Do not assume a client-queue fence
            // retires the provider's asynchronous presentation queue.
            Check(interop.Drain(),"previous input-copy retirement");
            input.Paint(c11.Get(),frame);
            Check(interop.CopyInput(input.producerColor.Get(),input.color),"color producer copy");
            Check(interop.CopyInput(input.producerHudless.Get(),input.hudless),"HUD-less producer copy");
            Check(interop.CopyInput(input.producerDepth.Get(),input.depth),"depth producer copy");
            Check(interop.CopyInput(input.producerMotion.Get(),input.motion),"motion producer copy");
            Check(interop.SignalD3D11(Work::FrameGeneration),"D3D11 producer signal");
            Check(interop.WaitD3D12(Work::FrameGeneration),"D3D12 producer wait");
            ID3D12GraphicsCommandList* commands{};
            Check(interop.Begin(Work::FrameGeneration,&commands),"begin input recording");
            auto tag=[&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource) {
                xefg_swapchain_d3d12_resource_data_t data{};
                data.type=type; data.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;
                data.resourceSize={width,height}; data.pResource=resource; data.incomingState=D3D12_RESOURCE_STATE_COMMON;
                FG(api.xefgSwapChainD3D12TagFrameResource(fg,commands,id,&data),"tag copied input");
            };
            tag(XEFG_SWAPCHAIN_RES_DEPTH,input.depth.texture12.Get());
            tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR,input.motion.texture12.Get());
            tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR,input.hudless.texture12.Get());
            ComPtr<ID3D12Resource> back;
            Check(proxy->GetBuffer(proxy->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"current proxy backbuffer");
            Check(Interop::RecordCopy(commands,input.color.texture12.Get(),back.Get()),"source color to proxy backbuffer");
            xefg_swapchain_frame_constant_data_t constants{};
            for(unsigned n=0;n<4;++n) { constants.viewMatrix[n*5]=1; constants.projectionMatrix[n*5]=1; }
            constants.viewMatrix[12]=4.0f*static_cast<float>(frame)/static_cast<float>(width);
            constants.motionVectorScaleX=constants.motionVectorScaleY=1;
            constants.resetHistory=frame==8 || frame==frames/2;
            // Zero requests the SDK's own estimate. No duplicate GPU timer and
            // no present-inclusive interval is mislabeled as rendering time.
            constants.frameRenderTime=0;
            FG(api.xefgSwapChainTagFrameConstants(fg,id,&constants),"tag frame constants");
            Check(interop.Submit(Work::FrameGeneration),"submit producer and SDK input copies");
            back.Reset();
            LL(api.xellAddMarkerData(ll,id,XELL_RENDERSUBMIT_END),"render submit end");
            FG(api.xefgSwapChainSetPresentId(fg,id),"same present id");
            LL(api.xellAddMarkerData(ll,id,XELL_PRESENT_START),"present start");
            Check(proxy->Present(0,0),"XeFG-owned present");
            LL(api.xellAddMarkerData(ll,id,XELL_PRESENT_END),"present end");
            xefg_swapchain_present_status_t status{};
            FG(api.xefgSwapChainGetLastPresentStatus(fg,&status),"last present status");
            std::printf("present round=%u id=%u enabled=%u reset=%u frames=%u interpolation=%d\n",
                round,id,status.isFrameGenEnabled,constants.resetHistory,status.framesPresented,static_cast<int>(status.frameGenResult));
            Require(status.isFrameGenEnabled==static_cast<unsigned>(enabled),"effective enabled state");
            Require(status.framesPresented>=1 && status.framesPresented<=2,"official x2 frame count bound");
            if(!enabled) { Require(status.framesPresented==1,"passthrough presents one frame"); ++off; }
            else if(constants.resetHistory) { Require(status.framesPresented==1,"reset suppresses interpolation"); ++cuts; }
            else if(status.framesPresented==2 && status.frameGenResult==XEFG_SWAPCHAIN_RESULT_SUCCESS) ++generated;
        }
        Require(generated>frames/3 && off==16 && cuts==2,"passthrough, reset and sustained SDK x2");
        FG(api.xefgSwapChainSetEnabled(fg,0),"disable before destruction");
        Check(interop.Drain(),"client queue retirement before release");
        c11->ClearState(); c11->Flush();
        proxy.Reset();
        // SDK Destroy owns retirement of its internal copies/presentation work.
        // No second presenter may be created before this call succeeds.
        FG(api.xefgSwapChainDestroy(fg),"destroy XeFG and internal presenter");
        LL(api.xellDestroyContext(ll),"destroy XeLL after XeFG");
        totalGenerated+=generated;
        std::printf("round_complete round=%u generatedPresents=%u offPresents=%u cuts=%u dimensions=%ux%u\n",
            round,generated,off,cuts,width,height);
    }
    Check(interop.Drain(),"final shared-resource retirement");
    if(debugMessages) {
        unsigned errors=0;
        const auto count=debugMessages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for(UINT64 n=0;n<count;++n) {
            SIZE_T size{}; debugMessages->GetMessage(n,nullptr,&size);
            std::vector<unsigned char> storage(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(debugMessages->GetMessage(n,message,&size),"read debug message");
            std::printf("D3D12 severity=%d id=%d %s\n",message->Severity,message->ID,message->pDescription);
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) ++errors;
        }
        Require(errors==0,"D3D12 debug layer contains no errors/corruption");
    }
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::printf("PASS mode=%s sdkGeneratedPresents=%u physicalCadenceVerified=false AMDValidated=%s\n",
        probeOnly?"capability-only":"D3D11-shared-inputs-off-x2-off-recreate",totalGenerated,ad.VendorId==0x1002?"SDK-only":"false");
}
