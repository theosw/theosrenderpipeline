#include "GpuFixture.h"
#include "FrameGen/SourceDLSSGNeuralResolve.h"
#include <cmath>
int main(){
    using namespace TheosRenderPipeline::SourceDLSSG;GPU gpu;NeuralResolveKernels kernels;
    Check(kernels.Initialize(gpu.device.Get()),"kernels");
    auto a=gpu.Texture(19,13,{0.25f,0.5f,0.75f,1}),b=gpu.Texture(19,13,{0.5f,0.25f,0.125f,1});
    auto output=gpu.Texture(19,13,{0,0,0,0});ResolveConstants constants;
    constants.sourceWidth=constants.targetWidth=19;constants.sourceHeight=constants.targetHeight=13;
    for(unsigned frame=0;frame<12;++frame)for(unsigned stage=0;stage<4;++stage){
        auto* input=frame<6?a.Get():b.Get();
        gpu.Begin();Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,stage,
            ResolveKernel::Downsample,constants,input,nullptr,nullptr,output.Get()),"record");gpu.End();
        const Pixel expected=frame<6?Pixel{0.25f,0.5f,0.75f,1}:Pixel{0.5f,0.25f,0.125f,1};
        for(const auto& p:gpu.Read(output.Get()))for(unsigned c=0;c<4;++c)Require(std::abs(p[c]-expected[c])<0.00001f,"cached/replaced table pixels");
        gpu.CheckDebug();
    }
    // Replacing output dimensions must invalidate the cached dispatch extent.
    auto resized=gpu.Texture(11,7,{0,0,0,0});constants.targetWidth=11;constants.targetHeight=7;
    gpu.Begin();Check(kernels.Record(gpu.device.Get(),gpu.list.Get(),0,0,ResolveKernel::Downsample,constants,b.Get(),nullptr,nullptr,resized.Get()),"resized record");gpu.End();
    for(const auto& p:gpu.Read(resized.Get()))Require(std::abs(p[0]-0.5f)<0.00001f&&p[3]==1,"resized pixels");
    gpu.CheckDebug();std::puts("resolve descriptors PASS: unchanged tables, input/output replacement, all slots/stages, COMMON end states");
}
