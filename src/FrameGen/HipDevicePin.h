#pragma once
#include <Windows.h>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// HIP/ZLUDA has crashed inside AMD's HIP runtime on PCs with a second AMD GPU
// (typically a Ryzen iGPU) enabled. HIP reads HIP_VISIBLE_DEVICES once, when it
// starts, so TRP asks a separate process (TRPHipProbe.exe) which HIP ordinal is
// Skyrim's GPU and sets the variable before HIP loads in Skyrim. HIP, ZLUDA and
// the NR runtime then see only that GPU.
namespace TheosRenderPipeline::HipDevicePin
{
	struct ProbeDevice
	{
		int ordinal{ -1 };
		std::uint64_t luid{};
		bool luidKnown{};
		std::string name;
	};

	struct Probe
	{
		bool hipLoaded{};
		int countResult{ -1 };
		int count{};
		std::string hipPath;
		std::vector<ProbeDevice> devices;
	};

	namespace Detail
	{
		inline std::string_view Value(std::string_view line, std::string_view key)
		{
			const auto at = line.find(key);
			if (at == std::string_view::npos) { return {}; }
			auto rest = line.substr(at + key.size());
			if (key == "name=" || key == "hip=") { return rest; }
			return rest.substr(0, rest.find(' '));
		}
		template <class T> inline bool Number(std::string_view text, T& out, int base = 10)
		{
			if (text.empty()) { return false; }
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out, base);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		}
	}

	// TRPHipProbe.exe output, one record per line:
	//   hip=<path>
	//   count=<n> result=<hipError_t>
	//   device <ordinal> luid=<16 hex> luid-result=<hipError_t> name=<text>
	inline Probe Parse(std::string_view text)
	{
		Probe probe;
		while (!text.empty()) {
			const auto end = text.find('\n');
			auto line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
			if (line.starts_with("hip=")) {
				probe.hipPath = std::string(Detail::Value(line, "hip="));
				probe.hipLoaded = !probe.hipPath.empty();
			} else if (line.starts_with("count=")) {
				Detail::Number(Detail::Value(line, "count="), probe.count);
				Detail::Number(Detail::Value(line, "result="), probe.countResult);
			} else if (line.starts_with("device ")) {
				ProbeDevice device;
				const auto ordinal = line.substr(7, line.find(' ', 7) - 7);
				if (!Detail::Number(ordinal, device.ordinal)) { continue; }
				int luidResult{ -1 };
				Detail::Number(Detail::Value(line, "luid-result="), luidResult);
				device.luidKnown = luidResult == 0 && Detail::Number(Detail::Value(line, "luid="), device.luid, 16) && device.luid != 0;
				device.name = std::string(Detail::Value(line, "name="));
				probe.devices.push_back(std::move(device));
			}
		}
		return probe;
	}

	enum class Outcome
	{
		NotNeeded,         // one AMD GPU: nothing to hide
		UserSetting,       // HIP_VISIBLE_DEVICES already set: respected
		HipAlreadyLoaded,  // too late: HIP read its environment already
		ProbeFailed,       // helper missing, failed, timed out or found no HIP
		NoMatch,           // no HIP device has Skyrim's LUID
		Ambiguous,         // more than one HIP device has it
		Pinned,
	};

	struct Decision
	{
		Outcome outcome{ Outcome::NotNeeded };
		int ordinal{ -1 };
	};

	// Whether to run the probe at all.
	inline Decision Precheck(std::size_t amdHardwareAdapters, bool userSetting, bool hipLoaded)
	{
		if (amdHardwareAdapters < 2) { return { Outcome::NotNeeded }; }
		if (userSetting) { return { Outcome::UserSetting }; }
		if (hipLoaded) { return { Outcome::HipAlreadyLoaded }; }
		return { Outcome::ProbeFailed };  // caller runs the probe
	}

	// Pick the HIP ordinal whose LUID is Skyrim's. LUID only: an ordinal or name
	// must never stand in for identity.
	inline Decision Select(const Probe& probe, std::uint64_t renderingLuid)
	{
		if (!probe.hipLoaded || probe.countResult != 0 || probe.count < 1 || renderingLuid == 0) { return { Outcome::ProbeFailed }; }
		Decision decision{ Outcome::NoMatch };
		for (const auto& device : probe.devices) {
			if (!device.luidKnown || device.luid != renderingLuid) { continue; }
			if (decision.outcome == Outcome::Pinned) { return { Outcome::Ambiguous }; }
			decision = { Outcome::Pinned, device.ordinal };
		}
		return decision;
	}

    // Runs TRPHipProbe.exe without a window and returns its standard output.
	// Only the pipe's write end is inherited; the probe is killed at the timeout.
	inline std::string Run(const std::filesystem::path& exe, DWORD timeoutMs, DWORD& exitCode, std::string& failure) {
	    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	    HANDLE read{}, write{};
	    if (!CreatePipe(&read, &write, &security, 1 << 16)) { failure = std::format("pipe failed ({})", GetLastError()); return {}; }
	    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
	    SIZE_T attributeSize{};
	    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
	    std::vector<std::byte> attributeStorage(attributeSize);
	    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	    bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
			UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write), nullptr, nullptr);
	    STARTUPINFOEXW startup{};
	    startup.StartupInfo.cb = sizeof(startup);
	    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	    startup.StartupInfo.hStdOutput = write; startup.StartupInfo.hStdError = write;
	    startup.lpAttributeList = listed ? attributes : nullptr;
	    PROCESS_INFORMATION process{};
	    std::wstring command = L"\"" + exe.wstring() + L"\"";
	    const BOOL started = listed && CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &process);
	    const DWORD startError = started ? 0 : GetLastError();
	    if (listed) { DeleteProcThreadAttributeList(attributes); }
	    CloseHandle(write);
	    if (!started) { CloseHandle(read); failure = std::format("could not start ({})", startError); return {}; }
	    if (WaitForSingleObject(process.hProcess, timeoutMs) != WAIT_OBJECT_0) {
			TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 2000);
			failure = std::format("timed out after {} ms", timeoutMs);
	    }
	    GetExitCodeProcess(process.hProcess, &exitCode);
	    std::string output; char buffer[4096]; DWORD got{};
	    while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) { output.append(buffer, got); }
	    CloseHandle(read); CloseHandle(process.hThread); CloseHandle(process.hProcess);
	    return output;
	}

	inline const char* Describe(Outcome outcome)
	{
		switch (outcome) {
		case Outcome::NotNeeded: return "one AMD GPU; HIP not restricted";
		case Outcome::UserSetting: return "HIP_VISIBLE_DEVICES already set by the user; left unchanged";
		case Outcome::HipAlreadyLoaded: return "HIP was already loaded in Skyrim; it cannot be restricted now";
		case Outcome::ProbeFailed: return "the HIP device probe failed; HIP sees every AMD GPU";
		case Outcome::NoMatch: return "no HIP device matches Skyrim's GPU; HIP sees every AMD GPU";
		case Outcome::Ambiguous: return "several HIP devices match Skyrim's GPU; HIP sees every AMD GPU";
		case Outcome::Pinned: return "HIP restricted to Skyrim's GPU";
		}
		return "unknown";
	}
}
