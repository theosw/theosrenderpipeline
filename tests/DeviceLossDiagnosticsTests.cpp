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
    Require(log.str().empty(), "absent diagnostics INI logs nothing");
    for (const auto& [text, expected] : std::vector<std::pair<std::string, bool>>{
             {"[DeviceLoss]\n", false}, {"[DeviceLoss]\nEnableDRED=false\n", false},
             {"[DeviceLoss]\nEnableDRED=true\n", true}}) {
        { std::ofstream file(path); file << text; }
        Require(DeviceLossDiagnostics::ReadDREDSetting(path.c_str()) == expected, "INI setting parsed");
    }
    std::filesystem::remove(path);
    Require(Contains(log, "[Diagnostics] TheosRenderPipeline.Diagnostics.ini EnableDRED=true"), "present INI is reported");
    DeviceLossDiagnostics diagnostic;
    const auto beforeConfigure = log.str().size();
    diagnostic.Configure(false);
    Require(log.str().size() == beforeConfigure, "DRED off leaves settings and log untouched");
    Require(!diagnostic.Report(S_OK, "successful operation", 0, nullptr, nullptr, {}), "success is not a failure");
    SetLastError(4321);
    Require(!diagnostic.Report(E_FAIL, "Streamline immediate presentation requires tearing support", 0, nullptr, nullptr, {}),
        "startup/configuration failure is not a GPU failure");
    Require(GetLastError() == 4321, "declined report preserves Win32 error");
    Require(!Contains(log, "[GPUFailure]"), "no GPU report for a non-GPU failure");
    Require(diagnostic.Report(DXGI_ERROR_DEVICE_REMOVED, "startup removal", 7, nullptr, nullptr, {}), "device-loss result reports");
    Require(GetLastError() == 4321, "failure diagnostics preserve Win32 error");
    Require(Contains(log, "reason11Available=false") && Contains(log, "reason12Available=false"), "missing devices are unavailable");
    Require(Contains(log, "retainedInteropFailure=false") && !Contains(log, "retirement wait"), "unobserved interop failure is explicit");
    Require(Contains(log, "DRED not queried: device12Available=false removalObserved=true"), "DRED needs a device");

    // A stalled retirement is not a removal, but the interop retained it.
    InteropFailureDiagnostics stalled;
    stalled.valid = true;
    stalled.stage = "fence stalled retirement";
    stalled.work = Work::FrameGeneration;
    stalled.slot = 2;
    stalled.result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    stalled.wait = { Work::FrameGeneration, 12, 5, 5, 140, 7, stalled.result };
    stalled.waitPolicy = { 20, 100 };
    stalled.waitPerformed = true;
    stalled.waitResult = WAIT_TIMEOUT;
    Require(diagnostic.Report(stalled.result, "begin NR before DLSS", 9, nullptr, nullptr, stalled), "retained interop fault reports");
    Require(Contains(log, "stage=fence stalled retirement work=frame-generation slot=2"), "retained stage and slot");
    Require(Contains(log, "retirement wait work=frame-generation target=12 completed=5->5 waitPerformed=true waitResult=0x00000102 "
        "elapsedMs=140 slices=7 sliceMs=20 stallLimitMs=100 result=0x80070102"), "wait record logged once with its policy");

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

static std::size_t Count(const std::ostringstream& log, std::string_view text)
{
    std::size_t count = 0;
    const auto all = log.str();
    for (auto at = all.find(text); at != std::string::npos; at = all.find(text, at + text.size())) { ++count; }
    return count;
}

