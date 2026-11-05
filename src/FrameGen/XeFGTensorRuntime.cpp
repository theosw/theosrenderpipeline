#include "XeFGTensorRuntime.h"
#include "XeFGUnlock.h"
#include "../PluginPaths.h"
#include <bcrypt.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <fstream>

namespace TheosRenderPipeline
{
    namespace {
        std::string Hash(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return {};
            BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
            auto result = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
            std::array<unsigned char, 65536> buffer{};
            while (result >= 0 && file) {
                file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                if (file.gcount()) result = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0);
            }
            std::array<unsigned char, 32> digest{};
            if (!file.eof()) result = -1;
            if (result >= 0) result = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (result < 0) return {};
            constexpr char hex[] = "0123456789abcdef";
            std::string out; for (auto b : digest) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
        }
    }
    bool XeFGTensorRuntime::Select(ID3D12Device* device, const std::filesystem::path& root, bool requested)
    {
        if (module_) return true;
        if (!requested) return false;
        auto refuse = [&](const char* text) { status_ = std::string(text) + "; existing XeFG runtime retained"; return false; };
        if (!XeFGUnlock::Available()) return refuse("Tensor backend requires Universal");
        if (!device || !root.is_absolute()) return refuse("Tensor device/path unavailable");
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return refuse("Tensor backend requires a supported NVIDIA adapter");
        directory_ = PluginPaths::Normalize(root / L"XeFG-Tensor");
        const std::pair<const wchar_t*, const char*> inputs[]{
            {L"libxess_fg.dll", "752b1491350a29fb2850fa4d0d14f62d79980e0b3cd067d147bb2469c1ec3182"},
            {L"libxell.dll", "d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7"},
            {L"tensor.fatbin", "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7"}};
        for (const auto& [name, expected] : inputs)
            if (Hash(directory_ / name) != expected) return refuse("Tensor runtime bundle missing or hash mismatch");
        // Selection precedes loading either Intel DLL. No new runtime is loaded
        // on a missing/invalid extension, so the ordinary loader remains usable.
        const auto path = directory_ / L"trp_xefg_tensor.dll";
        if (GetModuleHandleW(L"trp_xefg_tensor.dll")) return refuse("Tensor extension already has another owner");
        auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return refuse("Tensor extension not installed");
        auto get = reinterpret_cast<XeFGTensor::GetAPI>(GetProcAddress(module, "TRPXeFGTensorGetAPI"));
        XeFGTensor::API api{};
        if (!PluginPaths::EqualPath(PluginPaths::ModulePath(module), path) || !get || FAILED(get(&api)) ||
            api.size != sizeof(api) || api.version != XeFGTensor::Version || !api.start || !api.retire || !api.snapshot) {
            FreeLibrary(module); return refuse("Tensor extension API mismatch");
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) {
            FreeLibrary(module); return refuse("Tensor extension pin refused");
        }
        module_ = module; api_ = api; status_ = "Tensor runtime selected; awaiting model initialization";
        if (const auto getPacing = reinterpret_cast<XeFGPacing::GetAPI>(GetProcAddress(module, "TRPXeFGPacingGetAPI"))) {
            XeFGPacing::API pacing{};
            if (SUCCEEDED(getPacing(&pacing)) && pacing.size == sizeof(pacing) && pacing.version == XeFGPacing::Version &&
                pacing.registerContext && pacing.unregisterContext && pacing.reset && pacing.setMetering && pacing.snapshot && pacing.setLogger && pacing.setVSyncCadence)
                pacing_ = pacing;
        }
        return true;
    }
    HRESULT XeFGTensorRuntime::RegisterPacing(void* context, ID3D12Device* device, void (*logger)(const char*))
    {
        if (Unsafe()) return E_UNEXPECTED;
        if (!pacing_.registerContext) return S_FALSE;
        pacing_.setLogger(logger); pacing_.setMetering(meteringRequested_); pacing_.setVSyncCadence(vsyncCadenceRequested_);
        char reason[512]{};
        const auto hr = pacing_.registerContext(context, device, reason, sizeof(reason));
        reason[sizeof(reason)-1] = 0; pacingStatus_ = reason;
        const auto flags = PacingStats().flags;
        if ((flags & XeFGPacing::Unsafe) || (FAILED(hr) && (flags & XeFGPacing::Registered))) unsafe_ = true;
        // A clean refusal keeps the SDK's own pacing; uncertainty blocks replacement.
        return unsafe_ ? E_UNEXPECTED : hr;
    }
    HRESULT XeFGTensorRuntime::UnregisterPacing(void* context)
    {
        if (Unsafe()) return E_UNEXPECTED;
        if (!pacing_.unregisterContext) return S_OK;
        char reason[512]{};
        const auto hr = pacing_.unregisterContext(context, reason, sizeof(reason));
        reason[sizeof(reason)-1] = 0; pacingStatus_ = reason;
        if (FAILED(hr)) unsafe_ = true;
        return hr;
    }
    XeFGPacing::Stats XeFGTensorRuntime::PacingStats() const
    {
        XeFGPacing::Stats value{}; if (pacing_.snapshot) pacing_.snapshot(&value);
        if (unsafe_) value.flags |= XeFGPacing::Unsafe;
        return value;
    }
    HRESULT XeFGTensorRuntime::Start(ID3D12Device* device, HMODULE fg, HMODULE ll)
    {
        if (Unsafe()) return E_UNEXPECTED;
        if (!module_ || active_) return S_OK;
        char reason[512]{};
        const auto hr = api_.start(device, fg, ll, directory_.c_str(), reason, sizeof(reason));
        reason[sizeof(reason)-1] = 0;
        status_ = reason;
        const auto state = Snapshot();
        active_ = SUCCEEDED(hr) || (state.flags & XeFGTensor::Unsafe) != 0;
        return hr;
    }
    HRESULT XeFGTensorRuntime::Retire()
    {
        if (Unsafe()) return E_UNEXPECTED;
        if (!active_) return S_OK;
        char reason[512]{};
        const auto hr = api_.retire(reason, sizeof(reason)); reason[sizeof(reason)-1] = 0;
        status_ = reason;
        if (SUCCEEDED(hr)) active_ = false;
        else unsafe_ = true;
        return hr;
    }
    XeFGTensor::Stats XeFGTensorRuntime::Snapshot() const
    {
        XeFGTensor::Stats value{}; if (module_) api_.snapshot(&value);
        if (unsafe_) value.flags |= XeFGTensor::Unsafe;
        return value;
    }
}
