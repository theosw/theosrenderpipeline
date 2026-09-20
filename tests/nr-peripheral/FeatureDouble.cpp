#include "FrameGen/NeuralRenderingFeatureSession.h"
#include "FeatureDouble.h"
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace TheosRenderPipeline::NeuralRendering {
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"FEATURE DOUBLE FAIL: %s\n",why); std::exit(1); }
}
void Extent(ID3D12Resource* r, unsigned w, unsigned h, const char* why) {
    Require(r && r->GetDesc().Width==w && r->GetDesc().Height==h, why);
}
}
struct FeatureSession::State { bool initialized{}; std::uint64_t evaluations{}; std::string status{"scripted identity NR"}; };
FeatureSession::FeatureSession() : state_(new State) {}
FeatureSession::~FeatureSession() = default;
void FeatureSession::StateDeleter::operator()(State* s) const { delete s; }
bool FeatureSession::EnsureInitialized(const CreateInfo& info) {
    Require(info.displayWidth==Fixture::workWidth && info.displayHeight==Fixture::workHeight, "model extent");
    Require(info.renderWidth==(Fixture::peripheral?Fixture::workWidth:Fixture::guideWidth) &&
        info.renderHeight==(Fixture::peripheral?Fixture::workHeight:Fixture::guideHeight), "creation guide extent");
    if (!state_->initialized) { ++Fixture::creations; state_->initialized=true; }
    return true;
}
bool FeatureSession::RecordEvaluation(const EvaluationInput& in) {
    Require(state_->initialized && in.commandList && in.backbuffer==in.output, "modern feature input contract");
    Extent(in.color, Fixture::workWidth, Fixture::workHeight,"colour extent");
    Extent(in.output, Fixture::workWidth, Fixture::workHeight,"output extent");
    const auto gw=Fixture::peripheral?Fixture::workWidth:Fixture::guideWidth;
    const auto gh=Fixture::peripheral?Fixture::workHeight:Fixture::guideHeight;
    Extent(in.motionVectors,gw,gh,"motion extent"); Extent(in.depth,gw,gh,"depth extent");
    if (Fixture::worldOnly) Require(!in.ui && !in.tuning.uiCorrection,"world-only UI contract");
    else Extent(in.ui,Fixture::peripheral?Fixture::workWidth:Fixture::sourceWidth,
        Fixture::peripheral?Fixture::workHeight:Fixture::sourceHeight,"UI correction coordinates");
    if (Fixture::peripheral) Require(in.motionVectorScaleX==1 && in.motionVectorScaleY==1,"packed vectors already in model pixels");
    Require(in.color!=in.output,"two-pass output must not alias input");
    Require(state_->evaluations || in.reset,"new feature history reset");
    D3D12_RESOURCE_BARRIER b[2]{};
    for (auto& v:b) { v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    b[0].Transition.pResource=in.color;
    b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource=in.output;
    b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    in.commandList->ResourceBarrier(2,b); in.commandList->CopyResource(in.output,in.color);
    for (auto& v:b) std::swap(v.Transition.StateBefore,v.Transition.StateAfter);
    in.commandList->ResourceBarrier(2,b);
    ++state_->evaluations; ++Fixture::evaluations; Fixture::resets+=in.reset;
    return true;
}
bool FeatureSession::IsInitialized() const {return state_->initialized;}
RuntimeBuild FeatureSession::Build() const {return RuntimeBuild::Nexus3108;}
std::uint64_t FeatureSession::EvaluationsRecorded() const {return state_->evaluations;}
const std::string& FeatureSession::Status() const {return state_->status;}
}
