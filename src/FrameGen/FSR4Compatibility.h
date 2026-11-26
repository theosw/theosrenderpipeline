#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <array>
#include <filesystem>
#include <string>

namespace TheosRenderPipeline::FSR4Compatibility
{
    // Experimental, exact-build admission only. Sole SDK owner, before ANY query,
    // context or concurrent SDK call. A fresh process is required after refusal.
    // This module does not select a provider or claim neural execution.
    class Session final
    {
    public:
        enum class State { Fresh, Loaded, Patched, Refused, Poisoned };
        Session() = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        bool LoadVerified(const std::filesystem::path& directory);
        bool ApplyBeforeFirstQuery(ID3D12Device* device, LUID intendedAdapter, bool explicitlyRequested);
        HMODULE Loader() const { return state_ == State::Loaded || state_ == State::Patched ? loader_ : nullptr; }
        HMODULE Upscaler() const { return state_ == State::Loaded || state_ == State::Patched ? upscaler_ : nullptr; }
        State Status() const { return state_; }
        const std::string& Reason() const { return reason_; }
        const std::string& Audit() const { return audit_; }
    private:
        void CloseFiles();
        bool Refuse(const std::string& reason);
        State state_{State::Fresh};
        HMODULE loader_{}, upscaler_{}; // Deliberately resident, including failures.
        std::array<HANDLE, 2> files_{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        std::string reason_, audit_;
    };
    std::string Sha256(const void* data, size_t size);
    // Bounded, read-only inspection for the exact-build standalone observer.
    bool ReadMemory(const void* address, void* destination, size_t size);
    // Empty when the exact-build forced INT8 model is confirmed built behind ffxContext.
    std::string VerifyForcedModel(const void* ffxContext, HMODULE upscaler);
}
