#include "GpuFixture.h"
#include "FrameGen/SourceDLSSGNeuralRendering.h"
#include <cmath>
int main(){
    using namespace TheosRenderPipeline::SourceDLSSG;
    using TheosRenderPipeline::NeuralRendering::ResolveMethod;
    GPU gpu;unsigned cases=0;
    for(bool before:{false,true})for(int passes:{1,2})for(auto method:{ResolveMethod::Auto,ResolveMethod::Ratio})for(float scale:{1.0f,0.75f}){
        NeuralOptions options;options.enabled=true;options.beforeUpscaling=before;options.passes=passes;
        options.reconstruction.inputScale=scale;options.reconstruction.method=method;options.tuning.uiCorrection=!before;
        auto motion=gpu.Texture(32,17,{0,0,0,0}),depth=gpu.Texture(32,17,{0.5f,0.5f,0.5f,1});
        auto scene=gpu.Texture(64,33,{0.25f,0.25f,0.25f,1}),ui=gpu.Texture(64,33,{0.125f,0.125f,0.125f,0.25f});
        auto composed=gpu.Texture(64,33,{0.25f,0.25f,0.25f,1});
        NeuralPass pass;
        for(unsigned frame=0;frame<4;++frame){
            // After-DLSS replaces the HUD-less input; restore the same test scene
            // before each frame by copying an immutable source in COMMON.
            auto fresh=gpu.Texture(64,33,{0.25f,0.25f,0.25f,1});
            gpu.Begin();Check(Interop::RecordCopy(gpu.list.Get(),fresh.Get(),scene.Get()),"reset scene");
            Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0,false,1,1,
                motion.Get(),depth.Get(),before?nullptr:ui.Get(),scene.Get(),before?nullptr:composed.Get()),"NR recording");
            gpu.End();const float expected=0.25f+passes*0.03125f;
            for(const auto& pixel:gpu.Read(pass.Corrected())){
                for(unsigned c=0;c<3;++c)Require(std::abs(pixel[c]-expected)<0.001f,"final result includes every NR pass");
                Require(pixel[3]==1,"background initialization and alpha");}
            if(!before){for(const auto& pixel:gpu.Read(pass.Composed())){
                for(unsigned c=0;c<3;++c)Require(std::abs(pixel[c]-(expected*0.75f+0.125f))<0.001f,"UI composed once after final result");
                Require(pixel[3]==1,"composed alpha");}}
            // Readback assumes COMMON and the debug layer validates end states.
            (void)gpu.Read(motion.Get());(void)gpu.Read(depth.Get());(void)gpu.Read(scene.Get());
            gpu.CheckDebug();++cases;
        }
    }
    std::printf("NR pipeline PASS: %u cases; real D3D12/WARP, deterministic feature double, both placements/passes/scales/resolves/slot reuse\n",cases);
}
