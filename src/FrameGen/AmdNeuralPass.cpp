#include "AmdNeuralPass.h"
#include "AmdAdapterSelection.h"
#include "AdapterInventory.h"
#include "HipDevicePin.h"
#include "NeuralRuntimeSession.h"
#include "../../third_party/amd-nr/trp_diagnostics.h"
#include <cstring>
#include "../PluginPaths.h"
#include <dxgi1_4.h>
#include <winternl.h>
#include <bcrypt.h>
#include <d3dkmthk.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <cmath>
#include <fstream>
#include <format>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG {
    namespace {
        using Microsoft::WRL::ComPtr;
        constexpr std::string_view modelHash = "2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F";
        bool OriginalModel(const std::filesystem::path& path) {
            BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
            std::array<unsigned char,32> digest{};
            bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
            std::ifstream input(path, std::ios::binary);
            std::array<char,65536> buffer{};
            if (!input) ok = false;
            while (ok && input) {
                input.read(buffer.data(), buffer.size());
                if (input.gcount() > 0)
                    ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(input.gcount()), 0) >= 0;
            }
            ok = ok && input.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            std::string actual;
            for (auto byte : digest) actual += std::format("{:02X}", byte);
            return ok && actual == modelHash;
        }
        void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                     D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
            list->ResourceBarrier(1, &b);
        }
        float FiniteClamp(float value, float lo, float hi, float fallback) {
            return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
        }
    }
    std::filesystem::path AmdNeuralPass::Directory() {
        return PluginPaths::Directory() / L"TheosRenderPipeline" / L"AMDNR";
    }
    std::filesystem::path AmdNeuralPass::RuntimePath() { return Directory()/L"dlss5nr_runtime.dll"; }
    std::filesystem::path AmdNeuralPass::ModelPath() { return Directory().parent_path()/L"NVIDIA"/L"nvngx_dlssnr_310.9.16.dll"; }
    const char* AmdNeuralPass::Availability(bool amdAdapter) {
#if !defined(TRP_VENDOR_NEUTRAL_NR) && !defined(TRP_AMD_NR_TESTING)
        return NeuralRendering::kVendorEditionReason;
#else
        if (!amdAdapter) return NeuralRendering::kHIPGPUReason;
        if (const auto* reason = NeuralRendering::runtimeSession.Refusal(NeuralRendering::Backend::HIP)) return reason;
        return PrerequisiteReason();
#endif
    }
    const char* AmdNeuralPass::PrerequisiteReason() {
        // The package is immutable during a game session. Retry after a restart.
        static const auto missing = []() -> std::string {
            for (const wchar_t* name : {L"dlss5nr_runtime.dll", L"zluda/zluda_real.dll",
                 L"zluda/nvcuda.dll", L"zluda/nvapi64.dll", L"zluda/nvapi_trp_upstream.dll", L"zluda/nvngx.dll", L"kernels/kernels.txt",
                 L"cache/ComputeCache/zluda2.db", L"cache/ComputeCache/coverage.txt"}) {
                std::error_code error;
                if (!std::filesystem::is_regular_file(Directory()/name, error))
                    return "AMD NR prerequisite missing: " + std::filesystem::path(name).string();
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(ModelPath(), ec)) return "HIP/ZLUDA needs Standard's NVIDIA/nvngx_dlssnr_310.9.16.dll. Install Standard below Universal.";
            wchar_t hipPath[32768]{};
            bool found = false;
            for (const auto* name : {L"amdhip64_7.dll", L"amdhip64_6.dll", L"amdhip64.dll"}) {
                if (GetModuleHandleW(name) || SearchPathW(nullptr,name,nullptr,32768,hipPath,nullptr)) { found=true; break; }
            }
            if (!found) return "HIP/ZLUDA needs a compatible Windows AMD HIP runtime/driver on PATH. Install it and restart Skyrim.";
            return {};
        }();
        return missing.empty() ? nullptr : missing.c_str();
    }
    const char* AmdNeuralPass::AdmissionReason(const NeuralOptions& options, bool xeSSReady) {
        if (!xeSSReady) return "HIP/ZLUDA NR requires TRP XeSS. FSR has not been validated for this backend; select XeSS and Apply.";
        if (!options.beforeUpscaling || options.passes != 1) return "AMD NR requires one world pass before XeSS.";
        if (options.reconstruction.peripheralCompression || options.reconstruction.fusedPreparation ||
            options.reconstruction.modelRate != NeuralRendering::ModelRate::EveryFrame)
            return "AMD NR requires Every frame model rate, with peripheral compression and fused preparation off.";
        return PrerequisiteReason();
    }
    bool AmdNeuralPass::IsAMD(ID3D12Device* device) {
        if (!device) return false;
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x1002;
    }
    dlss5nr_settings AmdNeuralPass::Settings(const NeuralOptions& options) {
        dlss5nr_settings s{}; s.struct_size = sizeof(s); s.enabled = 1;
        s.resolution_scale = FiniteClamp(options.reconstruction.inputScale, .25f, 1.f, .5f);
        s.style = std::clamp(options.tuning.style, 0, 2);
        s.intensity = FiniteClamp(options.tuning.intensity, 0, 1, 1);
        s.local_tone = FiniteClamp(options.tuning.localToneStrength, 0, 1, 1);
        s.local_structure = FiniteClamp(options.tuning.localStructureStrength, 0, 1, 1);
        s.skin_structure = FiniteClamp(options.tuning.skinStructureStrength, -1, 1, -1);
        s.detail_strength = FiniteClamp(options.reconstruction.transferStrength, 0, 2, 1);
        s.colour_strength = FiniteClamp(options.reconstruction.colourStrength, 0, 2, 0);
        s.max_ratio = FiniteClamp(options.reconstruction.maxRatio, 1, 8, 2);
        s.max_age = 8; s.encoding = DLSS5NR_ENCODING_AUTO;
        s.exposure_mode = DLSS5NR_EXPOSURE_AUTO; s.exposure = 1;
        s.composition = DLSS5NR_COMPOSITION_RATIO; s.follow_motion = 1;
        s.network_history = 0; s.wait_for_network = options.amdWaitForNetwork ? 1 : 0;
        s.show_tracking = options.amdShowTracking ? 1 : 0;
        // Retain upstream result blending and the one-pass
        // contract and the existing post-submit Regular wait. Same-frame GPU
        // waiting is substantially more expensive and remains disabled.
        s.passes = 1; s.keep_local_tone = 1; s.blend_frames = 4; s.wait_inside = 0;
        s.network_preset = options.reconstruction.preset == 1 ? 1 : 0;
        return s;
    }
    bool AmdNeuralPass::VerifyHipAdapter(ID3D12Device* device) {
        namespace Match = TheosRenderPipeline::AMDAdapter;
        const auto luid = device->GetAdapterLuid();
        std::memcpy(&adapterLUID_, &luid, sizeof(adapterLUID_));
        auto emit = [&](int level, const std::string& text) { if (log) log(level, text.c_str()); };
        PinHipToRenderingGpu(luid);
        for (const auto* name : {L"amdhip64_7.dll", L"amdhip64_6.dll", L"amdhip64.dll"}) {
            hip_ = GetModuleHandleW(name);
            if (!hip_) hip_ = LoadLibraryW(name);
            if (hip_) break;
        }
        if (!hip_) { message_ = "AMD NR needs a compatible Windows AMD HIP runtime/driver on PATH; install it and restart"; return false; }
        using Count = int (*)(int*);
        using Bus = int (*)(char*, int, int);
        using Name = int (*)(char*, int, int);
        using Luid = int (*)(char*, unsigned*, int);
        const auto count = reinterpret_cast<Count>(GetProcAddress(hip_, "hipGetDeviceCount"));
        const auto bus = reinterpret_cast<Bus>(GetProcAddress(hip_, "hipDeviceGetPCIBusId"));
        const auto name = reinterpret_cast<Name>(GetProcAddress(hip_, "hipDeviceGetName"));
        const auto getLUID = reinterpret_cast<Luid>(GetProcAddress(hip_, "hipDeviceGetLuid"));
        int n{};
        const int countResult = count ? count(&n) : -1;
        emit(2, std::format("AMD NR adapter selection: HIP={} count={} result={} Skyrim LUID={:016X}",
            PluginPaths::ModulePath(hip_).string(), n, countResult, adapterLUID_));
        if (countResult != 0 || n < 1 || n > 64) {
            message_ = std::format("AMD NR HIP enumeration failed (result={}, devices={}); see plugin log", countResult, n);
            return false;
        }
        D3DKMT_OPENADAPTERFROMLUID open{}; open.AdapterLuid = luid;
        const auto openResult = D3DKMTOpenAdapterFromLuid(&open);
        LONG queryResult = openResult;
        Match::PCI windowsPCI{}; bool windowsPCIKnown = false;
        if (openResult == 0) {
            D3DKMT_ADAPTERADDRESS address{};
            D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter = open.hAdapter;
            query.Type = KMTQAITYPE_ADAPTERADDRESS; query.pPrivateDriverData = &address;
            query.PrivateDriverDataSize = sizeof(address);
            queryResult = D3DKMTQueryAdapterInfo(&query);
            D3DKMT_CLOSEADAPTER close{}; close.hAdapter = open.hAdapter; D3DKMTCloseAdapter(&close);
            windowsPCIKnown = queryResult == 0 && address.BusNumber <= 255 && address.DeviceNumber <= 31 && address.FunctionNumber <= 7;
            windowsPCI = {0,address.BusNumber,address.DeviceNumber,address.FunctionNumber};
            emit(2, std::format("AMD NR Windows adapter address: result=0x{:08X} PCI=0000:{:02X}:{:02X}.{:X} valid={}",
                static_cast<unsigned>(queryResult), address.BusNumber,address.DeviceNumber,address.FunctionNumber,windowsPCIKnown));
        } else {
            emit(1, std::format("AMD NR Windows adapter open failed: NTSTATUS=0x{:08X}; trying exact HIP LUID",static_cast<unsigned>(openResult)));
        }
        std::vector<Match::Identity> devices;
        for (int i=0;i<n;++i) {
            Match::Identity candidate; candidate.ordinal=i;
            char pci[64]{}, gpu[256]{}, bytes[8]{}; unsigned mask{};
            const int pciResult = bus ? bus(pci,sizeof(pci),i) : -1;
            const int luidResult = getLUID ? getLUID(bytes,&mask,i) : -1;
            candidate.pciKnown = pciResult==0 && Match::ParsePCI(pci,candidate.pci);
            std::memcpy(&candidate.luid,bytes,sizeof(candidate.luid));
            candidate.luidKnown = luidResult==0 && candidate.luid!=0;
            if (name) name(gpu,sizeof(gpu),i);
            emit(2, std::format("AMD NR HIP device {}: name={} PCI={} PCI-result={} LUID={:016X} LUID-result={} node-mask={}",
                i,gpu,pci,pciResult,candidate.luid,luidResult,mask));
            devices.push_back(candidate);
        }
        const auto selected = Match::Select(devices.data(),devices.size(),adapterLUID_,windowsPCIKnown ? &windowsPCI : nullptr);
        if (selected.result != Match::Result::Selected) {
            message_ = selected.result==Match::Result::Ambiguous ? "AMD NR adapter identity is ambiguous; see plugin log" :
                !windowsPCIKnown ? std::format("AMD NR no matching HIP LUID; Windows PCI query failed (0x{:08X}); see plugin log",static_cast<unsigned>(queryResult)) :
                "AMD NR no HIP device matches Skyrim's LUID/PCI identity; see plugin log";
            emit(0,message_); return false;
        }
        const auto& chosen=devices[static_cast<std::size_t>(selected.ordinal)];
        adapterPCI_ = chosen.pciKnown ? std::format("{:04x}:{:02x}:{:02x}.{:x}",
            chosen.pci.domain,chosen.pci.bus,chosen.pci.device,chosen.pci.function) : "";
        emit(2,std::format("AMD NR selected HIP device {} by {}; expected PCI={} LUID={:016X}; runtime must match independently",
            selected.ordinal,selected.byLUID ? "LUID" : "PCI",adapterPCI_,adapterLUID_));
        return true;
    }
    void AmdNeuralPass::PinHipToRenderingGpu(LUID rendering) {
        namespace Pin = TheosRenderPipeline::HipDevicePin;
        auto emit = [&](int level, const std::string& text) { if (log) log(level, text.c_str()); };
        std::size_t amd{};
        for (const auto& adapter : AdapterInventory::List(rendering)) { amd += adapter.vendor == 0x1002 && !adapter.software; }
        wchar_t existing[256]{};
        const bool userSetting = GetEnvironmentVariableW(L"HIP_VISIBLE_DEVICES", existing, 256) > 0;
        bool hipLoaded{};
        for (const auto* name : {L"amdhip64_7.dll", L"amdhip64_6.dll", L"amdhip64.dll"}) { hipLoaded |= GetModuleHandleW(name) != nullptr; }
        auto decision = Pin::Precheck(amd, userSetting, hipLoaded);
        if (decision.outcome != Pin::Outcome::ProbeFailed) {
            emit(decision.outcome == Pin::Outcome::NotNeeded ? 2 : 1, std::format("AMD NR GPU pin: {} AMD GPUs; {}{}", amd,
                Pin::Describe(decision.outcome), userSetting ? std::format(" (HIP_VISIBLE_DEVICES={})", std::filesystem::path(existing).string()) : ""));
            return;
        }
        // The probe is exported from this DLL (Universal) and run by Windows' rundll32.
        HMODULE self{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&AmdNeuralPass::Directory), &self);
        const auto dll = PluginPaths::RawModulePath(self);
        const auto rundll32 = Pin::Rundll32();
        const auto start = std::chrono::steady_clock::now();
        DWORD exitCode{}; std::string failure;
        std::string output;
        if (!self || dll.empty() || !GetProcAddress(self, "TRPHipProbeW")) { failure = "the HIP device probe is not in this TRP edition"; }
        else if (rundll32.empty()) { failure = "rundll32 was not found"; }
        else { output = Pin::Run(rundll32, Pin::ProbeCommand(rundll32, dll), 15000, exitCode, failure); }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::size_t lineStart = 0;
        while (lineStart < output.size()) {
            const auto end = output.find('\n', lineStart);
            auto line = output.substr(lineStart, end == std::string::npos ? std::string::npos : end - lineStart);
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (!line.empty()) { emit(2, "AMD NR GPU probe: " + line); }
            lineStart = end == std::string::npos ? output.size() : end + 1;
        }
        decision = failure.empty() ? Pin::Select(Pin::Parse(output), adapterLUID_) : Pin::Decision{ Pin::Outcome::ProbeFailed };
        if (decision.outcome == Pin::Outcome::Pinned) {
            const auto value = std::to_wstring(decision.ordinal);
            // Both the C runtime's copy and the process environment: HIP may read either.
            const bool set = _wputenv_s(L"HIP_VISIBLE_DEVICES", value.c_str()) == 0 &&
                SetEnvironmentVariableW(L"HIP_VISIBLE_DEVICES", value.c_str());
            if (!set) { decision = { Pin::Outcome::ProbeFailed }; failure = "the environment could not be set"; }
        }
        emit(decision.outcome == Pin::Outcome::Pinned ? 2 : 1, std::format("AMD NR GPU pin: {} AMD GPUs; {}{}; probe exit={} {:.0f} ms{}",
            amd, Pin::Describe(decision.outcome),
            decision.outcome == Pin::Outcome::Pinned ? std::format(" (HIP_VISIBLE_DEVICES={})", decision.ordinal) : "",
            exitCode, ms, failure.empty() ? "" : "; " + failure));
    }
    void AmdNeuralPass::Log(void* self, int level, const char* text) {
        auto& owner = *static_cast<AmdNeuralPass*>(self);
        if (owner.log) owner.log(level, text);
    }
    bool AmdNeuralPass::Initialize(ID3D12Device* device) {
        if (initialized_) return !failed_;
        initialized_ = true;
        if (!testAPI_) {
            if (const auto* reason = Availability(IsAMD(device))) { message_ = reason; return false; }
            if (const auto* reason = NeuralRendering::runtimeSession.Claim(NeuralRendering::Backend::HIP)) { message_ = reason; return false; }
            if (const auto* reason = PrerequisiteReason()) { message_ = reason; return false; }
            if (!OriginalModel(ModelPath())) {
                message_ = "AMD NR model identity differs; the original signed NVIDIA 310.9.16 model is required";
                return false;
            }
            if (!VerifyHipAdapter(device)) return false;
            for (const auto* name : {L"nvcuda.dll", L"nvapi64.dll", L"nvngx.dll", L"nvngx_dlssnr_310.9.16.dll"}) {
                const auto loaded = GetModuleHandleW(name);
                const auto expected = (std::wstring_view(name) == L"nvngx_dlssnr_310.9.16.dll") ? ModelPath() : Directory()/L"zluda"/name;
                if (loaded && !PluginPaths::EqualPath(PluginPaths::ModulePath(loaded), expected)) {
                    message_ = "AMD NR module conflict: " + std::filesystem::path(name).string() +
                        " loaded from " + PluginPaths::ModulePath(loaded).string();
                    return false;
                }
            }
            module_ = GetModuleHandleW(L"dlss5nr_runtime.dll");
            if (!module_) module_ = LoadLibraryExW((Directory()/L"dlss5nr_runtime.dll").c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module_ || !PluginPaths::EqualPath(PluginPaths::ModulePath(module_), Directory()/L"dlss5nr_runtime.dll")) {
                message_ = "AMD NR runtime could not be loaded from its isolated package directory"; return false;
            }
#define AMD_NR_LOAD(field, symbol) api_.field = reinterpret_cast<decltype(api_.field)>(GetProcAddress(module_, symbol));
            AMD_NR_LOAD(create, "dlss5nr_create"); AMD_NR_LOAD(destroy, "dlss5nr_destroy");
            AMD_NR_LOAD(defaults, "dlss5nr_default_settings"); AMD_NR_LOAD(settings, "dlss5nr_set_settings");
            AMD_NR_LOAD(process, "dlss5nr_process"); AMD_NR_LOAD(present, "dlss5nr_present");
            AMD_NR_LOAD(status, "dlss5nr_get_status");
            AMD_NR_LOAD(adapterSelectionVersion, "dlss5nr_trp_adapter_selection_version");
#undef AMD_NR_LOAD
            if (!api_.adapterSelectionVersion || api_.adapterSelectionVersion()!=1) {
                message_="AMD NR adapter-selection ABI v1 is missing; install the complete matching runtime bundle"; return false;
            }
        }
        if (!api_.create || !api_.destroy || !api_.defaults || !api_.settings ||
            !api_.process || !api_.present || !api_.status) {
            message_ = "AMD NR runtime C interface is incomplete"; return false;
        }
        const auto root = Directory();
        // Cache/log writes go to the user's local data directory, outside MO2's package.
        wchar_t appData[32768]{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, 32768);
        if (!testAPI_ && (!length || length >= 32768)) {
            message_ = "AMD NR cannot locate a writable cache directory"; return false;
        }
        const auto data = std::filesystem::path(appData) / L"TheosRenderPipeline" / L"AMDNR-0.7-310.9.16";
        if (!testAPI_) {
            std::error_code error;
            std::filesystem::create_directories(data, error);
            if (error) { message_ = "AMD NR cache directory could not be created"; return false; }
            // Copy once, before starting the worker; never share a partially staged database.
            const auto sourceCache = root/L"cache/ComputeCache";
            const auto targetCache = data/L"ComputeCache";
            std::filesystem::create_directories(targetCache, error);
            if (error) { message_ = "AMD NR translation cache directory could not be created"; return false; }
            for (const auto* name : {L"zluda2.db", L"coverage.txt"}) {
                const auto target = targetCache/name;
                const auto expected = std::filesystem::file_size(sourceCache/name, error);
                if (error) { message_ = "AMD NR packaged translation cache is unreadable"; return false; }
                if (!std::filesystem::is_regular_file(target, error) ||
                    std::filesystem::file_size(target, error) != expected) {
                    error.clear();
                    const auto temporary = std::filesystem::path(target.wstring() + L".staging");
                    std::filesystem::copy_file(sourceCache/name, temporary,
                        std::filesystem::copy_options::overwrite_existing, error);
                    if (error) { message_ = "AMD NR translation cache could not be staged"; return false; }
                    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        message_ = "AMD NR translation cache could not be published"; return false;
                    }
                }
            }
        }
        trp_dlss5nr_create_info extended{};
        auto& info=extended.base; info.struct_size = sizeof(extended); info.device = device;
        extended.adapter_pci_bus_id=adapterPCI_.empty() ? nullptr : adapterPCI_.c_str();
        extended.adapter_luid=adapterLUID_;
        const auto model = ModelPath();
        info.snippet_path = model.c_str();
        info.runtime_dir = root.c_str(); info.data_dir = data.c_str(); info.log = &Log; info.log_user = this;
        const auto result = api_.create(&info, &context_);
        if (result != 0 || !context_) {
            message_ = std::format("AMD NR creation failed ({})", result); return false;
        }
        return true;
    }
    AmdNeuralPass::~AmdNeuralPass() {
        // Host retirement is a required precondition. The destroy call runs on its own
        // thread; if it does not return in time the context is leaked, never freed
        // under a live worker, and HIP stays off for the session.
        if (context_) {
            struct Done { std::mutex mutex; std::condition_variable wake; bool value{}; };
            const auto done = std::make_shared<Done>();
            std::thread([destroy = api_.destroy, context = context_, done] {
                destroy(context);
                { std::lock_guard lock(done->mutex); done->value = true; }
                done->wake.notify_all();
            }).detach();
            std::unique_lock lock(done->mutex);
            if (!done->wake.wait_for(lock, destroyTimeout, [&] { return done->value; })) {
                NeuralRendering::runtimeSession.Fail(NeuralRendering::Backend::HIP, NeuralRendering::kHIPStalled);
                if (log) log(0, std::format("HIP/ZLUDA shutdown did not finish within {} ms; runtime left running, "
                    "HIP/ZLUDA off until Skyrim restarts", destroyTimeout.count()).c_str());
            }
            context_ = nullptr;
        }
        // Driver/model libraries keep process-wide callbacks, state and threads.
        // Retain the runtime and HIP modules as the upstream chain does. A later
        // owner reuses only exact owned paths; process exit releases the modules.
    }
    void AmdNeuralPass::Refresh() {
        if (!context_) return;
        status_ = {}; status_.struct_size = sizeof(status_); api_.status(context_, &status_);
        failed_ = status_.state == DLSS5NR_STATE_FAILED;
        const char* phase = failed_ ? "failed" : Running() ? "active" :
            status_.state == DLSS5NR_STATE_STARTING ? "loading" : "initializing / awaiting first composed result";
        message_ = std::format("AMD HIP/ZLUDA: {}; {}; composed results={} capture latency={}f result age={}f; {:.1f} ms; native kernels={}",
            phase, status_.message, status_.results, status_.result_latency, status_.result_age, status_.evaluation_ms, status_.native_kernels);
        const auto now = GetTickCount64();
        if (log && (!telemetryLogged_ || status_.state != lastState_ || now - lastTelemetryTick_ >= 10000)) {
            log(2, std::format("HIP/ZLUDA session={} state={} mode={} submissions={} composed={} captureLatency={}f resultAge={}f "
                "inferenceMs={:.2f} inferenceHz={:.2f} network={}x{} manifestKernels={} outputActive={} message={}",
                TRPAmdDiagnostics::Session(), status_.state, waitMode_ == 1 ? "regular-wait" : "async",
                submissions_, status_.results, status_.result_latency, status_.result_age,
                status_.evaluation_ms, status_.evaluations_per_second, status_.network_width, status_.network_height,
                status_.native_kernels, Running(), status_.message).c_str());
            lastTelemetryTick_ = now; lastState_ = status_.state; telemetryLogged_ = true;
        }
    }
    void AmdNeuralPass::CopyOriginal(ID3D12GraphicsCommandList* list, ID3D12Resource* color) {
        Barrier(list, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, output_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output_.Get(), color);
        Barrier(list, output_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    bool AmdNeuralPass::NeedsRecreation(const NeuralOptions& options, UINT w, UINT h, ID3D12Resource* color) const {
        if (!output_) return false;
        const auto d = color ? color->GetDesc() : D3D12_RESOURCE_DESC{};
        const auto out = output_->GetDesc();
        return guideWidth_ != w || guideHeight_ != h || (color &&
            (d.Width != out.Width || d.Height != out.Height || d.Format != out.Format)) ||
            !NeuralRendering::SameReconstructionResources(reconstruction_, options.reconstruction);
    }
    bool AmdNeuralPass::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                             const NeuralOptions& options, bool reset, bool inverted,
                             float scaleX, float scaleY, ID3D12Resource* motion,
                             ID3D12Resource* depth, ID3D12Resource* color) {
        if (!device || !list || !color || !motion || !depth || pending_ ||
            !std::isfinite(scaleX) || !std::isfinite(scaleY) || !scaleX || !scaleY) return false;
        const auto d = color->GetDesc(), m = motion->GetDesc(), z = depth->GetDesc();
        if (!options.beforeUpscaling || !options.WorldOnly() || options.passes != 1 ||
            d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || !d.Height ||
            d.Width > UINT_MAX || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
            m.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || z.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            !m.Width || !m.Height || m.Width > UINT_MAX || m.DepthOrArraySize != 1 || z.DepthOrArraySize != 1 ||
            m.MipLevels != 1 || z.MipLevels != 1 || m.SampleDesc.Count != 1 || z.SampleDesc.Count != 1 ||
            m.Width != z.Width || m.Height != z.Height) {
            message_ = "HIP/ZLUDA NR requires one world pass before XeSS with supported colour/guides"; return false;
        }
        const bool supportedColor = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (output_ && NeedsRecreation(options, static_cast<UINT>(m.Width), m.Height, color)) {
            message_ = "AMD NR resource change requires host retirement before recreation"; return false;
        }
        if (!output_) {
            auto out = d; out.Flags = supportedColor ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &out,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output_)))) return false;
            guideWidth_ = static_cast<UINT>(m.Width); guideHeight_ = m.Height;
            reconstruction_ = options.reconstruction;
        }
        if (!supportedColor) {
            failed_ = true; message_ = "AMD NR colour format is unsupported; preserving the original image and XeSS";
            CopyOriginal(list, color); return true;
        }
        if (failed_ || !Initialize(device)) {
            failed_ = true; CopyOriginal(list, color); return true;
        }
        auto settings = Settings(options);
        if (waitMode_ != settings.wait_for_network) {
            waitMode_ = settings.wait_for_network;
            if (log) log(2, std::format("HIP/ZLUDA session={} execution={} render={}x{} guides={}x{} inputScale={} "
                "history={} followMotion={} maxAge={}", TRPAmdDiagnostics::Session(), waitMode_ ? "regular-wait" : "async",
                d.Width, d.Height, m.Width, m.Height, settings.resolution_scale, settings.network_history,
                settings.follow_motion, settings.max_age).c_str());
            telemetryLogged_ = false;
        }
        api_.settings(context_, &settings);
        Refresh();
        if (failed_) { CopyOriginal(list, color); return true; }
        dlss5nr_frame frame{}; frame.struct_size = sizeof(frame); frame.command_list = list;
        frame.input = color; frame.output = output_.Get(); frame.input_state = frame.output_state = D3D12_RESOURCE_STATE_COMMON;
        frame.width = static_cast<UINT>(d.Width); frame.height = d.Height;
        frame.motion = motion; frame.depth = depth; frame.motion_state = frame.depth_state = D3D12_RESOURCE_STATE_COMMON;
        frame.motion_width = static_cast<UINT>(m.Width); frame.motion_height = m.Height;
        frame.depth_width = static_cast<UINT>(z.Width); frame.depth_height = z.Height;
        frame.motion_scale_x = scaleX; frame.motion_scale_y = scaleY; frame.depth_inverted = inverted;
        frame.pre_exposure = 1; frame.hdr = options.reconstruction.colorIsHDR; frame.reset = reset;
        // Failure may have partially recorded work: never guess states or overwrite it.
        if (api_.process(context_, &frame) != 0) {
            message_ = "AMD NR recording failed; host retains the unsubmitted frame and requires restart";
            failed_ = true; unsafeRecording_ = true; return false;
        }
        pending_ = true; return true;
    }
    bool AmdNeuralPass::Submitted(ID3D12CommandQueue* queue) {
        if (!pending_) return true;
        if (!queue || !context_) return false;
        pending_ = false; ++submissions_; api_.present(context_, queue); Refresh();
        return true; // A model failure is local to NR; process becomes a known original-image copy.
    }
}
