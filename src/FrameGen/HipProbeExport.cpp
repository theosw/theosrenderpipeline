// The HIP device probe, run as `rundll32 TheosRenderPipeline.dll,TRPHipProbe` in
// its own process (see HipDevicePin.h). It never runs inside Skyrim: there HIP
// would read its environment before TRP could restrict it.
#include <Windows.h>
#include <string>
#include "HipDevicePin.h"

extern "C" __declspec(dllexport) void CALLBACK TRPHipProbeW(HWND, HINSTANCE, LPWSTR, int)
{
	std::string out;
	const int code = TheosRenderPipeline::HipDevicePin::Enumerate(out);
	const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD written{};
	if (output && output != INVALID_HANDLE_VALUE) { WriteFile(output, out.data(), static_cast<DWORD>(out.size()), &written, nullptr); }
	ExitProcess(static_cast<UINT>(code));
}
