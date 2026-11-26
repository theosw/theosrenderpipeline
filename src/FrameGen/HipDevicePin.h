#pragma once
#include "../PluginPaths.h"
#include <Windows.h>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// HIP/ZLUDA has crashed inside AMD's HIP runtime on PCs with a second AMD GPU
// (typically a Ryzen iGPU) enabled. HIP reads HIP_VISIBLE_DEVICES once, when it
// starts, so TRP asks a separate process which HIP ordinal is Skyrim's GPU and sets
// the variable before HIP loads in Skyrim. The helper is Windows' rundll32 running
// TRPHipProbeW, exported from TRP's own DLL: nothing extra ships. HIP, ZLUDA and
// the NR runtime then see only that GPU.
namespace TheosRenderPipeline::HipDevicePin
{
	inline constexpr std::array<const wchar_t*, 3> kHipRuntimeDllNames{
		L"amdhip64_7.dll", L"amdhip64_6.dll", L"amdhip64.dll"
	};
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

	// Probe output (Enumerate), one record per line:
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
				const auto rest = line.substr(7);
				const auto space = rest.find(' ');
				const auto ordinal = space == std::string_view::npos ? rest : rest.substr(0, space);
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

	// The probe itself: loads HIP in the current (helper) process and lists each
	// device in the format Parse reads. Returns the helper's exit code.
	inline int Enumerate(std::string& out)
	{
		HMODULE hip{};
		for (const auto* name : kHipRuntimeDllNames) {
			hip = LoadLibraryW(name);  // the same search TRP uses in Skyrim
			if (hip) { break; }
		}
		if (!hip) { out += "error=no AMD HIP runtime found\n"; return 2; }
		out += "hip=" + PluginPaths::ToUtf8(PluginPaths::RawModulePath(hip)) + "\n";
		using Count = int (*)(int*);
		using Luid = int (*)(char*, unsigned*, int);
		using Name = int (*)(char*, int, int);
		const auto count = reinterpret_cast<Count>(GetProcAddress(hip, "hipGetDeviceCount"));
		const auto luid = reinterpret_cast<Luid>(GetProcAddress(hip, "hipDeviceGetLuid"));
		const auto name = reinterpret_cast<Name>(GetProcAddress(hip, "hipDeviceGetName"));
		if (!count) { out += "error=hipGetDeviceCount missing\n"; return 3; }
		int n{};
		const int result = count(&n);
		out += std::format("count={} result={}\n", n, result);
		for (int i = 0; result == 0 && i < n && i < 64; ++i) {
			char bytes[8]{}, gpu[256]{};
			unsigned mask{};
			const int luidResult = luid ? luid(bytes, &mask, i) : -1;
			if (name) { name(gpu, static_cast<int>(sizeof(gpu)), i); gpu[sizeof(gpu) - 1] = '\0'; }
			std::uint64_t value{};
			static_assert(sizeof(value) == sizeof(bytes));
			std::memcpy(&value, bytes, sizeof(value));
			for (auto& c : gpu) { if (c == '\n' || c == '\r') { c = ' '; } }
			out += std::format("device {} luid={:016X} luid-result={} name={}\n", i, value, luidResult, gpu);
		}
		return result == 0 ? 0 : 4;
	}

	// Windows' own rundll32, which runs the probe exported from TRP's DLL.
	inline std::filesystem::path Rundll32()
	{
		wchar_t system[MAX_PATH]{};
		const auto length = GetSystemDirectoryW(system, MAX_PATH);
		return length && length < MAX_PATH ? std::filesystem::path(system) / L"rundll32.exe" : std::filesystem::path{};
	}

	// "<rundll32>" "<TRP DLL>",TRPHipProbe  (rundll32 picks the Unicode TRPHipProbeW export).
	inline std::wstring ProbeCommand(const std::filesystem::path& rundll32, const std::filesystem::path& dll)
	{
		return L"\"" + rundll32.wstring() + L"\" \"" + dll.wstring() + L"\",TRPHipProbe";
	}

	// Runs the probe without a window and returns its standard output.
	// Only the pipe's write end is inherited; the probe is killed at the timeout.
	// The pipe is drained concurrently so verbose driver output cannot fill the
	// buffer and deadlock the child while waiting for exit.
	inline std::string Run(const std::filesystem::path& application, std::wstring command, DWORD timeoutMs,
		DWORD& exitCode, std::string& failure)
	{
		SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
		HANDLE read{}, write{};
		if (!CreatePipe(&read, &write, &security, 1 << 16)) {
			failure = std::format("pipe failed ({})", GetLastError());
			return {};
		}
		SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
		SIZE_T attributeSize{};
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
		std::vector<std::byte> attributeStorage(attributeSize);
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
		const bool initializedAttributes = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) != FALSE;
		const bool listed = initializedAttributes &&
			UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write), nullptr, nullptr) != FALSE;
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdOutput = write;
		startup.StartupInfo.hStdError = write;
		startup.lpAttributeList = listed ? attributes : nullptr;
		PROCESS_INFORMATION process{};
		const BOOL started = listed && CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, application.parent_path().c_str(), &startup.StartupInfo, &process);
		const DWORD startError = started ? 0 : GetLastError();
		if (initializedAttributes) { DeleteProcThreadAttributeList(attributes); }
		CloseHandle(write);
		if (!started) {
			CloseHandle(read);
			failure = std::format("could not start ({})", startError);
			return {};
		}
		std::string output;
		constexpr std::size_t kMaxOutputBytes = 1024 * 1024;
		std::thread reader([&] {
			char buffer[4096];
			DWORD got{};
			while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) {
				if (output.size() < kMaxOutputBytes) {
					const auto keep = (std::min)(static_cast<std::size_t>(got), kMaxOutputBytes - output.size());
					output.append(buffer, keep);
				}
			}
		});
		bool timedOut = false;
		if (WaitForSingleObject(process.hProcess, timeoutMs) != WAIT_OBJECT_0) {
			timedOut = true;
			TerminateProcess(process.hProcess, 1);
			WaitForSingleObject(process.hProcess, 2000);
			failure = std::format("timed out after {} ms", timeoutMs);
		}
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		if (timedOut) {
			// Unblock ReadFile even if a hung child/grandchild still holds the write end open.
			CancelSynchronousIo(reader.native_handle());
		}
		reader.join();
		CloseHandle(read);
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
