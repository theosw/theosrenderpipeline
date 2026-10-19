// TRP own-engine-only adapter. No NVIDIA Latewarp runtime or NGX feature is initialized.
#include "presenter/latewarp12.hpp"
namespace fw {
Latewarp12::~Latewarp12() = default;
bool Latewarp12::initialize(ID3D12Device*, const std::filesystem::path&, const std::filesystem::path&) { status_ = "TRP experiment uses XPAR's own engine"; return false; }
bool Latewarp12::evaluate(ID3D12GraphicsCommandList*, const LatewarpInputs&, bool, const Mat4&, const Mat4&, const Mat4&) { return false; }
void Latewarp12::release_feature() {}
void Latewarp12::shutdown() {}
bool Latewarp12::check(int, const char*) { return false; }
}
