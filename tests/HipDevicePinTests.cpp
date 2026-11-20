#include "FrameGen/HipDevicePin.h"
#include <Windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace Pin = TheosRenderPipeline::HipDevicePin;

static void Require(bool condition, const char* message)
{
	if (!condition) { throw std::runtime_error(message); }
}

int wmain(int argc, wchar_t** argv)
{
	try {
		// A Ryzen iGPU listed first, the Radeon card second (CRLF as a console writes it).
		const auto two = Pin::Parse(
			"hip=C:/Windows/System32/amdhip64_7.dll\r\n"
			"count=2 result=0\r\n"
			"device 0 luid=00000000000149BE luid-result=0 name=AMD Radeon(TM) Graphics\r\n"
			"device 1 luid=00000000000161AC luid-result=0 name=AMD Radeon RX 7900 XTX\r\n");
		Require(two.hipLoaded && two.count == 2 && two.countResult == 0 && two.devices.size() == 2, "probe output parsed");
		Require(two.devices[1].name == "AMD Radeon RX 7900 XTX" && two.devices[1].luid == 0x161AC, "device fields parsed");
		const auto pinned = Pin::Select(two, 0x161AC);
		Require(pinned.outcome == Pin::Outcome::Pinned && pinned.ordinal == 1, "Skyrim's GPU is pinned by LUID, not by position");
		Require(Pin::Select(two, 0x12345).outcome == Pin::Outcome::NoMatch, "an unknown LUID pins nothing");
		auto twice = two; twice.devices[0].luid = 0x161AC;
		Require(Pin::Select(twice, 0x161AC).outcome == Pin::Outcome::Ambiguous, "two matching devices pin nothing");
		auto unknown = two; unknown.devices[1].luidKnown = false;
		Require(Pin::Select(unknown, 0x161AC).outcome == Pin::Outcome::NoMatch, "a device without a LUID is never chosen");
		Require(Pin::Select(Pin::Parse("count=2 result=100\n"), 0x161AC).outcome == Pin::Outcome::ProbeFailed, "a failed count pins nothing");
		Require(Pin::Select(Pin::Parse("error=no AMD HIP runtime found\n"), 0x161AC).outcome == Pin::Outcome::ProbeFailed, "no HIP pins nothing");
		Require(Pin::Select(Pin::Parse(""), 0x161AC).outcome == Pin::Outcome::ProbeFailed, "empty output pins nothing");
		Require(Pin::Parse("device x luid=1 luid-result=0 name=a\n").devices.empty(), "a malformed record is skipped");
		Require(Pin::Precheck(1, false, false).outcome == Pin::Outcome::NotNeeded, "one AMD GPU needs no probe");
		Require(Pin::Precheck(2, true, false).outcome == Pin::Outcome::UserSetting, "the user's HIP_VISIBLE_DEVICES wins");
		Require(Pin::Precheck(2, false, true).outcome == Pin::Outcome::HipAlreadyLoaded, "a loaded HIP cannot be restricted");
		Require(Pin::Precheck(2, false, false).outcome == Pin::Outcome::ProbeFailed, "two AMD GPUs run the probe");
		if (argc > 1) {
			// The real helper on this PC. Without a Radeon it must fail cleanly.
			DWORD exitCode{};
			std::string failure;
			// The launcher TRP itself uses in Skyrim: handle list, no window, timeout.
			const auto output = Pin::Run(argv[1], 15000, exitCode, failure);
			Require(failure.empty(), "the probe ran to completion");
			const auto probe = Pin::Parse(output);
			std::printf("probe exit=%lu output:\n%s", exitCode, output.c_str());
			if (!probe.hipLoaded) {
				Require(exitCode == 2 && output.find("error=no AMD HIP runtime found") != std::string::npos, "no HIP is reported cleanly");
				Require(Pin::Select(probe, 0x161AC).outcome == Pin::Outcome::ProbeFailed, "no HIP leaves HIP unrestricted");
			} else {
				Require(probe.countResult == 0 && probe.count == static_cast<int>(probe.devices.size()), "a HIP PC lists every device");
			}
		}
		std::puts("HIP device pin: parsing, LUID selection, refusals and the real probe passed.");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
