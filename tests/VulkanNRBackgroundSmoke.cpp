#include "FrameGen/SourceDLSSGNeuralRendering.h"
#include <algorithm>
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <d3d12sdklayers.h>
using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char* text){if(FAILED(hr))throw std::runtime_error(text);}
void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&v);
}
int wmain(int argc,wchar_t** argv){try{
 if(argc<3)throw std::runtime_error("usage: TRPVulkanNRBackgroundSmoke <engine-folder> <weights-DLL> [--same-frame] [--validation] [--large] [--residual]");
 bool validation=false,background=true,large=false,residual=false,noCompare=false;
 for(int a=3;a<argc;++a){const std::wstring_view arg=argv[a];if(arg==L"--validation")validation=true;else if(arg==L"--same-frame")background=false;else if(arg==L"--large")large=true;else if(arg==L"--residual")residual=true;else if(arg==L"--no-compare")noCompare=true;else throw std::runtime_error("unknown option");}
 SetEnvironmentVariableW(L"TRP_VKNR_TEST_ROOT",argv[1]);SetEnvironmentVariableW(L"TRP_VKNR_TEST_WEIGHTS",argv[2]);
 SetEnvironmentVariableW(L"TRP_VKNR_TEST_VALIDATION",validation?L"1":nullptr);
 ComPtr<ID3D12Debug> debug;
 if(validation){check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer unavailable");debug->EnableDebugLayer();}
 ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c11;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11");
 ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(d11.As(&dxgi),"DXGI");check(dxgi->GetAdapter(&adapter),"adapter");
 ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d12)),"D3D12");
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
 Interop interop;check(interop.Initialize(d11.Get(),d12.Get(),queue.Get()),"interop");
 const UINT w=large?3030:512,h=large?854:256,gw=w,gh=h;
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
 NeuralPass pass;NeuralOptions options;options.enabled=true;options.backend=NeuralRendering::Backend::Vulkan;
 options=VendorNeutralOptions(options);options.vulkanScheduling=background?NeuralRendering::VulkanScheduling::Background:NeuralRendering::VulkanScheduling::SameFrame;
 options.reconstruction.inputScale=.5f;options.reconstruction.modelRate=NeuralRendering::ModelRate::EveryOtherFrame;
 options.reconstruction.method=residual?NeuralRendering::ResolveMethod::Residual:NeuralRendering::ResolveMethod::Ratio;
 UINT64 frequency{};check(queue->GetTimestampFrequency(&frequency),"frequency");
 ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=6;
 check(d12->CreateQueryHeap(&qd,IID_PPV_ARGS(&queries)),"queries");auto queryBuffer=buffer(48,D3D12_HEAP_TYPE_READBACK);
 UINT64* ticks{};check(queryBuffer->Map(0,nullptr,reinterpret_cast<void**>(&ticks)),"query map");
 bool pending[3]{},ran[3]{};std::vector<double> runs,skips;
 auto harvest=[&](size_t slot){if(pending[slot]){const auto ms=1000.*double(ticks[slot*2+1]-ticks[slot*2])/frequency;(ran[slot]?runs:skips).push_back(ms);pending[slot]=false;}};
 auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};d12->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
 unsigned busy=0;const auto start=std::chrono::steady_clock::now();
 auto deadline=std::chrono::steady_clock::now();
 for(unsigned i=0;i<239;++i){
  // The harness supplies a nominal 60-Hz capture clock, not a game/FG workload.
  if(i)std::this_thread::sleep_until(deadline);deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(16667);
  HRESULT begun;do{begun=interop.Begin(Work::Upscaling,&list,nullptr,true);if(begun==S_FALSE){++busy;std::this_thread::sleep_for(std::chrono::milliseconds(1));}}while(begun==S_FALSE);
  check(begun,"nonblocking begin");const auto slot=interop.CurrentSlot(Work::Upscaling);harvest(slot);
  list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,UINT(slot*2));
  options.compare.split=!noCompare&&i>=140;
  if(!pass.Record(d12.Get(),list,slot,options,i==0||i==69,false,float(gw),float(gh),motion.Get(),depth.Get(),nullptr,colour.Get(),nullptr,frequency,{},nullptr,{}, {},i+1,&interop,queue.Get())){
   if(pass.Preparing()){interop.Discard(Work::Upscaling);std::this_thread::sleep_for(std::chrono::milliseconds(10));--i;continue;}
   interop.Discard(Work::Upscaling);interop.Drain();pass.ReleaseAfterRetirement();throw std::runtime_error(pass.Status());}
  list=interop.CurrentList(Work::Upscaling);
  check(Interop::RecordCopy(list,pass.Corrected(),output.Get()),"corrected copy");
  list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,UINT(slot*2+1));
  list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,UINT(slot*2),2,queryBuffer.Get(),slot*16);
  pending[slot]=true;ran[slot]=pass.ModelActivity()==NRModelActivity::Ran;
  if(i==238){D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
   barrier(list,output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);}
  check(interop.Submit(Work::Upscaling),"consumer submit");
 }
 check(interop.Drain(),"final drain");
 for(size_t slot=0;slot<3;++slot)harvest(slot);queryBuffer->Unmap(0,nullptr);
 pass.RetireTelemetry();const auto telemetry=pass.Telemetry();
 auto report=[](const char* name,std::vector<double> values){std::sort(values.begin(),values.end());if(values.empty())return;std::cout<<name<<" samples="<<values.size()<<" p50="<<values[values.size()/2]<<" p95="<<values[size_t(.95*(values.size()-1))]<<" ms\n";};
 report("D3D12 NR span with submission",runs);report("D3D12 NR span without submission",skips);
 std::cout<<"engineInferenceMs="<<telemetry.vulkanInferenceMs<<" handoffMsPerFrame="<<telemetry.vulkanCopiesMs<<" amortizedWorkMs="<<telemetry.vulkanFrameMs<<" background="<<background<<"\n";

 if(validation){ComPtr<ID3D12InfoQueue> messages;check(d12.As(&messages),"D3D12 info queue");
  for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());messages->GetMessage(i,message,&bytes);
   if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<"\n";throw std::runtime_error("D3D12 validation error");}}
  std::cout<<"D3D12 validation: errors=0\n";}
 void* data{};check(readback->Map(0,nullptr,&data),"readback map");unsigned finite=0,nonblack=0,changed=0;
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){const auto* p=reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(data)+fp.Offset+size_t(y)*fp.Footprint.RowPitch)+x*4;
  for(unsigned c=0;c<3;++c){finite+=(p[c]&0x7c00)!=0x7c00;nonblack+=(p[c]&0x7fff)!=0;}
  // Compare draws a divider even over a no-op result; it is not evidence of NR.
  if(x>w/2+2)changed+=p[0]!=uint16_t(0x3400+(x%32)*8)||p[1]!=0x3800||p[2]!=0x3000;
  if(x==w*3/4&&y==h/2)std::cout<<"sample RGB half="<<std::hex<<p[0]<<","<<p[1]<<","<<p[2]<<std::dec<<"\n";}
 readback->Unmap(0,nullptr);
 std::cout<<"jobsBeforeRetirement="<<pass.VulkanJobsInFlight()<<" resultAge="<<pass.VulkanResultAge()<<" dropped="<<pass.VulkanDropped()<<"\n";
 if(!pass.ReleaseAfterRetirement())throw std::runtime_error(pass.Status());
 if(pass.VulkanJobsInFlight())throw std::runtime_error("job reservation survived successful drain");
 if(finite!=w*h*3||!nonblack||changed<w*h/10)throw std::runtime_error("output finite/nonblack/NR-change (excluding Compare divider) check failed");
 std::cout<<"PASS production NeuralPass Vulkan path: 239 frames, 3 recycled slots, RG16F guides, reset at frame 70, Compare after frame 140, finite="<<finite<<" nonblack="<<nonblack<<" changedPixels="<<changed<<" busyTryBegin="<<busy<<" steadyStateEngineCpuWaits=0 elapsedMs="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
