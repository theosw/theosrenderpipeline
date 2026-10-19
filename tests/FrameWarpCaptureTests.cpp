#include "FrameGen/FrameWarpCapture.h"
#include "presenter/renderer.hpp"
#include "presenter/pose.hpp"
#include <DirectXMath.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>
#include <limits>
#include <fstream>
using Microsoft::WRL::ComPtr;
using TheosRenderPipeline::FrameWarp::Capture;
constexpr UINT W = 320, H = 180;
int failures{};
#define EXPECT(c, text) do { if (!(c)) { ++failures; std::printf("FAIL: %s\n", text); } } while (false)
#define REQUIRE(c, text) do { if (!(c)) { std::printf("FAIL: %s\n", text); return 1; } } while (false)
static float Half(std::uint16_t h) { return std::ldexp(float((h & 1023) + ((h & 0x7c00) ? 1024 : 0)), ((h >> 10) & 31) ? int((h >> 10) & 31) - 25 : -24) * (h & 0x8000 ? -1.f : 1.f); }
static double Peak(const std::vector<std::uint16_t>& px, UINT w, UINT h) {
    double best=-1, pos=-1;
    for (UINT x=0; x<w; ++x) { double sum=0; for (UINT y=h*2/5; y<h*3/5; ++y) sum += Half(px[(std::size_t(y)*w+x)*4+1]); if (sum>best) {best=sum;pos=x;} }
    return pos;
}
static int Consume(fw::Shared* sh) {
    const auto cam = sh->slots[0].camera;
    const bool rightHanded = cam.view_to_clip[11] < 0;
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPFrameWarpConsumerFixture"; RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"TRP offline consumer",WS_OVERLAPPEDWINDOW,0,0,W,H,nullptr,nullptr,wc.hInstance,nullptr); REQUIRE(window,"hidden consumer window");
    // Cross-process named-handle intake + own warp. No game or external runtime.
    fw::Renderer renderer; std::string error;
    REQUIRE(renderer.init(sh->adapter,window,W,H,DXGI_FORMAT_R8G8B8A8_UNORM,0,error),error.c_str());
    REQUIRE(renderer.open_session(sh->producer_pid,sh->session,error),error.c_str());
    REQUIRE(InterlockedCompareExchange(&sh->slots[0].state,fw::kReading,fw::kReady)==fw::kReady,"consumer claims slot");
    REQUIRE(renderer.begin_frame(),"presenter begin");
    const auto source=renderer.ingest(*sh,0); REQUIRE(source.valid && source.has_depth && source.has_hudless && source.has_ui,"cross-device full layer intake");
    fw::CameraBasis basis{{0,0,0},{0,1,0},{0,0,1},{1,0,0}};
    fw::Mat4 proj; std::memcpy(proj.data(),cam.view_to_clip,64);
    auto run=[&](double yaw,bool first) {
        if (!first) REQUIRE(renderer.begin_frame(),"warp begin");
        const auto target=fw::apply_rotation(basis,{0,0,1},yaw,0,basis.pos);
        const auto transform=fw::clip_source_to_target(proj,fw::view_matrix(basis,{},fw::view_z_sign(proj.data())),fw::view_matrix(target,{},fw::view_z_sign(proj.data())));
        REQUIRE(renderer.own_warp(source,true,false,transform.data(),cam.depth_inverted!=0),"own engine warp");
        renderer.submit_work(); return 0;
    };
    REQUIRE(run(0,true)==0,"identity warp"); std::vector<std::uint16_t> pixels; UINT w{},h{};
    REQUIRE(renderer.read_back(1,pixels,w,h),"identity readback"); const auto x0=Peak(pixels,w,h);
    EXPECT(std::abs(x0-W/2.0)<3,"identity keeps world marker");
    const auto hud=[&] { const auto i=(std::size_t(16)*w+16)*4; return Half(pixels[i])>.98f && Half(pixels[i+1])<.02f; };
    EXPECT(hud(),"dedicated UI marker intact");
    const double yaw=.08726646, expected=std::tan(yaw)/cam.aspect*(W/2.0);
    REQUIRE(run(yaw,false)==0,"yaw warp"); REQUIRE(renderer.read_back(1,pixels,w,h),"yaw readback"); const auto x1=Peak(pixels,w,h);
    std::printf("%s: world marker %.1f -> %.1f, expected shift %.2f px; UI fixed\n",rightHanded?"RH standard depth":"LH inverted depth",x0,x1,expected);
    EXPECT(x1<x0 && std::abs((x0-x1)-expected)<3,"camera yaw moves scene in correct direction/magnitude"); EXPECT(hud(),"UI stays fixed under rotation");
    renderer.wait_idle(); InterlockedExchange(&sh->slots[0].state,fw::kFree);
    EXPECT(renderer.print_debug_messages()==0,"consumer debug layer clean");
    DestroyWindow(window);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc == 3 && std::strcmp(argv[1],"--consumer")==0) {
        const auto pid=static_cast<DWORD>(std::strtoul(argv[2],nullptr,10));
        const auto mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,fw::map_name(pid).c_str()); REQUIRE(mapping,"consumer mapping");
        auto* shared=static_cast<fw::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(fw::Shared))); REQUIRE(shared,"consumer view");
        REQUIRE(shared->magic==fw::kMagic && shared->version==fw::kVersion && shared->producer_pid==pid,"native protocol identity");
        const int result=Consume(shared); UnmapViewOfFile(shared); CloseHandle(mapping); return result;
    }
    const bool helperSmoke = argc == 3 && std::strcmp(argv[1],"--helper")==0;
    const std::filesystem::path helper = helperSmoke ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))) : std::filesystem::path{};
    const bool rightHanded = argc > 1 && !helperSmoke;
    ComPtr<ID3D12Debug> debug; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device;
    for (UINT i=0; factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) break;
        adapter.Reset();
    }
    if (!device) { std::printf("SKIP: no hardware D3D12 adapter\n"); return 77; }
    DXGI_ADAPTER_DESC1 adapterDesc{}; adapter->GetDesc1(&adapterDesc);
    std::wprintf(L"GPU: %ls\n",adapterDesc.Description);
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; REQUIRE(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))), "queue");
    ComPtr<ID3D12CommandAllocator> allocator; REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))), "allocator");
    ComPtr<ID3D12GraphicsCommandList> list; REQUIRE(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))), "list");
    ComPtr<ID3D12Fence> done; REQUIRE(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done))), "fixture fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); REQUIRE(event,"fixture event"); UINT64 doneValue{};
    auto submit=[&] { REQUIRE(SUCCEEDED(list->Close()),"close fixture list"); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        REQUIRE(SUCCEEDED(queue->Signal(done.Get(),++doneValue)),"fixture signal"); REQUIRE(SUCCEEDED(done->SetEventOnCompletion(doneValue,event)),"fixture event arm");
        REQUIRE(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"fixture retirement"); return 0; };
    auto begin=[&] { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr)); };
    std::array<ComPtr<ID3D12Resource>,5> textures;
    std::array<ID3D12Resource*,5> inputs{};
    ComPtr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,5,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))),"RTV heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int i=0;i<5;++i) {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=i<3?W:W*3/4; d.Height=i<3?H:H*3/4;
        d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Format=i<3?DXGI_FORMAT_R8G8B8A8_UNORM:i==3?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&textures[i]))),"source texture"); inputs[i]=textures[i].Get();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(i)*stride}; device->CreateRenderTargetView(inputs[i],nullptr,rtv);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={inputs[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET}; list->ResourceBarrier(1,&b);
        const float black[4]={0,0,0,i==2?0.f:1.f}; const float depth[4]={0.5f,0,0,0};
        list->ClearRenderTargetView(rtv,i==3?depth:black,0,nullptr);
        if (i<2) { const float white[4]={1,1,1,1}; D3D12_RECT rect{LONG(W/2),0,LONG(W/2+2),H}; list->ClearRenderTargetView(rtv,white,1,&rect); }
        if (i==0 || i==2) { const float red[4]={1,0,0,1}; D3D12_RECT rect{8,8,24,24}; list->ClearRenderTargetView(rtv,red,1,&rect); }
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); list->ResourceBarrier(1,&b);
    }
    REQUIRE(submit()==0,"source initialization"); REQUIRE(begin(),"first capture begin");
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TRPFrameWarpFixture"; RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"TRP offline FrameWarp fixture",WS_OVERLAPPEDWINDOW,0,0,W,H,nullptr,nullptr,wc.hInstance,nullptr); REQUIRE(window,"hidden fixture window");
    Capture capture; capture.Enable(true);
    fw::Camera cam{}; cam.valid=1; cam.near_plane=1; cam.far_plane=1000; cam.fov=1.5707963f; cam.aspect=float(W)/H; cam.depth_inverted=!rightHanded;
    cam.right[1]=cam.up[2]=cam.fwd[0]=1; cam.mvec_scale[0]=cam.mvec_scale[1]=1;
    DirectX::XMFLOAT4X4 packed{};
    const auto projection=rightHanded?DirectX::XMMatrixPerspectiveFovRH(cam.fov,cam.aspect,1,1000):DirectX::XMMatrixPerspectiveFovLH(cam.fov,cam.aspect,1000,1);
    DirectX::XMStoreFloat4x4(&packed,projection); std::memcpy(cam.view_to_clip,&packed,64);
    DirectX::XMStoreFloat4x4(&packed,DirectX::XMMatrixInverse(nullptr,projection)); std::memcpy(cam.clip_to_view,&packed,64);
    for (int i=0;i<4;++i) cam.clip_to_prev_clip[i*5]=1;
    const auto qpc=fw::qpc_now();
    EXPECT(!capture.Record(device.Get(),list.Get(),window,inputs,cam,qpc,false),"ineligible frame rejected");
    EXPECT(!capture.SharedForTests(),"no helper mapping for ineligible frame");
    auto invalidCamera=cam; invalidCamera.pos[0]=std::numeric_limits<float>::quiet_NaN();
    EXPECT(!capture.Record(device.Get(),list.Get(),window,inputs,invalidCamera,qpc,true),"non-finite camera rejected");
    EXPECT(!capture.Record(device.Get(),list.Get(),window,inputs,cam,0,true),"unknown camera sample time rejected");
    auto absent=inputs; absent[2]=nullptr;
    EXPECT(!capture.Record(device.Get(),list.Get(),window,absent,cam,qpc,true),"missing fresh UI rejected");
    EXPECT(!capture.SharedForTests(),"invalid inputs allocate no shared session");
    REQUIRE(capture.Record(device.Get(),list.Get(),window,inputs,cam,qpc,true,helper),"capture records");
    auto* sh=capture.SharedForTests(); REQUIRE(sh,"shared mapping");
    EXPECT(sh->slots[0].state==fw::kWriting && sh->latest_ready_frame==0,"unsubmitted frame invisible");
    EXPECT(!sh->settings.enabled,"helper hidden before first successful submit");
    EXPECT(sh->slots[0].qpc_sim_start==0 && sh->slots[0].qpc_constants==qpc,"unknown simulation time stays unknown");
    // Retiring before submission must fail even if the shared fence is currently 0.
    EXPECT(!capture.Reset(),"recorded resources retained without submission/retirement");
    REQUIRE(submit()==0,"capture submit"); capture.Submitted(queue.Get());
    REQUIRE(queue->Signal(done.Get(),++doneValue)==S_OK,"after capture signal"); done->SetEventOnCompletion(doneValue,event); REQUIRE(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"published copy completion");
    EXPECT(sh->slots[0].state==fw::kReady && sh->latest_ready_frame==1,"submitted frame published");
    EXPECT(sh->slots[0].camera.reset && sh->slots[0].tex[fw::kUi].valid,"reset camera and complete UI");
    EXPECT(sh->settings.warp_engine==1 && sh->settings.frame_cap==0 && sh->generation_calls==0,"own engine without add-on cap or FG");
    if (helperSmoke) {
        // Actual helper, hidden fixture window: checks launch/attach/job cleanup.
        const auto deadline=GetTickCount64()+10000;
        while (!sh->presenter.pid && GetTickCount64()<deadline) Sleep(10);
        REQUIRE(sh->presenter.pid,"actual presenter attaches to native mapping");
        HANDLE observed=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(sh->presenter.pid)); REQUIRE(observed,"owned helper process");
        capture.Suspend("fixture menu"); EXPECT(!sh->settings.enabled,"helper suspended for menu");
        EXPECT(capture.Reset(true),"retired source closes helper job");
        EXPECT(WaitForSingleObject(observed,5000)==WAIT_OBJECT_0,"owned helper exits on job close"); CloseHandle(observed);
        EXPECT(!capture.SharedForTests(),"helper mapping cleaned up");
        CloseHandle(event); DestroyWindow(window);
        std::printf("FrameWarp actual-helper lifecycle: %d failures\n",failures); return failures?1:0;
    }
    // Separate process: D3D12CreateDevice is a per-adapter singleton within one
    // process, so two same-process calls do not verify cross-process ownership.
    wchar_t exe[32768]{}; REQUIRE(GetModuleFileNameW(nullptr,exe,32768),"fixture executable");
    std::wstring command=L"\""+std::wstring(exe)+L"\" --consumer "+std::to_wstring(GetCurrentProcessId());
    const auto report=std::filesystem::path(exe).parent_path()/(L"FrameWarpConsumer-"+std::to_wstring(GetCurrentProcessId())+L".txt");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE reportHandle=CreateFileW(report.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); REQUIRE(reportHandle!=INVALID_HANDLE_VALUE,"consumer report");
    HANDLE nullInput=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=startup.hStdError=reportHandle; startup.hStdInput=nullInput;
    PROCESS_INFORMATION child{};
    REQUIRE(CreateProcessW(exe,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child),"separate consumer process");
    CloseHandle(child.hThread);
    const auto waited=WaitForSingleObject(child.hProcess,20000);
    if (waited!=WAIT_OBJECT_0) { TerminateProcess(child.hProcess,1); WaitForSingleObject(child.hProcess,5000); }
    DWORD childResult=1; GetExitCodeProcess(child.hProcess,&childResult); CloseHandle(child.hProcess);
    CloseHandle(reportHandle); if (nullInput!=INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    { std::ifstream saved(report); std::string line; while(std::getline(saved,line)) std::printf("consumer: %s\n",line.c_str()); }
    EXPECT(waited==WAIT_OBJECT_0 && childResult==0,"cross-process pixel/retirement fixture passes");
    // All Reading slots are GPU-owned; skip rather than overwrite or wait.
    for (auto& slot:sh->slots) InterlockedExchange(&slot.state,fw::kReading);
    REQUIRE(begin(),"busy ring begin"); EXPECT(!capture.Record(device.Get(),list.Get(),window,inputs,cam,fw::qpc_now(),true),"busy ring skips capture");
    EXPECT(capture.Snapshot().dropped==1,"busy ring drop counted");
    EXPECT(capture.Snapshot().copyGPUSamples>0,"copy timestamps collected only after retirement");
    std::printf("Synthetic 320x180 native capture: %.4f ms GPU average (%llu completed samples)\n",
        capture.Snapshot().copyGPUAverageMs, static_cast<unsigned long long>(capture.Snapshot().copyGPUSamples));
    for (auto& slot:sh->slots) InterlockedExchange(&slot.state,fw::kFree);
    capture.Suspend("menu"); EXPECT(!sh->settings.enabled,"menu suspension hides helper");
    REQUIRE(capture.Record(device.Get(),list.Get(),window,inputs,cam,fw::qpc_now(),true),"resumed fresh frame");
    EXPECT(sh->slots[0].camera.reset && sh->slots[0].camera.position_epoch>1,"resume invalidates temporal history");
    REQUIRE(submit()==0,"resumed copy submitted"); capture.Submitted(queue.Get());
    queue->Signal(done.Get(),++doneValue); done->SetEventOnCompletion(doneValue,event); REQUIRE(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"resume retired");
    REQUIRE(begin(),"camera quality rejection begin");
    InterlockedExchange(&sh->game_camera_check,1);
    const auto beforeCameraRejection=capture.Snapshot().published;
    EXPECT(!capture.Record(device.Get(),list.Get(),window,inputs,cam,fw::qpc_now(),true),"rejected native camera suspends capture");
    EXPECT(!sh->settings.enabled && capture.Snapshot().published==beforeCameraRejection,"camera rejection leaves normal presentation visible");
    REQUIRE(SUCCEEDED(list->Close()),"camera rejection list close"); InterlockedExchange(&sh->game_camera_check,0);
    // Hold the GPU behind a fixture fence. A Ready slot whose copy is still
    // queued must not be reused, even when no consumer has claimed it.
    ComPtr<ID3D12Fence> gate; REQUIRE(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))),"stall gate");
    REQUIRE(SUCCEEDED(queue->Wait(gate.Get(),1)),"fixture GPU stall");
    REQUIRE(begin(),"stalled capture begin");
    REQUIRE(capture.Record(device.Get(),list.Get(),window,inputs,cam,fw::qpc_now(),true),"stalled capture recorded");
    REQUIRE(SUCCEEDED(list->Close()),"stalled list close"); ID3D12CommandList* stalledLists[]={list.Get()}; queue->ExecuteCommandLists(1,stalledLists);
    capture.Submitted(queue.Get());
    REQUIRE(sh->slots[0].state==fw::kReady,"stalled copy is signalled but not completed");
    const auto stalledFrame=sh->slots[0].frame_id;
    for (int i=1;i<fw::kSlots;++i) InterlockedExchange(&sh->slots[i].state,fw::kReading);
    ComPtr<ID3D12CommandAllocator> probeAllocator; ComPtr<ID3D12GraphicsCommandList> probe;
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&probeAllocator))),"probe allocator");
    REQUIRE(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,probeAllocator.Get(),nullptr,IID_PPV_ARGS(&probe))),"probe list");
    EXPECT(!capture.Record(device.Get(),probe.Get(),window,inputs,cam,fw::qpc_now(),true),"in-flight Ready slot cannot be reclaimed");
    EXPECT(sh->slots[0].state==fw::kReady && sh->slots[0].frame_id==stalledFrame,"stalled slot metadata stays immutable");
    EXPECT(!capture.Reset(),"GPU stall prevents destruction without host retirement");
    REQUIRE(SUCCEEDED(gate->Signal(1)),"release fixture GPU stall");
    REQUIRE(SUCCEEDED(queue->Signal(done.Get(),++doneValue)),"stall retirement signal"); done->SetEventOnCompletion(doneValue,event);
    REQUIRE(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"stalled capture retires"); REQUIRE(SUCCEEDED(probe->Close()),"unused probe close");
    for (int i=1;i<fw::kSlots;++i) InterlockedExchange(&sh->slots[i].state,fw::kFree);
    // A failed publication must retain the recorded textures, leave the slot
    // Writing, and never re-enable or advance the published frame.
    REQUIRE(begin(),"failed publication begin");
    REQUIRE(capture.Record(device.Get(),list.Get(),window,inputs,cam,fw::qpc_now(),true),"failed-publication copy recorded");
    REQUIRE(submit()==0,"failed-publication copy executed");
    const auto published=capture.Snapshot().published;
    capture.Submitted(nullptr);
    EXPECT(capture.Snapshot().faulted && !sh->settings.enabled && capture.Snapshot().published==published,"failed Signal hides helper without publishing");
    EXPECT(sh->slots[0].state==fw::kWriting && !capture.Reset(),"failed Signal retains resources and rejects unproved cleanup");
    EXPECT(capture.Reset(true),"confirmed host retirement closes shared capture");
    if (info) for (UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes{}; info->GetMessage(i,nullptr,&bytes); std::vector<std::byte> storage(bytes); auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(i,msg,&bytes);
        if (msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { ++failures; std::printf("D3D12 ERROR: %s\n",msg->pDescription); }
    }
    CloseHandle(event); DestroyWindow(window);
    std::printf("FrameWarp native capture checks: %d failures\n",failures); return failures?1:0;
}
