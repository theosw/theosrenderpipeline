#include "FrameGen/AdapterInventory.h"
#include <cstdio>
#include <stdexcept>

namespace Inventory = TheosRenderPipeline::AdapterInventory;

static void Require(bool condition, const char* message)
{
	if (!condition) { throw std::runtime_error(message); }
}

int main()
{
	try {
		// No adapter has this LUID: nothing is marked as rendering.
		const auto none = Inventory::List(LUID{ 0x7FFFFFFF, 0x7FFFFFFF });
		Require(!none.empty(), "Windows reports at least one adapter");
		bool software = false;
		for (const auto& e : none) { Require(!e.rendering, "an unknown LUID marks no adapter"); software |= e.software; }
		Require(software, "the software adapter (WARP) is listed and flagged");
		// Each adapter's own LUID marks exactly that adapter.
		for (std::size_t i = 0; i < none.size(); ++i) {
			const auto marked = Inventory::List(none[i].luid);
			std::size_t rendering = 0;
			for (const auto& e : marked) { rendering += e.rendering; }
			Require(rendering == 1 && marked[i].rendering, "exactly the rendering adapter is marked");
			Require(Inventory::Format(i, marked[i]).ends_with(" (rendering)"), "the rendering adapter is labelled");
			std::size_t hardware = 0;
			for (const auto& e : marked) { hardware += !e.software; }
			Require(Inventory::OtherHardware(marked) == hardware - (marked[i].software ? 0 : 1), "other hardware GPUs exclude the rendering one");
		}
		for (std::size_t i = 0; i < none.size(); ++i) { std::printf("%s\n", Inventory::Format(i, none[i]).c_str()); }
		std::puts("Adapter inventory: listing, software flag and rendering mark passed.");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
