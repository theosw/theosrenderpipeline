#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nrtest {
struct SmallWeights {
    std::string dll_sha256;
    std::string resource_sha256;
    uint64_t resource_bytes = 0;
    uint32_t record_count = 0;
    std::map<std::string, std::vector<uint8_t>> entries;
};
// Reads the caller's DLL as bytes. No LoadLibrary, execution, or weight file writes.
SmallWeights load_small_weights(const std::filesystem::path& dll);
std::string weights_metadata_json(const SmallWeights& weights);
}
