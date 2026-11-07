#include "weights.hpp"
#include <sstream>
#define NR_DECODER_LIBRARY 1
#include "../vendor/mauri/windows/package/model-tools/dlssnr_extract_model.cpp"

namespace nrtest {
SmallWeights load_small_weights(const fs::path& path) {
    const auto bytes = read_file(path);
    SmallWeights result;
    result.dll_sha256 = Sha256::of(bytes.data(), bytes.size());
    if (result.dll_sha256 != kSmallDllSha256)
        throw Failure("Expected the user's NVIDIA 310.9.16 DLL, SHA-256 " + std::string(kSmallDllSha256) + "; got " + result.dll_sha256);
    const Span data{bytes.data(), bytes.size()};
    const auto resources = Pe(data, "WEIGHTS_CC_SMALL_FP8").weights_ht();
    if (resources.size() != 1) throw Failure("Expected exactly one small-model resource");
    result.resource_bytes = resources[0].n;
    result.resource_sha256 = Sha256::of(resources[0].p, size_t(resources[0].n));
    if (result.resource_bytes != 113893343 || result.resource_sha256 != "f743e8eb320f42bc9bdc992ee2bf51f7bb7c8688ced4ecf5ce6807ec4eef7962")
        throw Failure("Small-model resource identity mismatch");
    const auto records = parse_weights(resources[0]);
    result.record_count = uint32_t(records.size());
    if (records.size() != 77) throw Failure("Expected 77 small-model records");
    const auto tables = model_tables(true);
    const Layers layers(tables);
    Plan plan;
    for (const auto& record : records)
        plan["inventory/weights/" + record.name + ".bin"] = copy_of(record.payload, 0, i64(record.payload.n));
    plan_swin_family(records, layers, plan);
    plan_splitswin(records, layers, plan);
    plan_vit(records, layers, plan);
    plan_preblock(records, plan);
    plan_postblock(records, plan, true);
    for (unsigned i = 0; i < tables.entry_count; ++i) {
        const auto& entry = tables.entries[i];
        auto producer = plan.find(entry.name);
        if (producer == plan.end()) throw Failure(std::string("Missing decoded entry: ") + entry.name);
        auto& decoded = result.entries[entry.name];
        producer->second.produce(decoded);
        if (Sha256::of(decoded.data(), decoded.size()) != entry.sha256)
            throw Failure(std::string("Decoded entry SHA-256 mismatch: ") + entry.name);
    }
    if (result.entries.size() != 255 || tables.verify_extra[0])
        throw Failure("Incomplete small-model entry inventory");
    return result;
}

std::string weights_metadata_json(const SmallWeights& weights) {
    std::ostringstream out;
    out << "{\n  \"dll_sha256\": \"" << weights.dll_sha256 << "\",\n"
        << "  \"resource_sha256\": \"" << weights.resource_sha256 << "\",\n"
        << "  \"resource_bytes\": " << weights.resource_bytes << ",\n"
        << "  \"record_count\": " << weights.record_count << ",\n"
        << "  \"decoded_entries\": " << weights.entries.size() << ",\n"
        << "  \"dll_executed\": false,\n  \"weights_written_to_disk\": false,\n  \"entries\": [\n";
    bool comma = false;
    for (const auto& [name, bytes] : weights.entries) {
        if (comma) out << ",\n";
        comma = true;
        // Names are the compile-time allowlisted upstream names, not arbitrary paths.
        out << "    {\"name\": \"" << name << "\", \"bytes\": " << bytes.size()
            << ", \"sha256\": \"" << Sha256::of(bytes.data(), bytes.size()) << "\"}";
    }
    out << "\n  ]\n}\n";
    return out.str();
}
}
