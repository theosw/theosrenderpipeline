#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace TheosRenderPipeline::NeuralRendering::Bottleneck
{
    struct Record
    {
        std::string name;
        std::array<unsigned, 7> geometry{}; // grid, block, shared bytes
        std::vector<unsigned char> parameters;
    };

    // One plan belongs to one feature generation on one ordered host queue.
    // Submitted commits only a successfully queued evaluation. This does not
    // prove GPU retirement: the host still drains before releasing any resource.
    // A mismatch after dropping a launch rejects the entire unsubmitted list.
    class ReusePlan
    {
    public:
        void Begin(bool reset, bool enabled)
        {
            Need(!recording_ && !pending_, "Previous evaluation not submitted");
            if (reset) { warm_.clear(); valid_ = false; }
            current_.clear();
            reuse_ = enabled && valid_ && !reset && !lastReused_;
            recording_ = true;
        }
        bool Observe(Record record)
        {
            Need(recording_, "Launch outside evaluation");
            const auto index = current_.size();
            if (!warm_.empty()) {
                Need(index < warm_.size(), "Extra launch");
                const auto& previous = warm_[index];
                Need(record.name == previous.name && record.geometry == previous.geometry &&
                    record.parameters.size() == previous.parameters.size(), "Launch layout changed");
                if (reuse_ && index >= first_ && index <= last_)
                    Need(record.parameters == previous.parameters, "Bottleneck bindings changed");
            }
            current_.push_back(std::move(record));
            return reuse_ && index >= first_ && index <= last_;
        }
        void End()
        {
            Need(recording_, "No evaluation in progress");
            recording_ = false;
            if (!warm_.empty()) Need(current_.size() == warm_.size(), "Incomplete launch sequence");
            ValidateWarm(); // Recheck the current decoder binding/aliases before every submission.
            pending_ = true;
        }
        void Submitted()
        {
            Need(pending_ && !recording_, "Evaluation not ready for submission");
            pending_ = false;
            if (!reuse_) warm_ = current_;
            valid_ = true;
            lastReused_ = reuse_;
        }
        void Failed() { valid_ = false; pending_ = false; recording_ = false; warm_.clear(); }
        bool Reusing() const { return reuse_; }
        std::size_t First() const { return first_; }
        std::size_t Last() const { return last_; }
    private:
        static void Need(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
        static std::uint64_t Word(const Record& r, std::size_t index)
        {
            Need((index + 1) * 8 <= r.parameters.size(), "Short parameter block");
            std::uint64_t value{}; std::memcpy(&value, r.parameters.data() + index * 8, 8); return value;
        }
        void ValidateWarm()
        {
            // Independently observed on the pinned NR 310.8 binary. Exact names
            // and the 8-block/42-launch range deliberately reject other paths.
            const std::string prefix = "cc_vit_1d_";
            std::vector<std::string> expected{ prefix + "repack_2d_to_1d_fp8" };
            for (unsigned block = 0; block < 8; ++block) {
                expected.push_back(prefix + (block ? "ffn_expand_chained_fp8" : "ffn_expand_publish_fp8"));
                expected.push_back(prefix + "ffn_contract_chained_fp8");
                expected.push_back(prefix + "qkv_chained_fp8");
                expected.push_back(prefix + "attention_chained_fp8");
                expected.push_back(prefix + (block == 7 ? "projection_wait_fp8" : "projection_chained_fp8"));
            }
            expected.push_back(prefix + "repack_1d_to_2d_fp8");
            std::size_t count{};
            for (std::size_t i = 0; i < current_.size(); ++i) {
                if (!current_[i].name.starts_with(prefix)) continue;
                if (!count) first_ = i;
                Need(count < expected.size() && i == first_ + count && current_[i].name == expected[count],
                    "Unrecognized bottleneck sequence");
                ++count;
            }
            Need(count == expected.size(), "Missing bottleneck sequence");
            last_ = first_ + count - 1;
            Need(first_ > 0 && last_ + 1 < current_.size() &&
                current_[first_ - 1].name == "cc_split_swin_16h_final_head_512_wait_fp8" &&
                current_[last_ + 1].name == "cc_dec_input_upsample_1024_512_tilesync_fp8", "Unknown boundary kernels");
            Need(current_[first_].parameters.size() == 24 && current_[last_].parameters.size() == 24,
                "Unknown repack parameter layout");
            const auto output = Word(current_[last_], 1);
            Need(output != 0 && Word(current_[last_ + 1], 0) == output, "Decoder is not consuming the repack output");
            // Guard against observed aliasing. This scans argument values, not
            // device instructions, so it is a feasibility guard, not a proof of
            // all GPU memory dependencies (including offset/interior accesses).
            for (std::size_t i = 0; i < current_.size(); ++i) {
                if (i >= first_ && i <= last_) continue;
                const auto& r = current_[i];
                for (std::size_t word = 0; word * 8 + 8 <= r.parameters.size(); ++word)
                    Need(Word(r, word) != output || (i == last_ + 1 && word == 0), "Additional bottleneck output reference");
            }
        }
        std::vector<Record> warm_, current_;
        std::size_t first_{}, last_{};
        bool valid_{}, lastReused_{}, reuse_{}, recording_{}, pending_{};
    };
}
