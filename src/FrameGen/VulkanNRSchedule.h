#pragma once
#include <cstdint>
#include <optional>

namespace TheosRenderPipeline::NeuralRendering {
enum class VulkanScheduling : std::uint32_t { SameFrame, Background };
constexpr VulkanScheduling SanitizeVulkanScheduling(long value) {
    return value==1?VulkanScheduling::Background:VulkanScheduling::SameFrame;
}
// One independently owned job. Invalidation never makes its resources reusable:
// only a completion proof (or an explicit teardown drain) clears the reservation.
class VulkanNRSchedule {
public:
    static constexpr unsigned MaxAge=2, HistorySlots=MaxAge+1, MaxInFlight=1;
    struct Job { std::uint64_t frame{},generation{}; bool operator==(const Job&) const=default; };
    struct Completion { std::optional<Job> consume; bool dropped{}; };
    void Invalidate() { ++generation_;history_=false; }
    bool CanSubmit(std::uint64_t frame,unsigned interval) const {
        return !closing_&&!job_&&(!lastSubmit_||frame>=*lastSubmit_+(interval?interval:1));
    }
    bool Submit(std::uint64_t frame,unsigned interval) {
        if(!CanSubmit(frame,interval))return false;
        job_=Job{frame,generation_};lastSubmit_=frame;return true;
    }
    Completion Complete(std::uint64_t frame,bool gpuComplete) {
        if(!job_||!gpuComplete||frame<=job_->frame)return {};
        const auto job=*job_;job_.reset();
        if(closing_||job.generation!=generation_||frame-job.frame>MaxAge){++dropped_;return {{},true};}
        lastAge_=static_cast<unsigned>(frame-job.frame);lastCapture_=job.frame;return {job,false};
    }
    bool Valid(Job job,std::uint64_t frame) const {
        return !closing_&&job.generation==generation_&&frame>job.frame&&frame-job.frame<=MaxAge;
    }
    void Applied() { history_=true; }
    bool HasHistory() const { return history_; }
    unsigned InFlight() const { return job_?1:0; }
    unsigned ResultAge() const { return lastAge_; }
    std::uint64_t DisplayedAge(std::uint64_t frame) const { return history_&&frame>=lastCapture_?frame-lastCapture_:0; }
    std::uint64_t Dropped() const { return dropped_; }
    void Close() { closing_=true;Invalidate(); }
    void Drained() { job_.reset(); } // only after the owner proved GPU completion
private:
    std::optional<Job> job_;
    std::optional<std::uint64_t> lastSubmit_;
    std::uint64_t generation_{1},dropped_{},lastCapture_{};
    unsigned lastAge_{};
    bool history_{},closing_{};
};
struct VulkanNRMeasurements {
    std::uint64_t completed{},frames{};
    double networkTotalMs{},copyTotalMs{};
    double NetworkMs() const { return completed?networkTotalMs/completed:-1; }
    double CopiesPerFrameMs() const { return frames?copyTotalMs/frames:-1; }
    double NetworkPerFrameMs() const { return completed&&frames?networkTotalMs/frames:-1; }
};
}
