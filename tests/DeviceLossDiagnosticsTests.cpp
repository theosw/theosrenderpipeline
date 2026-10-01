#include "FrameGen/SourceDLSSGDeviceLoss.h"
#include <dxgi1_4.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace TheosRenderPipeline::SourceDLSSG;
using Microsoft::WRL::ComPtr;
static std::ostringstream* capturedLog{};

static void Require(bool value, const char* reason)
{
    if (!value) {
        if (capturedLog) { std::fputs(capturedLog->str().c_str(), stderr); }
        std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1);
    }
}
static void Check(HRESULT result, const char* reason)
{
    if (FAILED(result)) { std::fprintf(stderr, "HRESULT=0x%08X: %s\n", static_cast<unsigned>(result), reason); std::exit(1); }
}
static bool Contains(const std::ostringstream& log, const char* text)
{
    return log.str().find(text) != std::string::npos;
}

static void Contracts(std::ostringstream& log)
{
    const auto path = std::filesystem::current_path() / ("device-loss-test-" + std::to_string(GetCurrentProcessId()) + ".ini");
    Require(!std::filesystem::exists(path), "test INI must not replace an existing file");
    SetLastError(1234);
    Require(!DeviceLossDiagnostics::ReadDREDSetting(path.c_str()), "missing diagnostics INI defaults off");
    Require(GetLastError() == 1234, "INI observation preserves Win32 error");
    for (const auto& [text, expected] : std::vector<std::pair<std::string, bool>>{
             {"[DeviceLoss]\n", false}, {"[DeviceLoss]\nEnableDRED=false\n", false},
             {"[DeviceLoss]\nEnableDRED=true\n", true}}) {
        { std::ofstream file(path); file << text; }
        Require(DeviceLossDiagnostics::ReadDREDSetting(path.c_str()) == expected, "INI setting parsed");
    }
    std::filesystem::remove(path);
    DeviceLossDiagnostics diagnostic;
    diagnostic.Configure(false);
    Require(Contains(log, "existing process settings unchanged"), "disabled configuration is explicit");
    Require(!diagnostic.Report(S_OK, "successful operation", 0, nullptr, nullptr, {}), "success does not consume first failure");
    SetLastError(4321);
    Require(diagnostic.Report(E_FAIL, "startup failure", 7, nullptr, nullptr, {}), "partial startup reports once");
    Require(GetLastError() == 4321, "failure diagnostics preserve Win32 error");
    Require(Contains(log, "reason11Available=false") && Contains(log, "reason12Available=false"), "missing devices are unavailable");
    Require(Contains(log, "retainedInteropFailure=false"), "unobserved interop failure is explicit");
    const auto size = log.str().size();
    Require(!diagnostic.Report(DXGI_ERROR_DEVICE_REMOVED, "later failure", 8, nullptr, nullptr, {}), "first failure stays first");
    Require(log.str().size() == size, "repeated faults do not spam the log");

    D3D12_AUTO_BREADCRUMB_NODE1 node{};
    UINT completed = 2;
    D3D12_AUTO_BREADCRUMB_OP operations[]{D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE,
        D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER, D3D12_AUTO_BREADCRUMB_OP_DISPATCH};
    node.pCommandListDebugNameA = "NR\nlist";
    node.BreadcrumbCount = 3;
    node.pLastBreadcrumbValue = &completed;
    node.pCommandHistory = operations;
    DeviceLossDiagnostics::LogBreadcrumbs({&node});
    Require(Contains(log, "list=NR?list"), "names sanitized");
    Require(Contains(log, "name=Dispatch completed=false"), "next uncompleted op distinguished from completed count");
    completed = 4;
    const auto beforeInvalid = log.str().size();
    DeviceLossDiagnostics::LogBreadcrumbs({&node});
    const auto invalid = log.str().substr(beforeInvalid);
    Require(invalid.find("invalidCompleted=true") != std::string::npos && invalid.find("DRED op") == std::string::npos,
        "invalid completion count never indexes history");
    node.pLastBreadcrumbValue = nullptr;
    node.pCommandHistory = nullptr;
    DeviceLossDiagnostics::LogBreadcrumbs({&node});
    Require(Contains(log, "completedAvailable=false"), "missing completion pointer reported");
    node.pNext = &node;
    DeviceLossDiagnostics::LogBreadcrumbs({&node});
    Require(Contains(log, "breadcrumbLists=32 truncated=true"), "cyclic breadcrumb list bounded");
    node.pNext = nullptr;
    std::vector<D3D12_AUTO_BREADCRUMB_OP> ring(65536, D3D12_AUTO_BREADCRUMB_OP_DISPATCH);
    completed = 65539;
    node.BreadcrumbCount = 65540;
    node.pLastBreadcrumbValue = &completed;
    node.pCommandHistory = ring.data();
    DeviceLossDiagnostics::LogBreadcrumbs({&node});
    Require(Contains(log, "op index=65539") && Contains(log, "count=65540"), "64K ring wrap serialized safely");
    D3D12_DRED_ALLOCATION_NODE1 allocation{};
    allocation.ObjectNameA = "retained resource";
    allocation.pNext = &allocation;
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    fault.PageFaultVA = 0x1234;
    fault.pHeadExistingAllocationNode = &allocation;
    DeviceLossDiagnostics::LogPageFault(fault);
    Require(Contains(log, "pageFaultVA=0x0000000000001234"), "page fault address recorded");
    Require(Contains(log, "kind=existing count=32 truncated=true"), "cyclic allocation list bounded");
    Require(Contains(log, "kind=recently-freed count=0 truncated=false"), "absent allocations explicit");
    std::puts("PASS: settings, first failure, absent devices, preserved errors, bounded DRED serialization");
}