static void Routing(std::ostringstream& log)
{
    const auto removed = DXGI_ERROR_DEVICE_REMOVED;
    const auto stalledResult = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    InteropFailureDiagnostics stalled;
    stalled.valid = true;
    stalled.stage = "fence stalled retirement";
    stalled.work = Work::FrameGeneration;
    stalled.result = stalledResult;
    stalled.wait = { Work::FrameGeneration, 12, 5, 5, 140, 7, stalledResult };
    stalled.waitPolicy = { 20, 100 };
    stalled.waitPerformed = true;
    stalled.waitResult = WAIT_TIMEOUT;
    const auto sameWait = stalled.wait;
    const RetirementWaitDiagnostics otherWork{ Work::Upscaling, 30, 28, 30, 2100, 1, S_OK };

    auto r = RouteCheck(S_OK, S_OK, {}, nullptr);
    Require(!r.latch && !r.logExtendedWait, "success with nothing pending is silent");
    r = RouteCheck(S_OK, S_OK, {}, &otherWork);
    Require(!r.latch && r.logExtendedWait, "slow successful wait keeps its warning");
    r = RouteCheck(removed, S_OK, {}, nullptr);
    Require(r.latch && !r.logExtendedWait, "first failure latches");
    r = RouteCheck(stalledResult, S_OK, stalled, &sameWait);
    Require(r.latch && !r.logExtendedWait, "report carrying the failing wait replaces its warning");
    r = RouteCheck(stalledResult, S_OK, stalled, &otherWork);
    Require(r.latch && r.logExtendedWait, "a different work's wait is not in the report");
    r = RouteCheck(removed, S_OK, {}, &sameWait);
    Require(r.latch && r.logExtendedWait, "no retained interop fault: the report carries no wait");
    r = RouteCheck(removed, stalledResult, stalled, &sameWait);
    Require(!r.latch && r.logExtendedWait, "later failure is not reported, so its wait is still logged");
    r = RouteCheck(removed, stalledResult, stalled, nullptr);
    Require(!r.latch && !r.logExtendedWait, "later failure stays unlatched");
    r = RouteCheck(S_OK, stalledResult, stalled, nullptr);
    Require(!r.latch, "success after a fault does not clear or relatch it");

    // The backend applies only routing.latch to its fault and the reporter, so
    // a sequence of checks writes one report for the first failure.
    DeviceLossDiagnostics reporter;
    HRESULT fault = S_OK;
    const auto check = [&](HRESULT result, const char* operation, const InteropFailureDiagnostics& interop,
                           const RetirementWaitDiagnostics* wait) {
        const auto routing = RouteCheck(result, fault, interop, wait);
        if (routing.latch) {
            fault = result;
            (void)reporter.Report(result, operation, 1, nullptr, nullptr, interop);
        }
        return routing;
    };
    check(S_OK, "begin upscaling", {}, &otherWork);
    check(stalledResult, "begin NR before DLSS", stalled, &sameWait);
    check(removed, "outer Present", stalled, nullptr);
    check(removed, "Drain", stalled, &otherWork);
    Require(fault == stalledResult, "first failure stays the backend fault");
    Require(Count(log, "[GPUFailure] first failure") == 1, "exactly one GPU failure report");
    Require(Contains(log, "operation=begin NR before DLSS") && !Contains(log, "operation=outer Present") &&
        !Contains(log, "operation=Drain"), "report names the first failing operation only");
    std::puts("PASS: check routing, first-fault latch, extended-wait ownership and single report");
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
    Require(!healthy.Report(E_INVALIDARG, "validation rejection", 2, device11.Get(), device12.Get(), {}),
        "validation rejection on healthy devices is not a GPU failure");
    Require(!Contains(log, "[GPUFailure]"), "healthy devices produce no GPU report");

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
    Require(Contains(log, "reason12=0x887A0005 (DEVICE_REMOVED)"), "actual D3D12 removal reason written");
    std::printf("WARP removal retained stage=%s waitTarget=%llu waitPerformed=%u\n", retained.stage,
        static_cast<unsigned long long>(retained.wait.target), retained.waitPerformed);
    Require(Contains(log, "retirement wait work=upscaling target=") == (retained.wait.target != 0),
        "wait line present exactly when the failing operation had a retirement target");
    if (!retained.waitPerformed) {
        Require(!Contains(log, "waitResult="), "no wait result reported for a wait that never ran");
    }
    Require(Contains(log, "DRED interfaceResult=0x00000000"), "actual DRED interface queried");
    Require(Contains(log, "DRED breadcrumbsResult="), "actual DRED result or unavailability logged");
    if (dredEnabled) {
        Require(Contains(log, "DRED requested=true configured=true"), "DRED configured before device creation");
        // DRED 1.2 ships with Windows 10 2004 and later, the supported test hosts.
        Require(Contains(log, "breadcrumbContexts=true"), "marker/event context strings explicitly enabled");
        Require(Contains(log, "DRED breadcrumbsResult=0x00000000"), "enabled WARP breadcrumbs accessible");
        // Explicit RemoveDevice can return successful queries with empty data,
        // including with work pending. Do not fabricate a breadcrumb acceptance.
        Require(Contains(log, "name=CopyBufferRegion") || Contains(log, "DRED breadcrumbLists=0 truncated=false"),
            "captured work or explicit empty breadcrumb result");
    }
    Require(interop.Fault() == result, "diagnostics preserve terminal result");
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
    else if (argc > 1 && std::string_view(argv[1]) == "routing") { Routing(log); }
    else { SoftwareDeviceLoss(log, argc > 1 && std::string_view(argv[1]) == "dred"); }
    std::fputs(log.str().c_str(), stdout);
    return 0;
}
