#include "ReusePlan.h"
#include <iostream>
#include <functional>
#include <span>

using Bottleneck::Record;
using Bottleneck::ReusePlan;
void Check(bool ok) { if (!ok) throw std::runtime_error("Check failed"); }
void Reject(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::runtime_error&) { failed = true; }
    Check(failed);
}
Record R(std::string name, std::initializer_list<std::uint64_t> words)
{
    Record r{std::move(name), {1,1,1,32,1,1,0}, {}};
    r.parameters.resize(words.size() * 8);
    std::memcpy(r.parameters.data(), words.begin(), r.parameters.size()); return r;
}
std::vector<Record> Fixture()
{
    std::vector<Record> r{R("cc_split_swin_16h_final_head_512_wait_fp8", {0x1000,0x2000})};
    r.push_back(R("cc_vit_1d_repack_2d_to_1d_fp8", {0x2000,0x3000,0x800000008}));
    for (unsigned b = 0; b != 8; ++b) {
        r.push_back(R(b ? "cc_vit_1d_ffn_expand_chained_fp8" : "cc_vit_1d_ffn_expand_publish_fp8", {0x3000,0x4000}));
        r.push_back(R("cc_vit_1d_ffn_contract_chained_fp8", {0x4000,0x3000}));
        r.push_back(R("cc_vit_1d_qkv_chained_fp8", {0x3000,0x4000}));
        r.push_back(R("cc_vit_1d_attention_chained_fp8", {0x4000,0x3000}));
        r.push_back(R(b == 7 ? "cc_vit_1d_projection_wait_fp8" : "cc_vit_1d_projection_chained_fp8", {0x3000,0x4000}));
    }
    r.push_back(R("cc_vit_1d_repack_1d_to_2d_fp8", {0x4000,0x5000,0x800000008}));
    r.push_back(R("cc_dec_input_upsample_1024_512_tilesync_fp8", {0x5000,0x6000}));
    return r;
}
unsigned Evaluate(ReusePlan& plan, const std::vector<Record>& records, bool reset, bool enabled, bool retire = true)
{
    plan.Begin(reset, enabled); unsigned dropped{};
    for (const auto& record : records) dropped += plan.Observe(record);
    plan.End();
    if (retire) plan.Retired();
    return dropped;
}
int main() try
{
    const auto records = Fixture();
    { // Alternation, disable/re-enable, reset, and a different feature generation.
        ReusePlan plan;
        Check(Evaluate(plan, records, false, true) == 0);
        Check(Evaluate(plan, records, false, true) == 42);
        Check(Evaluate(plan, records, false, true) == 0);
        Check(Evaluate(plan, records, false, false) == 0);
        Check(Evaluate(plan, records, false, true) == 42);
        Check(Evaluate(plan, records, true, true) == 0);
        Check(Evaluate(plan, records, false, true) == 42);
        ReusePlan recreated;
        Check(Evaluate(recreated, records, false, true) == 0);
    }
    { // Recording does not make a result reusable before GPU completion.
        ReusePlan plan; Evaluate(plan, records, false, true, false);
        Reject([&] { plan.Begin(false, true); });
        plan.Retired(); Check(Evaluate(plan, records, false, true) == 42);
        plan.Failed(); Check(Evaluate(plan, records, false, true) == 0);
    }
    for (unsigned mutation = 0; mutation != 8; ++mutation) {
        ReusePlan plan; Evaluate(plan, records, false, true);
        auto changed = records;
        switch (mutation) {
        case 0: changed[5].name = "foreign_kernel"; break;
        case 1: changed[5].parameters[0] ^= 1; break;
        case 2: changed[5].geometry[0] = 2; break;
        case 3: changed.pop_back(); break;
        case 4: changed.push_back(records.back()); break;
        case 5: changed[5].parameters.resize(8); break;
        case 6: changed[1].parameters[8] ^= 1; break;
        case 7: changed.back().parameters[0] ^= 1; break;
        }
        // In the probe this failed recording is discarded, never submitted.
        Reject([&] { Evaluate(plan, changed, false, true); });
    }
    { // The consumer must use the retained output, with no extra argument aliases.
        auto changed = records; changed.back().parameters[0] ^= 1;
        ReusePlan plan; Reject([&] { Evaluate(plan, changed, false, true); });
        changed = records; changed.push_back(R("unexpected_reader", {0x5000}));
        ReusePlan other; Reject([&] { Evaluate(other, changed, false, true); });
    }
    { // A current input parameter may change outside the skipped range.
        ReusePlan plan; Evaluate(plan, records, true, false);
        auto changed = records; changed.front().parameters[0] ^= 1;
        Check(Evaluate(plan, changed, false, true) == 42);
    }
    std::cout << "PASS: alternation, reset, toggles, retirement, recreation, malformed order, bindings and alias guards\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
