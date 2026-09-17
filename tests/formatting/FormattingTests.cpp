#include "FrameGen/NeuralRenderingSubrect.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>
#include <vector>
struct Parameters {
    std::vector<std::pair<std::string,unsigned>> writes;
    void Set(const char* key,unsigned value) { writes.emplace_back(key,value); }
};
static void Require(bool ok) { if(!ok) { std::fputs("NR parameter check failed\n",stderr);std::exit(1); } }
int main() {
    using namespace TheosRenderPipeline::NeuralRendering;
    const std::array keys{Subrect::Color,Subrect::MVec,Subrect::Depth,Subrect::Output,
        Subrect::Backbuffer,Subrect::ControlMask,Subrect::UI,Subrect::UIAlpha,Subrect::BidirectionalDistortionField};
    const std::array names{"Color","MVec","Depth","Output","Backbuffer","ControlMask","UI","UIAlpha","BidirectionalDistortionField"};
    const std::array suffixes{"BaseX","BaseY","Width","Height"};
    Parameters parameters;
    for(unsigned i=0;i<keys.size();++i) for(unsigned round=0;round<2;++round) {
        const unsigned w=round?0:3413+i,h=round?0:960+i;
        parameters.writes.clear();SetSubrect(&parameters,keys[i],w,h);
        Require(parameters.writes.size()==4);
        const unsigned expected[]{0,0,w,h};
        for(unsigned k=0;k<4;++k) {
            Require(parameters.writes[k].first==std::format("DLSSNR.{}Subrect{}",names[i],suffixes[k]));
            Require(parameters.writes[k].second==expected[k]);
        }
    }
    std::puts("all nine NR subrect contracts, repeated zero/nonzero writes PASS");
}
