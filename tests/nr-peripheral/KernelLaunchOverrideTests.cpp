// Compile the hook implementation here so the real forwarding paths can be checked
// with NVAPI stubs, without exposing benchmark internals or needing a GPU/runtime.
#include "KernelProfiler.cpp"

namespace
{
	unsigned calls{};
	int status{};
	std::vector<KernelProfiler::Launch> forwarded;
	std::vector<KernelProfiler::LaunchEx> forwardedEx;
	void Require(bool ok, const char* what)
	{
		if (!ok) { std::printf("FAIL %s\n", what); std::exit(1); }
	}
	int __cdecl Chain(ID3D12GraphicsCommandList*, const KernelProfiler::Launch* kernels, std::uint32_t count)
	{
		++calls;
		forwarded.clear();
		if (count) { forwarded.assign(kernels, kernels + count); }
		return status;
	}
	int __cdecl ChainEx(ID3D12GraphicsCommandList*, const KernelProfiler::LaunchEx* kernels, std::uint32_t count)
	{
		++calls;
		forwardedEx.clear();
		if (count) { forwardedEx.assign(kernels, kernels + count); }
		return status;
	}
}

int main()
{
	using namespace KernelProfiler;
	SetEnvironmentVariableA("TRP_NR_KERNEL_PROFILE", nullptr);
	SetEnvironmentVariableA("TRP_NR_LAUNCH_OVERRIDE", "target:64x1x1");
	for (const auto* research : { "0", "2", "10", "" }) {
		SetEnvironmentVariableA("TRP_NR_PASCAL_RESEARCH", research);
		Require(!Install() && overrides.empty(), "override requires exact research=1");
	}
	SetEnvironmentVariableA("TRP_NR_PASCAL_RESEARCH", "1");
	SetEnvironmentVariableA("TRP_NR_LAUNCH_OVERRIDE", "target:0x1x1");
	Require(!Install() && overrides.empty(), "invalid spec does not install hooks");
	SetEnvironmentVariableA("TRP_NR_LAUNCH_OVERRIDE", nullptr);

	for (const auto* spec : { "target:64x1x1", "target:32x4x2:10x20x1:4096", "target:32x4x2::0;other:32x1x1;" }) {
		Require(ParseOverrides(spec), "valid specification");
	}
	for (const auto* spec : { "", ";", "target", ":32x1x1", "target:0x1x1", "target:-1x1x1", "target:32X1X1",
		"target:32x1x1junk", "target:32x1x1:0x1x1", "target:32x1x1:", "target:32x1x1::",
		"target:32x1x1::4294967296", "target:1024x2x1", "target:1x1x65", "target:4294967296x1x1",
		"target:32x1x1;target:64x1x1", "target:32x1x1;;other:32x1x1", "target:32x1x1::0:extra" }) {
		Require(ParseOverrides("previous:32x1x1"), "previous specification");
		Require(!ParseOverrides(spec) && overrides.empty(), "invalid specification rejects all rules");
	}
	Require(ParseOverrides("target:32x4x2:10x20x1:4096;other:64x1x1::0"), "forwarding rules");
	const auto a = reinterpret_cast<Handle>(1), b = reinterpret_cast<Handle>(2), c = reinterpret_cast<Handle>(3);
	names[a] = "target"; names[b] = "untouched"; names[c] = "other";
	const std::uint64_t params[]{ 0x1234, 0x5678 };
	void* pointers[]{ const_cast<std::uint64_t*>(params) };
	const Launch kernels[]{ { a, { 100, 200, 1 }, { 32, 4, 1 }, 128, params, sizeof(params) },
		{ b, { 30, 40, 1 }, { 32, 1, 1 }, 256, params, sizeof(params) },
		{ c, { 50, 60, 1 }, { 32, 1, 1 }, 512, params, sizeof(params) } };
	const LaunchEx ex[]{ { a, kernels[0].grid, kernels[0].block, 128, nullptr, 0, pointers },
		{ b, kernels[1].grid, kernels[1].block, 256, params, sizeof(params), nullptr } };
	realChain = Chain; realChainEx = ChainEx;
	profiling = false; collecting = true; barriers = true; dumpParams = true;
	capture.kernel = "target"; capture.regions.push_back({ 0, 16 }); capture.total = 16;
	status = -37;
	Require(LaunchChain(nullptr, kernels, 3) == status && calls == 1 && forwarded.size() == 3, "one whole ordinary chain and exact result");
	Require(forwarded[0].block.z == 2 && forwarded[0].grid.x == 10 && forwarded[0].grid.y == 20 && forwarded[0].sharedBytes == 4096,
		"ordinary geometry rewrite");
	Require(forwarded[0].params == params && forwarded[0].paramSize == sizeof(params) && forwarded[0].function == a, "parameter storage preserved");
	Require(forwarded[1].grid.x == 30 && forwarded[1].block.x == 32 && forwarded[1].sharedBytes == 256, "unmatched descriptor preserved");
	Require(forwarded[2].grid.x == 50 && forwarded[2].block.x == 64 && forwarded[2].sharedBytes == 0, "omitted grid and explicit zero shared");
	Require(kernels[0].block.z == 1 && kernels[0].grid.x == 100 && kernels[0].sharedBytes == 128, "input descriptor immutable");
	Require(nextQuery == 0 && !heap && !capture.recorded && dumped.empty(), "plain flags add no timestamps, capture or parameter dumps");
	calls = 0;
	Require(LaunchChainEx(nullptr, ex, 2) == status && calls == 1 && forwardedEx.size() == 2, "one whole Ex chain and exact result");
	Require(forwardedEx[0].kernelParams == pointers && !forwardedEx[0].params && forwardedEx[0].paramSize == 0 && forwardedEx[0].block.z == 2,
		"Ex pointer-array parameters preserved");
	Require(forwardedEx[1].params == params && forwardedEx[1].paramSize == sizeof(params) && !forwardedEx[1].kernelParams,
		"Ex packed parameters preserved");
	Require(ex[0].block.z == 1 && !capture.recorded && nextQuery == 0, "Ex input immutable and plain flags inert");
	Require(overrides.at("target").logged && overrides.at("other").logged, "first-application log state");
	calls = 0; status = 0; profiling = true; wholeChains = true; collecting = false;
	Require(LaunchChainEx(nullptr, ex, 2) == 0 && calls == 1 && forwardedEx.size() == 2, "profile=2 preserves Ex chains");
	Require(LaunchChain(nullptr, kernels, 3) == 0 && calls == 2 && forwarded.size() == 3, "profile=2 preserves ordinary chains");
	wholeChains = false; barriers = false; dumpParams = false; capture.kernel.clear(); calls = 0;
	Require(LaunchChain(nullptr, kernels, 3) == 0 && calls == 3 && forwarded.size() == 1, "profile=1 still splits ordinary chains");
	calls = 0;
	Require(LaunchChainEx(nullptr, ex, 2) == 0 && calls == 2 && forwardedEx.size() == 1, "profile=1 still splits Ex chains");
	overrides.clear(); profiling = false; calls = 0;
	Require(LaunchChain(nullptr, nullptr, 0) == 0 && calls == 1 && forwarded.empty(), "empty chain forwarded");
	Require(LaunchChainEx(nullptr, nullptr, 0) == 0 && calls == 2 && forwardedEx.empty(), "empty Ex chain forwarded");
	std::puts("PASS launch override parsing, research gate, ordinary/Ex forwarding, plain flags and whole-chain profiling");
}
