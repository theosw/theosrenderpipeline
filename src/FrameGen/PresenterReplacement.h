#pragma once

#include <dxgi.h>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace TheosRenderPipeline
{
    // Intel returns an SDK enum instead of the failing CreateSwapChain HRESULT.
    // Preserve its native diagnostic when available; unknown messages stay E_FAIL.
    inline HRESULT XeFGNativeCreationResult(std::string_view message)
    {
        constexpr std::string_view prefix = "Failed to create swap chain: 0x";
        const auto at = message.find(prefix);
        if (at == std::string_view::npos) { return E_FAIL; }
        const auto value = message.substr(at + prefix.size());
        std::uint32_t result{};
        if (value.size() < 8) { return E_FAIL; }
        const auto parsed = std::from_chars(value.data(), value.data() + 8, result, 16);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + 8 && FAILED(static_cast<HRESULT>(result)) ?
            static_cast<HRESULT>(result) : E_FAIL;
    }

    // The release callbacks drop native buffers and every proxy interface, then
    // the SDK's private owner. ClearState/Flush must follow the final release,
    // and a fresh queue fence must cover work submitted during SDK destruction.
    template<class Release, class Destroy, class Flush, class Drain>
    HRESULT ReleasePresenterAfterRetirement(Release&& release, Destroy&& destroy, Flush&& flush, Drain&& drain)
    {
        release();
        const auto result = destroy();
        if (FAILED(result)) { return result; }
        flush();
        return drain();
    }

    // One attempt per host frame, with no sleeps and no fault latch for a
    // refused creation. Partial SDK ownership must retire before any retry.
    class PresenterReplacement
    {
    public:
        static constexpr unsigned MaxAttempts = 3;
        void Start() { pending_ = true; native_ = false; attempts_ = 0; firstFailure_ = S_OK; }
        bool Pending() const { return pending_; }
        bool Native() const { return native_; }
        unsigned Attempts() const { return attempts_; }
        HRESULT FirstFailure() const { return firstFailure_; }

        template<class Create, class Discard> HRESULT Advance(Create&& create, Discard&& discard)
        {
            if (!pending_) { return E_UNEXPECTED; }
            ++attempts_;
            const auto result = create(native_);
            if (SUCCEEDED(result)) { pending_ = false; return S_OK; }
            if (SUCCEEDED(firstFailure_)) { firstFailure_ = result; }
            // Unknown GPU retirement/device state is never a retry permission.
            if (result == E_UNEXPECTED || result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
                result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR) { return result; }
            const auto retired = discard();
            if (FAILED(retired)) { return retired; }
            if (Retryable(result) && attempts_ < MaxAttempts) { return S_FALSE; }
            if (native_) { return result; }
            native_ = true; attempts_ = 0;
            return S_FALSE; // Try plain DXGI at the next frame boundary.
        }
        static bool Retryable(HRESULT result)
        {
            // Older XeFG SDKs can omit the native diagnostic and return E_FAIL.
            return result == E_ACCESSDENIED || result == E_FAIL || result == DXGI_ERROR_WAS_STILL_DRAWING ||
                result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        }
    private:
        bool pending_{}, native_{};
        unsigned attempts_{};
        HRESULT firstFailure_{S_OK};
    };
}
