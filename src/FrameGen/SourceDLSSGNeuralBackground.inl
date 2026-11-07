// Included inside SourceDLSSG. All reconstruction and temporal operations use
// the same kernels as the synchronous/model-rate path. Only job ownership differs.
bool NeuralPass::RecordVulkanBackground(ID3D12Device* device, ID3D12GraphicsCommandList*& list,
    std::size_t slot, const NeuralOptions& options, bool reset, bool depthInverted, float scaleX, float scaleY,
    ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* hudless, std::array<float,2> jitter,
    std::optional<std::uint32_t> applicationFrame, Interop* interop, ID3D12CommandQueue* queue)
{
    if(!interop||!queue){status_="Vulkan background submission context missing";return false;}
    auto& bg=background_;
    const auto frame=++bg.frame;
    reset |= rateContinuity_.Record(rateClock_(),applicationFrame);
    reset |= !SameModelOutputOptions(options,rateOptions_);
    rateOptions_=options;
    if(reset){bg.schedule.Invalidate();bg.forceReset=true;}
    bool ready{};
    if(!feature_.PollVulkan(slot,queue,ready)){status_=feature_.Status();return false;}
    const auto completion=bg.schedule.Complete(frame,ready);
    if(completion.dropped){if(!feature_.DropVulkan(queue)){status_=feature_.Status();return false;}bg.forceReset=true;}
    if(!EnsureModelRate(device,motion,hudless))return false;
    // The ordinary cadence path already owns two guide textures; extend that
    // set by one instead of allocating five full-resolution guides.
    if(!bg.history[0].guides){bg.history[0].guides=rateGuides_[0];bg.history[1].guides=rateGuides_[1];}
    auto allocate=[&](ID3D12Resource* like,Microsoft::WRL::ComPtr<ID3D12Resource>& out,DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN){
        if(out)return true;
        auto desc=like->GetDesc();if(format!=DXGI_FORMAT_UNKNOWN)desc.Format=format;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)))){
            status_="Vulkan background history allocation failed";return false;
        }
        return true;
    };
    auto copy=[&](ID3D12Resource* from,ID3D12Resource* to){
        if(FAILED(Interop::RecordCopy(list,from,to))){status_="Vulkan background history copy rejected";return false;}return true;
    };
    auto& current=bg.history[frame%bg.history.size()];
    if(!allocate(hudless,current.colour)||!allocate(motion,current.guides,DXGI_FORMAT_R16G16B16A16_FLOAT))return false;
    current.serial=frame;current.jitter=jitter;
    for(auto& value:current.jitter)if(!std::isfinite(value))value=0;
    const auto reconstruction=NeuralRendering::SanitizeReconstruction(options.reconstruction);
    const auto method=NeuralRendering::EffectiveResolve(reconstruction);
    const bool resolving=method!=NeuralRendering::ResolveMethod::Auto;
    auto* output=resolving?workOutput_.Get():corrected_.Get();
    ResolveConstants c;
    c.sourceWidth=UINT(hudless->GetDesc().Width);c.sourceHeight=hudless->GetDesc().Height;
    c.targetWidth=c.workWidth=UINT(output->GetDesc().Width);c.targetHeight=c.workHeight=output->GetDesc().Height;
    c.guideWidth=UINT(motion->GetDesc().Width);c.guideHeight=motion->GetDesc().Height;
    c.motionScaleX=scaleX;c.motionScaleY=scaleY;c.depthInverted=depthInverted;
    c.cameraJitterX=current.jitter[0];c.cameraJitterY=current.jitter[1];
    c.passthrough=!reconstruction.colorIsHDR;c.producerColor=reconstruction.producerColor;
    c.transferStrength=reconstruction.transferStrength;c.colourStrength=reconstruction.colourStrength;
    c.maxRatio=reconstruction.maxRatio;c.whitePoint=reconstruction.whitePoint;
    c.encodedFormat=hudless->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT?1:2;
    c.changeFloor=reconstruction.producerColor?0.02f*c.whitePoint:reconstruction.colorIsHDR?0.002f*c.whitePoint:0.02f;
    unsigned stage{}; // Each recording owns a distinct descriptor range, even for repeated reprojection.
    auto dispatch=[&](ResolveKernel kernel,ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* original,ID3D12Resource* out,ID3D12Resource* fourth=nullptr){
        const auto hr=resolve_.Record(device,list,slot,stage++,kernel,c,a,b,original,out,nullptr,fourth);
        if(FAILED(hr)){status_=std::format("Vulkan background reconstruction failed 0x{:08X}",UINT(hr));return false;}return true;
    };
    const bool timing=timestampHeap_&&mappedTimestamps_&&timestampFrequency_;
    const UINT query=UINT(slot*4);
    if(timing)list->EndQuery(timestampHeap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+2);
    if(!copy(hudless,current.colour.Get())||!dispatch(ResolveKernel::CaptureGuides,motion,depth,nullptr,current.guides.Get()))return false;

    // A finished result belongs to its capture colour/exposure, never this frame's.
    // Reconstruct there, store its change, then walk the existing one-frame
    // reprojection through every retained guide pair to the current frame.
    std::uint64_t from=frame-1;
    if(completion.consume){
        const auto job=*completion.consume;
        auto& capture=bg.history[job.frame%bg.history.size()];
        if(!bg.schedule.Valid(job,frame)||capture.serial!=job.frame){status_="Vulkan background capture history unavailable";return false;}
        NeuralRendering::FeatureSession::EvaluationInput input;
        input.color=bg.modelInput.Get();input.output=output;input.motionVectors=motion;input.depth=depth;
        if(!feature_.ConsumeVulkan(input,queue,slot,list)){status_=feature_.Status();return false;}
        const auto currentConstants=c;c=bg.captured;
        resolve_.SetExposure(c.adaptiveExposure?bg.exposure.Get():nullptr);
        if(resolving){
            if(method==NeuralRendering::ResolveMethod::Residual){
                c.mode=0;
                if(c.workWidth<c.sourceWidth&&c.workHeight<c.sourceHeight){
                    c.mode=2;
                    if(!dispatch(ResolveKernel::Residual,bg.modelInput.Get(),output,capture.colour.Get(),corrected_.Get()))return false;
                }else{
                    if(!dispatch(ResolveKernel::Residual,bg.modelInput.Get(),output,nullptr,residual_.Get()))return false;
                    c.mode=1;
                    if(!dispatch(ResolveKernel::Residual,capture.colour.Get(),residual_.Get(),nullptr,corrected_.Get()))return false;
                }
            }else{
                c.broad=reconstruction.broadStrength;c.detail=reconstruction.detailStrength;
                c.toneBands=!reconstruction.producerColor&&reconstruction.ToneBands();
                if(c.toneBands){
                    if(!allocate(bg.modelInput.Get(),toneData_,DXGI_FORMAT_R16G16_FLOAT)||!allocate(bg.modelInput.Get(),toneScratch_,DXGI_FORMAT_R16G16_FLOAT))return false;
                    c.mode=0;c.targetWidth=c.workWidth;c.targetHeight=c.workHeight;
                    if(!dispatch(ResolveKernel::PrepareTone,bg.modelInput.Get(),output,nullptr,toneData_.Get())||
                       !dispatch(ResolveKernel::FilterTone,toneData_.Get(),nullptr,nullptr,toneScratch_.Get()))return false;
                    c.mode=1;
                    if(!dispatch(ResolveKernel::FilterTone,toneScratch_.Get(),nullptr,nullptr,toneData_.Get()))return false;
                    resolve_.SetTone(toneData_.Get());
                }
                c.mode=1;c.targetWidth=c.sourceWidth;c.targetHeight=c.sourceHeight;
                c.jitterX=capture.jitter[0];c.jitterY=capture.jitter[1];
                if(!dispatch(ResolveKernel::Ratio,bg.modelInput.Get(),output,capture.colour.Get(),corrected_.Get()))return false;
                resolve_.SetTone(nullptr);
            }
        }
        c=currentConstants;
        if(!dispatch(ResolveKernel::StoreChange,capture.colour.Get(),corrected_.Get(),nullptr,rateChange_.Get()))return false;
        bg.schedule.Applied();from=job.frame;
    }
    if(bg.schedule.HasHistory()){
        for(auto next=from+1;next<=frame;++next){
            auto& previous=bg.history[(next-1)%bg.history.size()];auto& target=bg.history[next%bg.history.size()];
            if(previous.serial!=next-1||target.serial!=next){status_="Vulkan reprojection history discontinuity";return false;}
            c.cameraJitterX=target.jitter[0];c.cameraJitterY=target.jitter[1];c.previousJitterX=previous.jitter[0];c.previousJitterY=previous.jitter[1];
            if(!dispatch(ResolveKernel::Reproject,target.colour.Get(),rateChange_.Get(),target.guides.Get(),corrected_.Get(),previous.guides.Get())||
               !dispatch(ResolveKernel::StoreChange,target.colour.Get(),corrected_.Get(),nullptr,rateChange_.Get()))return false;
        }
    }else if(!copy(hudless,corrected_.Get()))return false; // Identical first/reset-frame fallback.

    // Prepare every frame, including frames with an outstanding job. This keeps
    // the host preparation cadence uniform; the immutable submitted copy is separate.
    c.mode=0;c.targetWidth=c.workWidth;c.targetHeight=c.workHeight;
    c.cameraJitterX=current.jitter[0];c.cameraJitterY=current.jitter[1];
    c.adaptiveExposure=resolving&&reconstruction.adaptiveExposure&&reconstruction.colorIsHDR&&!reconstruction.producerColor;
    c.exposureBias=reconstruction.exposureBias;
    if(c.adaptiveExposure){
        if(!exposure_){
            auto desc=hudless->GetDesc();desc.Width=desc.Height=1;desc.Format=DXGI_FORMAT_R32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&exposure_)))){status_="Vulkan exposure allocation failed";return false;}
        }
        const auto now=std::chrono::steady_clock::now();
        c.exposureRate=1-std::exp2(-std::clamp(std::chrono::duration<float>(now-exposureTime_).count(),0.0f,0.25f)/0.25f);
        c.exposureReset=reset||!exposureValid_;resolve_.SetExposure(exposure_.Get());
        if(!dispatch(ResolveKernel::MeasureExposure,hudless,nullptr,nullptr,exposure_.Get()))return false;
        exposureTime_=now;exposureValid_=true;c.exposureReset=false;
    }else{resolve_.SetExposure(nullptr);exposureValid_=false;}
    auto* colour=hudless;
    if(resolving){
        const bool steady=method==NeuralRendering::ResolveMethod::Ratio;
        if(encoded_){
            c.targetWidth=c.sourceWidth;c.targetHeight=c.sourceHeight;c.jitterX=steady?current.jitter[0]:0;c.jitterY=steady?current.jitter[1]:0;
            if(!dispatch(ResolveKernel::Ratio,hudless,nullptr,nullptr,encoded_.Get()))return false;
            colour=encoded_.Get();c.jitterX=c.jitterY=0;
        }
        if(workColor_){
            c.targetWidth=c.workWidth;c.targetHeight=c.workHeight;
            c.jitterX=steady&&colour==hudless?current.jitter[0]:0;c.jitterY=steady&&colour==hudless?current.jitter[1]:0;
            if(!dispatch(fusedColor_?ResolveKernel::PrepareColor:ResolveKernel::Downsample,colour,nullptr,nullptr,workColor_.Get()))return false;
            colour=workColor_.Get();c.jitterX=c.jitterY=0;
        }
    }
    const auto interval=NeuralRendering::ModelRateInterval(NeuralRendering::EffectiveModelRate(reconstruction.modelRate));
    const bool submit=bg.schedule.CanSubmit(frame,interval);
    if(submit){
        if(!allocate(colour,bg.modelInput)||!copy(colour,bg.modelInput.Get()))return false;
        if(c.adaptiveExposure&&(!allocate(exposure_.Get(),bg.exposure)||!copy(exposure_.Get(),bg.exposure.Get())))return false;
        bg.captured=c;
        NeuralRendering::FeatureSession::EvaluationInput input;
        input.color=bg.modelInput.Get();input.output=output;input.motionVectors=motion;input.depth=depth;
        const auto gap=frame-bg.lastCapture;
        input.reset=bg.forceReset||gap>2;
        if(!input.reset&&gap==2){
            auto& previous=bg.history[(frame-1)%bg.history.size()];
            c.previousJitterX=previous.jitter[0];c.previousJitterY=previous.jitter[1];
            if(!dispatch(ResolveKernel::ComposeMotion,motion,current.guides.Get(),previous.guides.Get(),rateMotion_.Get()))return false;
            input.motionVectors=rateMotion_.Get();
        }
        input.motionVectorScaleX=scaleX*float(c.workWidth)/c.sourceWidth;
        input.motionVectorScaleY=scaleY*float(c.workWidth)/c.sourceWidth;
        input.depthInverted=depthInverted;input.tuning=options.tuning;input.commandList=list;
        if(!feature_.EvaluateVulkan(input,*interop,queue,slot,list,true)){status_=feature_.Status();return false;}
        if(!bg.schedule.Submit(frame,interval)){status_="Vulkan scheduler reservation failed";return false;}
        bg.lastCapture=frame;bg.forceReset=false;
    }
    // Compare never contaminates stored NR change.
    if(options.compare.split){
        if(!allocate(corrected_.Get(),splitResult_))return false;
        c.targetWidth=c.sourceWidth;c.targetHeight=c.sourceHeight;
        c.splitX=NeuralRendering::SplitColumn(options.compare,c.sourceWidth);
        if(!dispatch(ResolveKernel::Split,hudless,corrected_.Get(),nullptr,splitResult_.Get())||!copy(splitResult_.Get(),corrected_.Get()))return false;
    }
    telemetry_.RecordModelRun(submit);modelActivity_=submit?NRModelActivity::Ran:NRModelActivity::Skipped;
    if(timing){
        list->EndQuery(timestampHeap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+3);
        list->ResolveQueryData(timestampHeap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+2,2,timestampReadback_.Get(),sizeof(std::uint64_t)*(query+2));
        pendingTiming_[slot]={feature_.EvaluationsRecorded(),0,true,false};
    }
    status_="Vulkan background; one job maximum; result reprojected from capture; experimental pacing";
    if(frame%120==0){
        const auto measured=feature_.VulkanMeasurements();
        logger::info("[Vulkan NR] background frame={} resultAge={} arrivalAge={} inFlight={} dropped/stale={} network={:.3f} ms copies/frame={:.3f} ms",
            frame,bg.schedule.DisplayedAge(frame),bg.schedule.ResultAge(),bg.schedule.InFlight(),bg.schedule.Dropped(),measured.NetworkMs(),measured.CopiesPerFrameMs());
    }
    return true;
}
