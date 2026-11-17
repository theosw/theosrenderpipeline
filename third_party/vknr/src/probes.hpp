#pragma once
#include "vk_context.hpp"
namespace nrtest {
struct ProbeResult {bool portable=false;bool rdna3_layout=false;std::string json;};
ProbeResult run_probes(Context& context,const std::filesystem::path& shaders);
}
