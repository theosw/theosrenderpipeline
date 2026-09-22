#include "FrameGen/NeuralRenderingFeatureSession.h"
#include "FeatureDouble.h"
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <cmath>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace TheosRenderPipeline::NeuralRendering {
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"FEATURE DOUBLE FAIL: %s\n",why); std::exit(1); }
}
void Extent(ID3D12Resource* r, unsigned w, unsigned h, const char* why) {
    Require(r && r->GetDesc().Width==w && r->GetDesc().Height==h, why);
}
}
struct FeatureSession::State {
    unsigned w{},h{},gw{},gh{},index{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    bool initialized{}; std::uint64_t evaluations{}; std::string status{"scripted identity NR"}; };
FeatureSession::FeatureSession() : state_(new State) {}
FeatureSession::~FeatureSession() = default;
void FeatureSession::StateDeleter::operator()(State* s) const { delete s; }
bool FeatureSession::EnsureInitialized(const CreateInfo& info) {
    if (!state_->initialized) {
        state_->index=Fixture::creations;
        state_->w=state_->index && Fixture::secondWidth?Fixture::secondWidth:Fixture::workWidth;
        state_->h=state_->index && Fixture::secondHeight?Fixture::secondHeight:Fixture::workHeight;
        state_->gw=Fixture::peripheral?state_->w:Fixture::guideWidth;
        state_->gh=Fixture::peripheral?state_->h:Fixture::guideHeight;
        if(Fixture::transform) Require(info.networkPreset==(state_->index?1:-1),"per-pass preset");
        ++Fixture::creations; state_->initialized=true;
    }
    Require(info.displayWidth==state_->w && info.displayHeight==state_->h,"model extent");
    Require(info.renderWidth==state_->gw && info.renderHeight==state_->gh,"creation guide extent");
    return true;
}
bool FeatureSession::RecordEvaluation(const EvaluationInput& in) {
    Require(state_->initialized && in.commandList && in.backbuffer==in.output, "modern feature input contract");
    Extent(in.color, state_->w, state_->h,"colour extent");
    Extent(in.output, state_->w, state_->h,"output extent");
    const auto gw=state_->gw;
    const auto gh=state_->gh;
    Extent(in.motionVectors,gw,gh,"motion extent"); Extent(in.depth,gw,gh,"depth extent");
    if (Fixture::worldOnly) Require(!in.ui && !in.tuning.uiCorrection,"world-only UI contract");
    else Extent(in.ui,Fixture::peripheral?state_->w:Fixture::sourceWidth,
        Fixture::peripheral?state_->h:Fixture::sourceHeight,"UI correction coordinates");
    if (Fixture::peripheral) Require(in.motionVectorScaleX==1 && in.motionVectorScaleY==1,"packed vectors already in model pixels");
    if(!Fixture::peripheral) Require(std::abs(in.motionVectorScaleX-float(Fixture::guideWidth)*state_->w/Fixture::sourceWidth)<.0001f,"uniform motion scale follows each model");
    Require(in.color!=in.output,"two-pass output must not alias input");
    Require(state_->evaluations || in.reset,"new feature history reset");
    if(Fixture::transform) {
        Require(in.tuning.intensity==(state_->index?.6f:.8f),"independent tuning reaches each evaluation");
        if(!state_->root) {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            Require(SUCCEEDED(in.output->GetDevice(IID_PPV_ARGS(&device))),"fixture device");
            D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,2};
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
            Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
            Require(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)),"fixture root serialize");
            Require(SUCCEEDED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&state_->root))),"fixture root");
            const char shader[]=R"(Texture2D<float4> a:register(t0); RWTexture2D<float4> b:register(u0);
            cbuffer P:register(b0){float gain,bias;}
            [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h;b.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
            float4 c=a.Load(int3(p.xy,0));b[p.xy]=float4(c.rgb*gain+bias,c.a);})";
            Require(SUCCEEDED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&blob,&error)),"fixture shader");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=state_->root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            Require(SUCCEEDED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&state_->pipeline))),"fixture pipeline");
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Require(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&state_->heap))),"fixture heap");
        }
        Microsoft::WRL::ComPtr<ID3D12Device> device;in.output->GetDevice(IID_PPV_ARGS(&device));
        auto cpu=state_->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=in.color->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(in.color,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=in.output->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(in.output,nullptr,&uav,cpu);
        auto* heap=state_->heap.Get();in.commandList->SetDescriptorHeaps(1,&heap);in.commandList->SetComputeRootSignature(state_->root.Get());
        in.commandList->SetPipelineState(state_->pipeline.Get());in.commandList->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        float constants[]{in.tuning.intensity,state_->index?.02f:.01f};in.commandList->SetComputeRoot32BitConstants(1,2,constants,0);
        in.commandList->Dispatch((state_->w+7)/8,(state_->h+7)/8,1);
    } else {
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
    }
    ++state_->evaluations; ++Fixture::evaluations; Fixture::resets+=in.reset;
    return true;
}
bool FeatureSession::IsInitialized() const {return state_->initialized;}
RuntimeBuild FeatureSession::Build() const {return RuntimeBuild::Nexus3108;}
std::uint64_t FeatureSession::EvaluationsRecorded() const {return state_->evaluations;}
const std::string& FeatureSession::Status() const {return state_->status;}
}
