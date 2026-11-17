#include "FrameGen/VulkanNRSchedule.h"
#include "FrameGen/SourceDLSSGNeuralTelemetry.h"
#include <iostream>
#include <stdexcept>
using namespace TheosRenderPipeline::NeuralRendering;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
    VulkanNRSchedule s;
    require(s.Submit(1,2),"first capture");
    require(!s.Complete(1,true).consume,"never consume on capture frame");
    require(!s.Complete(2,false).consume&&s.InFlight()==1,"unfinished job keeps reservation");
    require(!s.Submit(2,2),"bounded in-flight");
    auto result=s.Complete(2,true);
    require(result.consume&&s.Valid(*result.consume,2)&&s.ResultAge()==1,"one-frame result age");
    s.Applied();require(s.HasHistory(),"accepted history");
    require(!s.CanSubmit(2,2)&&s.Submit(3,2),"model-rate spacing");
    result=s.Complete(5,true);require(result.consume&&s.ResultAge()==2,"two-frame result age");
    const auto old=*result.consume;s.Invalidate();
    require(!s.HasHistory()&&!s.Valid(old,5),"reset invalidates already polled result");
    require(s.Submit(6,2),"new generation");s.Invalidate();
    require(s.InFlight()==1&&!s.CanSubmit(8,2),"reset cannot recycle in-flight resources");
    result=s.Complete(7,true);require(result.dropped&&!result.consume&&s.InFlight()==0,"stale generation discarded after completion");
    require(s.Submit(8,2),"resubmit after stale completion");
    result=s.Complete(11,true);require(result.dropped&&!result.consume&&s.Dropped()==2,"too-old result dropped");
    require(s.Submit(12,1),"retirement job");s.Close();
    require(!s.HasHistory()&&!s.CanSubmit(13,1)&&s.InFlight()==1,"retirement retains pending owner");
    result=s.Complete(13,true);require(result.dropped&&!result.consume,"no use after retire");
    s.Drained();require(!s.CanSubmit(14,1)&&!s.Valid(old,5),"drain never reopens a retired owner");
    // Long, variably delayed completion stream: every consumed job has retained guides.
    VulkanNRSchedule stream;unsigned jobs=0,accepted=0;
    for(unsigned f=1;f<1000;++f){
        if(f%37==0)stream.Invalidate();
        const auto r=stream.Complete(f,f%5==1||f%5==3);
        if(r.consume){require(stream.Valid(*r.consume,f)&&f-r.consume->frame<=2,"retained history bounds");++accepted;}
        if(stream.Submit(f,2))++jobs;
        require(stream.InFlight()<=VulkanNRSchedule::MaxInFlight,"bounded throughout variable latency");
    }
    require(jobs>100&&accepted>100,"stream exercised completion and backpressure");
    require(SanitizeVulkanScheduling(-1)==VulkanScheduling::SameFrame&&SanitizeVulkanScheduling(1)==VulkanScheduling::Background&&
        SanitizeVulkanScheduling(2)==VulkanScheduling::SameFrame,"invalid saved scheduling is conservative");
    VulkanNRMeasurements m{2,4,40,.8};require(m.NetworkMs()==20&&m.NetworkPerFrameMs()==10&&m.CopiesPerFrameMs()==.2,"separate timing domains");
    TheosRenderPipeline::SourceDLSSG::NeuralTelemetrySnapshot t;
    require(!t.HasInference()&&!t.HasFrame(),"unmeasured telemetry unavailable");
    t.vulkanInferenceMs=20;t.vulkanFrameMs=10.2;
    require(t.HasInference()&&t.HasFrame()&&t.AverageGPUMicroseconds()==20000&&t.AverageFrameGPUMicroseconds()==10200,"Vulkan timings without D3D12 inference queries");
    std::cout<<"PASS Vulkan scheduler: age, reset, retirement, backpressure, timing domains\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