static void SoftwareDeviceLoss(std::ostringstream& log, bool dredEnabled)
{
    DeviceLossDiagnostics diagnostic;
    diagnostic.Configure(dredEnabled);
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter1> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    DXGI_ADAPTER_DESC1 desc{};
    Check(warp->GetDesc1(&desc), "WARP description");
    Require((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, "device-removal fixture must use only a software adapter");
    ComPtr<ID3D12Device> device12;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12)), "WARP D3D12");
    D3D12_FEATURE_DATA_EXISTING_HEAPS existingHeaps{};
    const auto heapSupport = device12->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &existingHeaps, sizeof(existingHeaps));
    std::printf("WARP ExistingHeaps result=0x%08X supported=%u\n", static_cast<unsigned>(heapSupport), existingHeaps.Supported);
    ComPtr<ID3D11Device> device11;
    Check(D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device11, nullptr, nullptr), "WARP D3D11");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "WARP queue");
    Interop interop;
    Check(interop.Initialize(device11.Get(), device12.Get(), queue.Get()), "production interop");
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256;
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES gpuHeap{};
    gpuHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> upload, destination;
    Check(device12->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "WARP copy input");
    Check(device12->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&destination)), "WARP copy output");
    void* data = nullptr;
    Check(upload->Map(0, nullptr, &data), "map copy input");
    std::memset(data, 0x5A, 256);
    upload->Unmap(0, nullptr);
    for (unsigned i = 0; i < 3; ++i) {
        ID3D12GraphicsCommandList* list = nullptr;
        Check(interop.SignalD3D11(Work::Upscaling), "produce inputs");
        Check(interop.Begin(Work::Upscaling, &list), "begin healthy NR transport");
        list->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0, 256);
        Check(interop.Submit(Work::Upscaling), "submit healthy work");
        Check(interop.Drain(), "retire healthy work");
    }
    Require(interop.Ready() && !interop.LastFailure().valid, "healthy transport unchanged");
    DeviceLossDiagnostics healthy;
    Require(healthy.Report(E_INVALIDARG, "validation rejection", 2, device11.Get(), device12.Get(), {}), "non-removal failure report");
    Require(Contains(log, "reason12=0x00000000 (S_OK)"), "healthy device reason explicit");

    // Keep a named command list outstanding at removal. Completed/retired
    // work may legitimately have no retained DRED breadcrumb node.
    ComPtr<ID3D12Fence> gate;
    Check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "WARP pending-work gate");
    Check(queue->Wait(gate.Get(), 1), "block only the software queue");
    ID3D12GraphicsCommandList* pendingList = nullptr;
    Check(interop.Begin(Work::Upscaling, &pendingList), "record pending copy");
    pendingList->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0, 256);
    Check(interop.Submit(Work::Upscaling), "submit pending copy");

    ComPtr<ID3D12Device5> removable;
    Check(device12.As(&removable), "WARP removable device");
    removable->RemoveDevice(); // Only this process's verified software device.
    ID3D12GraphicsCommandList* list = nullptr;
    const auto result = interop.Begin(Work::Upscaling, &list);
    Require(result == DXGI_ERROR_DEVICE_REMOVED, "production Begin detects removed software device");
    Require(!list && !interop.Ready() && interop.Fault() == result, "failed Begin neither records nor recovers");
    const auto retained = interop.LastFailure();
    Require(retained.valid && retained.result == result && retained.work == Work::Upscaling,
        "first failing interop work and HRESULT retained");
    Require(std::string_view(retained.stage) != "unavailable", "exact failed operation retained");
    SetLastError(9876);
    Require(diagnostic.Report(result, "begin NR before DLSS", 12580, device11.Get(), device12.Get(), retained), "real device failure captured");
    Require(GetLastError() == 9876, "real device diagnostics preserve last error");
    Require(Contains(log, "operation=begin NR before DLSS sessionFrame=12580 result=0x887A0005"), "reporter boundary represented");
    Require(Contains(log, "retainedInteropFailure=true") && Contains(log, "work=upscaling slot=1"), "interop context written");
    Require(Contains(log, "DRED interfaceResult=0x00000000"), "actual DRED interface queried");
    Require(Contains(log, "DRED breadcrumbsResult="), "actual DRED result or unavailability logged");
    if (dredEnabled) {
        Require(Contains(log, "DRED requested=true configured=true"), "DRED configured before device creation");
        Require(Contains(log, "DRED breadcrumbsResult=0x00000000"), "enabled WARP breadcrumbs accessible");
        // Explicit RemoveDevice can return successful queries with empty data,
        // including with work pending. Do not fabricate a breadcrumb acceptance.
        Require(Contains(log, "name=CopyBufferRegion") || Contains(log, "DRED breadcrumbLists=0 truncated=false"),
            "captured work or explicit empty breadcrumb result");
    }
    const auto size = log.str().size();
    Require(!diagnostic.Report(result, "outer Present", 12581, device11.Get(), device12.Get(), retained), "follow-on Present failure suppressed");
    Require(log.str().size() == size && interop.Fault() == result, "diagnostics preserve terminal result");
    (void)interop.Drain();
    Require(interop.LastFailure().stage == retained.stage && interop.LastFailure().result == result,
        "later retirement failure cannot overwrite first evidence");
    // No completion proof after removal: keep pending fixture resources alive
    // until process exit, matching production's retained-ownership boundary.
    (void)gate.Detach(); (void)upload.Detach(); (void)destination.Detach();
    std::printf("PASS: production WARP interop removal, native reasons, DRED=%u, first-failure retention\n", dredEnabled);
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::ostringstream log;
    capturedLog = &log;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("device-loss-test", sink));
    if (argc > 1 && std::string_view(argv[1]) == "contracts") { Contracts(log); }
    else { SoftwareDeviceLoss(log, argc > 1 && std::string_view(argv[1]) == "dred"); }
    std::fputs(log.str().c_str(), stdout);
    return 0;
}
