#include "ReplayScene.h"
int main(){
    GPU gpu;const unsigned w=130,h=96; // Non-aligned rows exercise pitched readback.
    auto color=gpu.Texture(w,h,{},DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=gpu.Texture(w,h,{},DXGI_FORMAT_R32G32_FLOAT),depth=gpu.Texture(w,h,{},DXGI_FORMAT_R32_FLOAT);
    ReplayScene scene(gpu.device.Get(),color.Get(),motion.Get(),depth.Get());
    auto record=[&](float t,float p,unsigned s){gpu.Begin();scene.Record(gpu,color.Get(),motion.Get(),depth.Get(),t,p,s);gpu.End();};
    record(.5f,.4f,0);auto m=gpu.Read(motion.Get());auto d=gpu.Read(depth.Get());auto c=gpu.Read(color.Get());
    const auto edge=4*w+120;Near(m[edge][0],.022*h/w,1e-6,"background current-to-previous normalized camera motion");
    Near(d[edge][0],.25,1e-6,"reversed far depth");
    // The same world point follows this vector, with colour stable to sampling tolerance.
    record(.4f,.4f,0);auto previous=gpu.Read(color.Get());
    const unsigned oldX=unsigned(std::round(120+.022*h));
    Near(c[edge][0],previous[4*w+oldX][0],.003,"world-anchored colour follows guide");
    record(.5f,.4f,1);m=gpu.Read(motion.Get());d=gpu.Read(depth.Get());
    const unsigned objectX=unsigned((.30+.65*.5-.22*.5)*h),objectY=h/2;
    Near(d[objectY*w+objectX][0],.75,1e-6,"moving foreground near depth");
    Near(m[objectY*w+objectX][0],(.022-.065)*h/w,1e-6,"foreground motion includes object and camera");
    record(.5f,.5f,2);m=gpu.Read(motion.Get());d=gpu.Read(depth.Get());unsigned thin=0;
    for(unsigned i=0;i<m.size();++i){Near(m[i][0],0,1e-6,"cut guides have no history");thin+=d[i][0]>.5;}
    Require(thin>0&&thin<w*h/4,"thin geometry present");
    record(.5f,.4f,3);c=gpu.Read(color.Get());record(.7f,.6f,3);auto lit=gpu.Read(color.Get());
    double darkMean=0,litMean=0;
    // Average an unoccluded strip: a single moving fine edge can legitimately
    // outweigh the exposure step at one screen-fixed sample.
    for(unsigned i=0;i<8*w;++i){darkMean+=c[i][0];litMean+=lit[i][0];}
    Require(litMean>darkMean*2.2,"exposure change reaches unoccluded background");
    ReplayCapture capture(gpu,color.Get());gpu.Begin();capture.Record(gpu,color.Get());gpu.End();
    const auto path=std::filesystem::current_path()/("replay-fixture-"+std::to_string(GetCurrentProcessId())+".rgba16f");
    capture.Write(path);
    Require(std::filesystem::file_size(path)==w*h*8,"capture strips row padding");
    {
        std::ifstream file(path,std::ios::binary);
        std::vector<DirectX::PackedVector::HALF> raw(w*h*4);
        file.read(reinterpret_cast<char*>(raw.data()),std::streamsize(raw.size()*2));Require(bool(file),"capture byte read");
        for(unsigned i=0;i<lit.size();++i)for(unsigned channel=0;channel<4;++channel)
            Near(DirectX::PackedVector::XMConvertHalfToFloat(raw[i*4+channel]),lit[i][channel],0,"same-submission capture matches retired scene pixels");
    }
    std::filesystem::remove(path);
    std::puts("PASS replay camera/object guides, cuts, thin geometry, exposure and pitched capture pixels");
}
