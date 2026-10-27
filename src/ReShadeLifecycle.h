#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace TheosRenderPipeline
{
    enum class ReShadeLifecycleEvent { InitDevice, DestroyDevice, InitRuntime, DestroyRuntime };
    struct ReShadeLifecycleRecord
    {
        std::uint64_t sequence{}, runtime{}, device{}, nativeDevice{}, nativeQueue{}, backbuffer{};
        std::uint32_t api{};
        ReShadeLifecycleEvent event{};
        bool owned{};
    };
    struct ReShadeLifecycleSnapshot
    {
        std::array<ReShadeLifecycleRecord, 128> records{}; // Chronological, valid prefix.
        std::uint64_t total{}, overwritten{};
        std::size_t count{};
        bool enabled{};
    };
    // Passive, default-off and bounded. Values are identities, never retained
    // interfaces: observing a foreign runtime does not confer ownership.
    class ReShadeLifecycleTrace
    {
    public:
        void Enable(bool enabled) { std::scoped_lock lock(mutex_); enabled_ = enabled; }
        void Record(ReShadeLifecycleRecord record)
        {
            std::scoped_lock lock(mutex_);
            if (!enabled_) { return; }
            record.sequence = ++total_;
            records_[(total_ - 1) % records_.size()] = record;
        }
        ReShadeLifecycleSnapshot Snapshot() const
        {
            std::scoped_lock lock(mutex_);
            ReShadeLifecycleSnapshot result;
            result.enabled = enabled_; result.total = total_;
            result.count = static_cast<std::size_t>((std::min)(total_, std::uint64_t(records_.size())));
            result.overwritten = total_ - result.count;
            for (std::size_t i = 0; i < result.count; ++i) {
                result.records[i] = records_[(result.overwritten + i) % records_.size()];
            }
            return result;
        }
    private:
        mutable std::mutex mutex_;
        bool enabled_{};
        std::uint64_t total_{};
        std::array<ReShadeLifecycleRecord, 128> records_{};
    };
}
