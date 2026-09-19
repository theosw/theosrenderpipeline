// Deliberate test double: executes a known color adjustment on a real D3D12
// command list. This does not load or validate an NVIDIA inference runtime.
#include "FrameGen/NeuralRenderingFeatureSession.h"
#include "GpuFixture.h"
#include <d3dcompiler.h>
namespace TheosRenderPipeline::NeuralRendering {
struct FeatureSession::State {
    bool initialized{};std::uint64_t evaluations{};std::string status{"test feature"};
    ComPtr<ID3D12Device> device;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12DescriptorHeap> heap;
};
FeatureSession::FeatureSession():state_(new State){}
FeatureSession::~FeatureSession()=default;
void FeatureSession::StateDeleter::operator()(State* state)const{delete state;}
bool FeatureSession::EnsureInitialized(const CreateInfo& info){
    if(state_->initialized)return true;state_->device=info.device;
    D3D12_DESCRIPTOR_RANGE range[]{ {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1} };
    D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={2,range};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,1};
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=parameters;ComPtr<ID3DBlob> signature,shader;
    Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,nullptr),"test root");
    Check(info.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&state_->root)),"test root create");
    constexpr char source[]=R"(
cbuffer Options : register(b0) { uint VerifyBackground; };
Texture2D<float4> Color : register(t0); RWTexture2D<float4> Output : register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h;Output.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
float4 c=Color.Load(int3(p.xy,0));bool ok=!VerifyBackground||all(abs(Output[p.xy]-c)<0.0001);
Output[p.xy]=float4(c.rgb+0.03125,ok?c.a:-1);}
)";
    Check(D3DCompile(source,sizeof(source)-1,"test feature",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,nullptr),"test shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=state_->root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    Check(info.device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&state_->pipeline)),"test pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    Check(info.device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&state_->heap)),"test heap");state_->initialized=true;return true;
}
bool FeatureSession::RecordEvaluation(const EvaluationInput& input){
    Require(input.color!=input.output,"separate temporal input/output");
    Require(input.backbuffer==input.output,"reconstruction background aliases output");
    auto cpu=state_->heap->GetCPUDescriptorHandleForHeapStart();const auto stride=state_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=input.color->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    state_->device->CreateShaderResourceView(input.color,&srv,cpu);cpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=input.output->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    state_->device->CreateUnorderedAccessView(input.output,nullptr,&uav,cpu);auto* heap=state_->heap.Get();
    input.commandList->SetDescriptorHeaps(1,&heap);input.commandList->SetComputeRootSignature(state_->root.Get());input.commandList->SetPipelineState(state_->pipeline.Get());
    input.commandList->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());input.commandList->SetComputeRoot32BitConstant(1,input.tuning.uiCorrection,0);
    const auto d=input.output->GetDesc();input.commandList->Dispatch((UINT(d.Width)+7)/8,(d.Height+7)/8,1);++state_->evaluations;return true;
}
bool FeatureSession::IsInitialized()const{return state_->initialized;}
RuntimeBuild FeatureSession::Build()const{return RuntimeBuild::Build14;}
std::uint64_t FeatureSession::EvaluationsRecorded()const{return state_->evaluations;}
const std::string& FeatureSession::Status()const{return state_->status;}
}
