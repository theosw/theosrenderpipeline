// Lists the HIP devices this PC's AMD HIP runtime reports, with their Windows
// LUIDs, so TRP can restrict HIP to Skyrim's GPU before HIP loads in Skyrim
// (see src/FrameGen/HipDevicePin.h). Run by TRP without a window; prints one
// record per line to standard output and loads nothing but HIP.
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int wmain()
{
	HMODULE hip{};
	for (const auto* name : { L"amdhip64_7.dll", L"amdhip64_6.dll", L"amdhip64.dll" }) {
		hip = LoadLibraryW(name);  // the same search TRP uses in Skyrim
		if (hip) { break; }
	}
	if (!hip) {
		std::printf("error=no AMD HIP runtime found\n");
		return 2;
	}
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(hip, path, MAX_PATH);
	std::printf("hip=%ls\n", path);
	using Count = int (*)(int*);
	using Luid = int (*)(char*, unsigned*, int);
	using Name = int (*)(char*, int, int);
	const auto count = reinterpret_cast<Count>(GetProcAddress(hip, "hipGetDeviceCount"));
	const auto luid = reinterpret_cast<Luid>(GetProcAddress(hip, "hipDeviceGetLuid"));
	const auto name = reinterpret_cast<Name>(GetProcAddress(hip, "hipDeviceGetName"));
	if (!count) {
		std::printf("error=hipGetDeviceCount missing\n");
		return 3;
	}
	int n{};
	const int result = count(&n);
	std::printf("count=%d result=%d\n", n, result);
	for (int i = 0; result == 0 && i < n && i < 64; ++i) {
		char bytes[8]{}, gpu[256]{};
		unsigned mask{};
		const int luidResult = luid ? luid(bytes, &mask, i) : -1;
		if (name) { name(gpu, sizeof(gpu), i); }
		unsigned long long value{};
		std::memcpy(&value, bytes, sizeof(value));
		for (auto& c : gpu) { if (c == '\n' || c == '\r') { c = ' '; } }
		std::printf("device %d luid=%016llX luid-result=%d name=%s\n", i, value, luidResult, gpu);
	}
	std::fflush(stdout);
	return result == 0 ? 0 : 4;
}
