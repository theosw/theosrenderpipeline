#include "FrameGen/VulkanNRHost.h"
#include "FrameGen/NeuralRuntimeSession.h"
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <format>
#include <thread>
#include <stdexcept>
#include <d3d12sdklayers.h>
using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char* text){if(FAILED(hr))throw std::runtime_error(std::format("{} HRESULT=0x{:08X}",text,static_cast<unsigned>(hr)));}
void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&v);
}
#include "FSRNRHandoff.h"
int wmain(int argc,wchar_t** argv){try{
 if(argc==2){
  using namespace NeuralRendering;
  const auto mode=std::wstring_view(argv[1]);
  if(mode!=L"--unchecked" && mode!=L"--hip-owned" && mode!=L"--probe-cache")throw std::runtime_error("unknown probe test");
  if(GetModuleHandleW(L"TRPVulkanNR.dll")||GetModuleHandleW(L"vulkan-1.dll"))throw std::runtime_error("test needs a clean process");
  if(mode==L"--hip-owned"){
   if(runtimeSession.Claim(NeuralRendering::Backend::HIP))throw std::runtime_error("HIP claim failed");
   for(unsigned i=0;i<8;++i)if(std::string_view(VulkanNRHost::Probe(nullptr,true,true).reason)!=kRuntimeRestart)throw std::runtime_error("HIP must refuse explicit probes");
  }else if(mode==L"--unchecked"){
   for(unsigned i=0;i<8;++i)if(std::string_view(VulkanNRHost::Probe(nullptr).reason)!=kVulkanNotChecked)throw std::runtime_error("passive observation must stay unchecked");
   if(runtimeSession.Claim(NeuralRendering::Backend::HIP))throw std::runtime_error("passive observations must not claim Vulkan");
  }else{
   ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
   check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
   check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"WARP device");
   if(std::string_view(VulkanNRHost::Probe(device.Get()).reason)!=kVulkanNotChecked)throw std::runtime_error("device observation initiated probe");
   SetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS",L"Z:/TRP-intentionally-missing-probe-fixture/model.dll");
   const auto first=VulkanNRHost::Probe(device.Get(),true,true);
   SetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS",nullptr);
   const auto again=VulkanNRHost::Probe(device.Get(),true,true);
   if(first.available||!first.reason[0]||std::string_view(first.reason)!=again.reason)throw std::runtime_error("probe failure must be cached once per session");
   std::cout<<"Cached refusal: "<<first.reason<<'\n';
  }
  if(GetModuleHandleW(L"TRPVulkanNR.dll")||GetModuleHandleW(L"vulkan-1.dll"))throw std::runtime_error("observation/HIP/refused prerequisites loaded Vulkan code");
  std::wcout<<L"PASS: "<<mode<<L"; no Vulkan engine or loader loaded\n";return 0;
 }
 if(argc<3||argc>4)throw std::runtime_error("usage: TRPVulkanNRHostSmoke <engine-folder> <user-310.9.16-DLL> [--validation]");
 SetEnvironmentVariableW(L"TRP_VKNR_TEST_ROOT",argv[1]);SetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS",argv[2]);
 const bool validation=argc==4&&std::wstring_view(argv[3])==L"--validation";
 if(argc==4&&!validation)throw std::runtime_error("unknown test option");
 SetEnvironmentVariableW(L"TRP_VKNR_TEST_VALIDATION",validation?L"1":nullptr);
 ComPtr<ID3D12Debug> debug;
 if(validation){check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer unavailable");debug->EnableDebugLayer();}
 ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c11;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11");
 ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(d11.As(&dxgi),"DXGI");check(dxgi->GetAdapter(&adapter),"adapter");
 DXGI_ADAPTER_DESC adapterDesc{};check(adapter->GetDesc(&adapterDesc),"adapter description");
 if(std::wstring_view(adapterDesc.Description).find(L"RTX 4080 SUPER")==std::wstring_view::npos)throw std::runtime_error("smoke needs RTX 4080 SUPER");
 std::wcout<<L"GPU "<<adapterDesc.Description<<L"\n";
 ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)),"D3D12");
 if(std::string_view(NeuralRendering::VulkanNRHost::Probe(d12.Get()).reason)!=NeuralRendering::kVulkanNotChecked || GetModuleHandleW(L"TRPVulkanNR.dll"))
  throw std::runtime_error("Vulkan ran before selection");
 const auto selected=NeuralRendering::VulkanNRHost::Probe(d12.Get(),true,true);
 if(!selected.available)throw std::runtime_error(selected.reason);
 const auto cached=NeuralRendering::VulkanNRHost::Probe(d12.Get());
 if(!cached.available||std::string_view(cached.pathReason)!=selected.pathReason)throw std::runtime_error("selected probe not cached");
 if(!NeuralRendering::runtimeSession.Refusal(NeuralRendering::Backend::HIP))throw std::runtime_error("selected probe must retain ownership");
 std::cout<<"PASS: no engine before selection; explicit probe cached; HIP requires restart\n";
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
 Interop interop;check(interop.Initialize(d11.Get(),d12.Get(),queue.Get()),"interop");
 constexpr UINT w=256,h=128,gw=320,gh=180;
 auto texture=[&](DXGI_FORMAT f,UINT x,UINT y){ComPtr<ID3D12Resource> r;D3D12_RESOURCE_DESC desc{};
 desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=x;desc.Height=y;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=f;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;check(d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"texture");return r;};
 auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type){ComPtr<ID3D12Resource> r;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;check(d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;};
 auto colour=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,w,h),motion=texture(DXGI_FORMAT_R16G16_FLOAT,gw,gh),depth=texture(DXGI_FORMAT_R32_FLOAT,gw,gh),output=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,w,h);
 ID3D12GraphicsCommandList* list{};check(interop.Begin(Work::Upscaling,&list),"initial begin");std::vector<ComPtr<ID3D12Resource>> uploads;
 for(auto* r:{colour.Get(),motion.Get(),depth.Get()}){
  auto desc=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};d12->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
  void* data{};check(upload->Map(0,nullptr,&data),"upload map");memset(data,0,size_t(bytes));
  for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){
   auto* row=static_cast<unsigned char*>(data)+fp.Offset+size_t(y)*fp.Footprint.RowPitch;
   if(r==colour.Get()){auto* p=reinterpret_cast<uint16_t*>(row)+x*4;p[0]=uint16_t(0x3400+(x%32)*8);p[1]=0x3800;p[2]=0x3000;p[3]=0x3c00;}
   if(r==depth.Get())reinterpret_cast<float*>(row)[x]=.5f;
  }
  upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  barrier(list,r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(list,r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);uploads.push_back(upload);
 }
 check(interop.Submit(Work::Upscaling),"initial submit");check(interop.Drain(),"startup drain");uploads.clear();
 NeuralRendering::FeatureSession host;NeuralRendering::FeatureSession::CreateInfo create;
 create.device=d12.Get();create.displayWidth=w;create.displayHeight=h;create.renderWidth=gw;create.renderHeight=gh;
 create.backend=NeuralRendering::Backend::Vulkan; create.runtimePath=NeuralRendering::VulkanNRHost::Path();
 create.allowReconstructionRuntime=true; create.networkPreset=NeuralRendering::RequestedNetworkPreset(0);
 unsigned pending=0;double maxPollMs=0;
 const auto preparationStart=std::chrono::steady_clock::now();
 for(;;){
  const auto before=std::chrono::steady_clock::now();const bool ready=host.EnsureInitialized(create);
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
  maxPollMs=(std::max)(maxPollMs,ms);
  if(ready)break;
  if(!host.Preparing())throw std::runtime_error(host.Status());
  ++pending;if(std::chrono::steady_clock::now()-preparationStart>std::chrono::minutes(3))throw std::runtime_error("preparation timeout");
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 if(!pending||maxPollMs>250)throw std::runtime_error("Vulkan preparation blocked its caller");
 std::cout<<"preparation pending polls="<<pending<<" max caller ms="<<maxPollMs<<"\n";
 FSRHandoff fsr;fsr.Initialize(d11.Get(),d12.Get(),interop,w,h,gw,gh);
 auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};d12->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
 unsigned busy=0;const auto start=std::chrono::steady_clock::now();
 for(unsigned i=0;i<80;++i){
  HRESULT begun;do{begun=interop.Begin(Work::Upscaling,&list,nullptr,true);if(begun==S_FALSE){++busy;std::this_thread::sleep_for(std::chrono::milliseconds(1));}}while(begun==S_FALSE);
  check(begun,"nonblocking begin");const auto slot=interop.CurrentSlot(Work::Upscaling);
  NeuralRendering::FeatureSession::EvaluationInput in;in.commandList=list;in.color=colour.Get();in.output=output.Get();in.motionVectors=motion.Get();in.depth=depth.Get();in.backbuffer=output.Get();in.reset=i==0||i==70;in.motionVectorScaleX=w;in.motionVectorScaleY=h;
  if(!(host.EvaluateVulkan(in,interop,queue.Get(),slot,list))){interop.Discard(Work::Upscaling);interop.Drain();host.ReleaseAfterRetirement();throw std::runtime_error(host.Status());}
  if(i==79){D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
   barrier(list,output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);}
  check(interop.Submit(Work::Upscaling),"consumer submit");
  fsr.Evaluate(c11.Get(),interop,output.Get(),motion.Get(),depth.Get(),i==0||i==70);
 }
 check(interop.Drain(),"final drain");
 fsr.Verify(d11.Get(),c11.Get(),interop);
 if(validation){ComPtr<ID3D12InfoQueue> messages;check(d12.As(&messages),"D3D12 info queue");
  for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());messages->GetMessage(i,message,&bytes);
   if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<"\n";throw std::runtime_error("D3D12 validation error");}}
  std::cout<<"D3D12 validation: errors=0\n";}
 void* data{};check(readback->Map(0,nullptr,&data),"readback map");unsigned finite=0,nonblack=0,changed=0;
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){const auto* p=reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(data)+fp.Offset+size_t(y)*fp.Footprint.RowPitch)+x*4;
  for(unsigned c=0;c<3;++c){finite+=(p[c]&0x7c00)!=0x7c00;nonblack+=(p[c]&0x7fff)!=0;}changed+=p[0]!=uint16_t(0x3400+(x%32)*8);}
 readback->Unmap(0,nullptr);
 if(!host.ReleaseAfterRetirement())throw std::runtime_error(host.Status());
 if(finite!=w*h*3||!nonblack||!changed)throw std::runtime_error("output finite/nonblack/changed check failed");
 std::cout<<"PASS vendor-neutral host handoff: 80 frames, 3 recycled slots, RG16F guides, reset at frame 70, finite="<<finite<<" nonblack="<<nonblack<<" changedPixels="<<changed<<" busyTryBegin="<<busy<<" hostEvaluateCpuWaits=0 elapsedMs="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
